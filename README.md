# peelocpp-prompt

[![travis][travis-image]][travis-url]

C++17 fork of the [linenoise] readline replacement library.

[linenoise]: https://github.com/antirez/linenoise

## Features

* Single and multi line editing mode with the usual key bindings implemented.
* History handling.
* Completion.
* Hints (suggestions at the right of the prompt as you type).
* About 1,200 lines of BSD license source code.
* Only uses a subset of VT100 escapes (ANSI.SYS compatible).

# The API

Just like the original library, `peelocpp-prompt` attempts to be very easy to
use, and reading the example shipped with the library should get you up to
speed ASAP. Here is a list of functions provided by the library and how to use
them.

```cpp
std::optional<std::string> peelo::prompt::input(const std::string& prompt);
```

This shows the user a prompt with line editing and history capabilities. The
prompt you specify is used as a prompt, that is it will be printed to the left
of the cursor. The function returns the line composed by user as an instance of
`std::string` wrapped in `std::optional`, which contains no value if end of
file is reached.

When a tty is detected (the user is actually typing into a terminal session)
the maximum editable line length is `LINENOISE_MAX_LINE`. When instead the
standard input is not a tty, which happens every time you redirect a file
to a program, or use it in an Unix pipeline, there are no limits to the
length of the line that can be returned.

The canonical loop used by a program using `peelocpp-prompt` will be something
like this:

```cpp
while (auto line = peelo::prompt::input("hello> "))
{
    std::cout << "You wrote: " << line.value() << std::endl;
}
```

## Single line VS multi line editing

By default, `peelocpp-prompt` uses single line editing, that is, a single row
on the screen will be used, and as the user types more, the text will scroll
towards left to make room. This works if your program is one where the user is
unlikely to write a lot of text, otherwise multi line editing, where multiple
screens rows are used, can be a lot more comfortable.

In order to enable multi line editing use the following API call:

```cpp
peelo::prompt::set_multi_line(true);
```

You can disable it using `false` as argument.

## History

`peelocpp-prompt` supports history, so that the user does not have to retyp
again and again the same things, but can use the down and up arrows in order to
search and re-edit already inserted lines of text.

The followings are the history API calls:

```cpp
bool peelo::prompt::history::add(const std::string& line);
void peelo::prompt::history::set_max_size(std::size_t size);
```

Use `add` every time you want to add a new element to the top of the history
(it will be the first the user will see when using the up array).

Note that for history to work, you have to set a maximum size for the history
(which is 100 by default). This is accomplished using the `set_max_size`
function. Setting history size to `0` will disable history completely.

## Completion

`peelocpp-prompt` supports completion, which is the ability to complete the
user input when she or he presses the `<TAB>` key.

In order to use completion, you need to register a completion callback, which
is called every time the user presses `<TAB>`. This callback receives an
`std::vector` container to which instances of `std::string` can be inserted
into which will act as the completions for the current string.

The following is an example of registering a completion callback:

```cpp
peelo::prompt::completion::set_callback(completion);
```

The completion must be a function returning `void` and getting as input
instance of `const std::string` reference, which is the line the user has typed
so far, and `std::vector<std::string>` reference, which is the container where
the completions will be inserted in. An example will make it more clear:

```cpp
void completion(const std::string& buf, std::vector<std::string>& completions)
{
    if (!buf.empty() && buf[0] == 'h')
    {
        completions.push_back("hello");
        completions.push_back("hello there");
    }
}
```

Basically in your completion callback, you inspect the input, and insert list
of items that are good completions into the `std::vector` given as argument.

If you want to test the completion feature, compile the example program with
[CMake], run it, type `h` and press `<TAB>`.

[CMake]: https://cmake.org

## Hints

`peelocpp-prompt` has a feature called *hints* which is very useful when you
use `peelocpp-prompt` in order to implement a REPL (Read Eval Print Loop) for
a program that accepts commands and arguments, but may also be useful in other
conditions.

The feature shows, on the right of the cursor, as the user types, hints that
may be useful. The hints can be displayed using a different color compared
to the color the user is typing, and can also be bold.

For example as the user starts to type `"git remote add"`, with hints it's
possible to show on the right of the prompt a string `<name> <url>`.

The feature works similarly to the history feature, using a callback. To
register the callback we use:

```cpp
peelo::prompt::hints::set_callback(hints);
```

The callback itself is implemented like this:

```cpp
std::optional<std::string> hints(const std::string& buf,
                                 peelo::prompt::color& color,
                                 bool& bold)
{
    if (!buf.compare("git remote add"))
    {
        color = peelo::prompt::color::magenta;
        bold = false;

        return " <name> <url>";
    }

    return std::optional<std::string>();
}
```

The callback function returns the string that should be displayed wrapped
inside `std::optional`, or empty `std::optional` if no hint is available for
the text the user currently typed. The returned string will be trimmed as
needed depending on the number of columns available on the screen.

As you can see in the example above, a `color` (in xterm color terminal codes)
can be provided together with a `bold` attribute. If no color is set, the
current terminal foreground color is used. If no bold attribute is set,
non-bold text is printed.

An enumeration called `peelo::prompt::color` contains following available color
codes:

- none
- black
- red
- green
- yellow
- blue
- magenta
- cyan
- white

## Screen handling

Sometimes you may want to clear the screen as a result of something the
user typed. You can do this by calling the following function:

```cpp
peelo::prompt::clear_screen();
```

[travis-image]: https://travis-ci.com/peelonet/peelocpp-prompt.svg?branch=master
[travis-url]: https://travis-ci.com/peelonet/peelocpp-prompt
