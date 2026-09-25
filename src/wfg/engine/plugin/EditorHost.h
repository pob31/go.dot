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
    THE PARENT'S END OF ONE PLUGIN'S EDITING HELPER (the author's decision of
    2026-09-25): lays out the region, starts `wfg plugin-editor`, hands it the
    subject the window is about, and drains what the plugin's window did.

    OWNED BY THE DESKTOP CLIENT, NOT THE ENGINE. Opening a window changes
    nothing in the show - it is a machine-local thing, like a file chooser -
    so it is no command and the engine never hears of it. What the window
    DOES is ordinary: the client turns each value the helper reports into a
    `node.set` on the cue's `p<n>`, the same write a slider would send.

    Message thread only, like ProxyHost's poll. `poll()` is what notices the
    helper came up, failed, or went away; nothing here blocks except the
    destructor, which gives a helper a moment to leave before ending it.

    NAMES NO JUCE TYPE.
*/

#include <wfg/engine/plugin/EditorRegion.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wfg::plugin
{
    struct EditorLaunch
    {
        /** Empty means this process's own executable. */
        std::string executable;
        std::vector<std::string> leadingArgs { "plugin-editor" };
    };

    struct EditorSpec
    {
        std::string pluginId;           ///< the set entry
        std::string identifier;         ///< the scan's identifier, or the test gain's
        std::string name;               ///< the window's first title

        /** The scan's description, as XML; empty for the built-in test gain. */
        std::string descriptionXml;

        /** The entry's preset, absolute; empty for none. */
        std::string presetPath;

        /** Where the region and the description go. */
        std::string workFolder;

        int sampleRate = 48000;
        int blockSize = 512;
        int channels = 2;

        /** Everything but the window: for a machine with no screen. */
        bool headless = false;

        EditorLaunch launch;
    };

    class EditorHost
    {
    public:
        explicit EditorHost (EditorSpec);

        /*  Asks the helper to leave, waits a moment, ends it if it has not,
            and removes the files. */
        ~EditorHost();

        EditorHost (const EditorHost&) = delete;
        EditorHost& operator= (const EditorHost&) = delete;

        /** Lays out the region and starts the helper. False, with a sentence, when it could not. */
        bool start (std::string& why);

        enum class Status { starting, open, failed, ended };

        Status status() const noexcept;

        /** Why it failed, in a sentence; empty otherwise. */
        const std::string& problem() const noexcept;

        /** Message thread: came up, failed, went; and a waiting subject handed over. */
        void poll();

        /** What the window is about. */
        struct Subject
        {
            std::string cueId;
            std::string fxId;
            std::string title;
            std::string reason;
            bool greyed = true;
            std::vector<float> values;      ///< 0..1, or editor::restsAtPreset
        };

        /*  Hands the helper a subject. If it has not taken the last one yet,
            this one waits, replacing any other waiting - only the newest
            pick matters. */
        void setSubject (const Subject&);

        /** The current subject's values, moved elsewhere (an undo, the page). */
        void setLive (const std::vector<float>& values);

        void setVisible (bool);

        /** Brings the window forward, and lets the helper take the foreground. */
        void raise();

        struct Event
        {
            editor::EventKind kind = editor::EventKind::none;
            int index = 0;
            float value = 0.0f;
            std::uint32_t subjectSeq = 0;
        };

        /** What the plugin's window did since the last drain, in order. */
        std::vector<Event> drain();

        /*  The insert a subject was about, by the sequence an event carries;
            empty when the subject was greyed or is too old to remember. */
        std::string fxIdFor (std::uint32_t subjectSeq) const;

        /** Asks the helper to leave; `poll()` sees it go. */
        void leave();

        std::int64_t pid() const noexcept;

        //==============================================================================
        /*  FOR TESTS: a hand on the plugin's window, which CI has none of -
            parameter `index` to `value`, gesture and all; -2 is its close
            button - and what the helper's plugin has now. */
        void poke (int index, float value);
        float currentValue (int index) const;
        int paramCount() const noexcept;
        std::uint32_t subjectTaken() const noexcept;
        std::uint32_t loadMicroseconds() const noexcept;

        /** Ends the helper without asking, as a crash in its plugin would. */
        void kill();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
