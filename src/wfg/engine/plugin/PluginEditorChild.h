/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

/*
    THE PLUGIN'S OWN WINDOW, IN A PROCESS OF ITS OWN (the author's decision of
    2026-09-25, overriding §17.6's "no editor is ever opened").

        wfg plugin-editor --region=<path> --plugin=<identifier> --parent-pid=P
                          [--description=<file>] [--preset=<file>] [--name=<name>]
                          [--sample-rate=R] [--block-size=B] [--channels=C]
                          [--no-window]

    WHY ANOTHER PROCESS, and not the voice child that already has the plugin
    loaded: a plugin's window is the crashiest thing it has, and a crash
    there must take down a window, never a voice. So the helper loads its OWN
    copy of the plugin - never in the audio path, never processing a block -
    shows its window, and says what the hand does to it. The desktop client
    writes each turn to the cue, as a slider would: saved, undoable, and a
    voice playing that cue follows within two ticks.

    IT FOLLOWS THE PICK. The client hands it a subject - which cue, its title,
    its values - and the helper puts its plugin where that cue has it. A pick
    that has no such insert greys the window: the editor is hidden and a
    sentence says why, because a plugin's native view cannot be covered by
    anything drawn over it, and turning knobs that go nowhere would be worse
    than seeing none.

    THE SHOW'S KEYS COME BACK. The window belongs to another process, so the
    keyboard is the helper's while it is in front: Space and Esc pressed in
    it and not taken by the plugin are handed back to the client, which does
    exactly what its own keys do. Best effort, and said so: a plugin view
    that takes the keyboard for itself keeps it.

    `--no-window` is everything but the window, for a runner with no screen.
    The built-in `godot:test-gain` is a real processor here, with JUCE's
    generic editor, so the whole path runs in CI.

    NAMES NO JUCE TYPE.
*/

#include <string>
#include <vector>

namespace wfg::plugin
{
    /** Runs the helper to completion. `args` are the words after `plugin-editor`. */
    int runPluginEditor (const std::vector<std::string>& args);

    /*  If argv[1] is `plugin-editor`, runs it, stores the exit code and
        answers true; false for an ordinary run. */
    bool runPluginEditorIfAsked (int argc, char** argv, int& exitCode);

    /** The verb, spelled once. */
    inline constexpr const char* pluginEditorVerb = "plugin-editor";
}
