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

#include <wfg/client/ui/RunPaneComponent.h>

#include <wfg/client/ui/Look.h>

#include <utility>

namespace wfg::client::ui
{
    RunPaneComponent::RunPaneComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        list.setRowHeight (rowHeight());
        list.setWantsKeyboardFocus (false);
        list.getViewport()->setScrollBarsShown (true, false);
        addAndMakeVisible (list);

        applyTheme (theme);
    }

    int RunPaneComponent::rowHeight() const noexcept
    {
        /*  HALF AGAIN AS TALL AS A CUE ROW, because a run row is two things
            now: the line of words it always was, and the strip under it that
            says what is sounding and how far through it is. The extra is the
            strip's, so the words keep the height they were judged at.

            EVERY ROW IS THE SAME HEIGHT, which a `juce::ListBox` requires
            and which is right anyway: rows that grew and shrank as cues
            changed kind would make a busy pane jump under the eye reading it.

            SO THE PANE IS TALL ONLY WHEN SOMETHING IN IT NEEDS THE HEIGHT
            (author, 2026-09-18: "the progress bar of non-media cues and also
            the non-instant cues can have the progress cursor layer beneath,
            not vertically stacked, in the layout. This saves height. Keep
            media file progress bar with the coloured waveform vertically below
            as it is so we have some clarity"). A waveform is a PICTURE and
            wants its own band under the words; a cursor is one MARK and can
            lie behind them. A pane with no sound in it is the ordinary height
            and gives back the third of itself it was spending on nothing. */
        return juce::roundToInt (theme.row * theme.type * (tallRows ? 1.5 : 1.0));
    }

    void RunPaneComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        list.setRowHeight (rowHeight());
        list.setColour (juce::ListBox::backgroundColourId, Look::colour (theme, "panel"));
        list.setColour (juce::ListBox::outlineColourId, Look::colour (theme, "rule"));

        resized();
        repaint();
    }

    int RunPaneComponent::stripHeight() const noexcept
    {
        /*  A third of the row, which is enough for a waveform to have a shape
            and little enough that the words above it stay the row. */
        return juce::jmax (4, rowHeight() / 3);
    }

    bool RunPaneComponent::hasWaveform (const model::RunRow& entry) const
    {
        if (media == nullptr || entry.kind != "media" || entry.file.empty())
            return false;

        const auto found = media->find (entry.file);

        return found != media->end() && found->second.pyramid != nullptr;
    }

    bool RunPaneComponent::anyWaveform() const
    {
        for (const auto& entry : rows)
            if (hasWaveform (entry))
                return true;

        return false;
    }

    const std::vector<model::Column>& RunPaneComponent::columnsFor (const std::string& file,
                                                                   int width)
    {
        static const std::vector<model::Column> none;

        if (media == nullptr || file.empty() || width <= 0)
            return none;

        if (width != barsWidth)
        {
            bars.clear();
            barsWidth = width;
        }

        if (const auto drawn = bars.find (file); drawn != bars.end())
            return drawn->second;

        const auto found = media->find (file);

        /*  NOT ANALYSED YET IS NOT CACHED, because it is a state the analyser
            thread is about to leave: caching the empty answer would leave the
            bar blank for the life of the window. */
        if (found == media->end() || found->second.pyramid == nullptr)
            return none;

        return bars.emplace (file, model::waveform (*found->second.pyramid, width))
                   .first->second;
    }

    /*  WHAT GOES UNDER THE WORDS, which is one of three things and never two
        of them at once:

          a WAIT   - a bar in one colour, travelling the way that wait travels
          a SOUND  - the file's own waveform, coloured by what it sounds like
          anything else - a plain cursor on a shaded ground, in the run's colour

        The third is the author's (2026-09-18: "for non media files, ie without
        the coloured waveform, we should have just a cursor underneath the strip
        of the cue name"), and it is also where a media cue lands while its
        analysis is still being made: a waveform that is not ready yet is not a
        cue with nothing happening, so the cursor answers until the bar can. */
    void RunPaneComponent::paintStrip (const model::RunRow& entry, juce::Graphics& g,
                                       juce::Rectangle<int> strip, juce::Colour tint,
                                       bool layered)
    {
        if (entry.isWaiting())
        {
            paintCountdown (entry, g, strip, layered);
            return;
        }

        if (! layered && paintWaveform (entry, g, strip))
            return;

        paintCursor (entry, g, strip, tint, layered);
    }

    /*  A SHADED GROUND AND A CURSOR ON IT. The ground is what makes the strip a
        strip when there is no waveform to fill it, and the cursor is where the
        run has got to - drawn only when a LENGTH is known, because a cursor
        pinned at nought would be saying "the start" about something that has no
        measured end. */
    void RunPaneComponent::paintCursor (const model::RunRow& entry, juce::Graphics& g,
                                        juce::Rectangle<int> strip, juce::Colour tint,
                                        bool layered)
    {
        /*  BEHIND THE WORDS OR UNDER THEM. Layered, it draws no ground of its
            own - the row's is what it sits on - and everything is faint enough
            that a name reads over it; given a band, it can be solid. */
        if (! layered)
        {
            g.setColour (Look::colour (theme, "panel-in"));
            g.fillRect (strip);
        }

        if (! (entry.length > 0.0) || ! entry.launched())
            return;

        const auto through = model::playhead (entry.seconds, entry.length);
        const auto x = strip.getX() + juce::roundToInt (through * (strip.getWidth() - 1));

        /*  What is behind it, faintly, so the cursor reads as having come from
            somewhere rather than as a mark somebody put there. */
        g.setColour (tint.withAlpha (layered ? 0.14f : 0.25f));
        g.fillRect (strip.getX(), strip.getY(), x - strip.getX(), strip.getHeight());

        g.setColour (tint.withAlpha (layered ? 0.55f : 1.0f));
        g.fillRect (x, strip.getY(), 2, strip.getHeight());
    }

    /*  Answers whether it drew anything: a file the analyser has not been round
        to yet has no columns, and the caller falls back to a cursor rather than
        leaving the strip blank. */
    bool RunPaneComponent::paintWaveform (const model::RunRow& entry, juce::Graphics& g,
                                          juce::Rectangle<int> strip)
    {
        const auto& columns = columnsFor (entry.file, strip.getWidth());

        if (columns.empty())
            return false;

        const auto middle = strip.getCentreY();
        const auto half = strip.getHeight() / 2.0;

        /*  ONE LINE PER COLUMN, mirrored about the middle so it reads as a
            waveform rather than as a bar chart, and coloured by what that
            slice of the file SOUNDS like (§3.30). A minimum of one pixel, so
            silence is a line rather than a gap: a file that has not started is
            not a file that is missing. */
        for (auto at = 0; at < static_cast<int> (columns.size()); ++at)
        {
            const auto& column = columns[static_cast<std::size_t> (at)];

            g.setColour (juce::Colour::fromHSV (static_cast<float> (column.hue / 360.0),
                                                static_cast<float> (column.saturation),
                                                static_cast<float> (0.25 + column.lightness * 0.6),
                                                1.0f));

            const auto height = juce::jmax (1.0f, static_cast<float> (column.peak * half));

            g.fillRect (static_cast<float> (strip.getX() + at),
                        static_cast<float> (middle) - height, 1.0f, height * 2.0f);
        }

        /*  AND WHERE IT HAS GOT TO. Nought when the length is not known, which
            is a cue imported in this session - its duration arrives when the
            show is next opened - so the head sits at the left rather than
            sliding across a bar nobody has measured. */
        const auto through = model::playhead (entry.seconds, entry.length);

        if (entry.launched() && entry.length > 0.0)
        {
            const auto x = strip.getX() + juce::roundToInt (through * (strip.getWidth() - 1));

            g.setColour (Look::colour (theme, "ink"));
            g.fillRect (x, strip.getY(), 1, strip.getHeight());
        }

        return true;
    }

    void RunPaneComponent::paintCountdown (const model::RunRow& entry, juce::Graphics& g,
                                           juce::Rectangle<int> strip, bool layered)
    {
        /*  ONE COLOUR, TWO DIRECTIONS (author, 2026-09-18: "pre (right to
            left) and post (left to right) wait times can be one colour").

            A PRE-WAIT arrives at the cue: it empties right to left, so the
            moving edge travels towards the moment the thing fires, and an
            empty bar is a cue about to go. A POST-WAIT leaves the cue behind:
            it fills left to right, and a full bar is a run that is done. The
            MOTION is what tells them apart, which is exactly why one hue is
            enough - and why a hue would have been the weaker telling anyway
            (§4.8). */
        const auto left = model::countdown (entry.remaining, entry.waitTotal);
        const auto width = juce::roundToInt ((model::runsLeftToRight (entry.state) ? 1.0 - left
                                                                                   : left)
                                               * strip.getWidth());

        if (! layered)
        {
            g.setColour (Look::colour (theme, "panel-in"));
            g.fillRect (strip);
        }

        g.setColour (Look::colour (theme, "waiting").withAlpha (layered ? 0.18f : 0.75f));
        g.fillRect (strip.getX(), strip.getY(), width, strip.getHeight());
    }

    void RunPaneComponent::show (std::vector<model::RunRow> runs,
                                 std::shared_ptr<const audio::MediaRecords> mediaToUse)
    {
        media = std::move (mediaToUse);

        /*  THE WHOLE LIST OR NOTHING. A run's position moves every tick, so
            comparing row by row to find what changed would cost more than
            redrawing the dozen rows a busy pane holds - which is the opposite
            of the cue list's answer, and right for the opposite reason. What
            is still worth avoiding is `updateContent()` when the SET of runs
            has not changed, because that relays out every row. */
        const auto sameRuns = runs.size() == rows.size()
                           && std::equal (runs.begin(), runs.end(), rows.begin(),
                                          [] (const model::RunRow& a, const model::RunRow& b)
                                          { return a.id == b.id; });

        rows = std::move (runs);

        /*  AND WHETHER THE PANE NEEDS TO BE TALL, which only a waveform asks
            for. Asked after the rows are in, and only acted on when the answer
            CHANGES: setting the row height relays out every row. */
        if (const auto tall = anyWaveform(); tall != tallRows)
        {
            tallRows = tall;
            list.setRowHeight (rowHeight());
            list.updateContent();
            return;
        }

        if (sameRuns)
            list.repaint();
        else
            list.updateContent();
    }

    int RunPaneComponent::getNumRows()
    {
        return static_cast<int> (rows.size());
    }

    void RunPaneComponent::paintListBoxItem (int row, juce::Graphics& g,
                                             int width, int height, bool)
    {
        if (row < 0 || row >= static_cast<int> (rows.size()))
            return;

        const auto& entry = rows[static_cast<std::size_t> (row)];

        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;

        g.fillAll (Look::colour (theme, row % 2 == 0 ? "panel" : "panel-high"));

        /*  WHERE THE STRIP GOES, and there are two answers. A WAVEFORM takes
            a band of its own under the words, which is what the taller row
            exists for. Anything else lies BEHIND them across the whole row -
            drawn before them, so the name reads over it - and costs no height.

            Both stop short of the kill cross, so nothing runs under the one
            control in this pane. */
        const auto layered = ! hasWaveform (entry);

        const auto strip = layered
                             ? juce::Rectangle<int> (0, 0, width, height)
                                 .reduced (pad, 1).withTrimmedRight (unit * 3)
                             : juce::Rectangle<int> (0, height - stripHeight(), width,
                                                     stripHeight()).reduced (pad + unit, 1);

        if (! layered)
            height -= stripHeight();

        /*  THE STATE IS THE ROW'S LEFT EDGE AS WELL AS ITS MARK, so a pane
            read at a glance from across a booth still sorts what is sounding
            from what is waiting. Shape first, colour second (§4.8). */
        const auto colourFor = [this] (const std::string& state)
        {
            if (state == "playing")  return Look::colour (theme, "live");
            if (state == "armed")    return Look::colour (theme, "standby");
            if (state == "stopping") return Look::colour (theme, "stopping");
            if (state == "failed")   return Look::colour (theme, "failed");

            return Look::colour (theme, "waiting");
        };

        /*  A FADE IS NEITHER A SOUND NOR A WAIT and carries its own colour
            wherever it appears, because it is the one kind that changes
            something already sounding rather than starting or ending anything. */
        const auto tint = entry.kind == "fade" ? Look::colour (theme, "fade")
                                               : colourFor (entry.state);

        paintStrip (entry, g, strip, tint, layered);

        g.setColour (tint);
        g.fillRect (0, 0, 3, height);

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (pad, 0);
        area.removeFromLeft (pad);

        /*  THE KILL, at the right edge where the cross is drawn. Only a click
            on the cross sends it: a run stopped by a click that landed
            anywhere on the row is a cue an operator did not mean to stop. */
        /*  Wider and larger than it was, with room to its left, because it is
            the one control in this pane and it was asking for a small hand
            (author, 2026-09-18: "the X can be a bit larger too with some more
            spacing to its left"). */
        auto killCell = area.removeFromRight (unit * 3);
        g.setColour (Look::colour (theme, "ink-off"));
        g.setFont (Look::font (theme, 16.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")),
                    killCell, juce::Justification::centred, false);

        //  Where it has got to, when it has been let go.
        auto positionCell = area.removeFromRight (unit * 5);
        g.setColour (Look::colour (theme, "ink-faint"));
        g.setFont (Look::font (theme, 12.0f));
        g.drawText (entry.position, positionCell, juce::Justification::centredRight, false);

        //  The mark, or the word for every state that is not one of the two.
        auto stateCell = area.removeFromLeft (unit * 6);
        g.setColour (tint);
        g.setFont (Look::font (theme, entry.mark().empty() ? 11.0f : 13.0f));
        g.drawText (entry.mark().empty() ? juce::String (entry.state)
                                         : juce::String (juce::CharPointer_UTF8 (entry.mark().c_str())),
                    stateCell, juce::Justification::centredLeft, true);

        area.removeFromLeft (entry.depth * unit);

        /*  THE CUE'S NAME, because a run identifier is eight characters the
            engine drew and nobody recognises. A failure says why, in place of
            the name it would otherwise repeat from the row above. */
        g.setColour (Look::colour (theme, entry.error.empty() ? "ink" : "failed"));
        g.setFont (Look::font (theme, 13.0f));

        const auto name = entry.cueName.empty() ? juce::String (entry.cueId)
                                                : juce::String (entry.cueName);

        g.drawText (entry.error.empty() ? name : name + "  " + juce::String (entry.error),
                    area, juce::Justification::centredLeft, true);

        g.setColour (Look::colour (theme, "rule").withAlpha (0.5f));
        g.fillRect (0, height - 1, width, 1);
    }

    void RunPaneComponent::listBoxItemClicked (int row, const juce::MouseEvent& event)
    {
        if (row < 0 || row >= static_cast<int> (rows.size()) || ! actions.kill)
            return;

        const auto unit = juce::roundToInt (theme.type * 7.0);

        if (event.x >= getWidth() - unit * 3)
            actions.kill (rows[static_cast<std::size_t> (row)].id);
    }

    void RunPaneComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        /*  AN EMPTY PANE SAYS SO. A blank rectangle and a pane that has stopped
            answering look identical, and the difference matters most at the
            moment somebody is wondering why nothing is happening. */
        if (rows.empty())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawText ("nothing running", getLocalBounds(), juce::Justification::centred, false);
        }
    }

    void RunPaneComponent::resized()
    {
        list.setBounds (getLocalBounds());
    }
}
