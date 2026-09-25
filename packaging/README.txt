Go.dot - test build
===================

This is an early build for testing and comments. It is not signed, it is not
an installer, and it is not ready to run a show. Please do try it, and please
tell us what you find:

    https://github.com/pob31/go.dot/issues

A report is most useful with the build's name (the archive's name, or the
first line of `wfg --version`), your OS, your audio device, and the text the
terminal window printed.


Starting it
-----------

Every platform has a launcher beside the binary. With nothing else it opens
the empty show in the folder "Untitled", on your default audio device, in a
window. Give it a show folder to open that instead. Use Save As in the window
to keep a show of your own somewhere else.

  Windows   Double-click Go.dot.cmd, or drop a show folder on it.
            SmartScreen will warn that the publisher is unknown: choose
            "More info", then "Run anyway".

  macOS     Double-click Go.dot.command. The first time, macOS will refuse
            because the build is not notarized. Either open System Settings >
            Privacy & Security and choose "Open Anyway", or clear the
            quarantine once in Terminal, from this folder:

                xattr -dr com.apple.quarantine .

            The binary is universal (Apple silicon and Intel) and needs
            macOS 13.3 or later.

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
