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

#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096
static linenoiseHintsCallback *hintsCallback = NULL;
static linenoiseFreeHintsCallback *freeHintsCallback = NULL;

static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static char **history = NULL;

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
  int ifd;            /* Terminal stdin file descriptor. */
  int ofd;            /* Terminal stdout file descriptor. */
  /** Edited line buffer. */
  char buf[LINENOISE_MAX_LINE];
  /** Size of the line buffer. */
  std::size_t buflen;
  /** Prompt to display. */
  std::string prompt;
  size_t pos;         /* Current cursor position. */
  size_t oldpos;      /* Previous refresh cursor position. */
  size_t len;         /* Current edited line length. */
  size_t cols;        /* Number of columns in terminal. */
  size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
  int history_index;  /* The history index we are currently editing. */
};

static void freeHistory();

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (lndebug_fp == NULL) { \
            lndebug_fp = fopen("/tmp/lndebug.txt","a"); \
            fprintf(lndebug_fp, \
            "[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d\n", \
            (int)l->len,(int)l->pos,(int)l->oldpos,plen,rows,rpos, \
            (int)l->maxrows,old_rows); \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(fmt, ...)
#endif

/* ======================= Low level terminal handling ====================== */

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

    static void refresh(linenoiseState&);

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
      freeHistory();
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

    namespace completion
    {
      static std::optional<callback_type> callback;

      // This is an helper function for edit() and is called when the user
      // types the <tab> key in order to complete the string currently in the
      // input.
      //
      // The state of the editing is encapsulated into the pointed input state
      // structure as described in the structure definition.
      static int complete_line(struct linenoiseState& state)
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
              linenoiseState saved = state;

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
  }
}

/* Register a hits function to be called to show hits to the user at the
 * right of the prompt. */
void linenoiseSetHintsCallback(linenoiseHintsCallback *fn) {
    hintsCallback = fn;
}

/* Register a function to free the hints returned by the hints callback
 * registered with linenoiseSetHintsCallback(). */
void linenoiseSetFreeHintsCallback(linenoiseFreeHintsCallback *fn) {
    freeHintsCallback = fn;
}

/* =========================== Line editing ================================= */

/* Helper of refreshSingleLine() and refreshMultiLine() to show hints
 * to the right of the prompt. */
static void refreshShowHints(std::string& buffer, linenoiseState& l)
{
  const auto plen = l.prompt.length();
  char seq[64];
  peelo::prompt::color color = peelo::prompt::color::none;
  bool bold = false;
  char* hint;

  if (!hintsCallback || plen + l.len >= l.cols)
  {
    return;
  }
  if ((hint = hintsCallback(l.buf, color, bold)))
  {
    auto hintlen = std::strlen(hint);
    auto hintmaxlen = l.cols - (plen + l.len);

    if (hintlen > hintmaxlen)
    {
      hintlen = hintmaxlen;
    }
    if (bold && color == peelo::prompt::color::none)
    {
      color = peelo::prompt::color::white;
    }
    if (color != peelo::prompt::color::none || bold)
    {
      std::snprintf(
        seq,
        64,
        "\033[%d;%d;49m",
        bold ? 1 : 0,
        static_cast<int>(color)
      );
    } else {
      seq[0] = '\0';
    }
    buffer.append(seq, std::strlen(seq));
    buffer.append(hint, hintlen);
    if (color != peelo::prompt::color::none || bold)
    {
      buffer.append("\033[0m", 4);
    }

    // Call the function to free the hint returned.
    if (freeHintsCallback)
    {
      freeHintsCallback(hint);
    }
  }
}

namespace peelo
{
  namespace prompt
  {
    /**
     * Single line low level line refresh.
     *
     * Rewrite the currently edited line accordingly to the buffer content,
     * cursor position and number of columns of the terminal.
     */
    static void refresh_single_line(linenoiseState& state)
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
      refreshShowHints(buffer, state);

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
    static void refresh_multi_line(linenoiseState& state)
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
        lndebug("go down %d", old_rows - rpos);
        std::snprintf(seq, 64, "\x1b[%dB", old_rows - rpos);
        buffer.append(seq, std::strlen(seq));
      }

      // Now for every row clear it, go up.
      for (int j = 0; j < old_rows - 1; ++j)
      {
        lndebug("clear+up");
        buffer.append("\r\x1b[0K\x1b[1A");
      }

      // Clean the top line.
      lndebug("clear");
      buffer.append("\r\x1b[0K");

      // Write the prompt and the current buffer content.
      buffer.append(state.prompt);
      buffer.append(state.buf, state.len);

      // Show hits if any.
      refreshShowHints(buffer, state);

      // If we are at the very end of the screen with our prompt, we need to
      // emit a newline and move the prompt to the first column.
      if (state.pos &&
          state.pos == state.len &&
          (state.pos + plen) % state.cols == 0)
      {
        lndebug("<newline>");
        buffer.append("\n\r");
        if (++rows > static_cast<int>(state.maxrows))
        {
          state.maxrows = rows;
        }
      }

      // Move cursor to right position.
      rpos2 = ( plen + state.pos + state.cols) / state.cols;
      lndebug("rpos2 %d", rpos2);

      // Go up till we reach the expected positon.
      if (rows-rpos2 > 0)
      {
        lndebug("go-up %d", rows-rpos2);
        std::snprintf(seq, 64, "\x1b[%dA", rows - rpos2);
        buffer.append(seq, std::strlen(seq));
      }

      // Set column.
      col = (plen + static_cast<int>(state.pos)) % static_cast<int>(state.cols);
      lndebug("set col %d", 1+col);
      if (col)
      {
        std::snprintf(seq, 64, "\r\x1b[%dC", col);
      } else {
        std::snprintf(seq, 64, "\r");
      }
      buffer.append(seq, std::strlen(seq));

      lndebug("\n");
      state.oldpos = state.pos;

      ::write(fd, buffer.c_str(), buffer.length());
    }

    /**
     * Calls the two low level functions refresh_single_line() or
     * refresh_multi_line() according to the selected mode.
     */
    static void refresh(linenoiseState& state)
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
    static bool insert(linenoiseState& state, char c)
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
              && !hintsCallback)
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
    static void move_left(linenoiseState& state)
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
    static void move_right(linenoiseState& state)
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
    static void move_home(linenoiseState& state)
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
    static void move_end(linenoiseState& state)
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
    static void edit_history_next(linenoiseState& state, bool direction)
    {
      if (history_len < 2)
      {
        return;
      }
      // Update the current history entry before overwriting it with the next
      // one.
      std::free(history[history_len - 1 - state.history_index]);
      history[history_len - 1 - state.history_index] = ::strdup(state.buf);
      // Show the next entry.
      state.history_index += direction ? -1 : 1;
      if (state.history_index < 0)
      {
        state.history_index = 0;
        return;
      }
      else if (state.history_index >= history_len)
      {
        state.history_index = history_len - 1;
        return;
      }
      std::strncpy(
        state.buf,
        history[history_len - 1 - state.history_index],
        state.buflen
      );
      state.buf[state.buflen - 1] = '\0';
      state.len = state.pos = std::strlen(state.buf);
      refresh(state);
    }

    /**
     * Delete the character at the right of the cursor without altering the
     * cursor position. Basically this is what happens with the "Delete"
     * keyboard key.
     */
    static void delete_next_char(linenoiseState& state)
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
    static void delete_previous_char(linenoiseState& state)
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
    static void delete_previous_word(linenoiseState& state)
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
    static void transpose_characters(linenoiseState& state)
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
    static void kill_line(linenoiseState& state)
    {
      state.buf[0] = '\0';
      state.pos = 0;
      state.len = 0;
      refresh(state);
    }

    /**
     * Deletes characters from current position to end of line.
     */
    static void kill_end_of_line(linenoiseState& state)
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
    static void handle_esc(linenoiseState& state)
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
      linenoiseState state;

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
      linenoiseHistoryAdd("");

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
            std::free(history[--history_len]);
            if (multi_line)
            {
              move_end(state);
            }
            if (hintsCallback)
            {
              // Force a refresh without hints to leave the previous line as
              // the user typed it after a newline.
              const auto callback = hintsCallback;

              hintsCallback = nullptr;
              refresh(state);
              hintsCallback = callback;
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
              std::free(history[--history_len]);

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
  }
}

/* This special mode is used by linenoise in order to print scan codes
 * on screen for debugging / development purposes. It is implemented
 * by the linenoise_example program using the --keycodes option. */
void linenoisePrintKeyCodes(void) {
    char quit[4];

    printf("Linenoise key codes debugging mode.\n"
            "Press keys to see scan codes. Type 'quit' at any time to exit.\n");
    if (!peelo::prompt::enable_raw_mode(STDIN_FILENO)) return;
    memset(quit,' ',4);
    while(1) {
        char c;
        int nread;

        nread = read(STDIN_FILENO,&c,1);
        if (nread <= 0) continue;
        memmove(quit,quit+1,sizeof(quit)-1); /* shift string to left. */
        quit[sizeof(quit)-1] = c; /* Insert current char on the right. */
        if (memcmp(quit,"quit",sizeof(quit)) == 0) break;

        printf("'%c' %02x (%d) (type quit to exit)\n",
            isprint(c) ? c : '?', (int)c, (int)c);
        printf("\r"); /* Go left edge manually, we are in raw mode. */
        fflush(stdout);
    }
    peelo::prompt::disable_raw_mode(STDIN_FILENO);
}

namespace peelo
{
  namespace prompt
  {
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

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void freeHistory(void) {
    if (history) {
        int j;

        for (j = 0; j < history_len; j++)
            free(history[j]);
        free(history);
    }
}

/* This is the API call to add a new entry in the linenoise history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line) {
    char *linecopy;

    if (history_max_len == 0) return 0;

    /* Initialization on first call. */
    if (history == NULL) {
        history = static_cast<char**>(malloc(sizeof(char*)*history_max_len));
        if (history == NULL) return 0;
        memset(history,0,(sizeof(char*)*history_max_len));
    }

    /* Don't add duplicated lines. */
    if (history_len && !strcmp(history[history_len-1], line)) return 0;

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history,history+1,sizeof(char*)*(history_max_len-1));
        history_len--;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(int len) {
    char **neww;

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;

        neww = static_cast<char**>(malloc(sizeof(char*)*len));
        if (neww == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {
            int j;

            for (j = 0; j < tocopy-len; j++) free(history[j]);
            tocopy = len;
        }
        memset(neww,0,sizeof(char*)*len);
        memcpy(neww,history+(history_len-tocopy), sizeof(char*)*tocopy);
        free(history);
        history = neww;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(const char *filename) {
    mode_t old_umask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
    FILE *fp;
    int j;

    fp = fopen(filename,"w");
    umask(old_umask);
    if (fp == NULL) return -1;
    chmod(filename,S_IRUSR|S_IWUSR);
    for (j = 0; j < history_len; j++)
        fprintf(fp,"%s\n",history[j]);
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(const char *filename) {
    FILE *fp = fopen(filename,"r");
    char buf[LINENOISE_MAX_LINE];

    if (fp == NULL) return -1;

    while (fgets(buf,LINENOISE_MAX_LINE,fp) != NULL) {
        char *p;

        p = strchr(buf,'\r');
        if (!p) p = strchr(buf,'\n');
        if (p) *p = '\0';
        linenoiseHistoryAdd(buf);
    }
    fclose(fp);
    return 0;
}
