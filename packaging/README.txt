Go.dot - test build
===================

This is an early build for testing and comments. It is not an installer, the
Windows and Linux builds are not signed, and it is not ready to run a show. Please do try it, and please
tell us what you find:

    https://github.com/pob31/go.dot/issues

A report is most useful with the build's name (the archive's name, or the
first line of `wfg --version`), your OS, your audio device, and the text the
terminal window printed.


Starting it
-----------

On Windows and Linux a launcher sits beside the binary; on macOS the launcher
is the app. With nothing else it opens the empty show "Untitled", in a window,
on your system's default audio interface, with its show settings open so you
can pick another. On Windows and Linux, give the launcher a show folder to
open that instead.

In the window, "New show...", "Open show..." and "Save as..." work on show
folders anywhere you like; "Show settings..." picks the audio interface, and
a show remembers the one it was saved with. A new show opens on its settings.

  Windows   Double-click Go.dot.cmd, or drop a show folder on it.
            SmartScreen will warn that the publisher is unknown: choose
            "More info", then "Run anyway".

  macOS     Open the .dmg and drag Go.dot to Applications, then open it
            like any app. It is signed and notarized, so macOS opens it
            without a warning. Universal (Apple silicon and Intel), macOS
            13.3 or later.

            The first time, it copies the empty show to
            ~/Library/Application Support/Go.dot/Untitled and opens that.
            Each launch writes its log to ~/Library/Logs/Go.dot - attach the
            newest one to a report. The first mic cue or recording asks for
            microphone access: allow it, or inputs stay silent.

            The command-line tool is inside the app:
                /Applications/Go.dot.app/Contents/MacOS/wfg --version

  Linux     ./go.dot.sh  (or ./go.dot.sh ~/shows/Tuesday)
            Built on Ubuntu 24.04; it needs ALSA, FreeType, fontconfig and
            the X11 libraries, which a desktop install already has.

The web client is served beside the window: the terminal prints its address
(http://localhost:<port>/ui). A tablet on the same network can open it with
this machine's address in place of localhost.


Everything else
---------------

`wfg` is also a command-line tool. The commands a tester is likely to want:

    wfg --version          which build this is
    wfg devices            the audio devices it can play through
    wfg midi               the MIDI ports it can see
    wfg plugins --scan     look for VST3 (and AU, LV2) plugins
    wfg validate <show>    check a show folder and list every problem

On Windows it is `wfg.exe`, run from a Command Prompt in this folder.


Licence
-------

Go.dot is free software under the GNU General Public License, version 3 or
later: see LICENSE. The third-party code it contains, and those licences, are
listed in THIRD_PARTY_NOTICES.md. The source for this build is the tag of the
same name at https://github.com/pob31/go.dot.
