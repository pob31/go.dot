Go.dot - test build
===================

This is an early build for testing and comments. The Windows and Linux builds
are not signed, and it is not ready to run a show. Please do try it, and please
tell us what you find:

    https://github.com/pob31/go.dot/issues

A report is most useful with the build's name (the archive's name, or the
first line of `wfg --version`), your OS, your audio device, and the log -
where each system keeps it is below.


Starting it
-----------

On Windows and Linux a launcher sits beside the binary; on macOS the launcher
is the app. With nothing else it opens the show you last had open on this
computer. The very first time - or when that show has been moved, will not
load, or is already open in another window - it opens the empty show
"Untitled" instead, in a window, on your system's default audio interface,
with its show settings open so you can pick another. On Windows and Linux,
give the launcher a show folder, or the show's .wfg, to open that instead.

In the window, "New show...", "Open show..." and "Save as..." work on show
folders anywhere you like; "Show settings..." picks the audio interface, and
a show remembers the one it was saved with. A new show opens on its settings.
Open show... takes a show's folder or its .wfg, and a show that is already
open in a window is brought forward instead of being opened twice.

A show and its performances: a Show is the piece, a Performance each night
of it - another venue or the same one. File > "New performance..." makes one
inside the show's folder, as a copy of the show or of an earlier performance:

    Hamlet/                 the show, with media/ for the piece's sounds
      2026-10-03 Paris/     a performance, with media/ for its own
      2026-11-12 Lyon/

A performance finds a sound in its own media/ first, then in the show's.
Sounds you add to the show are the piece's; sounds you add to a performance
(an announcement) and its recordings stay with that performance.

The show's template (Hamlet.wfg in Hamlet/) is optional: "Make this the
show's template" in a performance's File menu makes one. When you close a
performance that differs from it - or choose "Update the show's
template..." - Go.dot lists what differs, cue by cue, and brings what you
tick back into the template, for the performances still to come.

  Windows   Two downloads, the same Go.dot. The setup (-setup.exe) installs
            it for you alone, or for everybody if you choose, adds it to the
            Start menu, and makes a double-click on a show's .wfg open it; it
            is taken away again from Settings > Apps. The zip installs
            nothing: unzip it anywhere and double-click Go.dot.exe, or drop a
            show folder or its .wfg on it. Either way SmartScreen will warn
            that the publisher is unknown: choose "More info", then "Run
            anyway".

            The first time, it copies the empty show to
            %APPDATA%\Go.dot\Untitled and opens that. Each launch writes its
            log to %APPDATA%\Go.dot\logs - attach the newest one to a report.

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

  Linux     Two downloads, the same Go.dot, built on Ubuntu 24.04. The
            package (.deb) installs it for everybody:
                sudo apt install ./go.dot-<version>-linux-x86_64.deb
            and then Go.dot is in the applications menu, `go.dot` starts it
            from a terminal, and a double-click on a show's .wfg opens it;
            `sudo apt remove go.dot` takes it away. The tarball installs
            nothing: unpack it and run ./go.dot.sh (or ./go.dot.sh
            ~/shows/Tuesday). Either needs ALSA, FreeType, fontconfig and
            the X11 libraries, which a desktop install already has.

            The first time, it copies the empty show to
            ~/.local/share/Go.dot/Untitled and opens that. Started from the
            menu or a double-click, each launch writes its log to
            ~/.local/state/Go.dot/logs - attach the newest one to a report;
            from a terminal, the terminal shows it.

            A multichannel interface: a desktop running PipeWire holds it,
            and ALSA then offers only two channels. Use JACK instead:
            install pipewire-jack, set the interface's profile to "Pro
            Audio" (pavucontrol, Configuration tab), and choose JACK in Show
            settings. go.dot.sh starts Go.dot through pw-jack when it is
            installed, so every channel is there and the desktop keeps its
            sound.

            From the tarball, to open a show's .wfg with a double-click,
            choose "Open .wfg files with this Go.dot" in the File menu, or
            run ./wfg associate  - for you alone, pointing at this folder:
            run it again if you move the folder, and ./wfg associate
            --remove to take it away. The package does this for you.

The web client is served beside the window: the log (or the terminal)
prints its address (http://localhost:<port>/ui). A tablet on the same network can open it with
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
