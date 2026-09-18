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
#include <wfg/engine/audio/Timbre.h>

#include <algorithm>
#include <utility>

namespace wfg::client::ui
{
    RunPaneComponent::RunPaneComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        viewport.setViewedComponent (&canvas, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setWantsKeyboardFocus (false);
        canvas.setWantsKeyboardFocus (false);
        addAndMakeVisible (viewport);

        applyTheme (theme);
    }

    int RunPaneComponent::rowHeight() const noexcept
    {
        /*  HALF AGAIN AS TALL AS A CUE ROW, because a run row is two things
            now: the line of words it always was, and the strip under it that
            says what is sounding and how far through it is. The extra is the
            strip's, so the words keep the height they were judged at.

            ROWS WERE ALL ONE HEIGHT while this was a `juce::ListBox`, which
            requires it. They are not now, and the reason is in the header:
            a picture wants room and a line does not.

            EACH ROW IS ITS OWN HEIGHT since 2026-09-18 ("only the active cues
            with the waveform should be taller, not the OSC, MIDI, fade cues"):
            this is the height of the WORDS, and `heightOf` adds a band to it
            for a row that has a picture to put there. A waveform is a PICTURE
            and wants its own band; a cursor is one MARK and lies behind the
            words. The words keep their own height either way, so the picture
            cannot squeeze the line above it. */
        return juce::roundToInt (theme.row * theme.type);
    }

    void RunPaneComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        resized();
        repaint();
    }

    int RunPaneComponent::stripHeight() const noexcept
    {
        /*  FOUR TIMES WHAT IT WAS (author, 2026-09-18: "you can make the
            waveform on the running cues panel four times their current
            height"). It was a third of a row, which was enough for a shape and
            not enough to read one: an envelope needs room before an operator
            can tell an attack from a swell.

            Added only to a row with a picture to draw, which is what keeps a
            wait, a fade or an OSC cue one line high. */
        return juce::roundToInt (theme.row * theme.type * 2.0);
    }

    int RunPaneComponent::heightOf (const model::RunRow& entry) const
    {
        return rowHeight() + (hasWaveform (entry) ? stripHeight() : 0);
    }

    int RunPaneComponent::topOf (int index) const
    {
        auto y = 0;

        for (auto at = 0; at < index && at < static_cast<int> (rows.size()); ++at)
            y += heightOf (rows[static_cast<std::size_t> (at)]);

        return y;
    }

    int RunPaneComponent::rowAt (int y) const
    {
        auto top = 0;

        for (auto at = 0; at < static_cast<int> (rows.size()); ++at)
        {
            const auto bottom = top + heightOf (rows[static_cast<std::size_t> (at)]);

            if (y >= top && y < bottom)
                return at;

            top = bottom;
        }

        return -1;
    }

    void RunPaneComponent::layOutRows()
    {
        /*  The canvas is as wide as the viewport shows and as tall as the rows
            add up to; a pane taller than its viewport scrolls. */
        canvas.setSize (juce::jmax (1, viewport.getMaximumVisibleWidth()),
                        juce::jmax (1, topOf (static_cast<int> (rows.size()))));
    }

    void RunPaneComponent::Canvas::paint (juce::Graphics& g)
    {
        for (auto at = 0; at < static_cast<int> (owner.rows.size()); ++at)
        {
            const auto top = owner.topOf (at);
            const auto height = owner.heightOf (owner.rows[static_cast<std::size_t> (at)]);

            juce::Graphics::ScopedSaveState state (g);
            g.setOrigin (0, top);
            g.reduceClipRegion (0, 0, getWidth(), height);

            /*  A RUN THAT IS DONE IS DRAWN DIMMED for the seconds the engine
                keeps publishing it (author, 2026-09-18: "done cues in the
                active cues list can be greyed/dimmed"), so what is still
                sounding stands out from what has just stopped without
                anybody reading the state word. A failure is not dimmed: it
                is the row somebody needs to read. */
            const auto done = owner.rows[static_cast<std::size_t> (at)].state == "done";

            if (done)
                g.beginTransparencyLayer (0.4f);

            owner.paintRow (at, g, getWidth(), height);

            if (done)
                g.endTransparencyLayer();
        }
    }

    void RunPaneComponent::Canvas::mouseDown (const juce::MouseEvent& event)
    {
        owner.pressed (event);
    }

    void RunPaneComponent::Canvas::mouseDrag (const juce::MouseEvent& event)
    {
        owner.dragged (event);
    }

    void RunPaneComponent::Canvas::mouseUp (const juce::MouseEvent& event)
    {
        owner.released (event);
    }

    void RunPaneComponent::Canvas::mouseMove (const juce::MouseEvent& event)
    {
        owner.hovered (event);
    }

    void RunPaneComponent::Canvas::mouseExit (const juce::MouseEvent&)
    {
        owner.unhovered();
    }

    bool RunPaneComponent::overCross (int x) const
    {
        const auto unit = juce::roundToInt (theme.type * 7.0);
        return x >= canvas.getWidth() - unit * 3;
    }

    void RunPaneComponent::hovered (const juce::MouseEvent& event)
    {
        const auto index = rowAt (event.y);
        const auto over = index >= 0 && overCross (event.x)
                            ? rows[static_cast<std::size_t> (index)].id
                            : std::string {};

        /*  AND WHETHER THE HEAD UNDER THE POINTER CAN BE TAKEN, said with
            the cursor before the press: a strip that scrubs shows the
            left-right arrows, so nobody finds out by dragging a row that
            does nothing. The strip and not the whole row, since the cross
            at its right end is the other gesture. */
        std::string strip;

        if (index >= 0 && over.empty())
        {
            const auto& entry = rows[static_cast<std::size_t> (index)];

            if (scrubbable (entry)
                 && stripFor (entry, canvas.getWidth(), heightOf (entry))
                        .translated (0, topOf (index)).contains (event.getPosition()))
                strip = entry.id;
        }

        if (over == hoverKill && strip == hoverScrub)
            return;

        hoverKill = over;
        hoverScrub = strip;
        canvas.setMouseCursor (! hoverKill.empty() ? juce::MouseCursor::PointingHandCursor
                               : ! hoverScrub.empty() ? juce::MouseCursor::LeftRightResizeCursor
                                                      : juce::MouseCursor::NormalCursor);
        canvas.repaint();
    }

    void RunPaneComponent::unhovered()
    {
        if (hoverKill.empty() && hoverScrub.empty())
            return;

        hoverKill.clear();
        hoverScrub.clear();

        if (! scrub.active())
            canvas.setMouseCursor (juce::MouseCursor::NormalCursor);

        canvas.repaint();
    }

    bool RunPaneComponent::wouldStop (const model::RunRow& entry) const
    {
        if (hoverKill.empty())
            return false;

        /*  Up the parent chain to the hovered run, or to the top. A dozen
            rows, so a walk per row per repaint is nothing; bounded by the row
            count so a cycle the engine would never publish cannot hang it. */
        const auto* at = &entry;

        for (std::size_t steps = 0; at != nullptr && steps <= rows.size(); ++steps)
        {
            if (at->id == hoverKill)
                return true;

            const auto parent = at->parentRun;
            at = nullptr;

            if (parent.empty())
                break;

            for (const auto& row : rows)
                if (row.id == parent)
                {
                    at = &row;
                    break;
                }
        }

        return false;
    }

    bool RunPaneComponent::hasWaveform (const model::RunRow& entry) const
    {
        /*  ARMED IS NOT SOUNDING (author, 2026-09-18: "the audio files are also
            showing when setting the standby cue"). Parking the pointer on a
            media cue arms it, which reserves a voice and maps the file so that
            GO is instant - and that reservation is a run, which is right and is
            why it shows at all. What it is not is a cue making a noise, and a
            picture of the whole file under a row that is merely READY says
            more than the row means. It gets the cursor instead, which costs no
            height, and the picture arrives when the sound does. */
        /*  AND DONE IS NOT SOUNDING EITHER (author, 2026-09-18: "hide the
            waveform right away and only keep the name on a thinner strip like
            regular cues"): the picture goes the tick the sound does, and the
            row drops to one line for the seconds it is still shown. */
        if (media == nullptr || entry.kind != "media" || entry.file.empty()
              || ! entry.launched() || entry.state == "done")
        {
            return false;
        }

        const auto found = media->find (entry.file);

        return found != media->end() && found->second.pyramid != nullptr;
    }

    double RunPaneComponent::lengthOf (const model::RunRow& entry) const
    {
        if (entry.length > 0.0)
            return entry.length;

        if (media == nullptr || entry.file.empty())
            return 0.0;

        const auto found = media->find (entry.file);

        if (found == media->end())
            return 0.0;

        if (found->second.seconds > 0.0)
            return found->second.seconds;

        //  Failing that, the pyramid knows how many samples it saw.
        if (found->second.pyramid != nullptr && found->second.pyramid->sampleRate > 0)
            return static_cast<double> (found->second.pyramid->samples)
                     / static_cast<double> (found->second.pyramid->sampleRate);

        return 0.0;
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

        const auto length = lengthOf (entry);

        if (! (length > 0.0) || ! entry.launched())
            return;

        const auto through = model::playhead (entry.seconds, length);
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

        /*  ON THE ROW'S OWN GROUND, with no fill of its own (author,
            2026-09-18: "the waveform background can be transparent showing the
            cell colour"). What separates the picture from the panel is the
            black trace along its edge below - which is the one place in this
            window a colour is not a token, because it is doing a job rather
            than saying anything. */
        const auto middle = strip.getCentreY();
        const auto half = strip.getHeight() / 2.0;

        const auto count = static_cast<int> (columns.size());

        /*  HOW FAR THIS COLUMN REACHES from the middle, in whole pixels,
            because the outline below has to land on the same edge the fill
            does - and a fill drawn in floats and an edge drawn in integers
            would disagree by a fraction all the way along. */
        const auto reach = [&columns, half] (int at)
        {
            return juce::jmax (1, juce::roundToInt (columns[static_cast<std::size_t> (at)].peak
                                                      * half));
        };

        /*  ONE LINE PER COLUMN, mirrored about the middle so it reads as a
            waveform rather than as a bar chart, and coloured by what that
            slice of the file SOUNDS like (§3.30). A minimum of one pixel, so
            silence is a line rather than a gap: a file that has not started is
            not a file that is missing. */
        for (auto at = 0; at < count; ++at)
        {
            const auto& column = columns[static_cast<std::size_t> (at)];

            g.setColour (juce::Colour::fromHSV (static_cast<float> (column.hue / 360.0),
                                                static_cast<float> (column.saturation),
                                                static_cast<float> (0.25 + column.lightness * 0.6),
                                                1.0f));

            const auto far = reach (at);

            g.fillRect (strip.getX() + at, middle - far, 1, far * 2);
        }

        /*  AND THE EDGE OF THE SHAPE, traced (author, 2026-09-18: "outline as a
            line not the whole background, just the edge the waveform shape").
            A box around the strip said where the picture was; this says what
            SHAPE it is, which is the thing an envelope is read for.

            Two pixels per column and a riser to its neighbour, so the line is
            continuous over a step rather than a row of dashes wherever the
            level changes quickly. */
        g.setColour (juce::Colours::black);

        for (auto at = 0; at < count; ++at)
        {
            const auto far = reach (at);
            const auto x = strip.getX() + at;

            g.fillRect (x, middle - far, 1, 1);
            g.fillRect (x, middle + far - 1, 1, 1);

            if (at == 0)
                continue;

            const auto before = reach (at - 1);
            const auto rise = juce::jmax (far, before) - juce::jmin (far, before);

            g.fillRect (x, middle - juce::jmax (far, before), 1, rise);
            g.fillRect (x, middle + juce::jmin (far, before), 1, rise);
        }

        /*  AND WHERE IT HAS GOT TO. Nought when the length is not known, which
            is a cue imported in this session - its duration arrives when the
            show is next opened - so the head sits at the left rather than
            sliding across a bar nobody has measured. */
        const auto length = lengthOf (entry);
        const auto through = model::playhead (entry.seconds, length);

        if (entry.launched() && length > 0.0)
        {
            /*  A HEAD THAT READS ON ANY COLOUR: two pixels of ink between one
                of black each side, since one pixel of ink over a yellow or a
                green frame vanished (author, 2026-09-18: "the cursor on the
                active cue waveform doesn't show or gets too slim to stay
                visible"). Black for the same reason the edge trace is: a job,
                not a token. */
            const auto x = strip.getX() + juce::roundToInt (through * (strip.getWidth() - 3));

            g.setColour (juce::Colours::black);
            g.fillRect (x, strip.getY(), 4, strip.getHeight());
            g.setColour (Look::colour (theme, "ink"));
            g.fillRect (x + 1, strip.getY(), 2, strip.getHeight());
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

        /*  A SCRUB IN PROGRESS RIDES THE PASS. The pointer against an edge of
            the window keeps the head sliding, and this - twenty-five times a
            second by the theme - is the clock it slides on; a run that has
            ended under the hand, or gone from the pane, lets the head go. */
        if (scrub.active())
        {
            const auto still = std::find_if (runs.begin(), runs.end(),
                                             [this] (const model::RunRow& row)
                                             { return row.id == scrubRun; });

            if (still == runs.end() || ! scrubbable (*still))
                endScrub();
            else if (scrubPush != 0)
            {
                scrub.push (scrubPush, 1.0 / juce::jmax (1.0, theme.refreshHz), scrubDistance);
                sendScrub (false);
            }
        }

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

        /*  The heights can change without the set changing - a media run's
            analysis arriving gives it a band - so the canvas is re-measured
            every pass. It is a sum over a dozen rows. */
        layOutRows();
        canvas.repaint();

        if (! sameRuns)
            repaint();          // the empty-pane sentence comes and goes with the set
    }

    void RunPaneComponent::paintRow (int index, juce::Graphics& g, int width, int height)
    {
        if (index < 0 || index >= static_cast<int> (rows.size()))
            return;

        const auto& entry = rows[static_cast<std::size_t> (index)];

        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;

        g.fillAll (Look::colour (theme, "panel-runs"));

        /*  WHERE THE STRIP GOES, and there are two answers. A WAVEFORM takes
            a band of its own under the words, which is what the taller row
            exists for. Anything else lies BEHIND them across the whole row -
            drawn before them, so the name reads over it - and costs no height.

            Both stop short of the kill cross, so nothing runs under the one
            control in this pane. */
        const auto layered = ! hasWaveform (entry);
        const auto strip = stripFor (entry, width, height);

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
            if (state == "done")     return Look::colour (theme, "ink-off");

            return Look::colour (theme, "waiting");
        };

        /*  A FADE IS NEITHER A SOUND NOR A WAIT and carries its own colour
            wherever it appears, because it is the one kind that changes
            something already sounding rather than starting or ending anything. */
        const auto tint = entry.kind == "fade" ? Look::colour (theme, "fade")
                                               : colourFor (entry.state);

        paintStrip (entry, g, strip, tint, layered);

        if (scrub.active() && entry.id == scrubRun)
            paintScrub (entry, g, strip);

        /*  WHAT THE HOVERED CROSS WOULD STOP is washed in the stopping colour
            and outlined, this row and every descendant: the answer to "what
            happens if I press this" given before the press, in a shape as
            well as a colour (§4.8). */
        const auto marked = wouldStop (entry);

        if (marked)
        {
            const auto stopping = Look::colour (theme, "stopping");
            g.setColour (stopping.withAlpha (0.18f));
            g.fillRect (0, 0, width, height);
            g.setColour (stopping);
            g.drawRect (0, 0, width, height, 1);
        }

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
        g.setColour (Look::colour (theme, entry.id == hoverKill ? "stopping"
                                           : marked ? "ink" : "ink-off"));
        g.setFont (Look::font (theme, 16.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")),
                    killCell, juce::Justification::centred, false);

        //  Where it has got to, when it has been let go.
        auto positionCell = area.removeFromRight (unit * 5);
        g.setColour (Look::colour (theme, "ink-faint"));
        g.setFont (Look::font (theme, 12.0f));
        g.drawText (entry.position, positionCell, juce::Justification::centredRight, false);

        /*  The mark, or the word for every state that is not one of the two.

            FITTED, because "preparing" did not fit and came out as "prepari..."
            - and a truncated state is the one thing on this row somebody reads
            to know what is happening (author's screenshot, 2026-09-18). A mark
            is a glyph and never needs it; only the words do. */
        auto stateCell = area.removeFromLeft (unit * 6);
        g.setColour (tint);
        g.setFont (Look::font (theme, entry.mark().empty() ? 11.0f : 13.0f));

        if (entry.mark().empty())
            g.drawFittedText (juce::String (entry.state), stateCell,
                              juce::Justification::centredLeft, 1, 0.6f);
        else
            g.drawText (juce::String (juce::CharPointer_UTF8 (entry.mark().c_str())),
                        stateCell, juce::Justification::centredLeft, false);

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

    void RunPaneComponent::clicked (const juce::MouseEvent& event)
    {
        const auto index = rowAt (event.y);

        if (index < 0 || ! actions.kill)
            return;

        /*  THE KILL, at the right edge where the cross is drawn. Only a click
            on the cross sends it: a run stopped by a click that landed
            anywhere on the row is a cue an operator did not mean to stop. */
        const auto unit = juce::roundToInt (theme.type * 7.0);

        if (event.x >= canvas.getWidth() - unit * 3)
            actions.kill (rows[static_cast<std::size_t> (index)].id);
    }

    juce::Rectangle<int> RunPaneComponent::stripFor (const model::RunRow& entry, int width,
                                                     int height) const
    {
        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;

        /*  WHERE THE STRIP GOES, and there are two answers. A WAVEFORM takes
            a band of its own under the words, which is what the taller row
            exists for. Anything else lies BEHIND them across the whole row -
            drawn before them, so the name reads over it - and costs no height.

            Both stop short of the kill cross, so nothing runs under the one
            control in this pane. One function for the painter and the hit
            test, so a scrub lands on the pixels the head is drawn over. */
        if (! hasWaveform (entry))
            return juce::Rectangle<int> (0, 0, width, height).reduced (pad, 1)
                       .withTrimmedRight (unit * 3);

        return juce::Rectangle<int> (0, height - stripHeight(), width, stripHeight())
                   .reduced (pad + unit, 1);
    }

    //==============================================================================
    bool RunPaneComponent::scrubbable (const model::RunRow& entry) const
    {
        /*  A SOUND WITH A KNOWN LENGTH, OR A SCENE THE ENGINE CAN TIME. A
            media run in its pre-wait, or armed and not let go, has no head
            to move; a fade, a wait or a message has no material; a manual
            sequence has an operator between its members and no second to
            seek to. What is left is what `run.seek` accepts. */
        if (entry.state != "playing")
            return false;

        if (entry.kind == "media")
            return entry.launched() && lengthOf (entry) > 0.0;

        return entry.kind == "group" && entry.timedGroup;
    }

    double RunPaneComponent::extentOf (const model::RunRow& entry) const
    {
        /*  HOW LONG A SCENE IS, FOR THE GEARING: the longest thing it is
            playing, since a scene has no file of its own to measure. A minute
            when nothing under it is known, which is a scale rather than a
            claim - the head is not clamped to it. */
        auto longest = 0.0;

        for (const auto& row : rows)
        {
            const auto* at = &row;

            for (std::size_t steps = 0; at != nullptr && steps <= rows.size(); ++steps)
            {
                if (at->parentRun.empty())
                {
                    at = nullptr;
                    break;
                }

                if (at->parentRun == entry.id)
                    break;

                const auto parent = at->parentRun;
                at = nullptr;

                for (const auto& candidate : rows)
                    if (candidate.id == parent)
                    {
                        at = &candidate;
                        break;
                    }
            }

            if (at != nullptr)
                longest = juce::jmax (longest, lengthOf (row));
        }

        return longest > 0.0 ? longest : 60.0;
    }

    double RunPaneComponent::secondsPerPixel (const model::RunRow& entry, int stripWidth) const
    {
        const auto span = entry.kind == "media" ? lengthOf (entry) : extentOf (entry);
        return span / static_cast<double> (juce::jmax (1, stripWidth - 1));
    }

    void RunPaneComponent::pressed (const juce::MouseEvent& event)
    {
        const auto index = rowAt (event.y);

        if (index < 0 || overCross (event.x) || ! actions.seek)
            return;

        const auto& entry = rows[static_cast<std::size_t> (index)];
        const auto strip = stripFor (entry, canvas.getWidth(), heightOf (entry))
                               .translated (0, topOf (index));

        if (! scrubbable (entry) || ! strip.contains (event.getPosition()))
            return;

        /*  THE HAND TAKES THE HEAD WHERE IT IS, not where the pointer landed:
            a press is a grab, and a grab that jumped the sound to the pixel
            under the finger would be a seek nobody meant. From here every
            pixel of travel moves it. The gearing halves every strip height
            above or below the strip - a band for a waveform, a line for a
            row - so the same reach means the same precision on both. */
        model::Scrub::Setup setup;
        setup.position = entry.seconds;
        setup.length = entry.kind == "media" ? lengthOf (entry) : 0.0;
        setup.secondsPerPixel = secondsPerPixel (entry, strip.getWidth());
        setup.unit = hasWaveform (entry) ? stripHeight() : rowHeight();
        setup.x = event.position.x;

        scrub.begin (setup);
        scrubRun = entry.id;
        scrubStrip = strip;
        scrubDistance = 0.0;
        scrubPush = 0;

        canvas.setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        canvas.repaint();
    }

    void RunPaneComponent::dragged (const juce::MouseEvent& event)
    {
        if (! scrub.active())
            return;

        /*  ABOVE OR BELOW THE STRIP, in pixels, is the gearing; inside it is
            nought and 1:1. Measured from the strip the head was taken from,
            not from whatever row the pointer is over now. */
        const auto y = event.position.y;
        scrubDistance = juce::jmax (0.0f,
                                    static_cast<float> (scrubStrip.getY()) - y,
                                    y - static_cast<float> (scrubStrip.getBottom()));

        scrub.moveTo (event.position.x, scrubDistance);

        /*  AGAINST THE WINDOW'S EDGE, the head keeps sliding that way until
            the pointer comes back. The WINDOW's edge and not the pane's: the
            pane's left edge is the middle of the window, and a drag that
            crossed it would be a hand reaching for coarse travel, not one
            asking for more. */
        scrubPush = 0;

        if (auto* top = getTopLevelComponent())
        {
            const auto inWindow = event.getEventRelativeTo (top).position.x;

            if (inWindow <= 0.0f)
                scrubPush = -1;
            else if (inWindow >= static_cast<float> (top->getWidth() - 1))
                scrubPush = 1;
        }

        sendScrub (false);
        canvas.repaint();
    }

    void RunPaneComponent::released (const juce::MouseEvent& event)
    {
        if (! scrub.active())
        {
            clicked (event);
            return;
        }

        /*  Letting go sends where the head settled - once, whether or not the
            throttle would have - and a press that never moved sends nothing:
            a grab is not a seek. */
        sendScrub (true);
        endScrub();
    }

    void RunPaneComponent::sendScrub (bool letGo)
    {
        if (! scrub.active() || ! actions.seek)
            return;

        const auto send = letGo ? scrub.settle()
                                : scrub.due (juce::Time::getMillisecondCounterHiRes());

        if (send)
            actions.seek (scrubRun, scrub.target());
    }

    void RunPaneComponent::endScrub()
    {
        scrub.end();
        scrubRun.clear();
        scrubPush = 0;
        canvas.setMouseCursor (! hoverScrub.empty() ? juce::MouseCursor::LeftRightResizeCursor
                                                    : juce::MouseCursor::NormalCursor);
        canvas.repaint();
    }

    void RunPaneComponent::paintScrub (const model::RunRow& entry, juce::Graphics& g,
                                       juce::Rectangle<int> strip)
    {
        /*  THE GHOST HEAD: where the hand has put it, drawn in the picked
            colour between two pixels of black so it reads over any frame of
            the waveform, beside the engine's own head, which stays where the
            sound is until the seek lands. Its clock and gearing sit in a
            small box next to it, on the strip's own ground, so a long track
            can be read to the tenth while it is being scrubbed. */
        const auto target = scrub.target();
        const auto span = entry.kind == "media" ? lengthOf (entry) : extentOf (entry);
        const auto through = span > 0.0 ? juce::jlimit (0.0, 1.0, target / span) : 0.0;
        const auto x = strip.getX() + juce::roundToInt (through * (strip.getWidth() - 3));

        const auto picked = Look::colour (theme, "picked");

        g.setColour (juce::Colours::black);
        g.fillRect (x, strip.getY(), 4, strip.getHeight());
        g.setColour (picked);
        g.fillRect (x + 1, strip.getY(), 2, strip.getHeight());

        auto words = juce::String (model::clockText (target));
        const auto gearing = model::rateText (scrub.rate());

        if (! gearing.empty())
            words += "  " + juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) + juce::String (gearing);

        g.setFont (Look::font (theme, 12.0f));

        const auto textWidth = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), words) + 8;
        const auto boxHeight = juce::jmin (strip.getHeight(), 16);
        auto box = juce::Rectangle<int> (x + 6, strip.getY(), textWidth, boxHeight);

        if (box.getRight() > strip.getRight())
            box.setX (juce::jmax (strip.getX(), x - 6 - textWidth));

        /*  A ROW WITH NO BAND has its words where the box would go, so the
            box sits at the strip's right end instead, beside the position
            the engine publishes - the two numbers a scrubbing hand compares. */
        if (! hasWaveform (entry))
        {
            const auto unit = juce::roundToInt (theme.type * 7.0);
            box = juce::Rectangle<int> (strip.getRight() - unit * 5 - 4 - textWidth, strip.getY(),
                                        textWidth, strip.getHeight());
        }

        g.setColour (Look::colour (theme, "panel-runs").withAlpha (0.85f));
        g.fillRect (box);
        g.setColour (picked);
        g.drawText (words, box, juce::Justification::centred, false);
    }

    void RunPaneComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-runs"));

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
        viewport.setBounds (getLocalBounds());
        layOutRows();
    }
}
