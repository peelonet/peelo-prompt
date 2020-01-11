#include <cstdlib>
#include <cstring>
#include <iostream>

#include <peelo/prompt.hpp>

int main(int argc, char** argv)
{
  const auto prgname = argv[0];
  peelo::prompt prompt;

  // Parse options, with --multiline we enable multi line editing.
  while(argc > 1)
  {
    argc--;
    argv++;
    if (!std::strcmp(*argv, "--multiline"))
    {
      prompt.set_multi_line(true);
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
  prompt.set_completion_callback(
    [] (const std::string& buf, peelo::prompt::completion_container_type& c)
    {
      if (!buf.empty() && buf[0] == 'h')
      {
        c.push_back("hello");
        c.push_back("hello there");
      }
    }
  );

  prompt.set_hints_callback(
    [] (
      const std::string& buf,
      peelo::prompt::color& color,
      bool& bold
    ) -> peelo::prompt::value_type
    {
      if (!strcasecmp(buf.c_str(), "hello"))
      {
        color = peelo::prompt::color::magenta;
        bold = false;

        return " World";
      }

      return peelo::prompt::value_type();
    }
  );

  // Now this is the main loop of the typical linenoise-based application.
  // The call to linenoise() will block as long as the user types something
  // and presses enter.
  //
  // The typed string is returned as a malloc() allocated string by
  // linenoise, so the user needs to free() it.
  while (auto line = prompt.input("hello> "))
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
      // Add line to history.
      prompt.add_to_history(value);
    }
    // The "/historylen" command will change the history len.
    else if (!value.compare(0, 11, "/historylen"))
    {
      prompt.set_history_max_size(std::atoi(value.c_str() + 11));
    }
    else if (value[0] == '/')
    {
      std::cout << "Unrecognized command: " << value << std::endl;
    }
  }

  return EXIT_SUCCESS;
}
