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
    THE PLUGINS' OWN WINDOWS, as the desktop client keeps them (the author's
    decision of 2026-09-25: "show the chain, bypass switch and open the native
    plugin UI as a popup").

    ONE HELPER A PLUGIN OF THE SET, at most, each a process of its own
    (EditorHost): Edit... on a box starts it, or brings its window forward if
    it is already up. Its window FOLLOWS THE PICK - once a pass, from the
    pass's own snapshot, every open window is told which cue it is about and
    what that cue says - and it greys when the pick has no such insert.

    WHAT THE WINDOW DOES IS AN ORDINARY WRITE. Each value the helper reports
    becomes one `node.set` on the cue's `p<n>`, with origin `window`, exactly
    as a slider would send it: saved, undoable, a drag coalesced into one step
    by the engine, a voice playing the cue following within two ticks. The
    cue it goes to is the one the value was MOVED under, not whichever is
    picked when it arrives.

    EDIT... ON AN INSERT THE CUE HAS NOT GOT switches it in first - that is
    `fx.create`, the chain's first switch-in - and opens the window when the
    tree shows it, unless the pick moves on first.

    THE LOCK CLOSES THEM ALL. Writes are refused while the show is locked, and
    a window whose knobs go nowhere is worse than none; Edit... says why.

    THE SHOW'S KEYS COME BACK: Space and Esc pressed in a plugin's window (and
    not taken by the plugin) reach the client's own key handling.

    Its own timer, at fifty hertz, drains the helpers and reads nothing from
    the tree; the one snapshot read stays in the client's pass.
*/

#include <wfg/client/model/FxEditor.h>
#include <wfg/engine/plugin/EditorHost.h>

#include <juce_events/juce_events.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::ui
{
    class PluginEditors final : private juce::Timer
    {
    public:
        struct Actions
        {
            /** One `node.set`: a parameter the plugin's window moved. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** `fx.create`, when Edit... is pressed on an insert the cue has not got. */
            std::function<void (const std::string& cueId, const std::string& pluginId)> createFx;

            /*  `fx.capture`: a plugin's whole state kept with a cue - the file
                the helper wrote, and every value, in the row's spelling. */
            std::function<void (const std::string& fxId, const std::string& stateFile,
                                const std::string& values)> capture;

            /** The show's key, pressed in a plugin's window: true for Esc, false for Space. */
            std::function<void (bool escape)> key;

            /** The words changed: the chain should say so. */
            std::function<void()> changed;
        };

        /*  `describe` is the console's door to this machine's scan;
            `workFolder` is where the helpers' regions go. A launch other than
            this process's own is for tests. */
        PluginEditors (Actions, std::function<std::string (const std::string& identifier)> describe,
                       std::string workFolder, plugin::EditorLaunch launch = {}, bool headless = false);
        ~PluginEditors() override;

        /** Edit... on a box: open that plugin's window on this cue, or bring it forward. */
        void edit (const tree::TreeSnapshot&, const std::string& cueId, const std::string& pluginId, bool locked);

        /** Once a pass: every window follows the pick; the lock closes them. */
        void follow (const tree::TreeSnapshot&, const std::string& pickedCueId, bool locked);

        /** What each plugin's box should say about its window, by entry. */
        const std::map<std::string, std::string>& words() const noexcept { return said; }

        /** Bumped whenever `words()` changes. */
        std::uint32_t wordsRevision() const noexcept { return revision; }

        /** Every window closed, now. */
        void closeAll();

        /** The helper for an entry, for tests; null when none is up. */
        plugin::EditorHost* hostFor (const std::string& pluginId) const;

        /** One pass of the timer's work, for tests that drive it by hand. */
        void service();

    private:
        void timerCallback() override;

        void start (const tree::TreeSnapshot&, const std::string& cueId, const std::string& pluginId);
        void say (const std::string& pluginId, const std::string& words);
        void hush (const std::string& pluginId);

        struct Open
        {
            std::unique_ptr<plugin::EditorHost> host;
            bool leaving = false;

            /*  Whether the show has a folder to keep a whole state in; a
                window on one that has not says to save it first. */
            bool canKeepState = false;

            /*  The last subject handed over, for telling a new cue (or a new
                state, after an undo) from new values on the same one. */
            model::EditorSubject sent;
        };

        void hand (Open&, const model::EditorSubject&);

        struct Waiting
        {
            std::string cueId, pluginId;
            std::uint32_t since = 0;
        };

        Actions actions;
        std::function<std::string (const std::string&)> describe;
        std::string workFolder;
        plugin::EditorLaunch launch;
        bool headless;

        std::map<std::string, Open> open;
        std::optional<Waiting> waiting;
        std::map<std::string, std::string> said;
        std::uint32_t revision = 0;
    };
}
