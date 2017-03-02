<!-- [![Build Status from master](https://travis-ci.org/wallix/vt-emulator.svg?branch=master)](https://travis-ci.org/wallix/vt-emulator) -->
Vt-emulator is a terminal emulator based to [Konsole](https://konsole.kde.org/download.php) ([repo](https://cgit.kde.org/konsole.git)) without graphical interface.

# Demo

```bash
bjam terminal_browser
cd browser
python -m SimpleHTTPServer 4104
script -f >(../bin/*/release/terminal_browser screen.json)
xdg-open http://localhost:4104/view_browser.html
ls -R
```

# Build

$ `bjam` or `bjam toolset=compiler` (see http://www.boost.org/build/doc/html/bbv2/overview/configuration.html)


# Install

Set paths environment (optional):

```bash
export LIB_PREFIX=build/lib
export INCLUDE_PREFIX=build/include/wallix/vt-emulator
export TTY_BROWSER_JS_FILE=build/browser
export TTY_BROWSER_CSS_FILE=build/browser
```

Then

$ `bjam install install-tty-browser`


# Package

With Wallix packager (https://github.com/wallix/packager).

`$ $PACKAGER_PROJECT/packager.py --version 1.0 --no-entry-changelog`

`$ dpkg-buildpackage -I.git -Ibin -uc -us`
