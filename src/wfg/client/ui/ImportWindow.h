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
    WHICH SCENES OF AN ABLETON LIVE SET BECOME GOs, and where the show goes
    (namespace draft §29, QS): a tick box a scene - its number, its name, the
    first line of its annotation - the named ones that do something ticked to
    start, so a rehearsal stash stays out unless somebody wants it; a scene
    that does nothing is listed, greyed, so the numbers read as Live's do.

    Its own window, as the template review is, so the cue list stays in view
    beside it.
*/

#include <wfg/client/model/Theme.h>
#include <wfg/engine/ImportChanges.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class ImportWindow final : public juce::DocumentWindow
    {
    public:
        struct Actions
        {
            /*  "Import": the scenes ticked, the folder to make, the folders added
                to look in, and whether the outputs are paired (§113) - the
                tick's answer, and true where the tick is not offered. */
            std::function<void (const std::vector<int>& scenes, const juce::File& into,
                                const std::vector<std::string>& searchFolders, bool pairOutputs)> import;

            /*  WHETHER TO OFFER THE PAIRING TICK (namespace draft §113, BCF): a
                QLab workspace's outputs are mono, and 1-2, 3-4... become stereo
                mixes unless the hand says the rig's are single speakers. A Live
                set's returns are already what they are. */
            bool offerPairs = false;

            /*  THE FILES LOOKED FOR (namespace draft §100, AXX), in the folders
                given too - asked off the message thread, `answer` called back on
                it. Unset, the window has no Files part: a Live set's. */
            std::function<void (const std::vector<std::string>& folders,
                                std::function<void (const ImportFiles&)> answer)> findFiles;

            /** Cancel, or the close button: nothing is written. */
            std::function<void()> cancel;
        };

        /*  `sets` is what was picked, for the sentence at the top; `scenes`
            the template's; `parent` and `name` where the show goes to start. */
        ImportWindow (const model::Theme& theme, const juce::StringArray& sets, const ImportScenes& scenes,
                      const juce::File& parent, const juce::String& name, Actions actions);
        ~ImportWindow() override;

        void closeButtonPressed() override;

    private:
        class Content;
        std::unique_ptr<Content> content;
        Actions actions;
    };
}
