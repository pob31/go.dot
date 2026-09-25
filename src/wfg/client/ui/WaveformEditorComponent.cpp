/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/client/ui/WaveformEditorComponent.h>

#include <wfg/client/ui/Look.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        /*  How near the pointer has to be to an edge to take hold of it, in
            pixels. Generous, because an in-point is one pixel wide and a hand
            on a trackpad is not. */
        constexpr int grabRadius = 7;

        /** A time, as a ruler writes it: 1.5 s, or 1:04.2 once there are minutes. */
        juce::String clockText (double seconds)
        {
            const auto whole = static_cast<int> (std::floor (std::abs (seconds)));
            const auto tenths = static_cast<int> (std::floor ((std::abs (seconds) - whole) * 10.0));
            const auto sign = seconds < 0.0 ? "-" : "";

            if (whole < 60)
                return juce::String (sign) + juce::String (whole) + "." + juce::String (tenths);

            return juce::String (sign) + juce::String (whole / 60) + ":"
                     + juce::String (whole % 60).paddedLeft ('0', 2) + "."
                     + juce::String (tenths);
        }

        /*  A step that gives a readable number of marks across the window, out
            of the divisions a person actually counts in: tenths, halves,
            seconds, then the usual clock steps. */
        double rulerStep (double span)
        {
            static const double steps[] { 0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0,
                                          10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 };

            for (const auto step : steps)
                if (span / step <= 12.0)
                    return step;

            return 900.0;
        }
    }

    WaveformEditorComponent::WaveformEditorComponent (const model::Theme& themeToUse,
                                                      Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        setWantsKeyboardFocus (false);

        RangeTableComponent::Actions typed;
        typed.set = actions.set;
        typed.say = actions.say;
        typed.createRange = actions.createRange;
        typed.removeRange = actions.removeRange;
        typed.splitRange = actions.splitRange;

        table = std::make_unique<RangeTableComponent> (theme, std::move (typed));
        addAndMakeVisible (*table);

        /*  PLAY AND PAUSE, so a region can be HEARD where it is being placed
            (author, 2026-09-21: *"we need to play/pause the clip for
            testing"*). It fires the cue itself rather than auditioning the
            file on the side: what somebody is checking is what the show will
            do, and a preview through a different path would answer a question
            nobody asked.

            PAUSE IS A STOP THAT REMEMBERS WHERE IT WAS. The engine has no
            per-run pause - `audio.connection` pauses the whole graph for a
            lost interface and that is a different thing - so this kills the
            run and keeps its position as the point, and play starts again
            from there. The difference from a true pause is audible only in
            the tail, which a media cue being auditioned does not have. */
        transport.setWantsKeyboardFocus (false);
        transport.onClick = [this]
        {
            if (reading.running && ! reading.runId.empty())
            {
                point = reading.position;
                askedToPlay = false;

                if (actions.stop)
                    actions.stop (reading.runId);

                return;
            }

            if (reading.cueKind != "media" || actions.play == nullptr)
                return;

            askedToPlay = true;
            playingRun = reading.runId;
            actions.play (reading.subject.objectId);
        };

        addAndMakeVisible (transport);
        sayWhichWayTheTransportGoes();
    }

    /*  WHAT THE BUTTON DOES NEXT, on the button - a triangle to start it and
        two bars to hold it. A SHAPE and not a colour (4.8), and the tooltip
        says it in words for anyone who reads the glyph the other way. */
    void WaveformEditorComponent::sayWhichWayTheTransportGoes()
    {
        const auto sounding = reading.running && ! reading.runId.empty();

        transport.setButtonText (juce::String::fromUTF8 (sounding ? "\xe2\x96\x8e\xe2\x96\x8e"
                                                                  : "\xe2\x96\xb6"));
        transport.setTooltip (sounding ? "Stop, and keep the playhead where it is"
                                       : "Play this cue from the playhead");
        transport.setEnabled (sounding || reading.cueKind == "media");
    }

    void WaveformEditorComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        if (table != nullptr)
            table->applyTheme (theme);

        repaint();
    }

    void WaveformEditorComponent::show (const model::FootReading& readingToUse,
                                        std::shared_ptr<const audio::MediaRecords> mediaToUse)
    {
        const auto wasFile = reading.file;
        const auto wasRanges = reading.ranges.size();

        reading = readingToUse;
        media = std::move (mediaToUse);

        /*  A NEW FILE IS A NEW VIEW. Keeping the old window would open the next
            cue zoomed into a second of it that means nothing there, which is
            the panel showing a reading about one file over the picture of
            another. */
        if (viewFile != reading.file)
        {
            viewFile = reading.file;
            view.reset (reading.fileLength);
            hover = {};
            grabbed = {};
        }
        else if (! juce::approximatelyEqual (view.length, reading.fileLength))
        {
            /*  The length arrives late for a file imported in this session, so
                a view built against nought is rebuilt when it turns up. */
            view.reset (reading.fileLength);
        }

        /*  THE TABLE IS HANDED THE SAME READING, never its own: two halves of
            one editor drawn from two moments would show an out-point in the
            box that the bar has already moved. */
        /*  THE SEEK THAT WAS WAITING FOR A RUN TO EXIST. Play was asked for
            from a point somewhere in the file; `cue.fire` made a run and it
            has just turned up, so this is where it is told to start there. */
        if (askedToPlay && reading.running && ! reading.runId.empty()
              && reading.runId != playingRun)
        {
            askedToPlay = false;
            playingRun = reading.runId;

            if (actions.seek != nullptr && point > 0.0)
                actions.seek (reading.runId, point);
        }

        if (! reading.running)
            playingRun.clear();

        sayWhichWayTheTransportGoes();

        if (table != nullptr)
        {
            table->setVisible (reading.notice.empty());
            table->show (reading);
            table->setPlayhead (headSeconds());
        }

        if (wasFile != reading.file || wasRanges != reading.ranges.size())
            resized();   // the table's width can change with what is in it

        repaint();       // the playhead moves every pass; the bar is cached below
    }

    void WaveformEditorComponent::setRightColumn (int width, int gap)
    {
        if (columnWidth == width && columnGap == gap)
            return;

        columnWidth = width;
        columnGap = gap;
        resized();
    }

    int WaveformEditorComponent::tableWidth() const
    {
        if (table == nullptr || ! table->isVisible())
            return 0;

        //  Never more than half: the picture is what this panel is for.
        return juce::jmin (columnWidth > 0 ? columnWidth : table->wantedWidth(), getWidth() / 2);
    }

    juce::Rectangle<int> WaveformEditorComponent::pictureArea() const
    {
        auto area = getLocalBounds();
        area.removeFromRight (tableWidth() + columnGap);
        return area;
    }

    juce::Rectangle<int> WaveformEditorComponent::rulerArea() const
    {
        const auto row = juce::roundToInt (theme.row * theme.type * 0.7);
        return pictureArea().removeFromBottom (row);
    }

    juce::Rectangle<int> WaveformEditorComponent::headArea() const
    {
        const auto row = juce::roundToInt (theme.row * theme.type * 0.8);
        return pictureArea().removeFromTop (row);
    }

    juce::Rectangle<int> WaveformEditorComponent::barArea() const
    {
        auto area = pictureArea();
        area.removeFromTop (headArea().getHeight());
        area.removeFromBottom (rulerArea().getHeight());
        return area.reduced (2, 4);
    }

    double WaveformEditorComponent::headSeconds() const
    {
        return reading.running ? reading.position : point;
    }

    void WaveformEditorComponent::moveHeadTo (int x, bool letGo)
    {
        const auto bar = barArea();

        point = std::min (std::max (secondsAt (x), 0.0),
                          reading.fileLength > 0.0 ? reading.fileLength : secondsAt (bar.getRight()));

        if (table != nullptr)
            table->setPlayhead (headSeconds());

        repaint();

        if (! reading.running || reading.runId.empty() || actions.seek == nullptr)
            return;

        /*  ONE PER POSITION THE HAND SETTLES ON, and one when it lets go. A
            record per pixel would be a hundred seeks a second down the log and
            a hundred voices stopped and re-asked. */
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (! letGo && (now - sentAtMs < 200.0
                          || juce::approximatelyEqual (point, sentSeek)))
        {
            return;
        }

        sentSeek = point;
        sentAtMs = now;
        actions.seek (reading.runId, point);
    }

    double WaveformEditorComponent::secondsAt (int x) const
    {
        const auto bar = barArea();
        return view.secondsForX (x - bar.getX(), bar.getWidth());
    }

    double WaveformEditorComponent::toleranceSeconds() const
    {
        const auto bar = barArea();

        if (bar.getWidth() <= 0 || ! (view.span() > 0.0))
            return 0.0;

        return view.span() * grabRadius / bar.getWidth();
    }

    const std::vector<model::Column>& WaveformEditorComponent::columns()
    {
        const auto bar = barArea();

        if (barsFile == reading.file && barsWidth == bar.getWidth()
              && juce::approximatelyEqual (barsFrom, view.from)
              && juce::approximatelyEqual (barsTo, view.to))
            return bars;

        bars.clear();
        barsFile = reading.file;
        barsWidth = bar.getWidth();
        barsFrom = view.from;
        barsTo = view.to;

        if (media == nullptr || reading.file.empty() || bar.getWidth() <= 0)
            return bars;

        const auto& analysed = *media;
        const auto found = analysed.find (reading.file);

        if (found == analysed.end() || found->second.pyramid == nullptr)
            return bars;

        bars = model::waveform (*found->second.pyramid, found->second.peaks.get(), bar.getWidth(),
                                view.from, view.to);
        return bars;
    }

    void WaveformEditorComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-runs"));

        if (! reading.notice.empty())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawFittedText (juce::String (reading.notice), pictureArea().reduced (12, 6),
                              juce::Justification::centred, 3);
            return;
        }

        const auto bar = barArea();

        paintBar (g, bar);
        paintRanges (g, bar);
        paintRuler (g, rulerArea());
        paintHead (g, headArea());
    }

    void WaveformEditorComponent::paintBar (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        const auto& drawn = columns();

        if (drawn.empty())
        {
            /*  NOT ANALYSED YET is a different thing from silence, and the
                engine's own answer for it is no shape at all (§3.30). */
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawFittedText ("waiting for the analysis of this file", bar,
                              juce::Justification::centred, 1);
            return;
        }

        const auto middle = bar.getCentreY();
        const auto half = bar.getHeight() / 2.0;
        const auto count = static_cast<int> (drawn.size());

        /*  The running pane's picture, given room: one line per column from
            its lowest sample to its highest - the wave's own shape where the
            finer level is there (2026-09-25: "more precise in level, not
            colour, when zooming in"), the peak mirrored about the middle where
            it is not - coloured by what that slice sounds like. A pixel at
            least, so silence is a line and not a gap. */
        for (auto at = 0; at < count; ++at)
        {
            const auto& column = drawn[static_cast<std::size_t> (at)];

            g.setColour (juce::Colour::fromHSV (static_cast<float> (column.hue / 360.0),
                                                static_cast<float> (column.saturation),
                                                static_cast<float> (0.25 + column.lightness * 0.6),
                                                1.0f));

            const auto top = middle - juce::roundToInt (column.high * half);
            const auto bottom = middle - juce::roundToInt (column.low * half);

            g.fillRect (bar.getX() + at, juce::jmin (top, bottom), 1, juce::jmax (1, std::abs (bottom - top)));
        }
    }

    void WaveformEditorComponent::paintRanges (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        const auto xOf = [this, bar] (double seconds)
        {
            return bar.getX() + juce::roundToInt (view.xForSeconds (seconds, bar.getWidth()));
        };

        /*  THE START OFFSET, where a cue with no ranges begins. Drawn even when
            it is nought, because "the top of the file" is a decision somebody
            can move and a marker that appears only once it is non-zero is one
            nobody discovers. */
        if (reading.ranges.empty())
        {
            const auto x = xOf (reading.startOffset);

            if (bar.contains (x, bar.getCentreY()))
            {
                g.setColour (Look::colour (theme, "standby"));
                g.fillRect (x, bar.getY(), 2, bar.getHeight());
            }
        }

        for (std::size_t at = 0; at < reading.ranges.size(); ++at)
        {
            const auto& range = reading.ranges[at];
            const auto a = xOf (range.in);
            const auto b = xOf (range.out);

            if (b < bar.getX() || a > bar.getRight())
                continue;

            const auto band = juce::Rectangle<int> (a, bar.getY(), juce::jmax (1, b - a),
                                                    bar.getHeight()).getIntersection (bar);

            //  A wash, so the picture under it still reads.
            g.setColour (Look::colour (theme, "picked").withAlpha (0.12f));
            g.fillRect (band);

            /*  THE JOIN IS DRAWN ONCE AND DIFFERENTLY. Where one range ends and
                the next begins, the two edges are the same instant and moving
                one alone would tear the loop - so it is one mark, in the colour
                of the thing it is, and it is grabbed as one. */
            const auto joined = at + 1 < reading.ranges.size()
                                  && model::sameInstant (range.out, reading.ranges[at + 1].in);

            const auto edge = [&] (int x, const char* token, int thickness)
            {
                if (x < bar.getX() - 2 || x > bar.getRight() + 2)
                    return;

                g.setColour (Look::colour (theme, token));
                g.fillRect (x - thickness / 2, bar.getY(), thickness, bar.getHeight());
            };

            const auto hot = [this] (const model::Hit& h, const std::string& id, model::Handle kind)
            {
                return h.handle == kind && h.rangeId == id;
            };

            const auto live = grabbed.handle != model::Handle::none ? grabbed : hover;

            edge (a, hot (live, range.id, model::Handle::in) ? "ink" : "live", 2);

            if (joined)
                edge (b, live.handle == model::Handle::slice && live.rangeId == range.id
                           ? "ink" : "standby", 3);
            else
                edge (b, hot (live, range.id, model::Handle::out) ? "ink" : "stopping", 2);

            //  The name and the loop count, where there is room for them.
            if (band.getWidth() > 46)
            {
                g.setColour (Look::colour (theme, "ink-dim"));
                g.setFont (Look::font (theme, 11.0f));

                const auto label = juce::String (range.name.empty()
                                                   ? "range " + std::to_string (range.index + 1)
                                                   : range.name)
                                     + (range.loops == 1 ? juce::String()
                                        : range.loops == 0
                                          ? " " + juce::String::fromUTF8 ("\xe2\x88\x9e")
                                          : " x" + juce::String (range.loops));

                g.drawText (label, band.reduced (4, 2), juce::Justification::topLeft, true);
            }
        }

        /*  AND THE PLAYHEAD. Where the sound IS when something is sounding
            this cue, and where it WOULD START otherwise - two different claims,
            so two different marks: solid ink for the one that is real, and a
            thinner line in the standby colour for the one that is an
            intention. Black either side of either, so it reads on any colour
            the analysis produced - the running pane's own trick. */
        {
            const auto x = xOf (headSeconds());

            if (x >= bar.getX() && x <= bar.getRight())
            {
                g.setColour (juce::Colours::black);
                g.fillRect (x - 2, bar.getY(), 4, bar.getHeight());
                g.setColour (Look::colour (theme, reading.running ? "ink" : "standby"));
                g.fillRect (x - 1, bar.getY(), reading.running ? 2 : 1, bar.getHeight());
            }
        }
    }

    /*  THE TRANSPORT'S OWN ROW: the button, and beside it where the head is
        and what it is doing. The clock is always written out, because the
        mark on the picture answers "roughly where" and placing an in-point
        wants "exactly when". */
    void WaveformEditorComponent::paintHead (juce::Graphics& g, juce::Rectangle<int> head)
    {
        auto area = head.withTrimmedLeft (head.getHeight() * 2 + 8);

        g.setFont (Look::font (theme, 12.0f));
        g.setColour (Look::colour (theme, reading.running ? "ink" : "ink-dim"));
        g.drawText (clockText (headSeconds()), area.removeFromLeft (70),
                    juce::Justification::centredLeft, false);

        g.setColour (Look::colour (theme, "ink-off"));
        g.setFont (Look::font (theme, 11.0f));

        g.drawText (reading.running ? "playing - drag the ruler to move the playhead"
                                     : "drag the ruler to place the playhead",
                    area, juce::Justification::centredLeft, true);
    }

    void WaveformEditorComponent::paintRuler (juce::Graphics& g, juce::Rectangle<int> ruler)
    {
        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (ruler.getX(), ruler.getY(), ruler.getWidth(), 1);

        if (! (view.span() > 0.0) || ruler.getWidth() <= 0)
            return;

        const auto step = rulerStep (view.span());
        const auto first = std::ceil (view.from / step) * step;

        g.setFont (Look::font (theme, 11.0f));

        for (auto seconds = first; seconds <= view.to; seconds += step)
        {
            const auto x = barArea().getX()
                             + juce::roundToInt (view.xForSeconds (seconds, barArea().getWidth()));

            g.setColour (Look::colour (theme, "rule"));
            g.fillRect (x, ruler.getY(), 1, 4);

            /*  TO THE RIGHT OF ITS MARK, AND TO THE LEFT AT THE END (author,
                2026-09-21: *"the last unit can be shift on the other side so
                it doesn't overflow in the range table"*). A figure is written
                after its tick everywhere it fits; the last one would run off
                the picture and under the numbers beside it, so it is written
                before its tick instead. The mark is what carries the instant
                either way - the text only names it. */
            const auto figure = clockText (seconds);
            const auto wide = juce::jmax (30, juce::GlyphArrangement::getStringWidthInt (
                                                  g.getCurrentFont(), figure) + 6);

            const auto after = x + 3 + wide <= ruler.getRight();

            g.setColour (Look::colour (theme, "ink-off"));
            g.drawText (figure,
                        juce::Rectangle<int> (after ? x + 3 : x - 3 - wide, ruler.getY(),
                                              wide, ruler.getHeight()),
                        after ? juce::Justification::centredLeft
                              : juce::Justification::centredRight,
                        false);
        }

        /*  AND WHETHER THIS IS THE WHOLE FILE OR A WINDOW INTO IT, said in
            words at the right: a zoomed bar and a whole one look identical
            otherwise, and somebody who has lost track of where they are should
            not have to count ruler marks to find out. */
        if (! view.isWholeThing())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.drawText (clockText (view.from) + " to " + clockText (view.to)
                          + "  " + juce::String::fromUTF8 ("\xc2\xb7") + "  double-click shows all",
                        ruler.reduced (6, 0), juce::Justification::centredRight, true);
        }
    }

    void WaveformEditorComponent::resized()
    {
        if (table != nullptr)
            table->setBounds (getLocalBounds().removeFromRight (tableWidth()));

        barsFrom = barsTo = 0.0;   // the window moved with the width

        barsWidth = 0;   // the bucketing is per width

        auto head = headArea();
        transport.setBounds (head.removeFromLeft (head.getHeight() * 2).reduced (2, 1));

        repaint();
    }

    void WaveformEditorComponent::mouseMove (const juce::MouseEvent& event)
    {
        if (rulerArea().contains (event.getPosition()))
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);

            if (hover.handle != model::Handle::none)
            {
                hover = {};
                repaint();
            }

            return;
        }

        if (reading.ranges.empty() || ! barArea().contains (event.getPosition()))
        {
            if (hover.handle != model::Handle::none)
            {
                hover = {};
                repaint();
            }

            return;
        }

        const auto found = model::hitTest (reading.ranges, secondsAt (event.x), toleranceSeconds());

        if (found.handle != hover.handle || found.rangeId != hover.rangeId)
        {
            hover = found;
            setMouseCursor (hover.handle == model::Handle::none
                              ? juce::MouseCursor::NormalCursor
                              : juce::MouseCursor::LeftRightResizeCursor);
            repaint();
        }
    }

    void WaveformEditorComponent::mouseDown (const juce::MouseEvent& event)
    {
        grabbed = {};
        panning = false;
        onRuler = false;

        /*  THE RULER IS THE PLAYHEAD'S STRIP (author, 2026-09-21: *"the
            playhead can be controlled in the time ruler"*). A press there puts
            the head where the pointer is and the drag carries it, which is
            what a time ruler does everywhere - and it keeps the BAR free for
            the in and out points, so neither gesture has to be aimed around
            the other. */
        if (rulerArea().contains (event.getPosition()))
        {
            onRuler = true;
            sentSeek = -1.0;
            moveHeadTo (event.x, false);
            return;
        }

        if (! barArea().contains (event.getPosition()))
            return;

        if (event.mods.isPopupMenu() || event.mods.isMiddleButtonDown())
        {
            panning = true;
            panFrom = event.x;
            return;
        }

        grabbed = model::hitTest (reading.ranges, secondsAt (event.x), toleranceSeconds());

        /*  A PRESS ON NOTHING PANS, which is the gesture people try first on a
            picture that is wider than its window. Grabbing an edge wins, so a
            hand aiming at a handle never scrolls the view out from under it. */
        if (grabbed.handle == model::Handle::none)
        {
            panning = true;
            panFrom = event.x;
        }
    }

    void WaveformEditorComponent::mouseDrag (const juce::MouseEvent& event)
    {
        if (onRuler)
        {
            moveHeadTo (event.x, false);
            return;
        }

        if (panning)
        {
            view.panBy (panFrom - event.x, barArea().getWidth());
            panFrom = event.x;
            repaint();
            return;
        }

        if (grabbed.handle == model::Handle::none || actions.set == nullptr)
            return;

        auto seconds = secondsAt (event.x);

        /*  SNAPPED TO WHAT IS ALREADY THERE unless the hand says otherwise -
            the other edges, both ends of the file - because a designer placing
            a loop point almost always means "where that one is". Alt lets go
            of the magnet, which is the same key the cue list uses to mean
            "not the ordinary reading of this drag". */
        if (! event.mods.isAltDown())
            seconds = model::snapTo (seconds,
                                     model::snapTargets (reading.ranges, grabbed.rangeId,
                                                         reading.fileLength),
                                     toleranceSeconds());

        const auto writes = model::dragTo (grabbed, seconds, reading.ranges, reading.fileLength);

        for (const auto& write : writes)
            actions.set (model::rangeAddress (write.rangeId, write.attribute),
                         osc::formatDouble (write.seconds));

        if (! writes.empty() && actions.say != nullptr)
            actions.say (juce::String (grabbed.handle == model::Handle::slice
                                         ? "loop point at " : "edge at ")
                           + clockText (writes.front().seconds));
    }

    void WaveformEditorComponent::mouseDoubleClick (const juce::MouseEvent&)
    {
        /*  BACK TO THE WHOLE FILE, which is the way out of a zoom that has got
            away from somebody. Deliberately NOT Escape: that key is PANIC in
            this window (PRD 4.4), and a panel that quietly took it for its own
            would swallow the one key an operator reaches for when a show is
            going wrong. */
        view.reset (reading.fileLength);
        grabbed = {};
        panning = false;
        repaint();
    }

    void WaveformEditorComponent::mouseUp (const juce::MouseEvent& event)
    {
        if (onRuler)
        {
            moveHeadTo (event.x, true);
            onRuler = false;
        }

        grabbed = {};
        panning = false;

        if (actions.say != nullptr)
            actions.say ({});

        repaint();
    }

    void WaveformEditorComponent::mouseExit (const juce::MouseEvent&)
    {
        if (hover.handle != model::Handle::none)
        {
            hover = {};
            repaint();
        }
    }

    void WaveformEditorComponent::mouseWheelMove (const juce::MouseEvent& event,
                                                  const juce::MouseWheelDetails& wheel)
    {
        if (! (reading.fileLength > 0.0))
            return;

        const auto bar = barArea();

        /*  TWO AXES, TWO JOBS (author, 2026-09-21: *"vertical scroll = zoom,
            horizontal scroll = scroll back and forth the view of the
            waveform"*). A trackpad sends both at once, so the larger movement
            wins rather than the picture doing both things at a slant. */
        if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        {
            /*  A WHEEL'S WORTH OF TRAVEL IS A QUARTER OF THE WINDOW, so the
                picture moves by an amount somebody can follow at any zoom -
                a fixed number of pixels would crawl when zoomed out and fly
                when zoomed in. */
            view.panBy (juce::roundToInt (-wheel.deltaX * bar.getWidth() * 0.25),
                        bar.getWidth());
            repaint();
            return;
        }

        if (juce::approximatelyEqual (wheel.deltaY, 0.0f))
            return;

        /*  ABOUT THE POINTER, so the second under it stays under it: that is
            what makes a wheel feel like magnifying the picture rather than
            scrolling it. `model/View` holds the arithmetic and the test. */
        view.zoomAbout (event.x - bar.getX(), bar.getWidth(),
                        wheel.deltaY > 0 ? 0.85 : 1.0 / 0.85);
        repaint();
    }
}
