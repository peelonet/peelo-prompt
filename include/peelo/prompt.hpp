/* linenoise.h -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * See linenoise.c for more information.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2014, Salvatore Sanfilippo <antirez at gmail dot com>
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
 */
#ifndef PEELO_PROMPT_HPP_GUARD
#define PEELO_PROMPT_HPP_GUARD

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace peelo
{
  namespace prompt
  {
    using value_type = std::optional<std::string>;

    value_type input(const std::string& prompt);

    /**
     * Clears the screen. Used to handle ^L.
     */
    void clear_screen();

    /**
     * Returns a boolean flag which tells whether multi line mode is currently
     * used or not.
     */
    bool is_multi_line();

    /**
     * Sets the flag whether multi line mode is being used or not.
     */
    void set_multi_line(bool flag);

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

    namespace completion
    {
      using container_type = std::vector<std::string>;
      using callback_type = std::function<void(
        const std::string&,
        container_type&
      )>;

      /**
       * Registers a callback function to be called during tab-completion.
       */
      void set_callback(const std::optional<callback_type>& callback);
    }
  }
}

typedef char*(linenoiseHintsCallback)(
  const char* buffer,
  peelo::prompt::color& color,
  bool& bold
);
typedef void(linenoiseFreeHintsCallback)(void *);
void linenoiseSetHintsCallback(linenoiseHintsCallback *);
void linenoiseSetFreeHintsCallback(linenoiseFreeHintsCallback *);

int linenoiseHistoryAdd(const char *line);
int linenoiseHistorySetMaxLen(int len);
int linenoiseHistorySave(const char *filename);
int linenoiseHistoryLoad(const char *filename);
void linenoisePrintKeyCodes(void);

#endif /* !PEELO_PROMPT_HPP_GUARD */
