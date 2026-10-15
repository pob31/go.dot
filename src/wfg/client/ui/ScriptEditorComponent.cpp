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

#include <wfg/client/ui/ScriptEditorComponent.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/process/ScriptText.h>

#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int statusHeight = 22;
    }

    /*  JUCE's editor, told what this panel needs of it: a write when it loses
        the keyboard, Ctrl or Cmd with S written and passed on, and Esc never
        kept. */
    class ScriptEditorComponent::Editor final : public juce::CodeEditorComponent
    {
    public:
        Editor (juce::CodeDocument& doc, juce::CodeTokeniser* tokens, ScriptEditorComponent& ownerToUse)
            : juce::CodeEditorComponent (doc, tokens), owner (ownerToUse)
        {
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key.getKeyCode() == juce::KeyPress::escapeKey)
                return false;

            if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'S' || key.getKeyCode() == 's'))
            {
                owner.commit();
                return false;
            }

            return juce::CodeEditorComponent::keyPressed (key);
        }

        void focusLost (FocusChangeType cause) override
        {
            juce::CodeEditorComponent::focusLost (cause);
            owner.commit();
        }

    private:
        ScriptEditorComponent& owner;
    };

    ScriptEditorComponent::ScriptEditorComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        useLanguage ("javascript");

        addAndMakeVisible (openButton);
        openButton.setTooltip ("The script in this machine's own editor for its language - edited there,"
                               " never run; each save comes back here.");
        openButton.onClick = [this]
        {
            commit();
            if (actions.openInEditor && ! reading.cueId.empty())
                actions.openInEditor (reading.cueId);
        };
    }

    ScriptEditorComponent::~ScriptEditorComponent()
    {
        commit();
    }

    std::string ScriptEditorComponent::typed() const
    {
        return document.getAllContent().toStdString();
    }

    void ScriptEditorComponent::useLanguage (const std::string& language)
    {
        const auto lua = language == "lua";
        if (editor != nullptr && languageShown == (lua ? "lua" : "javascript"))
            return;

        languageShown = lua ? "lua" : "javascript";

        if (editor != nullptr)
            removeChildComponent (editor.get());
        editor.reset();

        if (lua)
            tokeniser = std::make_unique<juce::LuaTokeniser>();
        else
            tokeniser = std::make_unique<juce::CPlusPlusCodeTokeniser>();

        editor = std::make_unique<Editor> (document, tokeniser.get(), *this);
        editor->setTabSize (4, true);
        editor->setLineNumbersShown (true);
        editor->setScrollbarThickness (10);
        addAndMakeVisible (*editor);
        colour();
        resized();
    }

    void ScriptEditorComponent::colour()
    {
        if (editor == nullptr)
            return;

        editor->setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f,
                                                        juce::Font::plain)));

        const auto ink = [this] (const char* name) { return Look::colour (theme, name); };

        editor->setColour (juce::CodeEditorComponent::backgroundColourId, ink ("panel-in"));
        editor->setColour (juce::CodeEditorComponent::defaultTextColourId, ink ("ink"));
        editor->setColour (juce::CodeEditorComponent::highlightColourId, ink ("picked").withAlpha (0.45f));
        editor->setColour (juce::CodeEditorComponent::lineNumberBackgroundId, ink ("panel"));
        editor->setColour (juce::CodeEditorComponent::lineNumberTextId, ink ("ink-faint"));
        editor->setColour (juce::CaretComponent::caretColourId, ink ("ink"));

        /*  EACH KIND OF WORD IN AN INK OF ITS OWN, from the theme - and never
            colour alone (PRD 4.8): the words still say what they are. */
        auto scheme = tokeniser->getDefaultColourScheme();
        for (auto& type : scheme.types)
        {
            const auto name = type.name;
            if (name == "Error")                                type.colour = ink ("failed");
            else if (name == "Comment")                         type.colour = ink ("ink-faint");
            else if (name == "Keyword")                         type.colour = ink ("kind-script");
            else if (name == "Integer" || name == "Float")      type.colour = ink ("kind-osc");
            else if (name == "String")                          type.colour = ink ("kind-process");
            else if (name == "Identifier")                      type.colour = ink ("ink");
            else                                                type.colour = ink ("ink-dim");
        }
        editor->setColourScheme (scheme);
    }

    void ScriptEditorComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        colour();
        repaint();
    }

    void ScriptEditorComponent::load (const std::string& text)
    {
        loaded = text;
        document.replaceAllContent (juce::String::fromUTF8 (text.data(), static_cast<int> (text.size())));
        document.clearUndoHistory();
        document.setSavePoint();
    }

    void ScriptEditorComponent::show (const model::CodeReading& next)
    {
        const auto sameCue = next.cueId == reading.cueId;

        //  Another cue picked: what was typed for the last one is written first.
        if (! sameCue)
        {
            commit();
            held.reset();
        }

        reading = next;
        useLanguage (reading.language);

        if (! sameCue)
        {
            load (reading.text);
        }
        else
        {
            //  The tree has caught up with what was written.
            if (held.has_value() && *held == reading.text)
                held.reset();

            /*  CHANGED ELSEWHERE - the system's editor, another window, an undo -
                shown when nothing typed here waits to be written. */
            const auto nothingWaiting = typed() == loaded && ! held.has_value();
            if (reading.text != loaded && nothingWaiting)
                load (reading.text);
        }

        editor->setReadOnly (reading.locked || reading.cueId.empty());
        openButton.setEnabled (! reading.locked && ! reading.cueId.empty());
        problemLine = model::lineOfProblem (reading.said);
        repaint();
    }

    void ScriptEditorComponent::commit()
    {
        if (reading.cueId.empty() || reading.locked || ! actions.set)
            return;

        const auto text = typed();
        if (text == loaded)
            return;

        actions.set ("/godot/cue/" + reading.cueId + "/source", text);
        loaded = text;
        held = text;
    }

    namespace
    {
        //  A cue's id as the list's drag names it: eight of Crockford's letters and digits.
        bool isCueId (const juce::String& said)
        {
            return said.length() == 8 && said.containsOnly ("0123456789ABCDEFGHJKMNPQRSTVWXYZ");
        }
    }

    bool ScriptEditorComponent::isInterestedInDragSource (const SourceDetails& details)
    {
        return ! reading.locked && ! reading.cueId.empty() && details.description.isString()
                 && isCueId (details.description.toString());
    }

    void ScriptEditorComponent::itemDropped (const SourceDetails& details)
    {
        if (! isInterestedInDragSource (details) || editor == nullptr)
            return;

        const auto line = process::scriptLineToFireCue (reading.language, details.description.toString().toStdString());
        const auto where = editor->getPositionAt (details.localPosition.x - editor->getX(),
                                                  details.localPosition.y - editor->getY());
        const juce::CodeDocument::Position start (document, where.getLineNumber(), 0);
        document.insertText (start, juce::String::fromUTF8 (line.c_str()) + "\n");
        editor->moveCaretTo (juce::CodeDocument::Position (document, where.getLineNumber() + 1, 0), false);
        commit();
    }

    juce::CodeEditorComponent& ScriptEditorComponent::editorForTest() noexcept
    {
        return *editor;
    }

    juce::String ScriptEditorComponent::statusForTest() const
    {
        auto words = juce::String (model::codeStatusWords (reading));
        if (reading.locked)
            words << " - the show is locked";
        if (! reading.said.empty())
            words << " - " << juce::String::fromUTF8 (reading.said.data(), static_cast<int> (reading.said.size()));
        return words;
    }

    void ScriptEditorComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        auto strip = getLocalBounds().removeFromBottom (statusHeight).reduced (6, 0);
        strip.removeFromRight (openButton.getWidth() + 6);
        const auto failing = problemLine > 0 || reading.state == "stuck";
        g.setColour (Look::colour (theme, failing ? "failed" : "ink-dim"));
        g.setFont (Look::font (theme, 13.0f));
        g.drawFittedText (statusForTest(), strip, juce::Justification::centredLeft, 1);
    }

    void ScriptEditorComponent::paintOverChildren (juce::Graphics& g)
    {
        //  THE LINE A PROBLEM NAMES, marked in the margin and underlined.
        if (problemLine <= 0 || editor == nullptr || problemLine > document.getNumLines())
            return;

        const juce::CodeDocument::Position at (document, problemLine - 1, 0);
        const auto glyph = editor->getCharacterBounds (at);
        if (glyph.getBottom() < 0 || glyph.getY() > editor->getHeight())
            return;

        const auto top = editor->getY() + glyph.getY();
        g.setColour (Look::colour (theme, "failed"));
        g.fillRect (editor->getX(), top, 4, glyph.getHeight());
        g.fillRect (editor->getX() + glyph.getX(), top + glyph.getHeight() - 2,
                    editor->getWidth() - glyph.getX() - 14, 2);
    }

    void ScriptEditorComponent::resized()
    {
        auto area = getLocalBounds();
        auto strip = area.removeFromBottom (statusHeight);
        openButton.setBounds (strip.removeFromRight (150).reduced (2));
        if (editor != nullptr)
            editor->setBounds (area);
    }
}
