/* linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 *
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When clear_screen() is called, two additional escape sequences are used in
 * order to clear the screen and position the cursor at home position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */
#include <peelo/prompt.hpp>

#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <deque>

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096

namespace peelo
{
  namespace prompt
  {
    static const char* unsupported_term[] =
    {
      "dumb",
      "cons25",
      "emacs",
      NULL
    };
    static bool multi_line = false;
    static bool raw_mode = false;
    static ::termios original_termios;
    static bool atexit_registered = false;

    static void disable_raw_mode(int);

    enum key
    {
      key_ctrl_a = 1,
      key_ctrl_b = 2,
      key_ctrl_c = 3,
      key_ctrl_d = 4,
      key_ctrl_e = 5,
      key_ctrl_f = 6,
      key_ctrl_h = 8,
      key_tab = 9,
      key_ctrl_k = 11,
      key_ctrl_l = 12,
      key_enter = 13,
      key_ctrl_n = 14,
      key_ctrl_p = 16,
      key_ctrl_t = 20,
      key_ctrl_u = 21,
      key_ctrl_w = 23,
      key_esc = 27,
      key_backspace = 127
    };

    /**
     * The input state structure represents the state during line editing. We
     * pass this state to functions implementing specific editing
     * functionalities.
     */
    namespace
    {
      struct input_state
      {
        /** Terminal stdin file descriptor. */
        int ifd;
        /** Terminal stdout file descriptor. */
        int ofd;
        /** Edited line buffer. */
        char buf[LINENOISE_MAX_LINE];
        /** Size of the line buffer. */
        std::size_t buflen;
        /** Prompt to display. */
        std::string prompt;
        /** Current cursor position. */
        std::size_t pos;
        /** Previous refresh cursor position. */
        std::size_t oldpos;
        /** Current edited line length. */
        std::size_t len;
        /** Number of columns in terminal. */
        std::size_t cols;
        /** Maximum number of rows used so far (multiline mode). */
        std::size_t maxrows;
        /** The history index we are currently editing. */
        int history_index;
      };
    }

    static void refresh(input_state&);

    bool is_multi_line()
    {
      return multi_line;
    }

    void set_multi_line(bool flag)
    {
      multi_line = flag;
    }

    /**
     * Returns true if the terminal name is in the list of terminals we know
     * are not able to understand basic escape sequences.
     */
    static bool is_unsupported_term()
    {
      auto term = std::getenv("TERM");

      if (!term)
      {
        return false;
      }
      for (int i = 0; unsupported_term[i]; ++i)
      {
        if (!::strcasecmp(term, unsupported_term[i]))
        {
          return true;
        }
      }

      return false;
    }

    /**
     * At exit we'll try to fix the terminal to the initial conditions.
     */
    static void atexit_callback()
    {
      disable_raw_mode(STDIN_FILENO);
    }

    /**
     * Raw mode: 1960 magic shit.
     */
    static bool enable_raw_mode(int fd)
    {
      ::termios raw;

      if (!::isatty(STDIN_FILENO))
      {
        return false;
      }

      if (!atexit_registered)
      {
        std::atexit(atexit_callback);
        atexit_registered = true;
      }

      if (::tcgetattr(fd, &original_termios) == -1)
      {
        return false;
      }

      // Modify the original one.
      raw = original_termios;

      // Input modes: No break, no CR to NL, no parity check, no strip char, no
      // start/stop output control.
      raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

      // Output modes: Disable post processing.
      raw.c_oflag &= ~(OPOST);

      // Control modes: Set 8 bit chars.
      raw.c_cflag |= (CS8);

      // Local modes: Echoing off, canonical off, no extended functions, no
      // signal chars (^Z, ^C).
      raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

      // Control chars: Set return condition: min number of bytes and timer. We
      // want to read to return every single byte, without timeout.
      raw.c_cc[VMIN] = 1;
      raw.c_cc[VTIME] = 0;

      // Put terminal in raw mode after flushing.
      if (::tcsetattr(fd, TCSAFLUSH, &raw) < 0)
      {
        return false;
      }

      raw_mode = true;

      return true;
    }

    static void disable_raw_mode(int fd)
    {
      if (raw_mode)
      {
        ::tcsetattr(fd, TCSAFLUSH, &original_termios);
        raw_mode = false;
      }
    }

    /**
     * Use the ESC [6n escape sequence to query the horizontal cursor position
     * and return it. On error, -1 is returned, on success the position of the
     * cursor.
     */
    static int get_cursor_position(int ifd, int ofd)
    {
      char buffer[32];
      int cols;
      int rows;
      std::size_t i = 0;

      // Report cursor location.
      if (::write(ofd, "\033[6n", 4) != 4)
      {
        return -1;
      }

      // Read the response: ESC [ rows ; cols R
      while (i < sizeof(buffer))
      {
        if (::read(ifd, buffer + i, 1) != 1 || buffer[i] == 'R')
        {
          break;
        }
        ++i;
      }
      buffer[i] = '\0';

      // Parse it.
      if (buffer[0] != key_esc || buffer[1] != '['
          || std::sscanf(buffer + 2, "%d;%d", &rows, &cols) != 2)
      {
        return -1;
      }

      return cols;
    }

    /**
     * Try to get the number of columns in the current terminal, or assume 80
     * if it fails.
     */
    static int get_columns(int ifd, int ofd)
    {
      ::winsize ws;

      if (::ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
      {
        // ioctl() failed. Try to query the terminal itself.
        int start;
        int cols;

        // Get the initial position so we can restore it later.
        if ((start = get_cursor_position(ifd, ofd)) == -1)
        {
          goto FAILED;
        }

        // Go right margin and get position.
        if (::write(ofd, "\033[999C", 6) != 6)
        {
          goto FAILED;
        }

        if ((cols = get_cursor_position(ifd, ofd)) == -1)
        {
          goto FAILED;
        }

        // Restore position.
        if (cols > start)
        {
          char sequence[32];

          std::snprintf(sequence, 32, "\033[%dD", cols - start);
          ::write(ofd, sequence, std::strlen(sequence));
        }

        return cols;
      } else {
        return ws.ws_col;
      }

FAILED:
      return 80;
    }

    void clear_screen()
    {
      ::write(STDOUT_FILENO, "\033[H\033[2J", 7);
    }

    /**
     * Beep, used for completion when there is nothing to complete or when all
     * the choices were already shown.
     */
    static void beep()
    {
      std::fprintf(stderr, "\x7");
      std::fflush(stderr);
    }

    namespace history
    {
      static size_type max_size = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
      static std::deque<std::string> container;

      bool add(const std::string& line)
      {
        if (max_size == 0)
        {
          return false;
        }

        // Don't add duplicated lines.
        if (!container.empty() && !container.back().compare(line))
        {
          return false;
        }

        if (container.size() == max_size)
        {
          container.pop_front();
        }

        container.push_back(line);

        return true;
      }

      size_type get_max_size()
      {
        return max_size;
      }

      void set_max_size(size_type size)
      {
        if (size == 0)
        {
          container.clear();
        } else {
          while (container.size() > size)
          {
            container.pop_front();
          }
        }
        max_size = size;
      }
    }

    namespace completion
    {
      static std::optional<callback_type> callback;

      // This is an helper function for edit() and is called when the user
      // types the <tab> key in order to complete the string currently in the
      // input.
      //
      // The state of the editing is encapsulated into the pointed input state
      // structure as described in the structure definition.
      static int complete_line(input_state& state)
      {
        container_type completions;
        int c;

        if (callback)
        {
          (*callback)(std::string(state.buf, state.len), completions);
        }

        if (completions.empty())
        {
          beep();
        } else {
          bool stop = false;
          container_type::size_type i = 0;

          while (!stop)
          {
            // Show completion or original buffer.
            if (i < completions.size())
            {
              const auto& completion = completions[i];
              input_state saved = state;

              state.len = state.pos = completion.length();
              std::strncpy(state.buf, completion.c_str(), state.buflen);
              refresh(state);
              state.len = saved.len;
              state.pos = saved.pos;
              std::strncpy(state.buf, saved.buf, state.buflen);
            } else {
              refresh(state);
            }

            if (::read(state.ifd, &c, 1) <= 0)
            {
              return -1;
            }

            switch (c)
            {
              case key_tab:
                i = (i + 1) % (completions.size() + 1);
                if (i == completions.size())
                {
                  beep();
                }
                break;

              case key_esc:
                // Re-show original buffer.
                if (i < completions.size())
                {
                  refresh(state);
                }
                stop = true;
                break;

              default:
                // Update buffer and return.
                if (i < completions.size())
                {
                  const auto written = std::snprintf(
                    state.buf,
                    state.buflen,
                    "%s",
                    completions[i].c_str()
                  );

                  state.len = state.pos = written;
                }
                stop = true;
                break;
            }
          }
        }

        return c;
      }

      void set_callback(const std::optional<callback_type>& cb)
      {
        callback = cb;
      }
    }

    namespace hints
    {
      static std::optional<callback_type> callback;

      void set_callback(const std::optional<callback_type>& cb)
      {
        callback = cb;
      }

      /**
       * Helper of refresh_single_line() and refresh_multi_line() to show hints
       * to the right of the prompt.
       */
      static void show(std::string& buffer, input_state& state)
      {
        const auto plen = state.prompt.length();
        char seq[64];
        color col = color::none;
        bool bold = false;

        if (!callback || plen + state.len >= state.cols)
        {
          return;
        }

        if (auto hint = (*callback)(std::string(state.buf, state.len),
                                    col,
                                    bold))
        {
          const auto& value = hint.value();
          auto hintlen = value.length();
          auto hintmaxlen = state.cols - (plen + state.len);

          if (hintlen > hintmaxlen)
          {
            hintlen = hintmaxlen;
          }
          if (bold && col == color::none)
          {
            col = color::white;
          }
          if (col != color::none || bold)
          {
            std::snprintf(
              seq,
              64,
              "\033[%d;%d;49m",
              bold ? 1 : 0,
              static_cast<int>(col)
            );
          } else {
            seq[0] = '\0';
          }
          buffer.append(seq, std::strlen(seq));
          buffer.append(std::begin(value), std::begin(value) + hintlen);
          if (col != color::none || bold)
          {
            buffer.append("\033[0m", 4);
          }
        }
      }
    }

    /**
     * Single line low level line refresh.
     *
     * Rewrite the currently edited line accordingly to the buffer content,
     * cursor position and number of columns of the terminal.
     */
    static void refresh_single_line(input_state& state)
    {
      char seq[64];
      auto plen = state.prompt.length();
      const auto fd = state.ofd;
      auto buf = state.buf;
      auto len = state.len;
      auto pos = state.pos;
      std::string buffer;

      while ((plen + pos) >= state.cols)
      {
        ++buf;
        --len;
        --pos;
      }
      while (plen + len > state.cols)
      {
        --len;
      }

      // Cursor to left edge.
      buffer.append(1, '\r');

      // Write the prompt and the current buffer content.
      buffer.append(state.prompt);
      buffer.append(buf, len);

      // Show hits if any.
      hints::show(buffer, state);

      // Erase to right.
      buffer.append("\033[0K");

      // Move cursor to original position. */
      std::snprintf(seq, 64, "\r\x1b[%dC", static_cast<int>(pos + plen));
      buffer.append(seq, std::strlen(seq));

      ::write(fd, buffer.c_str(), buffer.length());
    }

    /**
     * Multi line low level line refresh.
     *
     * Rewrite the currently edited line accordingly to the buffer content,
     * cursor position and number of columns of the terminal.
     */
    static void refresh_multi_line(input_state& state)
    {
      char seq[64];
      auto plen = state.prompt.length();
      // Rows used by current buf.
      int rows = (plen + state.len + state.cols - 1) / state.cols;
      // Cursor relative row.
      int rpos = (plen + state.oldpos + state.cols) / state.cols;
      // rpos after refresh.
      int rpos2;
      // Column position, zero based.
      int col;
      const int old_rows = state.maxrows;
      const int fd = state.ofd;
      std::string buffer;

      // Update maxrows if needed.
      if (rows > static_cast<int>(state.maxrows))
      {
        state.maxrows = rows;
      }

      // First step: Clear all the lines used before. To do so start by going
      // to the last row.
      if (old_rows - rpos > 0)
      {
        std::snprintf(seq, 64, "\x1b[%dB", old_rows - rpos);
        buffer.append(seq, std::strlen(seq));
      }

      // Now for every row clear it, go up.
      for (int j = 0; j < old_rows - 1; ++j)
      {
        buffer.append("\r\x1b[0K\x1b[1A");
      }

      // Clean the top line.
      buffer.append("\r\x1b[0K");

      // Write the prompt and the current buffer content.
      buffer.append(state.prompt);
      buffer.append(state.buf, state.len);

      // Show hits if any.
      hints::show(buffer, state);

      // If we are at the very end of the screen with our prompt, we need to
      // emit a newline and move the prompt to the first column.
      if (state.pos &&
          state.pos == state.len &&
          (state.pos + plen) % state.cols == 0)
      {
        buffer.append("\n\r");
        if (++rows > static_cast<int>(state.maxrows))
        {
          state.maxrows = rows;
        }
      }

      // Move cursor to right position.
      rpos2 = ( plen + state.pos + state.cols) / state.cols;

      // Go up till we reach the expected positon.
      if (rows-rpos2 > 0)
      {
        std::snprintf(seq, 64, "\x1b[%dA", rows - rpos2);
        buffer.append(seq, std::strlen(seq));
      }

      // Set column.
      col = (plen + static_cast<int>(state.pos)) % static_cast<int>(state.cols);
      if (col)
      {
        std::snprintf(seq, 64, "\r\x1b[%dC", col);
      } else {
        std::snprintf(seq, 64, "\r");
      }
      buffer.append(seq, std::strlen(seq));

      state.oldpos = state.pos;

      ::write(fd, buffer.c_str(), buffer.length());
    }

    /**
     * Calls the two low level functions refresh_single_line() or
     * refresh_multi_line() according to the selected mode.
     */
    static void refresh(input_state& state)
    {
      if (multi_line)
      {
        refresh_multi_line(state);
      } else {
        refresh_single_line(state);
      }
    }

    /**
     * Insert the character 'c' at cursor's current position. On error writing
     * to the terminal, false is returned, otherwise true.
     */
    static bool insert(input_state& state, char c)
    {
      if (state.len < state.buflen)
      {
        if (state.len == state.pos)
        {
          state.buf[state.pos] = c;
          ++state.pos;
          ++state.len;
          state.buf[state.len] = '\0';
          if (!peelo::prompt::multi_line
              && state.prompt.length() + state.len < state.cols
              && !hints::callback)
          {
            // Avoid a full update of the line in the trivial case.
            if (::write(state.ofd, &c, 1) == -1)
            {
              return false;
            }
          } else {
            refresh(state);
          }
        } else {
          std::memmove(
            static_cast<void*>(state.buf + state.pos + 1),
            static_cast<const void*>(state.buf + state.pos),
            state.len - state.pos
          );
          state.buf[state.pos] = c;
          ++state.len;
          ++state.pos;
          state.buf[state.len] = '\0';
          refresh(state);
        }
      }

      return true;
    }

    /**
     * Move cursor on the left.
     */
    static void move_left(input_state& state)
    {
      if (state.pos > 0)
      {
        --state.pos;
        refresh(state);
      }
    }

    /**
     * Move cursor on the right.
     */
    static void move_right(input_state& state)
    {
      if (state.pos != state.len)
      {
        ++state.pos;
        refresh(state);
      }
    }

    /**
     * Move cursor to the start of the line.
     */
    static void move_home(input_state& state)
    {
      if (state.pos != 0)
      {
        state.pos = 0;
        refresh(state);
      }
    }

    /**
     * Move cursor to the end of the line.
     */
    static void move_end(input_state& state)
    {
      if (state.pos != state.len)
      {
        state.pos = state.len;
        refresh(state);
      }
    }

    /**
     * Substitute the currently edited line with the next or previous history
     * entry as specified by 'direction'.
     */
    static void edit_history_next(input_state& state, bool direction)
    {
      const auto size = history::container.size();

      if (size < 2)
      {
        return;
      }
      // Update the current history entry before overwriting it with the next
      // one.
      history::container.back() = std::string(state.buf, state.len);
      // Show the next entry.
      state.history_index += direction ? -1 : 1;
      if (state.history_index < 0)
      {
        state.history_index = 0;
        return;
      }
      else if (state.history_index >= static_cast<int>(size))
      {
        state.history_index = size - 1;
        return;
      }

      const auto& entry = history::container[size - 1 - state.history_index];

      std::strncpy(state.buf, entry.c_str(), state.buflen);
      state.buf[state.buflen - 1] = '\0';
      state.len = state.pos = std::strlen(state.buf);
      refresh(state);
    }

    /**
     * Delete the character at the right of the cursor without altering the
     * cursor position. Basically this is what happens with the "Delete"
     * keyboard key.
     */
    static void delete_next_char(input_state& state)
    {
      if (state.len > 0 && state.pos < state.len)
      {
        std::memmove(
          static_cast<void*>(state.buf + state.pos),
          static_cast<const void*>(state.buf + state.pos + 1),
          state.len - state.pos - 1
        );
        --state.len;
        state.buf[state.len] = '\0';
        refresh(state);
      }
    }

    /**
     * Backspace implementation.
     */
    static void delete_previous_char(input_state& state)
    {
      if (state.pos > 0 && state.len > 0)
      {
        std::memmove(
          static_cast<void*>(state.buf + state.pos - 1),
          static_cast<const void*>(state.buf + state.pos),
          state.len - state.pos
        );
        --state.pos;
        --state.len;
        state.buf[state.len] = '\0';
        refresh(state);
      }
    }

    /**
     * Delete the previous word, maintaining the cursor at the start of the
     * current word.
     */
    static void delete_previous_word(input_state& state)
    {
      const auto old_pos = state.pos;
      std::size_t diff;

      while (state.pos > 0 && state.buf[state.pos - 1] == ' ')
      {
        --state.pos;
      }
      while (state.pos > 0 && state.buf[state.pos - 1] != ' ')
      {
        --state.pos;
      }
      diff = old_pos - state.pos;
      std::memmove(
        static_cast<void*>(state.buf + state.pos),
        static_cast<const void*>(state.buf + old_pos),
        state.len - old_pos + 1
      );
      state.len -= diff;
      refresh(state);
    }

    /**
     * Swaps current character with previous one.
     */
    static void transpose_characters(input_state& state)
    {
      if (state.pos > 0 && state.pos < state.len)
      {
        const auto aux = state.buf[state.pos - 1];

        state.buf[state.pos - 1] = state.buf[state.pos];
        state.buf[state.pos] = aux;
        if (state.pos != state.len - 1)
        {
          ++state.pos;
        }
        refresh(state);
      }
    }

    /**
     * Deletes the whole line.
     */
    static void kill_line(input_state& state)
    {
      state.buf[0] = '\0';
      state.pos = 0;
      state.len = 0;
      refresh(state);
    }

    /**
     * Deletes characters from current position to end of line.
     */
    static void kill_end_of_line(input_state& state)
    {
      state.buf[state.pos] = '\0';
      state.len = state.pos;
      refresh(state);
    }

    /**
     * Read the next two bytes representing the escape sequence. Use two calls
     * to handle slow terminals returning the two characters at different
     * times.
     */
    static void handle_esc(input_state& state)
    {
      char seq[3];

      if (::read(state.ifd, seq, 1) == -1
          || ::read(state.ifd, seq + 1, 1) == -1)
      {
        return;
      }

      // ESC [ sequences.
      if (seq[0] == '[')
      {
        if (std::isdigit(seq[1]))
        {
          // Extended escape, read additional byte.
          if (::read(state.ifd, seq + 2, 1) == -1)
          {
            return;
          }
          // Delete key.
          else if (seq[2] == '~' && seq[1] == '3')
          {
            delete_next_char(state);
          }
        } else {
          switch (seq[1])
          {
            case 'A': // Up
              edit_history_next(state, false);
              break;

            case 'B': // Down
              edit_history_next(state, true);
              break;

            case 'C': // Right
              move_right(state);
              break;

            case 'D': // Left
              move_left(state);
              break;

            case 'H': // Home
              move_home(state);
              break;

            case 'F': // End
              move_end(state);
              break;
          }
        }
      }
      // ESC O sequences.
      else if (seq[0] == 'O')
      {
        switch (seq[1])
        {
          case 'H': // Home
            move_home(state);
            break;

          case 'F': // End
            move_end(state);
            break;
        }
      }
    }

    /**
     * This function is the core of the line editing capability of this
     * library. It expects 'fd' to be already in "raw mode" so that every key
     * pressed will be returned ASAP to read().
     *
     * The resulting string is returned by the function when the user types
     * enter, or when ^D is typed.
     */
    static value_type edit(int stdin_fd,
                           int stdout_fd,
                           const std::string& prompt)
    {
      input_state state;

      // Populate the input state that we pass to functions implementing
      // specific editing functionalities.
      state.ifd = stdin_fd;
      state.ofd = stdout_fd;
      state.buflen = LINENOISE_MAX_LINE;
      state.prompt = prompt;
      state.oldpos = 0;
      state.pos = 0;
      state.len = 0;
      state.cols = get_columns(stdin_fd, stdout_fd);
      state.maxrows = 0;
      state.history_index = 0;

      // Buffer starts empty.
      state.buf[0] = '\0';
      // Make sure there is always space for the nulterm.
      --state.buflen;

      // Latest history entry is always our current buffer, that initially is
      // just an empty string.
      history::container.push_back(std::string());

      if (::write(state.ofd, prompt.c_str(), prompt.length()) == -1)
      {
        return value_type();
      }

      for (;;)
      {
        char c;

        if (::read(state.ifd, &c, 1) <= 0)
        {
          return value_type(std::string(state.buf, state.len));
        }

        // Only autocomplete when the callback is set. It returns < 0 when
        // there was an error reading from the fd. Otherwise it will return the
        // character that should be handled next.
        if (c == key_tab && completion::callback)
        {
          // Return on errors.
          if ((c = completion::complete_line(state)) < 0)
          {
            return value_type(std::string(state.buf, state.len));
          }
          // Read next character when 0.
          else if (c == 0)
          {
            continue;
          }
        }

        switch (c)
        {
          case key_enter:
            if (!history::container.empty())
            {
              history::container.pop_back();
            }
            if (multi_line)
            {
              move_end(state);
            }
            if (hints::callback)
            {
              // Force a refresh without hints to leave the previous line as
              // the user typed it after a newline.
              const auto callback = hints::callback;

              hints::callback.reset();
              refresh(state);
              hints::callback = callback;
            }
            return value_type(std::string(state.buf, state.len));

          case key_ctrl_c:
            errno = EAGAIN;
            return value_type();

          case key_backspace:
          case key_ctrl_h:
            delete_previous_char(state);
            break;

          case key_ctrl_d:
            // Remove character at the right of the cursor, or if the line is
            // empty, act as end-of-file.
            if (state.len > 0)
            {
              delete_next_char(state);
            } else {
              if (!history::container.empty())
              {
                history::container.pop_back();
              }

              return value_type();
            }
            break;

          case key_ctrl_t:
            transpose_characters(state);
            break;

          case key_ctrl_b:
            move_left(state);
            break;

          case key_ctrl_f:
            move_right(state);
            break;

          case key_ctrl_p:
            edit_history_next(state, false);
            break;

          case key_ctrl_n:
            edit_history_next(state, true);
            break;

          case key_esc:
            handle_esc(state);
            break;

          case key_ctrl_u:
            kill_line(state);
            break;

          case key_ctrl_k:
            kill_end_of_line(state);
            break;

          case key_ctrl_a:
            move_home(state);
            break;

          case key_ctrl_e:
            move_end(state);
            break;

          case key_ctrl_l:
            clear_screen();
            refresh(state);
            break;

          case key_ctrl_w:
            delete_previous_word(state);
            break;

          default:
            if (!insert(state, c))
            {
              return value_type();
            }
            break;
        }
      }
    }

    /**
     * This function calls the line editing function edit() using the STDIN
     * file descriptor set in raw mode.
     */
    static value_type input_raw(const std::string& prompt)
    {
      value_type result;

      if (!enable_raw_mode(STDIN_FILENO))
      {
        return value_type();
      }
      result = edit(STDIN_FILENO, STDOUT_FILENO, prompt);
      disable_raw_mode(STDIN_FILENO);
      std::printf("\n");

      return result;
    }

    /**
     * This function is called when input() is called with the standard input
     * file descriptor not attached to a TTY. So for example when the program
     * using this library is called in pipe or with a file redirected to it's
     * standard input. In this case, we want to be able to return the line
     * regardless of it's length.
     */
    static value_type input_no_tty()
    {
      std::string line;

      for (;;)
      {
        const auto c = std::fgetc(stdin);

        if (c == EOF || c == '\n')
        {
          if (c == EOF && line.empty())
          {
            return value_type();
          }

          return value_type(line);
        }
        line.append(1, static_cast<char>(c));
      }
    }

    // The high level function that is the main API of the linenoise library.
    // This function checks if the terminal has basic capabilities, just
    // checking for a blacklist of stupid terminals, and later either calls the
    // line editing function or uses dummy fgets() so that you will be able to
    // type something even in the most desperate of the conditions.
    value_type input(const std::string& prompt)
    {
      if (!::isatty(STDIN_FILENO))
      {
        // Not a TTY: Read from file / pipe. In this mode we don't want any
        // limit to the line size, so we call a function to handle that.
        return input_no_tty();
      }
      else if (is_unsupported_term())
      {
        char buffer[LINENOISE_MAX_LINE];
        std::size_t length;

        std::printf("%s", prompt.c_str());
        std::fflush(stdout);
        if (!std::fgets(buffer, LINENOISE_MAX_LINE, stdin))
        {
          return value_type();
        }
        length = std::strlen(buffer);
        while (length && (buffer[length-1] == '\n' || buffer[length-1] == '\r'))
        {
          --length;
          buffer[length] = '\0';
        }

        return value_type(std::string(buffer, length));
      }

      return input_raw(prompt);
    }
  }
}
