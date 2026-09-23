/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/client/ui/FootPanelComponent.h>
#include <wfg/client/ui/InspectorComponent.h>
#include <wfg/client/ui/SendMixerComponent.h>
#include <wfg/client/ui/RangeTableComponent.h>
#include <wfg/client/ui/RunPaneComponent.h>
#include <wfg/client/ui/SurfacePanelComponent.h>

#include <wfg/client/model/Fader.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/Surfaces.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace wfg::client;

namespace
{
    /*  EVERY BUTTON UNDER A COMPONENT, however deep. The table's rows live
        inside a viewport inside the panel, so a test that only looked at the
        direct children would find nothing and pass by accident. */
    void gatherButtons (juce::Component& from, std::vector<juce::Button*>& into)
    {
        for (auto* child : from.getChildren())
        {
            if (auto* button = dynamic_cast<juce::Button*> (child))
                into.push_back (button);

            gatherButtons (*child, into);
        }
    }

    std::vector<juce::Button*> buttonsUnder (juce::Component& from)
    {
        std::vector<juce::Button*> found;
        gatherButtons (from, found);
        return found;
    }

    juce::Button* buttonTipped (juce::Component& from, const juce::String& startsWith)
    {
        for (auto* button : buttonsUnder (from))
            if (auto* tips = dynamic_cast<juce::SettableTooltipClient*> (button))
                if (tips->getTooltip().startsWith (startsWith))
                    return button;

        return nullptr;
    }
}


TEST_CASE ("foot panel: it opens on one subject, draws a file, and a drag writes the range")
{
    /*  THE HOST AND ONE EDITOR, which is the author's shape for this panel
        (2026-09-21): it opens on a named subject rather than on a tab bar. A
        component test is where a construction fault shows as a stack rather
        than as a window that is not there. */
    std::vector<std::pair<std::string, std::string>> written;

    ui::FootPanelComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& value)
    { written.emplace_back (address, value); };

    auto closed = false;
    actions.close = [&] { closed = true; };

    auto grown = 0;
    actions.resizeBy = [&] (int pixels) { grown += pixels; };

    ui::FootPanelComponent panel (model::Theme {}, actions);
    panel.setSize (900, 220);

    //  Shut to begin with, and nothing drawn.
    CHECK_FALSE (panel.subject().isOpen());

    panel.open ({ model::Subject::Kind::waveform, "CUE00001" });
    CHECK (panel.subject().isOpen());
    CHECK (panel.subject().objectId == "CUE00001");

    /*  A FILE WITH SOMETHING IN IT, built by hand: one loud frame a quarter of
        the way through a ten-second file, so the bar has a shape and the
        column picking has something to pick. */
    auto pyramid = std::make_shared<wfg::audio::TimbrePyramid>();
    pyramid->sampleRate = 48000;
    pyramid->samples = 48000ull * 10ull;

    for (const auto count : { 512, 256, 128, 64 })
    {
        std::vector<wfg::audio::timbre::Frame> level;

        for (auto at = 0; at < count; ++at)
        {
            wfg::audio::timbre::Frame frame;
            frame.peak = static_cast<std::uint8_t> (at == count / 4 ? 255 : 30);
            frame.saturation = 180;
            frame.lightness = 120;
            level.push_back (frame);
        }

        pyramid->levels.push_back (std::move (level));
    }

    auto table = std::make_shared<wfg::audio::MediaRecords>();
    wfg::audio::MediaRecord record;
    record.seconds = 10.0;
    record.contentHash = "hash";
    record.pyramid = pyramid;
    table->emplace ("bed.wav", record);

    model::FootReading reading;
    reading.subject = panel.subject();
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.file = "bed.wav";
    reading.fileLength = 10.0;
    reading.ranges = { { "RNG00001", "verse", 1.0, 4.0, 1, 0 },
                       { "RNG00002", "chorus", 4.0, 8.0, 2, 1 } };
    reading.running = true;
    reading.position = 2.5;

    //  Twice, because a second pass with the same reading is the ordinary case.
    panel.show (reading, table);
    panel.show (reading, table);

    /*  IT DRAWS. An editor that threw or read past an end would take the
        window down rather than fail a check, so this is a crash test as much
        as a drawing one. */
    juce::Image canvas (juce::Image::ARGB, 900, 220, true);
    {
        juce::Graphics g (canvas);
        panel.paintEntireComponent (g, true);
    }

    //  The close button is the one control the host owns.
    juce::Button* shut = nullptr;

    for (auto* child : panel.getChildren())
        if (auto* button = dynamic_cast<juce::Button*> (child))
            shut = button;

    REQUIRE (shut != nullptr);
    shut->onClick();
    CHECK (closed);

    /*  AND SHUTTING IT IS THE SHELL'S TO DO, not the panel's: the action was
        called, and the panel is still on its subject until somebody tells it
        otherwise. One place decides what the panel is showing. */
    CHECK (panel.subject().isOpen());

    panel.open ({});
    CHECK_FALSE (panel.subject().isOpen());
}


TEST_CASE ("range table: every slice shows its times, and the arrow gives the next one this length")
{
    /*  The author, 2026-09-21: "With the in out times for all slices
        (including if there's a single slice). It would be great to be able
        copy a duration from one slice to the next so the next out point is at
        the same time from the previous. Also show the repeat and
        infinite/number of repeats."

        The arithmetic is asserted in ClientTests, with no window; what is
        checked here is that the buttons are wired to it and that the table
        builds and draws at all - a construction fault in a component shows as
        a stack here rather than as an empty panel on the night. */
    std::vector<std::pair<std::string, std::string>> written;

    ui::RangeTableComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& value)
    { written.emplace_back (address, value); };

    std::string made;
    actions.createRange = [&] (const std::string& cueId, double, double) { made = cueId; };

    std::string dropped;
    actions.removeRange = [&] (const std::string& id) { dropped = id; };

    ui::RangeTableComponent table (model::Theme {}, actions);
    table.setSize (table.wantedWidth(), 160);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::waveform, "CUE00001" };
    reading.cueKind = "media";
    reading.file = "bed.wav";
    reading.fileLength = 30.0;

    /*  ONE SLICE FIRST, which is the case the author called out: with a single
        range there is no join to drag and the table is the only place its two
        times are legible. */
    reading.ranges = { { "RNG00001", "verse", 1.0, 4.0, 1, 0 } };
    table.show (reading);

    juce::Image canvas (juce::Image::ARGB, table.getWidth(), 160, true);
    {
        juce::Graphics g (canvas);
        table.paintEntireComponent (g, true);
    }

    //  The one row has an arrow, and it is dead because there is no next range.
    auto* arrow = buttonTipped (table, "Give the next range");
    REQUIRE (arrow != nullptr);
    CHECK_FALSE (arrow->isEnabled());

    //  A second range, and the shape of the table changes with it.
    reading.ranges.push_back ({ "RNG00002", "chorus", 4.0, 20.0, 0, 1 });
    table.show (reading);

    arrow = buttonTipped (table, "Give the next range");
    REQUIRE (arrow != nullptr);
    CHECK (arrow->isEnabled());

    arrow->onClick();

    /*  ONE WRITE, ON THE NEXT RANGE'S OUT-POINT: 4.0 + (4.0 - 1.0). Its
        in-point is untouched, so the join with the first range survives. */
    REQUIRE (written.size() == 1);
    CHECK (written[0].first == "/godot/range/RNG00002/out");
    CHECK (written[0].second == "7");

    //  The cross removes the range it sits on, and the plus makes one on this cue.
    if (auto* cross = buttonTipped (table, "Removes this range"); cross != nullptr)
    {
        cross->onClick();
        CHECK (dropped == "RNG00001");
    }

    if (auto* plus = buttonTipped (table, "Adds a range"); plus != nullptr)
    {
        plus->onClick();
        CHECK (made == "CUE00001");
    }

    /*  AND THE REPEATS. The second range is set to for ever (nought), so its
        toggle is on; turning it off writes a number instead of a word, which
        is the one thing a bare box could never have said. */
    std::vector<juce::ToggleButton*> toggles;

    for (auto* button : buttonsUnder (table))
        if (auto* toggle = dynamic_cast<juce::ToggleButton*> (button))
            toggles.push_back (toggle);

    REQUIRE (toggles.size() == 2);
    CHECK_FALSE (toggles[0]->getToggleState());   // the first plays once
    CHECK (toggles[1]->getToggleState());         // the second goes round for ever

    written.clear();
    toggles[1]->setToggleState (false, juce::sendNotificationSync);

    REQUIRE_FALSE (written.empty());
    CHECK (written.back().first == "/godot/range/RNG00002/loops");
    CHECK (written.back().second != "0");
}

TEST_CASE ("inspector: an opener is a button that asks the window to open the panel, not a field")
{
    /*  The author, 2026-09-21: "the controls to show the waveform, the send
        levels, the EQ, the group timeline were in the inspector. No hunting in
        the menus." */
    std::vector<std::pair<std::string, std::string>> written;
    std::pair<std::string, std::string> opened;

    ui::InspectorComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text)
    { written.emplace_back (address, text); };

    actions.openPanel = [&] (const std::string& cueId, const std::string& subject)
    { opened = { cueId, subject }; };

    ui::InspectorComponent inspector (model::Theme {}, actions);
    inspector.setSize (320, 400);

    model::Inspection inspection;
    inspection.cueId = "CUE00001";
    inspection.cueName = "The bed";
    inspection.kind = "media";
    inspection.count = 1;

    model::Block block { "what it does", model::openersFor ("media", "CUE00001") };
    REQUIRE_FALSE (block.fields.empty());
    inspection.blocks.push_back (block);

    inspector.show (inspection);

    juce::Image canvas (juce::Image::ARGB, 320, 400, true);
    {
        juce::Graphics g (canvas);
        inspector.paintEntireComponent (g, true);
    }

    auto* door = buttonTipped (inspector, "Opens at the foot");
    REQUIRE (door != nullptr);

    //  It says what it opens, rather than a bare "Open" beside a label.
    CHECK (door->getButtonText().containsIgnoreCase ("waveform"));

    door->onClick();

    CHECK (opened.first == "CUE00001");
    CHECK (opened.second == "waveform");

    //  And it is a door, not a decision: nothing was written.
    CHECK (written.empty());
}

TEST_CASE ("send mixer: a strip per mix channel, and raising a silent one makes the send first")
{
    /*  The author, 2026-09-22: "there is a general level for the file and a
        send level for each mix channel. The fades operate as a DCA on top of
        this." The master strip is `media/level` and nothing new - which is
        what makes that sentence true rather than approximately true. */
    std::vector<std::pair<std::string, std::string>> written;
    std::vector<std::pair<std::string, std::string>> made;

    ui::SendMixerComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text)
    { written.emplace_back (address, text); };

    actions.createSend = [&] (const std::string& cueId, const std::string& busId)
    { made.emplace_back (cueId, busId); };

    ui::SendMixerComponent mixer (model::Theme {}, actions);
    mixer.setSize (420, 180);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::sends, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.cueLevel = -3.0;

    model::SendStrip reverb;
    reverb.busId = "BUS00001";
    reverb.name = "Reverb";
    reverb.widthWord = "Stereo";
    reverb.channelWord = "5-6";

    model::SendStrip foldback;
    foldback.busId = "BUS00002";
    foldback.name = "Foldback";
    foldback.widthWord = "Stereo";
    foldback.channelWord = "7-8";
    foldback.sendId = "SND00001";
    foldback.levelDb = -6.0;

    reading.sends = { reverb, foldback };

    mixer.show (reading);

    juce::Image canvas (juce::Image::ARGB, 420, 180, true);
    {
        juce::Graphics g (canvas);
        mixer.paintEntireComponent (g, true);
    }

    /*  ONE CROSS PER SEND THAT EXISTS, and none for the mix the cue does not
        feed yet: the asymmetry shows as the cross appearing rather than as a
        fader that will not move. */
    const auto crosses = buttonsUnder (mixer);
    CHECK (crosses.size() == 1);

    /*  THE VALUE BOXES, which are also how this test moves a fader. Typing a
        number is the same gesture as dragging one - both end in
        `levelWanted` - and it is the one a test can make without inventing a
        mouse event, which is how every other case in this file works. */
    std::vector<juce::Label*> boxes;

    for (auto* child : mixer.getChildren())
        for (auto* inner : child->getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (inner))
                boxes.push_back (label);

    REQUIRE (boxes.size() == 3);        // the master, and one per mix channel

    SUBCASE ("the numbers are drawn, because a fader's position is not enough")
    {
        /*  4.8: colour is never the sole carrier of information, and a strip
            read across a booth at a glance is read by its number. */
        CHECK (boxes[0]->getText() == "-3");
        CHECK (boxes[1]->getText() == "-inf");
        CHECK (boxes[2]->getText() == "-6");
    }

    SUBCASE ("raising a strip with no send behind it makes the send before the level")
    {
        /*  The document holds a `Send` only where somebody set one, so the
            first move of a silent fader is two writes - and the level cannot
            go out first, because the address it would be written to does not
            exist yet. */
        boxes[1]->setText ("0", juce::sendNotificationSync);

        REQUIRE (made.size() == 1);
        CHECK (made[0].first == "CUE00001");
        CHECK (made[0].second == "BUS00001");

        //  And nothing was written to an address that is not there yet.
        CHECK (written.empty());

        /*  THEN THE LEVEL, once the tree has the object in it. The reading
            comes back with the send present, and the level the hand asked for
            goes out addressed to it. */
        reading.sends[0].sendId = "SND00002";
        mixer.show (reading);

        REQUIRE (written.size() == 1);
        CHECK (written[0].first == "/godot/send/SND00002/level");
        CHECK (written[0].second == "0");
    }

    SUBCASE ("a send that is already there is written straight to")
    {
        boxes[2]->setText ("-12", juce::sendNotificationSync);

        REQUIRE (written.size() == 1);
        CHECK (written[0].first == "/godot/send/SND00001/level");
        CHECK (written[0].second == "-12");
        CHECK (made.empty());
    }

    SUBCASE ("and the master writes the cue's own level, which is the DCA")
    {
        boxes[0]->setText ("0", juce::sendNotificationSync);

        REQUIRE (written.size() == 1);
        CHECK (written[0].first == "/godot/cue/CUE00001/level");
        CHECK (written[0].second == "0");
        CHECK (made.empty());
    }

    SUBCASE ("and a show with no mix channels says so rather than drawing an empty desk")
    {
        reading.sends.clear();
        reading.notice = "This show declares no mix channels yet.";
        mixer.show (reading);

        CHECK (buttonsUnder (mixer).empty());

        juce::Image blank (juce::Image::ARGB, 420, 180, true);
        juce::Graphics g (blank);
        mixer.paintEntireComponent (g, true);
    }
}

TEST_CASE ("active cue errors: collapsed drawer retains failures and respects edit mode")
{
    std::string inspected;
    ui::RunPaneComponent::Actions actions;
    actions.inspectError = [&] (const std::string& id) { inspected = id; };
    ui::RunPaneComponent pane (model::Theme {}, actions);
    pane.setSize (450, 500);
    juce::TextButton* toggle = nullptr;
    juce::TextButton* clear = nullptr;
    juce::ListBox* errors = nullptr;
    for (auto* child : pane.getChildren())
    {
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
            (button->getButtonText() == "Clear" ? clear : toggle) = button;
        if (auto* list = dynamic_cast<juce::ListBox*> (child)) errors = list;
    }
    REQUIRE (toggle != nullptr); REQUIRE (clear != nullptr); REQUIRE (errors != nullptr);
    CHECK_FALSE (errors->isVisible());
    CHECK_FALSE (clear->isEnabled());
    model::RunRow failed;
    failed.id = "run-1"; failed.cueId = "cue-1"; failed.cueName = "Thunder";
    failed.error = "missing-media"; failed.state = "failed";
    pane.show ({ failed }, {});
    pane.show ({ failed }, {});
    CHECK (toggle->getButtonText() == "> Errors (1)");
    CHECK_FALSE (errors->isVisible());
    toggle->setToggleState (true, juce::dontSendNotification); toggle->onClick();
    CHECK (errors->isVisible());
    CHECK (errors->getListBoxModel()->getNameForRow (0).contains ("missing-media"));
    pane.show ({}, {});
    CHECK (errors->getListBoxModel()->getNumRows() == 1);
    errors->getListBoxModel()->returnKeyPressed (0);
    CHECK (inspected == "cue-1");
    inspected.clear();
    pane.setEditing (false);
    errors->getListBoxModel()->returnKeyPressed (0);
    CHECK (inspected.empty());
    failed.id = "run-2";
    pane.show ({ failed }, {});
    REQUIRE (errors->getListBoxModel()->getNumRows() == 2);
    auto* row = errors->getComponentForRowNumber (0);
    REQUIRE (row != nullptr);
    auto* dismiss = dynamic_cast<juce::TextButton*> (row->getChildComponent (0));
    REQUIRE (dismiss != nullptr);
    dismiss->onClick();
    CHECK (inspected.empty());
    pane.show ({ failed }, {});
    CHECK (errors->getListBoxModel()->getNumRows() == 1);
    CHECK (errors->getListBoxModel()->getNameForRow (0).contains ("Thunder"));
    clear->onClick();
    pane.show ({ failed }, {});
    CHECK (errors->getListBoxModel()->getNumRows() == 0);
    CHECK_FALSE (clear->isEnabled());
    CHECK (toggle->getButtonText() == "v Errors (0)");
}

TEST_CASE ("surface panel: a pad press sends strip.press with a velocity, a fader drag touches, sets and releases the strip's target")
{
    /*  The author's decision AB (2026-09-23): a virtual surface that arms a
        sampler group and plays it from the mouse on a machine with no MIDI.
        Every gesture is a named command (4.11), so what is asserted here is
        the Events a hand would send - no engine, no snapshot, rows built by
        hand as the model would read them. */
    std::vector<wfg::Event> sent;

    ui::SurfacePanelComponent panel (model::Theme {},
                                     [&sent] (wfg::Event event) { sent.push_back (std::move (event)); });
    panel.setSize (900, 420);

    model::SurfaceRow desk;
    desk.id = "SRF00001";
    desk.name = "Desk";
    desk.profile = "virtual";
    desk.strips = 3;
    desk.connected = true;

    //  An armed member on a sampler strip, its fader parked at the bottom.
    model::StripRow gunshot;
    gunshot.id = "STP00001";
    gunshot.surface = desk.id;
    gunshot.index = 0;
    gunshot.role = "sampler";
    gunshot.endpoint = "absolute";
    gunshot.target = "/godot/run/RUN00001/trim";
    gunshot.word = "armed";
    gunshot.cue = "CUE00001";
    gunshot.holder = "RUN00001";
    gunshot.cueName = "Gunshot";
    gunshot.cueColour = "#c04040";
    gunshot.hasLevel = true;
    gunshot.levelDb = -120.0;

    //  A sampler strip with nothing on it: no node for a hand to hold.
    model::StripRow idle;
    idle.id = "STP00002";
    idle.surface = desk.id;
    idle.index = 1;
    idle.role = "sampler";
    idle.endpoint = "absolute";
    idle.word = "free";

    //  A dca strip, riding its DCA's trim.
    model::StripRow band;
    band.id = "STP00003";
    band.surface = desk.id;
    band.index = 2;
    band.role = "dca";
    band.dca = "DCA00001";
    band.endpoint = "absolute";
    band.target = "/godot/dca/DCA00001/trim";
    band.word = "dca";
    band.dcaName = "Band";
    band.hasLevel = true;
    band.levelDb = -6.0;

    const std::vector<model::SurfaceRow> surfaces { desk };
    const std::vector<model::StripRow> strips { gunshot, idle, band };

    panel.show (surfaces, strips);

    //  One column per strip, in the order drawn.
    REQUIRE (panel.columnCount() == 3u);

    //  It draws: a construction fault shows as a stack here rather than on the night.
    juce::Image canvas (juce::Image::ARGB, 900, 420, true);
    {
        juce::Graphics g (canvas);
        panel.paintEntireComponent (g, true);
    }

    SUBCASE ("a pad press carries the velocity of where it landed, and letting go releases the strip")
    {
        //  The top of the pad is the hardest hit.
        panel.pressPad (0, 1.0);

        REQUIRE (sent.size() == 1u);
        CHECK (sent[0].command == "strip.press");
        CHECK (sent[0].origin == "window");
        REQUIRE (sent[0].args.size() == 2u);
        CHECK (sent[0].args[0].getString() == "STP00001");
        CHECK (sent[0].args[1].getInt32() == 127);

        panel.releasePad (0);

        REQUIRE (sent.size() == 2u);
        CHECK (sent[1].command == "strip.release");
        CHECK (sent[1].args[0].getString() == "STP00001");

        //  The bottom edge is the softest hit there is, and never no hit at all.
        panel.pressPad (0, 0.0);
        panel.releasePad (0);

        REQUIRE (sent.size() == 4u);
        REQUIRE (sent[2].args.size() == 2u);
        CHECK (sent[2].args[1].getInt32() == 1);

        //  In between is in between.
        panel.pressPad (0, 0.5);

        REQUIRE (sent.size() == 5u);
        REQUIRE (sent[4].args.size() == 2u);
        CHECK (sent[4].args[1].getInt32() > 1);
        CHECK (sent[4].args[1].getInt32() < 127);

        panel.releasePad (0);
        CHECK (sent.size() == 6u);
    }

    SUBCASE ("a fader ride is a touch, one set a pass, and a release")
    {
        const std::string target = "/godot/run/RUN00001/trim";

        /*  THE TOUCH GOES AT ONCE - it is what tells the engine a hand is on
            the fader, so a dip to the bottom is a ride and not a release. */
        panel.dragFader (0, model::fractionForDb (0.0));

        REQUIRE (sent.size() == 1u);
        CHECK (sent[0].command == "node.touch");
        CHECK (sent[0].args[0].getString() == target);

        //  The value waits for the pass, which is the clock a ride is sent on.
        panel.show (surfaces, strips);

        REQUIRE (sent.size() == 2u);
        CHECK (sent[1].command == "node.set");
        REQUIRE (sent[1].args.size() == 2u);
        CHECK (sent[1].args[0].getString() == target);
        CHECK (sent[1].args[1].getString() == "0");

        //  Two moves inside one pass are one write, of the later.
        panel.dragFader (0, 0.2);
        panel.dragFader (0, 0.5);
        CHECK (sent.size() == 2u);

        panel.show (surfaces, strips);

        REQUIRE (sent.size() == 3u);
        CHECK (sent[2].command == "node.set");
        CHECK (sent[2].args[1].getString()
                 == wfg::osc::formatDouble (std::round (model::dbForFraction (0.5) * 10.0) / 10.0));

        //  Letting go sends what no pass has yet, then gives the node back.
        panel.dragFader (0, 1.0);
        panel.endFader (0);

        REQUIRE (sent.size() == 5u);
        CHECK (sent[3].command == "node.set");
        CHECK (sent[3].args[1].getString() == wfg::osc::formatDouble (model::loudestDb));
        CHECK (sent[4].command == "node.release");
        CHECK (sent[4].args[0].getString() == target);

        //  And nothing more on the next pass.
        panel.show (surfaces, strips);
        CHECK (sent.size() == 5u);
    }

    SUBCASE ("a strip riding nothing takes no drag")
    {
        panel.dragFader (1, 0.9);
        panel.show (surfaces, strips);
        panel.endFader (1);

        CHECK (sent.empty());
    }

    SUBCASE ("a dca strip's fader rides the DCA, and its pad puts the trim back at unity")
    {
        panel.dragFader (2, model::fractionForDb (-12.0));
        panel.endFader (2);

        REQUIRE (sent.size() == 3u);
        CHECK (sent[0].command == "node.touch");
        CHECK (sent[0].args[0].getString() == "/godot/dca/DCA00001/trim");
        CHECK (sent[1].command == "node.set");
        CHECK (sent[1].args[1].getString() == "-12");
        CHECK (sent[2].command == "node.release");

        /*  THE PAD OF A DCA STRIP IS ITS GATE, which resets the trim: a
            `strip.press` there would only ever be refused. */
        panel.pressPad (2, 0.5);
        panel.releasePad (2);

        REQUIRE (sent.size() == 4u);
        CHECK (sent[3].command == "node.set");
        CHECK (sent[3].args[0].getString() == "/godot/dca/DCA00001/trim");
        CHECK (sent[3].args[1].getString() == "0");
    }

    SUBCASE ("the number keys are the first eight columns, at velocity 100")
    {
        CHECK (panel.keyPressed (juce::KeyPress ('1')));

        REQUIRE (sent.size() == 1u);
        CHECK (sent[0].command == "strip.press");
        REQUIRE (sent[0].args.size() == 2u);
        CHECK (sent[0].args[0].getString() == "STP00001");
        CHECK (sent[0].args[1].getInt32() == 100);

        //  A held key repeats, and a repeat is not a second strike.
        panel.keyPressed (juce::KeyPress ('1'));
        CHECK (sent.size() == 1u);

        //  Nobody is at this keyboard, so the key is up and the pad goes.
        panel.keyStateChanged (false);

        REQUIRE (sent.size() == 2u);
        CHECK (sent[1].command == "strip.release");
        CHECK (sent[1].args[0].getString() == "STP00001");
    }
}

TEST_CASE ("run pane: a sampler group counts its members in words")
{
    /*  §16.7: a sampler group's run reads its members as a count - a bank of
        pads is a dozen rows saying the same thing - and waiting for a strip
        or a voice is pending whatever the state says. */
    model::RunRow group;
    group.id = "RUN00010";
    group.cueName = "Bank A";
    group.kind = "group";
    group.state = "playing";

    const auto member = [&group] (const char* runId, const char* runState, const char* waitingFor)
    {
        model::RunRow row;
        row.id = runId;
        row.cueName = "Clip";
        row.kind = "media";
        row.state = runState;
        row.pending = waitingFor;
        row.parentRun = group.id;
        return row;
    };

    std::vector<model::RunRow> rows { group,
                                      member ("RUN00011", "armed", ""),
                                      member ("RUN00012", "armed", ""),
                                      member ("RUN00013", "armed", "voice"),
                                      member ("RUN00014", "armed", "STP00004"),
                                      member ("RUN00015", "playing", ""),
                                      member ("RUN00016", "stopping", "") };

    //  Another group's member is not this group's to count.
    auto stranger = member ("RUN00020", "playing", "");
    stranger.parentRun = "RUN00019";
    rows.push_back (stranger);

    CHECK (model::samplerCounts (rows, group.id)
             == "armed 2 \xc2\xb7 pending 2 \xc2\xb7 playing 1 \xc2\xb7 stopping 1");
    CHECK (model::samplerCounts (rows, "RUN00099").empty());

    //  And the pane draws the words without falling over.
    rows[0].samplerWords = model::samplerCounts (rows, group.id);
    rows[1].samplerWords = "on 1";

    ui::RunPaneComponent pane (model::Theme {}, {});
    pane.setSize (450, 300);
    pane.show (rows, {});

    juce::Image canvas (juce::Image::ARGB, 450, 300, true);
    juce::Graphics g (canvas);
    pane.paintEntireComponent (g, true);
}
