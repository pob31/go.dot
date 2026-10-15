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
    A SCRIPT CUE'S CODE AT THE FOOT (namespace draft §66, AMO, AMT).

    The one place a script is read and written - the inspector does not show
    it (AMO). JUCE's code editor, coloured by the script's language in the
    theme's inks: Lua by JUCE's Lua colouring, JavaScript by its C++ one, the
    nearest JUCE has. Under the code, a line that says how the script is - its
    language, and running, starting, late or stuck - and the last thing it
    printed; a problem the language reported on a line is marked on that line.

    ONE WRITE PER FINISHED EDIT (AMT): when the editor loses the keyboard, on
    Ctrl or Cmd with S - which then goes on to save the show - and when
    another cue is picked or the panel shuts. Never a keystroke: an edit opens
    the running script again from its beginning (ACM). While the tree catches
    up the text sent is kept, and an edit made elsewhere - in the system's
    editor (AMU) - replaces what is shown only when nothing typed here waits.

    UNDER THE LOCK it shows the code and changes nothing.

    AND TWO DOORS (AMU, AMV): "Open in your editor" hands the source to this
    machine's own editor for its language, each save there coming back; and a
    cue dragged here from the list types the line that fires it, on the line
    it is let go on.

    IT TAKES THE KEYS A CODE EDITOR USES: Space and Return type. Esc is still
    the PANIC, from here as from anywhere.
*/

#include <wfg/client/model/Code.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class ScriptEditorComponent final : public juce::Component,
                                        public juce::DragAndDropTarget
    {
    public:
        struct Actions
        {
            /** One `node.set`, a row as text. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;

            /** The source opened in this machine's own editor (AMU). */
            std::function<void (const std::string& cueId)> openInEditor;
        };

        ScriptEditorComponent (const model::Theme&, Actions);

        /*  Whatever was typed and not yet written is written as it goes. */
        ~ScriptEditorComponent() override;

        void show (const model::CodeReading& reading);
        void applyTheme (const model::Theme&);

        /*  Writes what was typed, if it differs from what the cue holds. */
        void commit();

        /*  A CUE DRAGGED FROM THE LIST (AMV): the list's row drag names its
            id; let go here, the line that fires it is typed where it landed. */
        bool isInterestedInDragSource (const SourceDetails& details) override;
        void itemDropped (const SourceDetails& details) override;

        void paint (juce::Graphics&) override;
        void paintOverChildren (juce::Graphics&) override;
        void resized() override;

        /*  FOR A TEST: the editor and its document, the line marked, and what
            the line under the code says. */
        juce::CodeEditorComponent& editorForTest() noexcept;
        juce::CodeDocument& documentForTest() noexcept { return document; }
        int markedLine() const noexcept { return problemLine; }
        juce::String statusForTest() const;
        bool isReadOnly() const noexcept { return reading.locked; }
        juce::TextButton& openButtonForTest() noexcept { return openButton; }

        /*  THE ? (AMW): the help card for the script's language, read-only in
            place of the code, and the ready-made scripts beside it to pick
            from; again, back to the script. */
        void showHelp (bool shown);
        bool showingHelp() const noexcept { return helping; }
        juce::TextButton& helpButtonForTest() noexcept { return helpButton; }
        juce::ComboBox& cardsForTest() noexcept { return cards; }
        juce::String helpTextForTest() const { return helpDocument.getAllContent(); }

        /*  FOR A TEST: where the ready-made scripts are, in place of Go.dot's
            own folder. */
        void setReadyMadeFolder (std::string folder) { readyMadeOverride = std::move (folder); }

    private:
        class Editor;

        model::Theme theme;
        Actions actions;
        model::CodeReading reading;

        juce::CodeDocument document;
        std::unique_ptr<juce::CodeTokeniser> tokeniser;
        std::unique_ptr<Editor> editor;
        juce::TextButton openButton { "Open in your editor" };

        juce::TextButton helpButton { "?" };
        juce::ComboBox cards;
        juce::CodeDocument helpDocument;
        std::unique_ptr<juce::CodeEditorComponent> helpView;
        bool helping = false;
        std::string readyMadeOverride;
        std::vector<std::string> cardNames;

        std::string readyMade() const;
        void showCard (int index);
        std::string languageShown;

        std::string loaded;                 // the cue's text as last put in the editor
        std::optional<std::string> held;    // written, until the tree has it
        int problemLine = 0;

        void load (const std::string& text);
        void useLanguage (const std::string& language);
        void colour();
        std::string typed() const;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScriptEditorComponent)
    };
}
