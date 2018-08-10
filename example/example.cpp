#include <cstdlib>
#include <cstring>
#include <iostream>

#include <peelo/prompt.hpp>


void completion(const char* buf, linenoiseCompletions* lc)
{
  if (buf[0] == 'h')
  {
    linenoiseAddCompletion(lc, "hello");
    linenoiseAddCompletion(lc, "hello there");
  }
}

char* hints(const char* buf, int* color, int* bold)
{
  if (!strcasecmp(buf, "hello"))
  {
    *color = 35;
    *bold = 0;

    return strdup(" World");
  }

  return NULL;
}

int main(int argc, char** argv)
{
  const auto prgname = argv[0];

  // Parse options, with --multiline we enable multi line editing.
  while(argc > 1)
  {
    argc--;
    argv++;
    if (!std::strcmp(*argv, "--multiline"))
    {
      linenoiseSetMultiLine(1);
      std::cout << "Multi-line mode enabled." << std::endl;
    }
    else if (!std::strcmp(*argv, "--keycodes"))
    {
      linenoisePrintKeyCodes();
      std::exit(EXIT_SUCCESS);
    } else {
      std::cerr << "Usage: "
                << prgname
                << " [--multiline] [--keycodes]"
                << std::endl;
      std::exit(EXIT_FAILURE);
    }
  }

  // Set the completion callback. This will be called every time the
  // user uses the <tab> key.
  linenoiseSetCompletionCallback(completion);
  linenoiseSetHintsCallback(hints);

  // Load history from file. The history file is just a plain text file
  // where entries are separated by newlines.
  linenoiseHistoryLoad("history.txt"); // Load the history at startup

  // Now this is the main loop of the typical linenoise-based application.
  // The call to linenoise() will block as long as the user types something
  // and presses enter.
  //
  // The typed string is returned as a malloc() allocated string by
  // linenoise, so the user needs to free() it.
  while (auto line = linenoise("hello> "))
  {
    const auto value = line.value();

    // Skip empty lines.
    if (value.empty())
    {
      continue;
    }

    // Do something with the string.
    if (value[0] != '/')
    {
      std::cout << "echo: '" << value << "'" << std::endl;
      linenoiseHistoryAdd(value.c_str()); // Add to the history.
      linenoiseHistorySave("history.txt"); // Save the history on disk.
    }
    else if (!value.compare(0, 11, "/historylen"))
    {
      // The "/historylen" command will change the history len.
      auto length = std::atoi(value.c_str() + 11);

      linenoiseHistorySetMaxLen(length);
    }
    else if (value[0] == '/')
    {
      std::cout << "Unrecognized command: " << value << std::endl;
    }
  }

  return EXIT_SUCCESS;
}
