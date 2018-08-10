#include <cstdlib>
#include <cstring>
#include <iostream>

#include <peelo/prompt.hpp>

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
      peelo::prompt::set_multi_line(true);
      std::cout << "Multi-line mode enabled." << std::endl;
    } else {
      std::cerr << "Usage: "
                << prgname
                << " [--multiline]"
                << std::endl;
      std::exit(EXIT_FAILURE);
    }
  }

  // Set the completion callback. This will be called every time the
  // user uses the <tab> key.
  peelo::prompt::completion::set_callback(
    [] (const std::string& buf, peelo::prompt::completion::container_type& c)
    {
      if (!buf.empty() && buf[0] == 'h')
      {
        c.push_back("hello");
        c.push_back("hello there");
      }
    }
  );

  peelo::prompt::hints::set_callback(
    [] (
      const std::string& buf,
      peelo::prompt::color& color,
      bool& bold
    ) -> peelo::prompt::hints::value_type
    {
      if (!strcasecmp(buf.c_str(), "hello"))
      {
        color = peelo::prompt::color::magenta;
        bold = false;

        return " World";
      }

      return peelo::prompt::hints::value_type();
    }
  );

  // Load history from file. The history file is just a plain text file
  // where entries are separated by newlines.
  linenoiseHistoryLoad("history.txt"); // Load the history at startup

  // Now this is the main loop of the typical linenoise-based application.
  // The call to linenoise() will block as long as the user types something
  // and presses enter.
  //
  // The typed string is returned as a malloc() allocated string by
  // linenoise, so the user needs to free() it.
  while (auto line = peelo::prompt::input("hello> "))
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
