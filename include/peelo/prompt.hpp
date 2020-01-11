/*
 * Copyright (c) 2010-2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2018-2020, peelo.net
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
 */
#ifndef PEELO_PROMPT_HPP_GUARD
#define PEELO_PROMPT_HPP_GUARD

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if !defined(PEELO_PROMPT_MAX_LINE)
# define PEELO_PROMPT_MAX_LINE 4096
#endif
#if !defined(PEELO_PROMPT_DEFAULT_HISTORY_MAX_LEN)
# define PEELO_PROMPT_DEFAULT_HISTORY_MAX_LEN 100
#endif

namespace peelo
{
  class prompt
  {
  public:
    /**
     * Enumeration of different colors supported by hints.
     */
    enum class color
    {
      none = -1,
      black = 30,
      red = 31,
      green = 32,
      yellow = 33,
      blue = 34,
      magenta = 35,
      cyan = 36,
      white = 37
    };

    using value_type = std::optional<std::string>;
    using history_container_type = std::deque<std::string>;
    using completion_container_type = std::vector<std::string>;
    using completion_callback_type = std::function<void(
      const std::string&,
      completion_container_type&
    )>;
    using hints_callback_type = std::function<value_type(
      const std::string& buffer,
      color& col,
      bool& bold
    )>;

    /**
     * The input state structure represents the state during line editing. We
     * pass this state to functions implementing specific editing
     * functionalities.
     */
    struct state
    {
      /** Terminal stdin file descriptor. */
      int ifd;
      /** Terminal stdout file descriptor. */
      int ofd;
      /** Edited line buffer. */
      char buf[PEELO_PROMPT_MAX_LINE];
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

    enum class key
    {
      ctrl_a = 1,
      ctrl_b = 2,
      ctrl_c = 3,
      ctrl_d = 4,
      ctrl_e = 5,
      ctrl_f = 6,
      ctrl_h = 8,
      tab = 9,
      ctrl_k = 11,
      ctrl_l = 12,
      enter = 13,
      ctrl_n = 14,
      ctrl_p = 16,
      ctrl_t = 20,
      ctrl_u = 21,
      ctrl_w = 23,
      esc = 27,
      backspace = 127
    };

    explicit prompt()
      : m_multi_line(false)
      , m_raw_mode(false)
      , m_history_max_size(PEELO_PROMPT_DEFAULT_HISTORY_MAX_LEN) {}

    ~prompt()
    {
      disable_raw_mode(STDIN_FILENO);
    }

    prompt(const prompt&) = delete;
    prompt(prompt&&) = delete;
    void operator=(const prompt&) = delete;
    void operator=(prompt&&) = delete;

    /**
     * The high level function that is the main API of the linenoise library.
     * This function checks if the terminal has basic capabilities, just
     * checking for a blacklist of stupid terminals, and later either calls the
     * line editing function or uses dummy fgets() so that you will be able to
     * type something even in the most desperate of the conditions.
     */
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
        char buf[PEELO_PROMPT_MAX_LINE];
        std::size_t length;

        std::printf("%s", prompt.c_str());
        std::fflush(stdout);
        if (!std::fgets(buf, PEELO_PROMPT_MAX_LINE, stdin))
        {
          return value_type();
        }
        length = std::strlen(buf);
        while (length && (buf[length-1] == '\n' || buf[length-1] == '\r'))
        {
          --length;
          buf[length] = '\0';
        }

        return std::make_optional<std::string>(buf, length);
      }

      return input_raw(prompt);
    }

    /**
     * Clears the screen. Used to handle ^L.
     */
    void clear_screen()
    {
      ::write(STDOUT_FILENO, "\033[H\033[2J", 7);
    }

    /**
     * Returns a boolean flag which tells whether multi line mode is currently
     * used or not.
     */
    inline bool is_multi_line() const
    {
      return m_multi_line;
    }

    /**
     * Sets the flag whether multi line mode is being used or not.
     */
    inline void set_multi_line(bool flag)
    {
      m_multi_line = true;
    }

    /**
     * Adds new entry in the history.
     */
    bool add_to_history(const std::string& line)
    {
      if (m_history_max_size == 0)
      {
        return false;
      }

      // Don't add duplicated lines.
      if (!m_history_container.empty() &&
          !m_history_container.back().compare(line))
      {
        return false;
      }

      if (m_history_container.size() == m_history_max_size)
      {
        m_history_container.pop_front();
      }

      m_history_container.push_back(line);

      return true;
    }

    /**
     * Returns the maximum size of the history.
     */
    inline std::size_t get_history_max_size() const
    {
      return m_history_max_size;
    }

    /**
     * Sets the maximum size of the history.
     */
    void set_history_max_size(std::size_t size)
    {
      if (size == 0)
      {
        m_history_container.clear();
      } else {
        while (m_history_container.size() > size)
        {
          m_history_container.pop_front();
        }
      }
      m_history_max_size = size;
    }

    /**
     * Registers a callback function to be called during tab-completion.
     */
    inline void set_completion_callback(
      const std::optional<completion_callback_type>& callback
    )
    {
      m_completion_callback = callback;
    }

    /**
     * Register a callback to be called to display hints to the user at the
     * right of the prompt.
     */
    inline void set_hints_callback(
      const std::optional<hints_callback_type>& callback
    )
    {
      m_hints_callback = callback;
    }

    /**
     * Returns true if the terminal name is in the list of terminals we know
     * are not able to understand basic escape sequences.
     */
    static bool is_unsupported_term()
    {
      static const char* unsupported_term[] =
      {
        "dumb",
        "cons25",
        "emacs",
        NULL
      };
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

  private:
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

    /**
     * This function calls the line editing function edit() using the STDIN
     * file descriptor set in raw mode.
     */
    value_type input_raw(const std::string& prompt)
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
     * This function is the core of the line editing capability of this
     * library. It expects 'fd' to be already in "raw mode" so that every key
     * pressed will be returned ASAP to read().
     *
     * The resulting string is returned by the function when the user types
     * enter, or when ^D is typed.
     */
    value_type edit(int stdin_fd, int stdout_fd, const std::string& prompt)
    {
      struct state state;

      // Populate the input state that we pass to functions implementing
      // specific editing functionalities.
      state.ifd = stdin_fd;
      state.ofd = stdout_fd;
      state.buflen = PEELO_PROMPT_MAX_LINE;
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
      m_history_container.push_back(std::string());

      if (::write(state.ofd, prompt.c_str(), prompt.length()) == -1)
      {
        return value_type();
      }

      for (;;)
      {
        char c;

        if (::read(state.ifd, &c, 1) <= 0)
        {
          return std::make_optional<std::string>(state.buf, state.len);
        }

        // Only autocomplete when the callback is set. It returns < 0 when
        // there was an error reading from the fd. Otherwise it will return the
        // character that should be handled next.
        if (c == static_cast<int>(key::tab) && m_completion_callback)
        {
          // Return on errors.
          if ((c = complete_line(state)) < 0)
          {
            return std::make_optional<std::string>(state.buf, state.len);
          }
          // Read next character when 0.
          else if (c == 0)
          {
            continue;
          }
        }

        switch (c)
        {
          case static_cast<int>(key::enter):
            if (!m_history_container.empty())
            {
              m_history_container.pop_back();
            }
            if (m_multi_line)
            {
              move_end(state);
            }
            if (m_hints_callback)
            {
              // Force a refresh without hints to leave the previous line as
              // the user typed it after a newline.
              const auto callback = m_hints_callback;

              m_hints_callback.reset();
              refresh(state);
              m_hints_callback = callback;
            }
            return std::make_optional<std::string>(state.buf, state.len);

          case static_cast<int>(key::ctrl_c):
            errno = EAGAIN;
            return value_type();

          case static_cast<int>(key::backspace):
          case static_cast<int>(key::ctrl_h):
            delete_previous_char(state);
            break;

          case static_cast<int>(key::ctrl_d):
            // Remove character at the right of the cursor, or if the line is
            // empty, act as end-of-file.
            if (state.len > 0)
            {
              delete_next_char(state);
            } else {
              if (!m_history_container.empty())
              {
                m_history_container.pop_back();
              }

              return value_type();
            }
            break;

          case static_cast<int>(key::ctrl_t):
            transpose_characters(state);
            break;

          case static_cast<int>(key::ctrl_b):
            move_left(state);
            break;

          case static_cast<int>(key::ctrl_f):
            move_right(state);
            break;

          case static_cast<int>(key::ctrl_p):
            edit_history_next(state, false);
            break;

          case static_cast<int>(key::ctrl_n):
            edit_history_next(state, true);
            break;

          case static_cast<int>(key::esc):
            handle_esc(state);
            break;

          case static_cast<int>(key::ctrl_u):
            kill_line(state);
            break;

          case static_cast<int>(key::ctrl_k):
            kill_end_of_line(state);
            break;

          case static_cast<int>(key::ctrl_a):
            move_home(state);
            break;

          case static_cast<int>(key::ctrl_e):
            move_end(state);
            break;

          case static_cast<int>(key::ctrl_l):
            clear_screen();
            refresh(state);
            break;

          case static_cast<int>(key::ctrl_w):
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
     * Raw mode: 1960 magic shit.
     */
    bool enable_raw_mode(int fd)
    {
      ::termios raw;

      if (!::isatty(STDIN_FILENO))
      {
        return false;
      }

      if (::tcgetattr(fd, &m_original_termios) == -1)
      {
        return false;
      }

      // Modify the original one.
      raw = m_original_termios;

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

      m_raw_mode = true;

      return true;
    }

    void disable_raw_mode(int fd)
    {
      if (m_raw_mode)
      {
        ::tcsetattr(fd, TCSAFLUSH, &m_original_termios);
        m_raw_mode = false;
      }
    }

    /**
     * Calls the two low level functions refresh_single_line() or
     * refresh_multi_line() according to the selected mode.
     */
    void refresh(struct state& state)
    {
      if (m_multi_line)
      {
        refresh_multi_line(state);
      } else {
        refresh_single_line(state);
      }
    }

    /**
     * Single line low level line refresh.
     *
     * Rewrite the currently edited line accordingly to the buffer content,
     * cursor position and number of columns of the terminal.
     */
    void refresh_single_line(struct state& state)
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
      show_hints(buffer, state);

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
    void refresh_multi_line(struct state& state)
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
      show_hints(buffer, state);

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
      rpos2 = (plen + state.pos + state.cols) / state.cols;

      // Go up till we reach the expected positon.
      if (rows - rpos2 > 0)
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
      if (buffer[0] != static_cast<int>(key::esc) ||
          buffer[1] != '[' ||
          std::sscanf(buffer + 2, "%d;%d", &rows, &cols) != 2)
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

    /**
     * Beep, used for completion when there is nothing to complete or when all
     * the choices were already shown.
     */
    static void beep()
    {
      std::fprintf(stderr, "\x7");
      std::fflush(stderr);
    }

    /**
     * Insert the character 'c' at cursor's current position. On error writing
     * to the terminal, false is returned, otherwise true.
     */
    bool insert(struct state& state, char c)
    {
      if (state.len < state.buflen)
      {
        if (state.len == state.pos)
        {
          state.buf[state.pos] = c;
          ++state.pos;
          ++state.len;
          state.buf[state.len] = '\0';
          if (!m_multi_line
              && state.prompt.length() + state.len < state.cols
              && !m_hints_callback)
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
    void move_left(struct state& state)
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
    void move_right(struct state& state)
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
    void move_home(struct state& state)
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
    void move_end(struct state& state)
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
    void edit_history_next(struct state& state, bool direction)
    {
      const auto size = m_history_container.size();

      if (size < 2)
      {
        return;
      }
      // Update the current history entry before overwriting it with the next
      // one.
      m_history_container.back() = std::string(state.buf, state.len);
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

      const auto& entry = m_history_container[size - 1 - state.history_index];

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
    void delete_next_char(struct state& state)
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
    void delete_previous_char(struct state& state)
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
    void delete_previous_word(struct state& state)
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
    void transpose_characters(struct state& state)
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
    void kill_line(struct state& state)
    {
      state.buf[0] = '\0';
      state.pos = 0;
      state.len = 0;
      refresh(state);
    }

    /**
     * Deletes characters from current position to end of line.
     */
    void kill_end_of_line(struct state& state)
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
    void handle_esc(struct state& state)
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
          else if (seq[2] == '~')
          {
            switch (seq[1])
            {
              case '3': // Delete key
                delete_next_char(state);
                break;

              case '1': // Home (PuTTY)
                move_home(state);
                break;

              case '4': // End (PuTTY)
                move_end(state);
                break;
            }
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
     * This is an helper function for edit() and is called when the user
     * types the <tab> key in order to complete the string currently in the
     * input.
     *
     * The state of the editing is encapsulated into the pointed input state
     * structure as described in the structure definition.
     */
    int complete_line(struct state& state)
    {
      completion_container_type completions;
      int c = -1;

      if (m_completion_callback)
      {
        (*m_completion_callback)(
          std::string(state.buf, state.len),
          completions
        );
      }

      if (completions.empty())
      {
        beep();
      } else {
        bool stop = false;
        completion_container_type::size_type i = 0;

        while (!stop)
        {
          // Show completion or original buffer.
          if (i < completions.size())
          {
            const auto& completion = completions[i];
            struct state saved = state;

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
            case static_cast<int>(key::tab):
              i = (i + 1) % (completions.size() + 1);
              if (i == completions.size())
              {
                beep();
              }
              break;

            case static_cast<int>(key::esc):
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

    /**
     * Helper of refresh_single_line() and refresh_multi_line() to show hints
     * to the right of the prompt.
     */
    void show_hints(std::string& buffer, struct state& state)
    {
      const auto plen = state.prompt.length();
      char seq[64];
      color col = color::none;
      bool bold = false;

      if (!m_hints_callback || plen + state.len >= state.cols)
      {
        return;
      }

      if (auto hint = (*m_hints_callback)(std::string(state.buf, state.len),
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

  private:
    bool m_multi_line;
    bool m_raw_mode;
    ::termios m_original_termios;
    std::size_t m_history_max_size;
    history_container_type m_history_container;
    std::optional<completion_callback_type> m_completion_callback;
    std::optional<hints_callback_type> m_hints_callback;
  };
}

#endif /* !PEELO_PROMPT_HPP_GUARD */
