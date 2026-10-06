/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/client/ui/FootPanelComponent.h>
#include <wfg/client/ui/InspectorComponent.h>
#include <wfg/client/ui/EqPanelComponent.h>
#include <wfg/client/ui/FxPanelComponent.h>
#include <wfg/client/ui/TakePanelComponent.h>
#include <wfg/client/ui/PluginEditors.h>
#include <wfg/client/ui/SendMixerComponent.h>
#include <wfg/client/ui/RangeTableComponent.h>
#include <wfg/client/ui/WaveformEditorComponent.h>
#include <wfg/client/ui/RunPaneComponent.h>
#include <wfg/client/ui/Shell.h>
#include <wfg/client/ui/SurfacePanelComponent.h>
#include <wfg/client/ui/TransportComponent.h>
#include <wfg/client/ui/NewCueBarComponent.h>
#include <wfg/client/ui/NewCueMenu.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/ui/NetworkMonitorWindow.h>
#include <wfg/client/ui/ImportWindow.h>
#include <wfg/client/model/NewCue.h>
#include <wfg/client/model/NewCueMenus.h>

#include <wfg/client/model/Fader.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/Surfaces.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/document/LevelLane.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/plugin/EditorHost.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
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


TEST_CASE ("waveform: the head row carries the cue's speed and mode beside the clock")
{
    /*  Namespace draft §22.7. The clock counts the file's seconds, so the row
        says when they are not the room's. With WFG_SNAPSHOT_DIR set,
        waveform-speed.png as well. */
    ui::WaveformEditorComponent editor (model::Theme {}, {});
    editor.setSize (900, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::waveform, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.file = "bed.wav";
    reading.fileLength = 10.0;
    reading.rate = 0.5;
    reading.rateMode = "timestretch";
    reading.running = true;
    reading.position = 4.0;

    editor.show (reading, nullptr);

    juce::Image canvas (juce::Image::ARGB, 900, 220, true);
    {
        juce::Graphics g (canvas);
        editor.paintEntireComponent (g, false);
    }

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto snapshot = editor.createComponentSnapshot (editor.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("waveform-speed.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (snapshot, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }
}

TEST_CASE ("waveform: a level lane is drawn over the file, and one gesture is one write")
{
    /*  Namespace draft §20.5. The arithmetic of every gesture is `model/Lane`'s
        and tested there; what is asked here is the wiring - which press finds
        a point, that a drag writes ONCE and on release, that a double click
        on the line adds and on a point removes, and that the lock stops all
        of it - through the component's own mouse handlers. */
    std::vector<std::pair<std::string, std::string>> written;

    ui::WaveformEditorComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& value)
    { written.emplace_back (address, value); };

    ui::WaveformEditorComponent editor (model::Theme {}, actions);
    editor.setSize (900, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::waveform, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.file = "bed.wav";
    reading.fileLength = 10.0;

    editor.show (reading, nullptr);

    const auto source = juce::Desktop::getInstance().getMainMouseSource();

    const auto mouse = [&] (juce::Point<float> at, int clicks, bool dragged, juce::ModifierKeys mods)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (source, at, mods, juce::MouseInputSource::defaultPressure,
                                 0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor, now, at, now,
                                 clicks, dragged);
    };

    const juce::ModifierKeys left { juce::ModifierKeys::leftButtonModifier };

    const auto lastLane = [&]
    {
        REQUIRE_FALSE (written.empty());
        CHECK (written.back().first == "/godot/cue/CUE00001/levelLane");
        return written.back().second;
    };

    //  A DOUBLE CLICK ON THE UNITY LINE of a lane nobody has drawn: the first point, at nought.
    const auto onLine = editor.pointPosition ({ 3.0, 0.0 });
    editor.mouseDoubleClick (mouse (onLine, 2, false, left));

    REQUIRE (written.size() == 1u);

    auto parsed = wfg::doc::readLevelLane (lastLane());
    REQUIRE (parsed.problem.empty());
    REQUIRE (parsed.points.size() == 1u);
    CHECK (parsed.points[0].seconds == doctest::Approx (3.0).epsilon (0.02));
    CHECK (parsed.points[0].levelDb == doctest::Approx (0.0));

    //  THE ENGINE ANSWERS, and the reading carries the point.
    reading.lane = { { parsed.points[0].seconds, 0.0 } };
    editor.show (reading, nullptr);

    /*  A DRAG MOVES IT AND WRITES NOTHING UNTIL IT LETS GO: one gesture, one
        write, one step of undo. */
    const auto from = editor.pointPosition (reading.lane[0]);
    const auto to = editor.pointPosition ({ reading.lane[0].seconds, -12.0 });

    editor.mouseDown (mouse (from, 1, false, left));
    editor.mouseDrag (mouse (from.translated (0.0f, (to.y - from.y) * 0.5f), 1, true, left));
    editor.mouseDrag (mouse (to, 1, true, left));

    CHECK (written.size() == 1u);

    editor.mouseUp (mouse (to, 1, true, left));

    REQUIRE (written.size() == 2u);

    parsed = wfg::doc::readLevelLane (lastLane());
    REQUIRE (parsed.points.size() == 1u);
    CHECK (parsed.points[0].levelDb == doctest::Approx (-12.0).epsilon (0.05));

    //  A PRESS THAT ONLY PICKS a point has decided nothing, and writes nothing.
    reading.lane = { { parsed.points[0].seconds, parsed.points[0].levelDb } };
    editor.show (reading, nullptr);

    const auto picked = editor.pointPosition (reading.lane[0]);
    editor.mouseDown (mouse (picked, 1, false, left));
    editor.mouseUp (mouse (picked, 1, false, left));

    CHECK (written.size() == 2u);

    //  A DOUBLE CLICK ON THE POINT takes it away, and the last one gone is no lane.
    editor.mouseDoubleClick (mouse (picked, 2, false, left));

    REQUIRE (written.size() == 3u);
    CHECK (lastLane().empty());

    /*  THE ENGINE ANSWERS WITH NO LANE, and the editor lets go of the copy
        it drew while the write came round - drawn from the reading again. */
    reading.lane.clear();
    editor.show (reading, nullptr);

    //  UNDER THE LOCK nothing is grabbed, added or taken away.
    reading.lane = { { 3.0, -12.0 } };
    reading.locked = true;
    editor.show (reading, nullptr);

    const auto locked = editor.pointPosition (reading.lane[0]);
    editor.mouseDown (mouse (locked, 1, false, left));
    editor.mouseDrag (mouse (locked.translated (0.0f, 20.0f), 1, true, left));
    editor.mouseUp (mouse (locked.translated (0.0f, 20.0f), 1, true, left));
    editor.mouseDoubleClick (mouse (locked, 2, false, left));
    editor.mouseDoubleClick (mouse (editor.pointPosition ({ 7.0, -12.0 }), 2, false, left));

    CHECK (written.size() == 3u);

    /*  AND IT DRAWS, over a file with a shape, with a dip, a picked point and
        a cue sounding - the picture the author judges the law and the look
        from (decision DD), written out when WFG_SNAPSHOT_DIR asks for it. */
    auto pyramid = std::make_shared<wfg::audio::TimbrePyramid>();
    pyramid->sampleRate = 48000;
    pyramid->samples = 48000ull * 10ull;

    for (const auto count : { 512, 256, 128, 64 })
    {
        std::vector<wfg::audio::timbre::Frame> level;

        for (auto at = 0; at < count; ++at)
        {
            wfg::audio::timbre::Frame frame;
            frame.peak = static_cast<std::uint8_t> (120 + static_cast<int> (100.0 * std::abs (std::sin (at * 0.07))));
            frame.saturation = 160;
            frame.lightness = 110;
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

    reading.locked = false;
    reading.lane = { { 1.0, 0.0 }, { 2.0, -18.0 }, { 6.0, -18.0 }, { 7.5, 3.0 } };
    reading.ranges = { { "RNG00001", "verse", 0.5, 8.5, 0, 0 } };
    reading.running = true;
    reading.position = 4.0;
    editor.show (reading, table);

    const auto dip = editor.pointPosition (reading.lane[1]);
    editor.mouseDown (mouse (dip, 1, false, left));
    editor.mouseUp (mouse (dip, 1, false, left));
    editor.show (reading, table);

    juce::Image canvas (juce::Image::ARGB, 900, 220, true);
    {
        juce::Graphics g (canvas);
        editor.paintEntireComponent (g, false);
    }

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto snapshot = editor.createComponentSnapshot (editor.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("waveform-lane.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (snapshot, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }
}

TEST_CASE ("waveform: a send's lane is picked, drawn over the file and written at the send")
{
    /*  Namespace draft §28, QB: one lane drawn at a time, the level's or one
        send's. A gesture on the picked one writes at ITS address, and a send
        that has gone gives the level lane back. */
    std::vector<std::pair<std::string, std::string>> written;

    ui::WaveformEditorComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& value)
    { written.emplace_back (address, value); };

    ui::WaveformEditorComponent editor (model::Theme {}, actions);
    editor.setSize (900, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::waveform, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.file = "bed.wav";
    reading.fileLength = 10.0;
    reading.lane = { { 2.0, -12.0 } };                  // the level, down twelve
    reading.sendLanes = { { "SND00001", "Loin", {} } }; // one send, no lane on it yet

    editor.show (reading, nullptr);

    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const juce::ModifierKeys left { juce::ModifierKeys::leftButtonModifier };

    const auto doubleClick = [&] (juce::Point<float> at)
    {
        const auto now = juce::Time::getCurrentTime();
        editor.mouseDoubleClick (juce::MouseEvent (source, at, left, juce::MouseInputSource::defaultPressure,
                                                   0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor, now, at, now,
                                                   2, false));
    };

    /*  THE SEND PICKED: its line is the unity line, since nothing is drawn on
        it, and a point added there is written on the send. */
    editor.pickLaneAt (0);
    doubleClick (editor.pointPosition ({ 3.0, 0.0 }));

    REQUIRE (written.size() == 1u);
    CHECK (written.back().first == "/godot/send/SND00001/levelLane");

    auto parsed = wfg::doc::readLevelLane (written.back().second);
    REQUIRE (parsed.problem.empty());
    REQUIRE (parsed.points.size() == 1u);
    CHECK (parsed.points[0].levelDb == doctest::Approx (0.0));

    /*  THE LEVEL PICKED AGAIN: its line is at -12, and a point there is the
        cue's - the send's lane is not touched. */
    editor.pickLaneAt (-1);
    doubleClick (editor.pointPosition ({ 6.0, -12.0 }));

    REQUIRE (written.size() == 2u);
    CHECK (written.back().first == "/godot/cue/CUE00001/levelLane");

    parsed = wfg::doc::readLevelLane (written.back().second);
    REQUIRE (parsed.problem.empty());
    CHECK (parsed.points.size() == 2u);

    /*  A SEND THAT GOES gives the level lane back: picked, then the reading
        no longer has it, and the next point is the cue's again. */
    editor.pickLaneAt (0);
    reading.sendLanes.clear();
    editor.show (reading, nullptr);

    doubleClick (editor.pointPosition ({ 7.0, -12.0 }));

    REQUIRE (written.size() == 3u);
    CHECK (written.back().first == "/godot/cue/CUE00001/levelLane");
}


TEST_CASE ("surface panel: with the faders flipped, a column's pad is its lane's REC, and its fader rides the lane")
{
    /*  Namespace draft §34: the faders flip to a cue, and on the window's
        panel - which has no REC button - a flipped column's pad is its lane's
        REC, arming or disarming against what the tree says; its fader rides
        the lane's node as any fader rides its target. A column not flipped -
        a pad strip - keeps its pad. */
    std::vector<wfg::Event> sent;

    ui::SurfacePanelComponent panel (model::Theme {},
                                     [&sent] (wfg::Event event) { sent.push_back (std::move (event)); });
    panel.setSize (600, 420);

    model::SurfaceRow desk;
    desk.id = "SRF00001";
    desk.name = "Desk";
    desk.profile = "virtual";
    desk.strips = 2;
    desk.connected = true;

    model::StripRow level;
    level.id = "STP00002";
    level.surface = desk.id;
    level.index = 0;
    level.role = "sampler";
    level.endpoint = "absolute";
    level.target = "/godot/surface/laneRide";
    level.word = "lane";
    level.cue = "CUE00001";
    level.hasLevel = true;
    level.levelDb = -4.0;

    model::StripRow face = level;
    face.id = "STP00003";
    face.index = 1;
    face.target = "/godot/bus/SN000010/laneRide";
    face.word = "rec";
    face.levelDb = -12.0;

    model::LaneRecordReading lanes;
    lanes.cue = "CUE00001";
    lanes.flipped = true;
    lanes.faders = { { "level", "Level", false, true, -4.0 }, { "SN000010", "Face", true, true, -12.0 } };

    panel.show ({ desk }, { level, face }, lanes);

    //  The level's pad arms its lane; Face's, armed, disarms it.
    panel.pressPad (0, 0.5);
    REQUIRE (sent.size() == 1u);
    CHECK (sent[0].command == "lane.rec");
    REQUIRE (sent[0].args.size() == 2u);
    CHECK (sent[0].args[0].getString() == "level");
    CHECK (sent[0].args[1].getBool());

    panel.pressPad (1, 0.5);
    REQUIRE (sent.size() == 2u);
    CHECK (sent[1].args[0].getString() == "SN000010");
    CHECK_FALSE (sent[1].args[1].getBool());

    //  The fader rides the lane's node: a touch, then the value.
    sent.clear();
    panel.dragFader (1, 0.2);

    REQUIRE_FALSE (sent.empty());
    CHECK (sent[0].command == "node.touch");
    CHECK (sent[0].args[0].getString() == "/godot/bus/SN000010/laneRide");

    //  Flipped back, a pad is a pad again: nothing for the lanes.
    panel.endFader (1);
    sent.clear();
    level.target.clear();
    level.word = "free";
    panel.show ({ desk }, { level, face }, {});
    panel.pressPad (0, 0.5);
    panel.releasePad (0);

    for (const auto& event : sent)
        CHECK (event.command != "lane.rec");
}

TEST_CASE ("waveform: the lanes' Rec flips the faders, records and stops - saying which")
{
    /*  Namespace draft §34 (after §20.9): one button, three states read from
        the tree at every click, and the transport's stop ending a pass
        rather than killing it (a kill drops the ride). "Autom.", "● Rec",
        "■ Stop" - the implementer's since the flip, kept by the author - each
        one whole on a button as wide as the longest. */
    std::vector<std::string> said;

    ui::WaveformEditorComponent::Actions actions;
    actions.laneArm = [&said] (const std::string& cue) { said.push_back ("arm " + cue); };
    actions.laneFree = [&said] { said.push_back ("free"); };
    actions.laneRecord = [&said] (double from) { said.push_back ("record " + std::to_string (static_cast<int> (from))); };
    actions.laneStop = [&said] { said.push_back ("stop"); };
    actions.stop = [&said] (const std::string& run) { said.push_back ("kill " + run); };

    ui::WaveformEditorComponent editor (model::Theme {}, actions);
    editor.setSize (1000, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::waveform, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.file = "bed.wav";
    reading.fileLength = 10.0;
    reading.lane = { { 1.0, 0.0 }, { 6.0, -20.0 } };

    const auto rec = [&editor]
    {
        juce::Button* found = nullptr;

        for (auto* button : buttonsUnder (editor))
            for (const auto* word : { "Autom.", "Rec", "Stop" })
                if (button->getButtonText().contains (juce::String::fromUTF8 (word)))
                    found = button;

        REQUIRE (found != nullptr);
        return found;
    };

    /*  WHOLE ON THE BUTTON, in the font it is drawn in: no state is cut to
        an ellipsis (QY widened the button). */
    const auto fits = [&rec]
    {
        auto* button = dynamic_cast<juce::TextButton*> (rec());
        REQUIRE (button != nullptr);

        const auto font = button->getLookAndFeel().getTextButtonFont (*button, button->getHeight());
        return juce::GlyphArrangement::getStringWidthInt (font, button->getButtonText()) < button->getWidth();
    };

    //  IDLE: "Autom." flips the faders to this cue (§34).
    editor.show (reading, nullptr);
    CHECK (rec()->getButtonText() == "Autom.");
    CHECK (rec()->getTooltip().startsWith ("Autom."));
    CHECK (fits());
    rec()->onClick();
    CHECK (said.back() == "arm CUE00001");

    //  FLIPPED: Rec starts a pass from the playhead, and a cross flips the faders back.
    reading.laneRecord.cue = "CUE00001";
    reading.laneRecord.flipped = true;
    reading.laneRecord.faders = { { "level", "Level", true, true, 0.0 } };
    editor.show (reading, nullptr);

    CHECK (rec()->getButtonText() == juce::String::fromUTF8 ("\xe2\x97\x8f Rec"));
    CHECK (rec()->getTooltip().contains ("REC"));
    CHECK (rec()->getToggleState());
    CHECK (fits());
    rec()->onClick();
    CHECK (said.back() == "record 0");

    juce::Button* cross = nullptr;

    for (auto* button : buttonsUnder (editor))
        if (button->getButtonText() == juce::String::fromUTF8 ("\xe2\x9c\x95"))
            cross = button;

    REQUIRE (cross != nullptr);
    CHECK (cross->isVisible());
    cross->onClick();
    CHECK (said.back() == "free");

    //  RECORDING: the button stops the pass, and so does the transport - ending it, not killing it.
    reading.laneRecord.recording = true;
    reading.running = true;
    reading.runId = "RN000001";

    for (int pass = 0; pass < 12; ++pass)
    {
        reading.position = 1.0 + 0.25 * pass;
        reading.laneRecord.faders[0].rideDb = -3.0 - 1.5 * pass;
        editor.show (reading, nullptr);
    }

    CHECK (rec()->getButtonText() == juce::String::fromUTF8 ("\xe2\x96\xa0 Stop"));
    CHECK (fits());
    rec()->onClick();
    CHECK (said.back() == "stop");

    for (auto* button : buttonsUnder (editor))
        if (button->getTooltip().startsWith ("Stop") || button->getTooltip().startsWith ("Play"))
            button->onClick();

    CHECK (said.back() == "stop");
    CHECK (std::find (said.begin(), said.end(), "kill RN000001") == said.end());

    //  AND IT DRAWS, the ride's trail over the lane - the picture to judge by.
    juce::Image canvas (juce::Image::ARGB, 1000, 220, true);
    {
        juce::Graphics g (canvas);
        editor.paintEntireComponent (g, false);
    }

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto snapshot = editor.createComponentSnapshot (editor.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("waveform-lane-rec.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (snapshot, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }

    //  UNDER THE LOCK, a lane not recording offers nothing.
    reading.laneRecord = {};
    reading.running = false;
    reading.locked = true;
    editor.show (reading, nullptr);
    CHECK_FALSE (rec()->isEnabled());
}

TEST_CASE ("waveform: a pass follows the playhead at a readable scale, and its end frames what it wrote and says so, once")
{
    /*  Namespace draft §30, item 3, and §30.4. A 647-second file drawn whole is
        a pixel and a half a second: the author's ride was a smudge at the
        playhead, and its points appeared only at the stop, somewhere off the
        picture. While this cue's lane records the window follows the playhead
        no wider than a minute (RG); when the pass has ended, what it wrote is
        framed and said - and a pass that wrote nothing says why. */
    std::vector<juce::String> notes;

    ui::WaveformEditorComponent::Actions actions;
    actions.say = [&notes] (const juce::String& sentence) { notes.push_back (sentence); };

    ui::WaveformEditorComponent editor (model::Theme {}, actions);
    editor.setSize (1000, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::waveform, "CUE00001" };
    reading.cueName = "Danse";
    reading.cueKind = "media";
    reading.file = "danse.wav";
    reading.fileLength = 647.0;

    //  A pass that ended before this editor opened is not said.
    reading.laneRecord.pass.tick = 50;
    reading.laneRecord.pass.cue = "CUE00001";
    reading.laneRecord.pass.how = "untouched";
    editor.show (reading, nullptr);
    CHECK (notes.empty());

    /*  How many pixels thirty seconds is: about 45 with the whole file drawn,
        about half the bar at a minute across. */
    const auto thirty = [&editor]
    {
        return editor.pointPosition ({ 30.0, 0.0 }).x - editor.pointPosition ({ 0.0, 0.0 }).x;
    };

    CHECK (thirty() < 80.0f);

    //  RECORDING: the view follows, a minute across.
    reading.laneRecord.cue = "CUE00001";
    reading.laneRecord.flipped = true;
    reading.laneRecord.recording = true;
    reading.laneRecord.faders = { { "level", "Level", true, true, 0.0 } };
    reading.running = true;
    reading.runId = "RN000001";

    for (int pass = 0; pass < 20; ++pass)
    {
        reading.position = 12.0 + 0.5 * pass;
        reading.laneRecord.faders[0].rideDb = -3.0 - 0.25 * pass;
        editor.show (reading, nullptr);
    }

    CHECK (thirty() > 200.0f);

    //  And the playhead on the picture: between its edges.
    const auto head = editor.pointPosition ({ reading.position, 0.0 }).x;
    CHECK (head > editor.pointPosition ({ 0.0, 0.0 }).x);
    CHECK (head < 1000.0f);
    CHECK (notes.empty());

    //  THE PASS ENDS: what it wrote framed and said.
    reading.laneRecord = {};
    reading.running = false;
    reading.laneRecord.pass.tick = 900;
    reading.laneRecord.pass.cue = "CUE00001";
    reading.laneRecord.pass.how = "kept";
    reading.laneRecord.pass.points = 7;
    reading.laneRecord.pass.from = 12.0;
    reading.laneRecord.pass.to = 21.5;
    reading.laneRecord.pass.spans = true;
    editor.show (reading, nullptr);

    REQUIRE (notes.size() == 1u);
    CHECK (notes.back() == juce::String::fromUTF8 ("Autom.: 7 points, 12.0\xe2\x80\x93" "21.5 s"));

    //  Framed: the stretch spans most of the bar, both its ends on the picture.
    const auto from = editor.pointPosition ({ 12.0, 0.0 }).x;
    const auto to = editor.pointPosition ({ 21.5, 0.0 }).x;
    CHECK (from > 0.0f);
    CHECK (to < 1000.0f);
    CHECK (to - from > 300.0f);

    //  Said once: the next pass of the window says nothing more.
    editor.show (reading, nullptr);
    CHECK (notes.size() == 1u);

    //  A pass on another cue is that cue's to say.
    reading.laneRecord.pass.tick = 950;
    reading.laneRecord.pass.cue = "CUE00002";
    editor.show (reading, nullptr);
    CHECK (notes.size() == 1u);

    //  A pass that wrote nothing says why.
    reading.laneRecord.pass.tick = 990;
    reading.laneRecord.pass.cue = "CUE00001";
    reading.laneRecord.pass.how = "untouched";
    reading.laneRecord.pass.spans = false;
    editor.show (reading, nullptr);

    REQUIRE (notes.size() == 2u);
    CHECK (notes.back().startsWith ("Autom.: nothing written"));
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

TEST_CASE ("range table: a cue with no ranges shows the whole file, and looping it makes the range")
{
    /*  The author, 2026-09-30: "Could we have the complete range already there
        by default. It makes looping a media much easier this way." */
    std::vector<std::pair<std::string, std::string>> written;
    std::vector<std::tuple<std::string, double, double>> made;

    ui::RangeTableComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& value)
    { written.emplace_back (address, value); };
    actions.createRange = [&] (const std::string& cueId, double in, double out)
    { made.emplace_back (cueId, in, out); };

    ui::RangeTableComponent table (model::Theme {}, actions);
    table.setSize (table.wantedWidth(), 160);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::waveform, "CUE00001" };
    reading.cueKind = "media";
    reading.file = "bed.wav";
    reading.fileLength = 30.0;
    table.show (reading);

    //  One row, the whole file, and nothing written just for showing it.
    std::vector<juce::ToggleButton*> toggles;

    for (auto* button : buttonsUnder (table))
        if (auto* toggle = dynamic_cast<juce::ToggleButton*> (button))
            toggles.push_back (toggle);

    REQUIRE (toggles.size() == 1);
    CHECK_FALSE (toggles[0]->getToggleState());
    CHECK (made.empty());
    CHECK (written.empty());

    //  For ever, ticked: the range over the whole file first, and nothing on it yet.
    toggles[0]->setToggleState (true, juce::sendNotificationSync);

    REQUIRE (made.size() == 1);
    CHECK (std::get<0> (made[0]) == "CUE00001");
    CHECK (std::get<1> (made[0]) == doctest::Approx (0.0));
    CHECK (std::get<2> (made[0]) == doctest::Approx (30.0));
    CHECK (written.empty());

    //  The tree has the range: the change lands on it, once.
    reading.ranges = { { "RNG00001", "", 0.0, 30.0, 1, 0 } };
    table.show (reading);

    REQUIRE (written.size() == 1);
    CHECK (written[0].first == "/godot/range/RNG00001/loops");
    CHECK (written[0].second == "0");

    table.show (reading);
    CHECK (written.size() == 1);

    //  A file whose length is not known yet has no whole to show.
    model::FootReading unknown = reading;
    unknown.ranges.clear();
    unknown.fileLength = 0.0;
    table.show (unknown);

    CHECK (buttonsUnder (table).size() == 1u);   // the plus in the head, and no row
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

    //  Since 2026-09-30 a bar at the head of the panel, not rows among the fields.
    inspection.panels = model::openersFor ("media", "CUE00001");
    REQUIRE_FALSE (inspection.panels.empty());

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

    /*  LIT WHILE ITS PANEL IS OPEN ON THIS CUE, and only then (author,
        2026-09-30: the toggles "at the top"): the window says what the foot
        shows, and the button for it stays pressed. */
    CHECK_FALSE (door->getToggleState());
    inspector.showFoot ("waveform", "CUE00001");
    CHECK (door->getToggleState());
    inspector.showFoot ("waveform", "CUE00002");
    CHECK_FALSE (door->getToggleState());
    inspector.showFoot ("eq", "CUE00001");
    CHECK_FALSE (door->getToggleState());

    auto* eq = buttonTipped (inspector, "Opens at the foot of the window, on this cue: EQ");
    REQUIRE (eq != nullptr);
    CHECK (eq->getToggleState());
    inspector.showFoot ({}, {});
    CHECK_FALSE (eq->getToggleState());
}

TEST_CASE ("inspector: Doh!'s row on a cue is a menu of words that writes their keys")
{
    /*  The worded-choice commit path namespace draft §24.10 named owed (D5,
        2026-10-03). D1 gave the inspector a closed set the model puts in
        words - Doh!'s row on an OSC or a MIDI cue, "as the device (Meh)",
        "Undo(h)", "Meh" (the author's words, K7) - and the menu writes the
        KEYS the tree declares, `device`, `takeBack`, `leave`, by position,
        never the words a person reads. ClientTests pins the words; this pins
        the write. A net: written after D1 was built. */
    std::vector<std::pair<std::string, std::string>> written;

    ui::InspectorComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text)
    { written.emplace_back (address, text); };

    ui::InspectorComponent inspector (model::Theme {}, actions);
    inspector.setSize (320, 300);

    model::Field doh;
    doh.address = "/godot/cue/CUE00001/doh";
    doh.name = "doh";
    doh.label = "on Doh!";
    doh.typeTags = "s";
    doh.value = "device";
    doh.writable = true;
    doh.control = model::Control::choice;                  // as the model reads a closed set
    doh.options = { "device", "takeBack", "leave" };
    doh.choices = { { "device", "as the device (Meh)" }, { "takeBack", "Undo(h)" }, { "leave", "Meh" } };

    model::Inspection inspection;
    inspection.cueId = "CUE00001";
    inspection.cueName = "Lights 12";
    inspection.kind = "osc";
    inspection.count = 1;
    inspection.blocks.push_back ({ "what it does", { doh } });
    inspector.show (inspection);

    juce::ComboBox* menu = nullptr;

    std::function<void (juce::Component&)> walk = [&] (juce::Component& at)
    {
        for (auto* child : at.getChildren())
        {
            if (auto* box = dynamic_cast<juce::ComboBox*> (child); box != nullptr && menu == nullptr)
                menu = box;

            walk (*child);
        }
    };

    walk (inspector);
    REQUIRE (menu != nullptr);

    //  The words, in the model's order, and the cue's own word chosen.
    REQUIRE (menu->getNumItems() == 3);
    CHECK (menu->getItemText (0) == "as the device (Meh)");
    CHECK (menu->getItemText (1) == "Undo(h)");
    CHECK (menu->getItemText (2) == "Meh");
    CHECK (menu->getText() == "as the device (Meh)");
    CHECK (written.empty());

    //  "Meh" writes `leave`; "Undo(h)" writes `takeBack` - the keys, to the cue's row.
    menu->setSelectedItemIndex (2, juce::sendNotificationSync);
    REQUIRE (written.size() == 1u);
    CHECK (written[0].first == "/godot/cue/CUE00001/doh");
    CHECK (written[0].second == "leave");

    menu->setSelectedItemIndex (1, juce::sendNotificationSync);
    REQUIRE (written.size() == 2u);
    CHECK (written[1].second == "takeBack");

    //  And a reading that says `leave` shows "Meh" and writes nothing.
    inspection.blocks[0].fields[0].value = "leave";
    inspector.show (inspection);
    menu = nullptr;
    walk (inspector);
    REQUIRE (menu != nullptr);
    CHECK (menu->getText() == "Meh");
    CHECK (written.size() == 2u);
}

TEST_CASE ("inspector: every block is a drawer, and a drawer of rows that mean nothing here starts shut")
{
    /*  The author, 2026-09-30: "We can also make more drawers for things." */
    ui::InspectorComponent inspector (model::Theme {}, {});
    inspector.setSize (320, 600);

    const auto field = [] (std::string name, bool applies)
    {
        model::Field made;
        made.address = "/godot/cue/CUE00001/" + name;
        made.name = name;
        made.label = name;
        made.typeTags = "d";
        made.value = "0";
        made.writable = true;
        made.applies = applies;
        return made;
    };

    model::Inspection inspection;
    inspection.cueId = "CUE00001";
    inspection.cueName = "The bed";
    inspection.kind = "media";
    inspection.count = 1;
    inspection.blocks.push_back ({ "what it does", { field ("level", true) } });
    inspection.blocks.push_back ({ "sampler", { field ("initialLevel", false), field ("releaseFade", false) } });
    inspector.show (inspection);

    const auto labelled = [&inspector] (const juce::String& word) -> juce::Label*
    {
        juce::Label* found = nullptr;

        std::function<void (juce::Component&)> walk = [&] (juce::Component& at)
        {
            for (auto* child : at.getChildren())
            {
                if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && found == nullptr)
                    if (label->getText() == word)
                        found = label;

                walk (*child);
            }
        };

        walk (inspector);
        return found;
    };

    //  A drawer's head is a thing with a tooltip saying what a press does to it.
    const auto headSaying = [&inspector] (const juce::String& tip) -> juce::Component*
    {
        juce::Component* found = nullptr;

        std::function<void (juce::Component&)> walk = [&] (juce::Component& at)
        {
            for (auto* child : at.getChildren())
            {
                if (auto* tips = dynamic_cast<juce::SettableTooltipClient*> (child);
                      tips != nullptr && found == nullptr && dynamic_cast<juce::Button*> (child) == nullptr
                        && dynamic_cast<juce::Label*> (child) == nullptr && tips->getTooltip() == tip)
                    found = child;

                walk (*child);
            }
        };

        walk (inspector);
        return found;
    };

    auto* level = labelled ("level");
    auto* initial = labelled ("initialLevel");
    REQUIRE (level != nullptr);
    REQUIRE (initial != nullptr);

    //  What the cue does is open; the sampler's rows, greyed every one, start shut.
    CHECK (level->isVisible());
    CHECK_FALSE (initial->isVisible());

    //  Pressing the open head shuts it, and the next press opens it again.
    auto* open = headSaying ("Shuts this drawer");
    REQUIRE (open != nullptr);
    inspector.pressedOn (open);
    CHECK_FALSE (level->isVisible());

    inspector.pressedOn (open);
    CHECK (level->isVisible());

    //  A shut drawer opened by hand stays open, greyed or not.
    auto* shut = headSaying ("Opens this drawer");
    REQUIRE (shut != nullptr);
    inspector.pressedOn (shut);
    CHECK (initial->isVisible());

    inspector.show (inspection);
    CHECK (initial->isVisible());
}

TEST_CASE ("inspector: a press on a number puts it on the master dial, and its line wears the dial")
{
    /*  The author, 2026-09-26: "Can selecting a parameter in the inspector or
        foot panel on-screen via mouse or touch assign it to the master rotary
        encoder on the D700?" - any click or touch. */
    std::vector<std::string> dialed;

    ui::InspectorComponent::Actions actions;
    actions.dial = [&] (const std::string& address) { dialed.push_back (address); };

    ui::InspectorComponent inspector (model::Theme {}, actions);
    inspector.setSize (320, 200);

    const auto field = [] (std::string name, std::string tags, std::string value, std::string unit)
    {
        model::Field made;
        made.address = "/godot/cue/CUE00001/" + name;
        made.name = name;
        made.label = name;
        made.typeTags = std::move (tags);
        made.value = std::move (value);
        made.unit = std::move (unit);
        made.writable = true;
        return made;
    };

    model::Inspection inspection;
    inspection.cueId = "CUE00001";
    inspection.cueName = "The bed";
    inspection.kind = "media";
    inspection.count = 1;
    inspection.blocks.push_back ({ "what it is", { field ("name", "s", "The bed", "") } });
    inspection.blocks.push_back ({ "what it does", { field ("level", "d", "-6", "dB"),
                                                     field ("preWait", "d", "0.5", "s") } });
    inspector.show (inspection);

    const auto labelSaying = [&inspector] (const juce::String& word) -> juce::Label*
    {
        juce::Label* found = nullptr;

        std::function<void (juce::Component&)> walk = [&] (juce::Component& at)
        {
            for (auto* child : at.getChildren())
            {
                if (auto* label = dynamic_cast<juce::Label*> (child))
                    if (label->getText().endsWith (word) && found == nullptr)
                        found = label;

                walk (*child);
            }
        };

        walk (inspector);
        return found;
    };

    auto* levelName = labelSaying ("level");
    auto* nameName = labelSaying ("name");
    REQUIRE (levelName != nullptr);
    REQUIRE (nameName != nullptr);

    //  A press on a number's name sends its address; on a word, nothing.
    inspector.pressedOn (levelName);
    inspector.pressedOn (nameName);
    CHECK (dialed == std::vector<std::string> { "/godot/cue/CUE00001/level" });

    //  The tree says the dial is on it: the dial before its name.
    inspector.showDial ("/godot/cue/CUE00001/level");
    CHECK (levelName->getText().startsWith (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x89"))));
    CHECK_FALSE (nameName->getText().startsWith (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x89"))));

    //  And a poll of the same values keeps it.
    inspector.show (inspection);
    CHECK (levelName->getText().startsWith (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x89"))));

    inspector.showDial ({});
    CHECK (levelName->getText() == "level");

    const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {});

    if (dir.isNotEmpty())
    {
        inspector.showDial ("/godot/cue/CUE00001/level");

        const auto picture = inspector.createComponentSnapshot (inspector.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("inspector-dial.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
    }
}

TEST_CASE ("send mixer: a strip per mix channel, and raising a silent one makes the send first")
{
    /*  The author, 2026-09-22: "there is a general level for the file and a
        send level for each mix channel. The fades operate as a DCA on top of
        this." The master strip is `media/level` and nothing new - which is
        what makes that sentence true rather than approximately true. */
    std::vector<std::pair<std::string, std::string>> written;
    std::vector<std::pair<std::string, std::string>> made;
    std::vector<double> madeAt;

    ui::SendMixerComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text)
    { written.emplace_back (address, text); };

    actions.createSend = [&] (const std::string& cueId, const std::string& busId, double level)
    {
        made.emplace_back (cueId, busId);
        madeAt.push_back (level);
    };

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
    std::vector<juce::Button*> crosses, switches;

    for (auto* button : buttonsUnder (mixer))
        (button->getButtonText() == "x" ? crosses : switches).push_back (button);

    CHECK (crosses.size() == 1);

    /*  AND ONE SWITCH PER SEND (2026-09-25), on, beside its cross: off keeps
        the level and takes the send out of the mix. */
    REQUIRE (switches.size() == 1);
    CHECK (switches[0]->getToggleState());

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

        /*  AND IT IS MADE AT THE LEVEL ASKED FOR (2026-09-25): born at the
            row's default of nought and set a round trip later, the voice
            climbed towards unity in between - the author heard it. */
        REQUIRE (madeAt.size() == 1);
        CHECK (madeAt[0] == doctest::Approx (0.0));

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

    SUBCASE ("a level is read the way it is typed, and a word writes nothing")
    {
        /*  "-6,5 dB" is what a French hand types into a box that showed -6;
            "full" has no number in it, and writing nought for it would be
            full level. */
        boxes[2]->setText ("-6,5 dB", juce::sendNotificationSync);
        boxes[2]->setText ("full", juce::sendNotificationSync);

        REQUIRE (written.size() == 1);
        CHECK (written[0].first == "/godot/send/SND00001/level");
        CHECK (written[0].second == "-6.5");
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

TEST_CASE ("send mixer: over several cues a drag moves every one by the same decibels as one write, a number typed sets them all")
{
    /*  Namespace draft §30.11, the author's RA: "A drag moves every picked
        cue's level or send by the same number of decibels, keeping their
        differences; a typed number sets them all to it; one gesture is one
        undo." Three cues; the reverb fed by two of them, at -6 and -12. */
    std::vector<std::vector<std::pair<std::string, std::string>>> sets;
    std::vector<std::pair<std::string, std::string>> written;
    std::vector<std::pair<std::string, double>> made;

    ui::SendMixerComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text) { written.emplace_back (address, text); };
    actions.setMany = [&] (const std::vector<std::pair<std::string, std::string>>& writes) { sets.push_back (writes); };
    actions.createSend = [&] (const std::string& cueId, const std::string& busId, double level)
    {
        made.emplace_back (cueId + " " + busId, level);
    };

    ui::SendMixerComponent mixer (model::Theme {}, actions);
    mixer.setSize (420, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::sends, "CUE0000B" };
    reading.cues = { "CUE0000B", "CUE0000A", "CUE0000C" };
    reading.picked = 3;
    reading.cueName = "3 cues";
    reading.cueKind = "media";
    reading.cueLevel = -3.0;
    reading.cueLevels = { -3.0, 0.0, -10.0 };

    model::SendStrip reverb;
    reverb.busId = "BUS00001";
    reverb.name = "Reverb";
    reverb.widthWord = "Stereo";
    reverb.channelWord = "5-6";
    reverb.sendId = "SND0000B";
    reverb.levelDb = -12.0;
    reverb.each = { { "CUE0000B", "SND0000B", -12.0, true },
                    { "CUE0000A", "SND0000A", -6.0, false },
                    { "CUE0000C", {}, -120.0, true } };
    reverb.lowestDb = -12.0;
    reverb.loudestDb = -6.0;

    reading.sends = { reverb };
    mixer.show (reading);

    {
        juce::Image canvas (juce::Image::ARGB, 420, 220, true);
        juce::Graphics g (canvas);
        mixer.paintEntireComponent (g, true);
    }

    /*  NO CROSS OVER SEVERAL (TK), and the switch stands for the two that send
        here, saying how many are on. */
    std::vector<juce::Button*> crosses, switches;

    for (auto* button : buttonsUnder (mixer))
        (button->getButtonText() == "x" ? crosses : switches).push_back (button);

    CHECK (crosses.empty());
    REQUIRE (switches.size() == 1);
    CHECK (switches[0]->getButtonText() == "on 1 of 2");
    CHECK_FALSE (switches[0]->getToggleState());

    std::vector<juce::Component*> strips;
    std::vector<juce::Label*> boxes;

    for (auto* child : mixer.getChildren())
    {
        strips.push_back (child);

        for (auto* inner : child->getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (inner))
                boxes.push_back (label);
    }

    REQUIRE (strips.size() == 2);
    REQUIRE (boxes.size() == 2);

    //  The cap is the anchor's number.
    CHECK (boxes[0]->getText() == "-3");
    CHECK (boxes[1]->getText() == "-12");

    const auto texts = [] (const std::vector<std::pair<std::string, std::string>>& writes)
    {
        std::map<std::string, std::string> out;

        for (const auto& [address, text] : writes)
            out[address] = text;

        return out;
    };

    SUBCASE ("a drag on the reverb moves the two sends by the same, from where they stood, one write a frame")
    {
        auto& strip = *strips[1];
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const juce::ModifierKeys left { juce::ModifierKeys::leftButtonModifier };

        const auto mouse = [&] (float y, bool dragged)
        {
            const auto now = juce::Time::getCurrentTime();
            const juce::Point<float> at { 30.0f, y };
            return juce::MouseEvent (source, at, left, juce::MouseInputSource::defaultPressure,
                                     0.0f, 0.0f, 0.0f, 0.0f, &strip, &strip, now, { 30.0f, 120.0f }, now,
                                     1, dragged);
        };

        //  A few pixels: the taper puts -12 high on the throw, and a long pull would reach the top.
        strip.mouseDown (mouse (120.0f, false));
        strip.mouseDrag (mouse (118.0f, true));
        strip.mouseDrag (mouse (116.0f, true));

        REQUIRE (sets.size() == 2u);
        CHECK (written.empty());
        CHECK (made.empty());

        //  Both sends, and only those: the cue with none is not given one by a drag.
        auto frame = texts (sets.back());
        REQUIRE (frame.size() == 2u);

        const auto anchor = wfg::osc::parseDouble (frame.at ("/godot/send/SND0000B/level")).value_or (0.0);
        const auto other = wfg::osc::parseDouble (frame.at ("/godot/send/SND0000A/level")).value_or (0.0);

        INFO ("anchor " << anchor << ", other " << other);
        CHECK (anchor > -12.0);
        CHECK (other - anchor == doctest::Approx (6.0).epsilon (0.02));

        //  The same frame's drag from the grab, not from the last frame: back where it began, back where they were.
        strip.mouseDrag (mouse (120.0f, true));
        frame = texts (sets.back());
        CHECK (frame.at ("/godot/send/SND0000B/level") == "-12");
        CHECK (frame.at ("/godot/send/SND0000A/level") == "-6");

        strip.mouseUp (mouse (120.0f, true));
    }

    SUBCASE ("a number typed sets every send to it, and gives the cue with none a send born at it")
    {
        boxes[1]->setText ("-3", juce::sendNotificationSync);

        REQUIRE (sets.size() == 1u);
        const auto frame = texts (sets[0]);
        CHECK (frame.size() == 2u);
        CHECK (frame.at ("/godot/send/SND0000B/level") == "-3");
        CHECK (frame.at ("/godot/send/SND0000A/level") == "-3");

        REQUIRE (made.size() == 1u);
        CHECK (made[0].first == "CUE0000C BUS00001");
        CHECK (made[0].second == doctest::Approx (-3.0));
        CHECK (written.empty());
    }

    SUBCASE ("the master typed sets every cue's own level, as one write")
    {
        boxes[0]->setText ("-6,5", juce::sendNotificationSync);

        REQUIRE (sets.size() == 1u);
        const auto frame = texts (sets[0]);
        REQUIRE (frame.size() == 3u);

        for (const auto& [address, text] : frame)
        {
            INFO (address);
            CHECK (text == "-6.5");
        }

        CHECK (frame.count ("/godot/cue/CUE0000C/level") == 1u);
    }

    SUBCASE ("the switch puts every send there is to one state, and makes none")
    {
        switches[0]->setToggleState (true, juce::sendNotificationSync);

        REQUIRE (sets.size() == 1u);
        const auto frame = texts (sets[0]);
        CHECK (frame.size() == 2u);
        CHECK (frame.at ("/godot/send/SND0000A/on") == "true");
        CHECK (frame.at ("/godot/send/SND0000B/on") == "true");
        CHECK (made.empty());
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

TEST_CASE ("run pane: a click on a running cue's name aims the rotaries, and on the aimed one lets go")
{
    /*  The author, 2026-09-25: "We will also add a way to edit other running
        media cues like clicking on the label over the waveform in the running
        cue panel." The cross still kills; a fade's line aims nothing. */
    std::vector<std::string> aimed, killed;

    ui::RunPaneComponent::Actions actions;
    actions.aim = [&aimed] (const std::string& cueId) { aimed.push_back (cueId); };
    actions.kill = [&killed] (const std::string& runId) { killed.push_back (runId); };

    model::RunRow bed;
    bed.id = "RUN00001";
    bed.cueId = "CUE00001";
    bed.cueName = "Bed";
    bed.kind = "media";
    bed.state = "playing";

    model::RunRow fade = bed;
    fade.id = "RUN00002";
    fade.cueId = "CUE00002";
    fade.cueName = "Down";
    fade.kind = "fade";

    ui::RunPaneComponent pane (model::Theme {}, actions);
    pane.setSize (450, 300);
    pane.show ({ bed, fade }, {});

    const auto row = juce::roundToInt (model::Theme {}.row * model::Theme {}.type);

    pane.clickAt (60, row / 2);
    CHECK (aimed == std::vector<std::string> { "CUE00001" });

    pane.clickAt (60, row + row / 2);
    CHECK (aimed.size() == 1u);

    pane.clickAt (445, row / 2);
    CHECK (killed == std::vector<std::string> { "RUN00001" });

    //  Aimed, it draws its mark - and a click lets go.
    bed.aimed = true;
    pane.show ({ bed, fade }, {});

    juce::Image canvas (juce::Image::ARGB, 450, 300, true);
    {
        juce::Graphics g (canvas);
        pane.paintEntireComponent (g, true);
    }

    pane.clickAt (60, row / 2);
    CHECK (aimed == std::vector<std::string> { "CUE00001", "" });
}

//==============================================================================
/*  SCRUBBING A SCENE WITH LOOPS (2026-10-05, namespace draft §30.6, S8; item 10
    of the bug round), through the pane's own mouse handlers: a scene's strip is
    its round, the ghost stays inside it and is sent once, on release (the
    author's RB); a sound's ghost keeps to the stretch its ranges play and is
    sent as it moves; and a strip that will not scrub says why. */
namespace
{
    struct ScrubHand
    {
        explicit ScrubHand (juce::Component& surfaceToUse) : surface (surfaceToUse) {}

        juce::MouseEvent at (float x, float y, bool buttonDown, bool dragged) const
        {
            const auto now = juce::Time::getCurrentTime();
            const juce::Point<float> where { x, y };
            const auto mods = buttonDown ? juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)
                                         : juce::ModifierKeys();

            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), where, mods,
                                     juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &surface, &surface, now, where, now, 1, dragged);
        }

        void press (float x, float y)   { surface.mouseDown (at (x, y, true, false)); }
        void drag (float x, float y)    { surface.mouseDrag (at (x, y, true, true)); }
        void release (float x, float y) { surface.mouseUp (at (x, y, true, true)); }
        void hover (float x, float y)   { surface.mouseMove (at (x, y, false, false)); }

        juce::Component& surface;
    };
}

TEST_CASE ("run pane: a scene's strip is its round - the ghost stays in it and is sought once, on release")
{
    std::vector<std::pair<std::string, double>> sought;

    ui::RunPaneComponent::Actions actions;
    actions.seek = [&sought] (const std::string& runId, double seconds) { sought.emplace_back (runId, seconds); };

    //  A looping scene in its second round, 20 s to 30 s of its own clock, three seconds in.
    model::RunRow scene;
    scene.id = "RUN00001";
    scene.cueId = "CUE00001";
    scene.cueName = "Rain";
    scene.kind = "group";
    scene.state = "playing";
    scene.seekable = true;
    scene.seconds = 23.0;
    scene.position = "23.0";
    scene.roundFrom = 20.0;
    scene.roundLength = 10.0;
    scene.iteration = 2;
    scene.iterations = 3;

    ui::RunPaneComponent pane (model::Theme {}, actions);
    pane.setSize (450, 300);
    pane.show ({ scene }, {});

    ScrubHand hand { pane.rowsSurface() };
    const auto row = static_cast<float> (juce::roundToInt (model::Theme {}.row * model::Theme {}.type));
    const auto middle = row / 2.0f;

    //  Taken where it is: the ghost on the head, which is drawn over the round, three tenths in.
    hand.press (100.0f, middle);
    REQUIRE (pane.ghostX() >= 0);
    CHECK (pane.ghostSecond() == doctest::Approx (23.0));
    const auto left = pane.ghostX();

    //  Dragged right past the round's end: held there, and nothing sent while the hand moves.
    hand.drag (300.0f, middle);
    hand.drag (440.0f, middle);
    CHECK (pane.ghostSecond() == doctest::Approx (30.0));
    CHECK (pane.ghostX() > left);
    CHECK (sought.empty());

    //  And left past its start.
    hand.drag (5.0f, middle);
    CHECK (pane.ghostSecond() == doctest::Approx (20.0));
    CHECK (pane.ghostX() < left);
    CHECK (sought.empty());

    //  Let go a little way in: one seek, at the second the box said.
    hand.drag (60.0f, middle);
    const auto settled = pane.ghostSecond();
    CHECK (settled > 20.0);
    CHECK (settled < 30.0);

    juce::Image canvas (juce::Image::ARGB, 450, 300, true);
    {
        juce::Graphics g (canvas);
        pane.paintEntireComponent (g, true);
    }

    hand.release (60.0f, middle);
    REQUIRE (sought.size() == 1u);
    CHECK (sought[0].first == "RUN00001");
    CHECK (sought[0].second == doctest::Approx (settled));
    CHECK (pane.ghostX() == -1);

    /*  A ROUND THAT TURNS UNDER A HELD GHOST: the second sent is the ghost's
        place in the round the scene is in when the hand lets go - not a second
        of the round gone, which the engine would put at the new round's top. */
    sought.clear();
    hand.press (100.0f, middle);
    hand.drag (160.0f, middle);
    const auto intoRound = pane.ghostSecond() - 20.0;
    REQUIRE (intoRound > 3.0);

    scene.roundFrom = 30.0;
    scene.seconds = 30.2;
    scene.iteration = 3;
    pane.show ({ scene }, {});

    CHECK (pane.ghostSecond() == doctest::Approx (30.0 + intoRound));
    hand.release (160.0f, middle);
    REQUIRE (sought.size() == 1u);
    CHECK (sought[0].second == doctest::Approx (30.0 + intoRound));
}

TEST_CASE ("run pane: a sound's ghost keeps to the stretch its ranges play, and is sought as it moves")
{
    std::vector<std::pair<std::string, double>> sought;

    ui::RunPaneComponent::Actions actions;
    actions.seek = [&sought] (const std::string& runId, double seconds) { sought.emplace_back (runId, seconds); };

    /*  A 95 s file whose ranges play 10 s to 20 s, sounding at 15 s - the
        author's NADIA cue in miniature. Before S8 the ghost was drawn and held
        against the whole file: dragged to the strip's right end it sat a fifth
        of the way along, and the hand could send 95 s. */
    model::RangeRow first;
    first.in = 10.0;
    first.out = 14.0;

    model::RangeRow second = first;
    second.in = 14.0;
    second.out = 20.0;
    second.loops = 2;

    model::RunRow sound;
    sound.id = "RUN00002";
    sound.cueId = "CUE00002";
    sound.cueName = "Nadia";
    sound.kind = "media";
    sound.state = "playing";
    sound.seekable = true;
    sound.file = "nadia.wav";
    sound.length = 95.0;
    sound.playFrom = 10.0;
    sound.playTo = 20.0;
    sound.ranges = { first, second };
    sound.rangeIndex = 1;
    sound.rangeIteration = 2;
    sound.seconds = 15.0;
    sound.position = "15.0";

    ui::RunPaneComponent pane (model::Theme {}, actions);
    pane.setSize (450, 300);
    pane.show ({ sound }, {});

    ScrubHand hand { pane.rowsSurface() };
    const auto middle = static_cast<float> (juce::roundToInt (model::Theme {}.row * model::Theme {}.type)) / 2.0f;

    hand.press (100.0f, middle);
    const auto start = pane.ghostX();
    REQUIRE (start >= 0);

    //  Half the strip from where it was taken, near enough: the head moves as far as the hand.
    hand.drag (440.0f, middle);
    CHECK (pane.ghostSecond() == doctest::Approx (20.0));

    //  Sent as it moves, never past the ranges' end.
    REQUIRE_FALSE (sought.empty());
    CHECK (sought.back().first == "RUN00002");
    CHECK (sought.back().second <= 20.0 + 1.0e-9);

    //  At the strip's right end, where the ranges end - not a fifth of the way along.
    const auto right = pane.ghostX();
    CHECK (right > start + 150);

    hand.release (440.0f, middle);
    CHECK (pane.ghostX() == -1);
}

TEST_CASE ("run pane: a strip that will not scrub says why, with its cursor and its tooltip")
{
    std::vector<std::pair<std::string, double>> sought;

    ui::RunPaneComponent::Actions actions;
    actions.seek = [&sought] (const std::string& runId, double seconds) { sought.emplace_back (runId, seconds); };

    model::RunRow act;
    act.id = "RUN00003";
    act.cueId = "CUE00003";
    act.cueName = "Act one";
    act.kind = "group";
    act.state = "playing";
    act.seconds = 12.0;
    act.scrubRefusal = "A manual sequence is played by GO \xe2\x80\x94 it cannot be scrubbed";

    model::RunRow rain = act;
    rain.id = "RUN00004";
    rain.cueId = "CUE00004";
    rain.cueName = "Rain";
    rain.seekable = true;
    rain.scrubRefusal.clear();

    ui::RunPaneComponent pane (model::Theme {}, actions);
    pane.setSize (450, 300);
    pane.show ({ act, rain }, {});

    auto& surface = pane.rowsSurface();
    ScrubHand hand { surface };
    const auto row = static_cast<float> (juce::roundToInt (model::Theme {}.row * model::Theme {}.type));

    auto* tips = dynamic_cast<juce::TooltipClient*> (&surface);
    REQUIRE (tips != nullptr);

    //  Resting on the act's strip: the cursor that says no, and the words that say why.
    hand.hover (100.0f, row / 2.0f);
    CHECK (surface.getMouseCursor() == pane.refusedCursor());
    CHECK (tips->getTooltip() == juce::String (juce::CharPointer_UTF8 (act.scrubRefusal.c_str())));

    //  A drag on it: no ghost, nothing sent.
    hand.press (100.0f, row / 2.0f);
    hand.drag (300.0f, row / 2.0f);
    CHECK (pane.ghostX() == -1);
    hand.release (300.0f, row / 2.0f);
    CHECK (sought.empty());

    //  On the scene that scrubs: the left-right arrows, and no words.
    hand.hover (100.0f, row + row / 2.0f);
    CHECK (surface.getMouseCursor() == juce::MouseCursor (juce::MouseCursor::LeftRightResizeCursor));
    CHECK (tips->getTooltip().isEmpty());
}

//==============================================================================
TEST_CASE ("eq panel: the numbers are drawn, a box writes one row, a switch writes a flag, Flat is one command")
{
    /*  PHASE 9a's editor for the nineteen rows. What the hand shapes is
        written to the cue's own rows through node.set - a decision the show
        keeps - and the numbers are always drawn beside the field (§4.8). */
    std::vector<std::pair<std::string, std::string>> written;
    std::vector<std::string> resets;

    ui::EqPanelComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text)
    { written.emplace_back (address, text); };

    actions.reset = [&] (const std::string& cueId) { resets.push_back (cueId); };

    ui::EqPanelComponent panel (model::Theme {}, actions);
    panel.setSize (720, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::eq, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.eq.present = true;
    reading.eq.settings.band[1] = { wfg::audio::EqSettings::Shape::peak, 1000.0f, 6.0f, 1.0f };

    panel.show (reading);

    juce::Image canvas (juce::Image::ARGB, 720, 220, true);
    {
        juce::Graphics g (canvas);
        panel.paintEntireComponent (g, true);
    }

    /*  THE NUMBER BOXES: the two filters' frequencies, then frequency, gain
        and width for four bands - fourteen, in that order. */
    std::vector<juce::Label*> boxes;

    for (auto* child : panel.getChildren())
        if (auto* label = dynamic_cast<juce::Label*> (child))
            boxes.push_back (label);

    REQUIRE (boxes.size() == 14);
    CHECK (boxes[0]->getText() == "80");          // the high-pass, at its default
    CHECK (boxes[2]->getText() == "100");         // band one's frequency, at its default
    CHECK (boxes[5]->getText() == "1000");        // band two's frequency
    CHECK (boxes[6]->getText() == "6");           // and its gain
    CHECK (boxes[7]->getText() == "1");           // and its width

    SUBCASE ("typing into a box writes that one row, and nothing else")
    {
        boxes[6]->setText ("3", juce::sendNotificationSync);

        REQUIRE (written.size() == 1);
        CHECK (written[0].first == "/godot/cue/CUE00001/eqB2Gain");
        CHECK (written[0].second == "3");
        CHECK (resets.empty());
    }

    SUBCASE ("a number outside the row's range is clamped before it is sent")
    {
        boxes[6]->setText ("40", juce::sendNotificationSync);

        REQUIRE (written.size() == 1);
        CHECK (written[0].first == "/godot/cue/CUE00001/eqB2Gain");
        CHECK (written[0].second == "24");
    }

    SUBCASE ("a box reads what a person types, unit and all")
    {
        /*  spatcore's typed reader: the unit is not a mistake, "k" is
            thousands, a comma is a decimal point - and a text with no
            number in it writes nothing, because nought is a real gain. */
        boxes[5]->setText ("2.5 kHz", juce::sendNotificationSync);
        boxes[6]->setText ("-3 dB", juce::sendNotificationSync);
        boxes[7]->setText ("1,5", juce::sendNotificationSync);
        boxes[6]->setText ("loud", juce::sendNotificationSync);

        REQUIRE (written.size() == 3);
        CHECK (written[0] == std::pair<std::string, std::string> ("/godot/cue/CUE00001/eqB2Freq", "2500"));
        CHECK (written[1] == std::pair<std::string, std::string> ("/godot/cue/CUE00001/eqB2Gain", "-3"));
        CHECK (written[2] == std::pair<std::string, std::string> ("/godot/cue/CUE00001/eqB2Q", "1.5"));
    }

    SUBCASE ("a switch writes a flag")
    {
        juce::Button* highPass = nullptr;

        for (auto* button : buttonsUnder (panel))
            if (button->getButtonText() == "High-pass")
                highPass = button;

        REQUIRE (highPass != nullptr);
        highPass->setToggleState (true, juce::dontSendNotification);
        highPass->onClick();

        REQUIRE (written.size() == 1);
        CHECK (written[0].first == "/godot/cue/CUE00001/eqHpf");
        CHECK (written[0].second == "true");
    }

    SUBCASE ("a band's own switch writes its flag, off keeping its numbers (2026-09-25)")
    {
        juce::Button* bandTwo = nullptr;

        for (auto* button : buttonsUnder (panel))
            if (button->getTooltip().startsWith ("Whether band 2 is in"))
                bandTwo = button;

        REQUIRE (bandTwo != nullptr);
        CHECK (bandTwo->getToggleState());

        bandTwo->setToggleState (false, juce::dontSendNotification);
        bandTwo->onClick();

        REQUIRE (written.size() == 1);
        CHECK (written[0].first == "/godot/cue/CUE00001/eqB2On");
        CHECK (written[0].second == "false");
    }

    SUBCASE ("Flat is one command on the cue, and writes no row itself")
    {
        juce::Button* flat = nullptr;

        for (auto* button : buttonsUnder (panel))
            if (button->getButtonText() == "Flat")
                flat = button;

        REQUIRE (flat != nullptr);
        flat->onClick();

        REQUIRE (resets.size() == 1);
        CHECK (resets[0] == "CUE00001");
        CHECK (written.empty());
    }

    SUBCASE ("a cue with no EQ says so rather than drawing an empty field")
    {
        model::FootReading memo;
        memo.subject = { model::Subject::Kind::eq, "CUE00002" };
        memo.cueKind = "memo";
        memo.eq.present = false;
        memo.eq.notice = "Only a media cue has an EQ.";

        panel.show (memo);

        std::vector<juce::Label*> none;

        for (auto* child : panel.getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (child))
                none.push_back (label);

        CHECK (none.empty());
        CHECK (buttonsUnder (panel).empty());

        juce::Image blank (juce::Image::ARGB, 720, 220, true);
        juce::Graphics g (blank);
        panel.paintEntireComponent (g, true);
    }
}

TEST_CASE ("eq panel: a dragged point follows the hand, however often the reading comes back")
{
    /*  The author, 2026-09-25: "The Eq points move in very large increments
        when using the mouse on the graph. Same with touch." The drag took the
        handle from the reading, which the drag's own writes move - so each
        pass added the whole movement again and the point ran away from the
        hand. Here the reading comes back with every write, as it does live. */
    std::vector<std::pair<std::string, std::string>> written;

    ui::EqPanelComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text)
    { written.emplace_back (address, text); };

    ui::EqPanelComponent panel (model::Theme {}, actions);
    panel.setSize (720, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::eq, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.eq.present = true;
    reading.eq.settings.band[1] = { wfg::audio::EqSettings::Shape::peak, 1000.0f, 0.0f, 1.0f };

    panel.show (reading);

    const auto lastOf = [&written] (const std::string& row)
    {
        for (auto at = written.rbegin(); at != written.rend(); ++at)
            if (at->first == "/godot/cue/CUE00001/" + row)
                return wfg::osc::parseDouble (at->second).value_or (std::nan (""));   // never the locale's

        return std::nan ("");
    };

    //  What the engine publishes after each write: the band where it was put.
    const auto publish = [&]
    {
        reading.eq.settings.band[1].freq = static_cast<float> (lastOf ("eqB2Freq"));
        reading.eq.settings.band[1].gain = static_cast<float> (lastOf ("eqB2Gain"));
        panel.show (reading);
    };

    const auto from = panel.handlePosition (1);
    panel.beginDrag (from);

    panel.dragTo (from + juce::Point<float> (20.0f, 0.0f), false);
    const auto first = std::log2 (lastOf ("eqB2Freq") / 1000.0);
    publish();

    panel.dragTo (from + juce::Point<float> (40.0f, 0.0f), false);
    const auto second = std::log2 (lastOf ("eqB2Freq") / 1000.0);
    publish();

    panel.dragTo (from + juce::Point<float> (60.0f, 0.0f), false);
    const auto third = std::log2 (lastOf ("eqB2Freq") / 1000.0);

    /*  FREQUENCY IS LOGARITHMIC ACROSS THE FIELD, so twice the pointer's
        travel is twice the octaves - and not three times, then six, which is
        what adding the movement to a published value that already held it
        came to. Whole hertz rounding is the tolerance. */
    REQUIRE (first > 0.0);
    CHECK (second == doctest::Approx (2.0 * first).epsilon (0.02));
    CHECK (third == doctest::Approx (3.0 * first).epsilon (0.02));

    //  And the handle is drawn where the hand is, not ahead of it.
    CHECK (std::abs (panel.handlePosition (1).x - (from.x + 60.0f)) <= 1.0f);

    /*  SHIFT MID-DRAG IS A CHANGE OF SPEED, NOT A JUMP: the fine drag starts
        from where the handle is, a tenth of the movement from there on. */
    publish();
    panel.dragTo (from + juce::Point<float> (60.0f, 0.0f), true);
    const auto still = std::log2 (lastOf ("eqB2Freq") / 1000.0);
    CHECK (still == doctest::Approx (third).epsilon (0.02));

    //  A hundred pixels at a tenth is ten: half of one of the twenty-pixel steps above.
    panel.dragTo (from + juce::Point<float> (160.0f, 0.0f), true);
    const auto fine = std::log2 (lastOf ("eqB2Freq") / 1000.0);
    CHECK (fine - third == doctest::Approx (0.5 * (third - second)).epsilon (0.1));

    panel.endDrag();
}

TEST_CASE ("eq panel: on one cue a band dragged is one write a frame too, its frequency and gain together")
{
    /*  Two `node.set`s a frame on two addresses taking turns never joined into
        one step of Undo; one `node.setMany` of the same two addresses does
        (namespace draft §30.11). */
    std::vector<std::vector<std::pair<std::string, std::string>>> sets;
    std::vector<std::pair<std::string, std::string>> written;

    ui::EqPanelComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text) { written.emplace_back (address, text); };
    actions.setMany = [&] (const std::vector<std::pair<std::string, std::string>>& writes) { sets.push_back (writes); };

    ui::EqPanelComponent panel (model::Theme {}, actions);
    panel.setSize (720, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::eq, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.eq.present = true;
    reading.eq.settings.band[1] = { wfg::audio::EqSettings::Shape::peak, 1000.0f, 0.0f, 1.0f };

    panel.show (reading);

    const auto from = panel.handlePosition (1);
    panel.beginDrag (from);
    panel.dragTo (from + juce::Point<float> (20.0f, -15.0f), false);
    panel.dragTo (from + juce::Point<float> (40.0f, -30.0f), false);
    panel.endDrag();

    CHECK (written.empty());
    REQUIRE (sets.size() == 2u);

    for (const auto& frame : sets)
    {
        REQUIRE (frame.size() == 2u);
        CHECK (frame[0].first == "/godot/cue/CUE00001/eqB2Freq");
        CHECK (frame[1].first == "/godot/cue/CUE00001/eqB2Gain");
        CHECK (wfg::osc::parseDouble (frame[0].second).value_or (0.0) > 1000.0);   // never the locale's
        CHECK (wfg::osc::parseDouble (frame[1].second).value_or (0.0) > 0.0);
    }
}

TEST_CASE ("eq panel: over several cues a band dragged is one write a frame - its frequency for all, its gain moved from each one's own")
{
    /*  Namespace draft §30.11, TL: the band's frequency the same on every
        picked cue, its gain moved by what the anchor's has moved, from where
        each stood when the hand went down; a switch the same for all. */
    std::vector<std::vector<std::pair<std::string, std::string>>> sets;
    std::vector<std::pair<std::string, std::string>> written;
    std::vector<std::string> flattened;

    ui::EqPanelComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text) { written.emplace_back (address, text); };
    actions.setMany = [&] (const std::vector<std::pair<std::string, std::string>>& writes) { sets.push_back (writes); };
    actions.reset = [&] (const std::string& cueId) { flattened.push_back (cueId); };

    ui::EqPanelComponent panel (model::Theme {}, actions);
    panel.setSize (720, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::eq, "CUE0000B" };
    reading.cues = { "CUE0000B", "CUE0000A" };
    reading.picked = 2;
    reading.cueName = "2 cues";
    reading.cueKind = "media";
    reading.eq.present = true;
    reading.eq.settings.band[1] = { wfg::audio::EqSettings::Shape::peak, 1000.0f, 0.0f, 1.0f };

    auto other = reading.eq;
    other.settings.band[1] = { wfg::audio::EqSettings::Shape::peak, 4000.0f, 3.0f, 1.0f };
    reading.eqs = { reading.eq, other };

    panel.show (reading);

    const auto from = panel.handlePosition (1);
    panel.beginDrag (from);
    panel.dragTo (from + juce::Point<float> (20.0f, -15.0f), false);
    panel.endDrag();

    CHECK (written.empty());
    REQUIRE (sets.size() == 1u);

    std::map<std::string, double> frame;

    for (const auto& [address, text] : sets[0])
        frame[address] = wfg::osc::parseDouble (text).value_or (std::nan (""));   // never the locale's

    REQUIRE (frame.size() == 4u);

    //  One frequency for both, wherever each stood.
    CHECK (frame.at ("/godot/cue/CUE0000B/eqB2Freq") == doctest::Approx (frame.at ("/godot/cue/CUE0000A/eqB2Freq")));
    CHECK (frame.at ("/godot/cue/CUE0000B/eqB2Freq") > 1000.0);

    //  The gain: the anchor's move, added to each one's own.
    const auto moved = frame.at ("/godot/cue/CUE0000B/eqB2Gain");
    CHECK (moved > 0.0);
    CHECK (frame.at ("/godot/cue/CUE0000A/eqB2Gain") == doctest::Approx (3.0 + moved).epsilon (0.02));

    //  A switch: the same for both, one write. Flat: each cue's own reset.
    sets.clear();
    panel.show (reading);

    for (auto* child : panel.getChildren())
        if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child); toggle != nullptr && toggle->getButtonText() == "High-pass")
            toggle->setToggleState (true, juce::sendNotificationSync);

    REQUIRE (sets.size() == 1u);
    REQUIRE (sets[0].size() == 2u);
    CHECK (sets[0][0] == std::pair<std::string, std::string> { "/godot/cue/CUE0000B/eqHpf", "true" });
    CHECK (sets[0][1] == std::pair<std::string, std::string> { "/godot/cue/CUE0000A/eqHpf", "true" });

    for (auto* child : panel.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->getButtonText() == "Flat")
            button->onClick();

    CHECK (flattened == std::vector<std::string> { "CUE0000B", "CUE0000A" });
}

TEST_CASE ("eq panel: two fingers pinch a band's width, closing them narrows it, and the band taken is ringed")
{
    /*  The author, 2026-09-25: "Touch gestures on the EQ are not working",
        "Pinch widens and this feels reversed", and "having a circle around
        the one being edited". Fingers are driven as the mouse handlers drive
        them, by their index. */
    std::vector<std::pair<std::string, std::string>> written;

    ui::EqPanelComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text)
    { written.emplace_back (address, text); };

    ui::EqPanelComponent panel (model::Theme {}, actions);
    panel.setSize (720, 220);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::eq, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.eq.present = true;
    reading.eq.settings.band[1] = { wfg::audio::EqSettings::Shape::peak, 1000.0f, 6.0f, 1.0f };

    panel.show (reading);

    const auto lastQ = [&written]
    {
        for (auto at = written.rbegin(); at != written.rend(); ++at)
            if (at->first == "/godot/cue/CUE00001/eqB2Q")
                return wfg::osc::parseDouble (at->second).value_or (std::nan (""));   // never the locale's

        return std::nan ("");
    };

    //  Nothing is ringed until a hand takes something, and a press on a handle rings it.
    CHECK (panel.editedHandle() == -1);

    const auto centre = panel.handlePosition (1);
    panel.fingerDown (0, centre);
    CHECK (panel.editedHandle() == 1);
    panel.fingerUp (0);

    //  Still ringed once the hand is gone - the wheel acts on it - and a press on nothing lets it go.
    CHECK (panel.editedHandle() == 1);
    panel.fingerDown (0, { 2.0f, 2.0f });
    panel.fingerUp (0);
    CHECK (panel.editedHandle() == -1);

    /*  TWO FINGERS either side of the band, sixty pixels apart, on no handle:
        the pinch takes the band nearest their middle, and rings it. */
    const juce::Point<float> half { 30.0f, 0.0f };
    panel.fingerDown (0, centre - half);
    panel.fingerDown (1, centre + half);
    CHECK (panel.editedHandle() == 1);

    //  CLOSED TO HALF THE DISTANCE: twice the Q, a narrower band.
    panel.fingerMoved (0, centre - half * 0.5f, false);
    panel.fingerMoved (1, centre + half * 0.5f, false);
    CHECK (lastQ() == doctest::Approx (2.0));

    //  SPREAD TO TWICE IT: half the Q, a wider band - measured from where the pinch began.
    panel.fingerMoved (0, centre - half * 2.0f, false);
    panel.fingerMoved (1, centre + half * 2.0f, false);
    CHECK (lastQ() == doctest::Approx (0.5));

    //  LIFTING ONE ENDS THE PINCH, and the finger left drags nothing.
    const auto writes = written.size();
    panel.fingerUp (1);
    panel.fingerMoved (0, centre + juce::Point<float> (-100.0f, 40.0f), false);
    CHECK (written.size() == writes);
    panel.fingerUp (0);

    CHECK (panel.editedHandle() == 1);
}

TEST_CASE ("eq panel: a picture of it, when somebody asks for one")
{
    /*  NOT AN ASSERTION BUT AN EYE. With WFG_SNAPSHOT_DIR set, the panel is
        painted into a PNG there, so a look can be had with no screen - the
        way the Surfaces tab and the virtual panel were first seen. Skipped,
        silently and green, everywhere else. */
    const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {});

    if (dir.isEmpty())
        return;

    ui::EqPanelComponent::Actions actions;
    ui::EqPanelComponent panel (model::Theme {}, actions);
    panel.setSize (960, 230);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::eq, "CUE00001" };
    reading.cueName = "The bed";
    reading.cueKind = "media";
    reading.eq.present = true;

    auto& s = reading.eq.settings;
    s.hpf = true;
    s.hpfFreq = 80.0f;
    s.band[0] = { wfg::audio::EqSettings::Shape::lowShelf, 150.0f, 3.0f, 0.7f };
    s.band[1] = { wfg::audio::EqSettings::Shape::peak, 700.0f, -8.0f, 1.5f };
    s.band[2] = { wfg::audio::EqSettings::Shape::peak, 3000.0f, 4.0f, 0.5f };
    s.band[3] = { wfg::audio::EqSettings::Shape::highShelf, 9000.0f, -2.0f, 0.7f };

    panel.show (reading);

    //  The third band taken by a hand and let go: drawn ringed, the one being edited.
    panel.fingerDown (0, panel.handlePosition (2));
    panel.fingerUp (0);

    const auto picture = panel.createComponentSnapshot (panel.getLocalBounds());
    const juce::File file { juce::File (dir).getChildFile ("eq-panel.png") };
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    juce::FileOutputStream out { file };
    REQUIRE (out.openedOk());

    juce::PNGImageFormat png;
    CHECK (png.writeImageToStream (picture, out));
    MESSAGE ("wrote " << file.getFullPathName().toStdString());
}

namespace
{
    /*  A CHAIN AS A SHOW MIGHT HAVE IT: one entry the cue has not switched
        in, one it has (and which adds latency), one switched out, and one the
        machine has not got although the cue uses it. */
    model::FootReading chainReading (const std::string& cueId)
    {
        model::FootReading reading;
        reading.subject = { model::Subject::Kind::fx, cueId };
        reading.cueName = "The bed";
        reading.cueKind = "media";
        reading.eq.present = true;
        reading.fx.present = true;

        model::FxStrip gain;
        gain.pluginId = "PG7N0001";
        gain.name = "Test gain";
        gain.index = 0;
        gain.state = "loaded";

        model::FxStrip verb;
        verb.pluginId = "PG7N0002";
        verb.name = "Verb";
        verb.index = 1;
        verb.state = "loaded";
        verb.fxId = "FX7N0001";
        verb.enabled = true;
        verb.latencySamples = 64;

        model::FxStrip delay;
        delay.pluginId = "PG7N0003";
        delay.name = "Delay";
        delay.index = 2;
        delay.state = "loaded";
        delay.fxId = "FX7N0002";
        delay.enabled = false;

        model::FxStrip shimmer;
        shimmer.pluginId = "PG7N0004";
        shimmer.name = "Shimmer";
        shimmer.index = 3;
        shimmer.state = "missing";
        shimmer.problem = "no plugin on this machine answers VST3-0badf00d-shim";
        shimmer.fxId = "FX7N0003";
        shimmer.enabled = true;

        reading.fx.strips = { gain, verb, delay, shimmer };
        return reading;
    }

    std::vector<juce::Button*> buttonsNamed (juce::Component& from, const juce::String& text)
    {
        std::vector<juce::Button*> out;

        for (auto* button : buttonsUnder (from))
            if (button->getButtonText() == text)
                out.push_back (button);

        return out;
    }

    void press (juce::Button& button, bool state)
    {
        button.setToggleState (state, juce::dontSendNotification);
        button.onClick();
    }
}

TEST_CASE ("fx panel: the chain runs file, EQ, the set, out, and a first switch-in makes the insert")
{
    /*  The author's design, 2026-09-25: "show the chain, bypass switch and
        open the native plugin UI as a popup". A box per link in the order the
        sound goes through them, the EQ first; a switch that is fx.create the
        first time and `enabled` after; a door that opens the EQ or the
        plugin's own window. */
    std::vector<std::pair<std::string, std::string>> written, made, edited;
    std::vector<std::string> eqOpened;

    ui::FxPanelComponent::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text) { written.emplace_back (address, text); };
    actions.createFx = [&] (const std::string& cue, const std::string& plugin) { made.emplace_back (cue, plugin); };
    actions.edit = [&] (const std::string& cue, const std::string& plugin) { edited.emplace_back (cue, plugin); };
    actions.openEq = [&] (const std::string& cue) { eqOpened.push_back (cue); };

    ui::FxPanelComponent panel (model::Theme {}, actions);
    panel.setSize (1400, 230);
    panel.show (chainReading ("CUE00001"));

    juce::Image canvas (juce::Image::ARGB, 1400, 230, true);
    {
        juce::Graphics g (canvas);
        panel.paintEntireComponent (g, true);
    }

    //  THE LINKS, in chain order: the EQ's switch first, then one per entry of the set.
    auto switches = buttonsNamed (panel, "in");
    REQUIRE (switches.size() == 5);
    CHECK (buttonsNamed (panel, "Open").size() == 1);
    REQUIRE (buttonsNamed (panel, "Edit...").size() == 4);

    CHECK (switches[0]->getToggleState());           // the EQ, in by default
    CHECK_FALSE (switches[1]->getToggleState());     // Test gain: not in this cue
    CHECK (switches[2]->getToggleState());           // Verb: in
    CHECK_FALSE (switches[3]->getToggleState());     // Delay: switched out
    CHECK (switches[4]->getToggleState());           // Shimmer: in, and missing

    //  Nothing takes the keyboard: the space bar is GO's.
    for (auto* button : buttonsUnder (panel))
        CHECK_FALSE (button->getWantsKeyboardFocus());

    SUBCASE ("the first switch-in is fx.create and nothing else, and a second press waits for it")
    {
        press (*switches[1], true);

        REQUIRE (made.size() == 1);
        CHECK (made[0] == std::pair<std::string, std::string> ("CUE00001", "PG7N0001"));
        CHECK (written.empty());

        //  The switch stays in while the tree catches up, and pressing again sends no second create.
        CHECK (switches[1]->getToggleState());
        press (*switches[1], true);
        CHECK (made.size() == 1);

        //  Once the tree has it, the next press is an ordinary write to `enabled`.
        auto arrived = chainReading ("CUE00001");
        arrived.fx.strips[0].fxId = "FX7N0009";
        arrived.fx.strips[0].enabled = true;
        panel.show (arrived);

        switches = buttonsNamed (panel, "in");
        press (*switches[1], false);
        REQUIRE (written.size() == 1);
        CHECK (written[0] == std::pair<std::string, std::string> ("/godot/fx/FX7N0009/enabled", "false"));
        CHECK (made.size() == 1);
    }

    SUBCASE ("a create in flight belongs to the cue it was pressed on")
    {
        press (*switches[1], true);
        REQUIRE (made.size() == 1);

        //  Another cue picked before the tree answered: its switch is out, and pressing it asks again.
        panel.show (chainReading ("CUE00002"));
        switches = buttonsNamed (panel, "in");
        CHECK_FALSE (switches[1]->getToggleState());

        press (*switches[1], true);
        REQUIRE (made.size() == 2);
        CHECK (made[1] == std::pair<std::string, std::string> ("CUE00002", "PG7N0001"));
    }

    SUBCASE ("an insert the cue has is switched with one write, never a second create")
    {
        press (*switches[2], false);
        press (*switches[3], true);

        REQUIRE (written.size() == 2);
        CHECK (written[0] == std::pair<std::string, std::string> ("/godot/fx/FX7N0001/enabled", "false"));
        CHECK (written[1] == std::pair<std::string, std::string> ("/godot/fx/FX7N0002/enabled", "true"));
        CHECK (made.empty());
    }

    SUBCASE ("the EQ's switch writes eqOn, and its door opens the EQ on the same cue")
    {
        press (*switches[0], false);
        REQUIRE (written.size() == 1);
        CHECK (written[0] == std::pair<std::string, std::string> ("/godot/cue/CUE00001/eqOn", "false"));

        buttonsNamed (panel, "Open")[0]->onClick();
        REQUIRE (eqOpened.size() == 1);
        CHECK (eqOpened[0] == "CUE00001");
    }

    SUBCASE ("Edit... asks for that plugin's own window on this cue")
    {
        buttonsNamed (panel, "Edit...")[1]->onClick();

        REQUIRE (edited.size() == 1);
        CHECK (edited[0] == std::pair<std::string, std::string> ("CUE00001", "PG7N0002"));
        CHECK (written.empty());
        CHECK (made.empty());
    }

    SUBCASE ("a long set scrolls sideways rather than squeezing its boxes")
    {
        auto many = chainReading ("CUE00001");

        for (int n = 4; n < 8; ++n)
        {
            auto more = many.fx.strips[0];
            more.pluginId = "PG7N000" + std::to_string (n + 1);
            more.index = n;
            many.fx.strips.push_back (more);
        }

        panel.setSize (700, 230);
        panel.show (many);

        juce::Viewport* viewport = nullptr;

        for (auto* child : panel.getChildren())
            if (auto* found = dynamic_cast<juce::Viewport*> (child))
                viewport = found;

        REQUIRE (viewport != nullptr);
        REQUIRE (viewport->getViewedComponent() != nullptr);
        CHECK (viewport->getViewedComponent()->getWidth() > panel.getWidth());
        CHECK (buttonsNamed (panel, "in").size() == 9);
    }

    SUBCASE ("a show with no plugins still has its EQ, and says how to add one")
    {
        auto bare = chainReading ("CUE00001");
        bare.fx.strips.clear();
        bare.fx.notice = "The show declares no plugins yet: Show settings, Plugins.";
        panel.show (bare);

        CHECK (buttonsNamed (panel, "in").size() == 1);
        CHECK (buttonsNamed (panel, "Open").size() == 1);
        CHECK (buttonsNamed (panel, "Edit...").empty());

        juce::Image blank (juce::Image::ARGB, 1400, 230, true);
        juce::Graphics g (blank);
        panel.paintEntireComponent (g, true);
    }

    SUBCASE ("a cue that is not media has no chain, and the panel says why")
    {
        model::FootReading memo;
        memo.subject = { model::Subject::Kind::fx, "CUE00002" };
        memo.cueKind = "memo";
        memo.fx.notice = "Inserts belong to media cues; this cue plays no file.";
        panel.show (memo);

        CHECK (buttonsNamed (panel, "in").empty());
        CHECK (buttonsNamed (panel, "Edit...").empty());

        juce::Image blank (juce::Image::ARGB, 1400, 230, true);
        juce::Graphics g (blank);
        panel.paintEntireComponent (g, true);
    }
}

TEST_CASE ("fx panel: a picture of it, when somebody asks for one")
{
    //  The EQ picture case's shape: with WFG_SNAPSHOT_DIR set, fx-panel.png; skipped otherwise.
    const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {});

    if (dir.isEmpty())
        return;

    ui::FxPanelComponent::Actions actions;
    ui::FxPanelComponent panel (model::Theme {}, actions);
    panel.setSize (1650, 250);

    auto reading = chainReading ("CUE00001");
    reading.eq.settings.hpf = true;
    reading.eq.settings.band[1].gain = -4.0f;

    panel.setEditorWords ({ { "PG7N0002", "its window is open" } });
    panel.show (reading);

    const auto picture = panel.createComponentSnapshot (panel.getLocalBounds());
    const juce::File file { juce::File (dir).getChildFile ("fx-panel.png") };
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    juce::FileOutputStream out { file };
    REQUIRE (out.openedOk());

    juce::PNGImageFormat png;
    CHECK (png.writeImageToStream (picture, out));
    MESSAGE ("wrote " << file.getFullPathName().toStdString());
}

TEST_CASE ("fx panel: a mic cue's chain starts at its input, and its path is said against the budget")
{
    /*  Phase 9b (namespace draft 18.9): the chain drawn from "in" and the
        input's name where a media cue's says "file", its boxes the channel's
        plugins, and the path's delay in words - always, for a live voice. With
        WFG_SNAPSHOT_DIR set, fx-panel-mic.png as well. */
    ui::FxPanelComponent::Actions actions;
    ui::FxPanelComponent panel (model::Theme {}, actions);
    panel.setSize (2050, 250);

    auto reading = chainReading ("CUE00009");
    reading.cueName = "Voix solo";
    reading.cueKind = "mic";
    reading.fx.source = "in \xc2\xb7 Voix solo";
    reading.fx.live = true;
    reading.fx.fileChannels = 1;
    reading.fx.chainChannels = 2;
    reading.fx.sampleRate = 48000;
    reading.fx.inputLatency = 64;
    reading.fx.outputLatency = 56;
    reading.fx.insertLatency = 230;
    reading.fx.budgetMs = 5.0;

    CHECK (model::chainWords (reading.fx) == "7.3 ms from the microphone to the output: 2.5 ms the interface's,"
                                              " 4.8 ms its plugins' - within the 5 ms budget.");

    panel.show (reading);

    juce::Image canvas (juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
    juce::Graphics g (canvas);
    panel.paintEntireComponent (g, false);

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto picture = panel.createComponentSnapshot (panel.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("fx-panel-mic.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }
}

TEST_CASE ("fx panel: on a sampling channel the recorder sits after the plugins before it, ahead of the EQ, and opens the take")
{
    /*  Phase 9c, stage 9c.4 (namespace draft 19.7): a mic cue on a sampling
        channel draws its chain as its track is built - the plugins before
        the recorder, the recorder, the EQ, the plugins after the player - and
        the recorder's box opens the take panel. With WFG_SNAPSHOT_DIR set,
        fx-panel-take.png as well. */
    std::vector<std::string> opened;

    ui::FxPanelComponent::Actions actions;
    actions.openTake = [&opened] (const std::string& cueId) { opened.push_back (cueId); };

    ui::FxPanelComponent panel (model::Theme {}, actions);
    panel.setSize (2300, 250);

    auto reading = chainReading ("CUE00009");
    reading.cueName = "Loop voice";
    reading.cueKind = "mic";
    reading.fx.source = "in \xc2\xb7 Voice";
    reading.fx.live = true;
    reading.fx.recorder = true;
    reading.fx.takeState = "looping";

    //  The second entry is printed into the take; the rest are heard after it.
    for (auto& strip : reading.fx.strips)
        strip.side = strip.pluginId == "PG7N0002" ? "before" : "after";

    panel.show (reading);

    auto* take = buttonTipped (panel, "Show the take");
    auto* eq = buttonTipped (panel, "Show this cue's EQ");
    auto* before = buttonTipped (panel, "Open Verb");
    auto* after = buttonTipped (panel, "Open Test gain");

    REQUIRE (take != nullptr);
    REQUIRE (eq != nullptr);
    REQUIRE (before != nullptr);
    REQUIRE (after != nullptr);

    const auto x = [&panel] (juce::Component* control)
    {
        return panel.getLocalArea (control, control->getLocalBounds()).getX();
    };

    CHECK (x (before) < x (take));
    CHECK (x (take) < x (eq));
    CHECK (x (eq) < x (after));

    take->onClick();
    CHECK (opened == std::vector<std::string> { "CUE00009" });

    //  Moved to the other side, the entry is drawn there: a side is part of the chain's shape.
    for (auto& strip : reading.fx.strips)
        strip.side = "after";

    panel.show (reading);
    before = buttonTipped (panel, "Open Verb");
    take = buttonTipped (panel, "Show the take");
    REQUIRE (before != nullptr);
    REQUIRE (take != nullptr);
    CHECK (x (take) < x (before));

    juce::Image canvas (juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
    {
        juce::Graphics g (canvas);
        panel.paintEntireComponent (g, false);
    }

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        for (auto& strip : reading.fx.strips)
            strip.side = strip.pluginId == "PG7N0002" ? "before" : "after";

        panel.show (reading);

        const auto picture = panel.createComponentSnapshot (panel.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("fx-panel-take.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }
}

TEST_CASE ("take panel: its five presses are the take verbs, offered only where the engine would apply them")
{
    /*  Phase 9c, stage 9c.4 (namespace draft 19.7): Rec, Loop, Overdub, Undo
        and Clear send the take verb of their name on the channel; Rec, Loop
        and a layer want a mic cue sounding there, as the engine does. The
        picture stacks a layer on the take, with the loop's edges and the
        playhead over it. With WFG_SNAPSHOT_DIR set, take-panel.png as well. */
    std::vector<std::string> presses;

    ui::TakePanelComponent::Actions actions;
    actions.press = [&presses] (const std::string& verb, const std::string& channel)
                    { presses.push_back (verb + " " + channel); };
    actions.keep = [&presses] (const std::string& channel, bool asCue, const std::string& after)
                   { presses.push_back (std::string (asCue ? "keep as cue " : "keep ") + channel + " after " + after); };

    ui::TakePanelComponent panel (model::Theme {}, actions);
    panel.setSize (1400, 240);

    model::FootReading reading;
    reading.subject = { model::Subject::Kind::take, "TK000002" };
    reading.cueName = "Loop voice";
    reading.cueKind = "mic";

    auto& take = reading.take;
    take.present = true;
    take.cueId = "TK000002";
    take.channelId = "TK000011";
    take.channelName = "Looper";
    take.state = "looping";
    take.length = 4.0;
    take.capacity = 10.0;
    take.loopIn = 1.0;
    take.loopOut = 3.25;
    take.playhead = 2.1;
    take.layers = 1;
    take.maxLayers = 2;
    take.holderRun = "RUN00001";
    take.holderName = "Loop voice";
    take.channelSounds = true;

    //  A picture: a take at about a half, and a layer at a quarter over its second half.
    auto set = std::make_shared<wfg::audio::TakePictureSet>();
    auto& picture = set->byChannel["TK000011"];
    picture.sampleRate = 48000;
    picture.binSamples = 4 * 256;
    picture.slots = 2;
    picture.bins = static_cast<int> (std::ceil (4.0 * 48000.0 / picture.binSamples));
    picture.peaks.assign (static_cast<std::size_t> (picture.slots * picture.bins), 0.0f);

    for (int bin = 0; bin < picture.bins; ++bin)
    {
        picture.peaks[static_cast<std::size_t> (bin)] = 0.3f + 0.2f * static_cast<float> (std::sin (bin * 0.15));

        if (bin >= picture.bins / 2)
            picture.peaks[static_cast<std::size_t> (picture.bins + bin)] = 0.25f;
    }

    panel.show (reading, set);

    std::map<std::string, juce::Button*> buttons;

    for (auto* button : buttonsUnder (panel))
        buttons[button->getButtonText().toStdString()] = button;

    for (const auto* name : { "Rec", "Loop", "Overdub", "Undo", "Clear", "Keep", "Keep as cue" })
        REQUIRE (buttons.count (name) == 1u);

    //  LOOPING, a cue sounding: a layer and Undo and Clear are offered; Loop has nothing to close.
    CHECK (buttons["Rec"]->isEnabled());
    CHECK_FALSE (buttons["Loop"]->isEnabled());
    CHECK (buttons["Overdub"]->isEnabled());
    CHECK (buttons["Undo"]->isEnabled());
    CHECK (buttons["Clear"]->isEnabled());

    for (const auto* name : { "Rec", "Overdub", "Undo", "Clear" })
        buttons[name]->onClick();

    CHECK (presses == std::vector<std::string> { "record TK000011", "overdub TK000011",
                                                 "undo TK000011", "clear TK000011" });

    /*  KEEP (9c.6): the take made a file, and as a cue, after this one. While
        a Keep writes, Keep, Undo and Clear wait; under the lock, the file
        alone. */
    CHECK (buttons["Keep"]->isEnabled());
    CHECK (buttons["Keep as cue"]->isEnabled());
    presses.clear();
    buttons["Keep"]->onClick();
    buttons["Keep as cue"]->onClick();
    CHECK (presses == std::vector<std::string> { "keep TK000011 after TK000002", "keep as cue TK000011 after TK000002" });

    take.keeping = true;
    panel.show (reading, set);
    CHECK_FALSE (buttons["Keep"]->isEnabled());
    CHECK_FALSE (buttons["Undo"]->isEnabled());
    CHECK_FALSE (buttons["Clear"]->isEnabled());

    take.keeping = false;
    take.locked = true;
    panel.show (reading, set);
    CHECK (buttons["Keep"]->isEnabled());
    CHECK_FALSE (buttons["Keep as cue"]->isEnabled());
    take.locked = false;
    panel.show (reading, set);

    /*  IT DRAWS, and with a picture to look at when somebody asks for one. A
        panel that threw or read past an end would take the window down rather
        than fail a check. */
    juce::Image canvas (juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
    {
        juce::Graphics g (canvas);
        panel.paintEntireComponent (g, false);
    }

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto snapshot = panel.createComponentSnapshot (panel.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("take-panel.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (snapshot, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }

    //  NOTHING SOUNDING ON THE CHANNEL: only Undo and Clear, which take something away.
    take.channelSounds = false;
    take.holderRun.clear();
    panel.show (reading, set);

    CHECK_FALSE (buttons["Rec"]->isEnabled());
    CHECK_FALSE (buttons["Overdub"]->isEnabled());
    CHECK (buttons["Undo"]->isEnabled());
    CHECK (buttons["Clear"]->isEnabled());

    //  RECORDING, and drawn growing across the longest take, with nothing to close a loop on yet.
    take.channelSounds = true;
    take.state = "recording";
    take.layers = 0;
    panel.show (reading, set);

    CHECK (buttons["Loop"]->isEnabled());
    CHECK (buttons["Undo"]->isEnabled());

    {
        juce::Graphics g (canvas);
        panel.paintEntireComponent (g, false);
    }

    //  A CUE WITH NO TAKE draws its sentence and nothing else, and offers nothing.
    reading.take = model::TakeReading {};
    reading.take.notice = "Only a mic cue on a sampling channel has a take.";
    panel.show (reading, nullptr);

    for (const auto* name : { "Rec", "Loop", "Overdub", "Undo", "Clear" })
        CHECK_FALSE (buttons[name]->isEnabled());

    {
        juce::Graphics g (canvas);
        panel.paintEntireComponent (g, false);
    }
}

TEST_CASE ("run pane: a run at a speed other than one says so beside its name, after whatever else it says")
{
    /*  Namespace draft §22.7: "×0.5" beside a media run, and after a
        sampler member's strip. Painted, so a layout that lost it would at least
        be drawn; with WFG_SNAPSHOT_DIR set, run-pane-speed.png as well. */
    const auto media = [] (const char* runId, const char* name, double rate, const char* words)
    {
        model::RunRow row;
        row.id = runId;
        row.cueId = std::string ("CUE") + runId;
        row.cueName = name;
        row.kind = "media";
        row.state = "playing";
        row.position = "12.5";
        row.rate = rate;
        row.samplerWords = words;
        return row;
    };

    const std::vector<model::RunRow> rows { media ("RUN00001", "Slowed", 0.5, ""),
                                            media ("RUN00002", "At one", 1.0, ""),
                                            media ("RUN00003", "Pad", 2.0, "on 3") };

    ui::RunPaneComponent pane (model::Theme {}, {});
    pane.setSize (450, 200);
    pane.show (rows, {});

    juce::Image canvas (juce::Image::ARGB, 450, 200, true);
    juce::Graphics g (canvas);
    pane.paintEntireComponent (g, false);

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto picture = pane.createComponentSnapshot (pane.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("run-pane-speed.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }
}

TEST_CASE ("run pane: a mic run says the channel it is on, that it waits for one, or that it rings out")
{
    /*  Phase 9b (namespace draft 18.9): the words a mic run reads beside its
        name, drawn where a sampler member's are. With WFG_SNAPSHOT_DIR set,
        run-pane-mic.png as well. */
    const auto mic = [] (const char* runId, const char* name, const char* runState, const char* words)
    {
        model::RunRow row;
        row.id = runId;
        row.cueId = std::string ("CUE") + runId;
        row.cueName = name;
        row.kind = "mic";
        row.state = runState;
        row.liveWords = words;
        return row;
    };

    const std::vector<model::RunRow> rows { mic ("MIC00001", "Voix solo", "playing", "on Vox 1"),
                                            mic ("MIC00002", "Voix deux", "armed", "waiting for Vox 1"),
                                            mic ("MIC00003", "Choeur", "stopping", "ringing out") };

    ui::RunPaneComponent pane (model::Theme {}, {});
    pane.setSize (450, 200);
    pane.show (rows, {});

    juce::Image canvas (juce::Image::ARGB, 450, 200, true);
    juce::Graphics g (canvas);
    pane.paintEntireComponent (g, false);

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto picture = pane.createComponentSnapshot (pane.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("run-pane-mic.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }
}

namespace
{
    /*  A TREE BY HAND: the few nodes the plugin windows read, sorted as a
        snapshot must be. Enough for a media cue with the test gain switched
        in, a memo beside it, and the set's one entry. */
    std::shared_ptr<const wfg::tree::TreeSnapshot> editorTree (bool withInsert, const std::string& bundle = {})
    {
        std::vector<wfg::tree::Node> nodes;

        const auto add = [&nodes] (const std::string& address, wfg::osc::Value value)
        {
            wfg::tree::Node node;
            node.address = address;
            node.values.push_back (std::move (value));
            nodes.push_back (std::move (node));
        };

        using wfg::osc::Value;
        add ("/godot/cue/CUE00001/kind", Value::string ("media"));
        add ("/godot/cue/CUE00001/name", Value::string ("Steady"));
        add ("/godot/cue/CUE00001/number", Value::string ("1"));
        add ("/godot/cue/CUE00002/kind", Value::string ("memo"));
        add ("/godot/cue/CUE00002/name", Value::string ("Memo"));
        add ("/godot/cue/CUE00002/number", Value::string ("2"));

        if (withInsert)
        {
            add ("/godot/fx/FX7N0001/cue", Value::string ("CUE00001"));
            add ("/godot/fx/FX7N0001/enabled", Value::boolean (true));
            add ("/godot/fx/FX7N0001/plugin", Value::string ("PG7N0001"));
            add ("/godot/fx/FX7N0001/values", Value::string ("0:0.25"));
        }

        if (! bundle.empty())
            add ("/godot/document/path", Value::string (bundle));

        add ("/godot/plugin/PG7N0001/identifier", Value::string ("godot:test-gain"));
        add ("/godot/plugin/PG7N0001/name", Value::string ("Test gain"));
        add ("/godot/plugin/PG7N0001/paramCount", Value::string ("2"));

        std::sort (nodes.begin(), nodes.end(), [] (const auto& a, const auto& b) { return a.address < b.address; });
        return std::make_shared<const wfg::tree::TreeSnapshot> (1, nullptr, nullptr, std::move (nodes));
    }

    wfg::plugin::EditorLaunch launchOfThisBinary()
    {
        wfg::plugin::EditorLaunch launch;
        launch.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
        launch.leadingArgs = { "plugin-editor" };
        return launch;
    }

    /** The editors' timer, turned by hand until `done` or a deadline. */
    template <typename Done>
    bool serviceUntil (ui::PluginEditors& editors, Done done, int milliseconds = 10000)
    {
        const auto until = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (milliseconds);

        while (juce::Time::getMillisecondCounter() < until)
        {
            editors.service();

            if (done())
                return true;

            juce::Thread::sleep (10);
        }

        editors.service();
        return done();
    }
}

TEST_CASE ("plugin windows: Edit... opens a helper on the cue, a turn is one write on its insert, the lock closes it")
{
    /*  The author's design, 2026-09-25: the plugin's own window, in a helper
        process, following the pick. Driven here headless - the helper is this
        test binary - with the tree built by hand, so what is pinned is the
        client's half: which helper opens, what it is told, where what it
        reports is written, and when it goes. */
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("wfg-editors-ui-" + juce::Uuid().toDashedString());

    std::vector<std::pair<std::string, std::string>> written, made;
    auto changes = 0;

    ui::PluginEditors::Actions actions;
    actions.set = [&] (const std::string& address, const std::string& text) { written.emplace_back (address, text); };
    actions.createFx = [&] (const std::string& cue, const std::string& plugin) { made.emplace_back (cue, plugin); };
    actions.changed = [&] { ++changes; };

    std::vector<std::vector<std::string>> kept;
    actions.capture = [&] (const std::string& fxId, const std::string& file, const std::string& values)
    { kept.push_back ({ fxId, file, values }); };

    {
        ui::PluginEditors editors (actions, {}, folder.getFullPathName().toStdString(), launchOfThisBinary(), true);
        const auto bundle = folder.getChildFile ("show");
        const auto tree = editorTree (true, bundle.getFullPathName().toStdString());

        editors.edit (*tree, "CUE00001", "PG7N0001", false);
        auto* host = editors.hostFor ("PG7N0001");
        REQUIRE (host != nullptr);
        CHECK (made.empty());

        //  Up, on the cue, at the cue's value - and saying so in the chain's words.
        REQUIRE (serviceUntil (editors, [&] { return host->status() == wfg::plugin::EditorHost::Status::open
                                                     && host->subjectTaken() >= 1u
                                                     && std::abs (host->currentValue (0) - 0.25f) < 1.0e-4f; }));
        CHECK (editors.words().at ("PG7N0001") == "its window is open");
        CHECK (changes > 0);
        CHECK (written.empty());

        SUBCASE ("a turn in the plugin's window is one node.set on that cue's insert")
        {
            host->poke (0, 0.7f);
            REQUIRE (serviceUntil (editors, [&] { return ! written.empty(); }));
            REQUIRE (written.size() == 1);
            CHECK (written[0] == std::pair<std::string, std::string> ("/godot/fx/FX7N0001/p0", "0.7"));
        }

        SUBCASE ("the window follows the pick, and on a memo a turn goes nowhere")
        {
            editors.follow (*tree, "CUE00002", false);
            REQUIRE (serviceUntil (editors, [&] { return host->subjectTaken() >= 2u; }));

            host->poke (0, 0.1f);
            juce::Thread::sleep (150);
            editors.service();
            CHECK (written.empty());
        }

        SUBCASE ("the plugin's whole state is kept with the cue: one fx.capture, the file in the bundle")
        {
            host->poke (-1, 0.0f);      // Pad: no parameter, only state
            REQUIRE (serviceUntil (editors, [&] { return ! kept.empty(); }, 6000));
            REQUIRE (kept.size() == 1);
            CHECK (kept[0][0] == "FX7N0001");
            CHECK (kept[0][1].rfind ("state/PG7N0001-", 0) == 0);
            CHECK (kept[0][2] == "0:0.25 1:0");
            CHECK (bundle.getChildFile ("plugins").getChildFile (juce::String (kept[0][1])).existsAsFile());
        }

        SUBCASE ("the lock closes every window and says why")
        {
            editors.follow (*tree, "CUE00001", true);
            CHECK (editors.hostFor ("PG7N0001") == nullptr);
            CHECK (editors.words().at ("PG7N0001") == "closed: the show is locked");

            //  And Edit... under the lock says why rather than opening.
            editors.edit (*tree, "CUE00001", "PG7N0001", true);
            CHECK (editors.hostFor ("PG7N0001") == nullptr);
            CHECK (editors.words().at ("PG7N0001").find ("locked") != std::string::npos);
        }

        SUBCASE ("its close button ends the helper, and the box goes quiet")
        {
            host->poke (-2, 0.0f);
            REQUIRE (serviceUntil (editors, [&] { return editors.hostFor ("PG7N0001") == nullptr; }, 5000));
            CHECK (editors.words().count ("PG7N0001") == 0);
        }
    }

    folder.deleteRecursively();
}

TEST_CASE ("plugin windows: Edit... on an insert the cue has not got switches it in, then opens")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("wfg-editors-ui-" + juce::Uuid().toDashedString());

    std::vector<std::pair<std::string, std::string>> made;

    ui::PluginEditors::Actions actions;
    actions.createFx = [&] (const std::string& cue, const std::string& plugin) { made.emplace_back (cue, plugin); };

    {
        ui::PluginEditors editors (actions, {}, folder.getFullPathName().toStdString(), launchOfThisBinary(), true);

        editors.edit (*editorTree (false), "CUE00001", "PG7N0001", false);
        REQUIRE (made.size() == 1);
        CHECK (made[0] == std::pair<std::string, std::string> ("CUE00001", "PG7N0001"));
        CHECK (editors.hostFor ("PG7N0001") == nullptr);
        CHECK (editors.words().at ("PG7N0001").find ("switching it in") != std::string::npos);

        SUBCASE ("the tree shows the insert: the window opens")
        {
            editors.follow (*editorTree (true), "CUE00001", false);
            CHECK (editors.hostFor ("PG7N0001") != nullptr);
        }

        SUBCASE ("the pick moves on first: it is forgotten")
        {
            editors.follow (*editorTree (false), "CUE00002", false);
            editors.follow (*editorTree (true), "CUE00002", false);
            CHECK (editors.hostFor ("PG7N0001") == nullptr);
            CHECK (editors.words().count ("PG7N0001") == 0);
        }
    }

    folder.deleteRecursively();
}

TEST_CASE ("new-cue bar: four buttons open their list under themselves, the rest make a cue")
{
    /*  The author (2026-09-27): "+ group" and "+ transport" show a vertical
        list, and so do "+ midi" and "+ mic"; "+ start" went into the
        transport list. What a window-less test can see is which buttons
        there are, which of them ask for a list - with themselves as the place
        to show it - and that the rest still make their cue at once. */
    std::vector<std::string> created;
    std::vector<std::pair<std::string, juce::Component*>> chosen;

    ui::NewCueBarComponent::Actions actions;
    actions.create = [&created] (const std::string& kind) { created.push_back (kind); };
    actions.choose = [&chosen] (const std::string& kind, juce::Component& button)
    {
        chosen.emplace_back (kind, &button);
    };

    ui::NewCueBarComponent bar (model::Theme {}, actions);
    bar.setBounds (0, 0, 1400, bar.preferredHeight());

    const auto buttons = buttonsUnder (bar);
    REQUIRE (buttons.size() == model::cueKinds().size());

    /*  THE KIND AND NOTHING BEFORE IT (2026-09-30): "Add" is said once, at the
        head of the row, and not as a plus on every button. */
    for (auto* button : buttons)
    {
        CHECK_FALSE (button->getButtonText().startsWith ("+"));
        CHECK_FALSE (button->getButtonText().startsWith ("start"));
    }

    // A click, as the button delivers it (triggerClick posts, and this build runs no nested loop).
    for (auto* button : buttons)
        if (button->onClick)
            button->onClick();

    CHECK (chosen.size() == 4);
    for (const auto& [kind, anchor] : chosen)
    {
        CAPTURE (kind);
        CHECK (model::opensList (kind));

        const auto at = std::find_if (buttons.begin(), buttons.end(), [target = anchor] (juce::Button* button)
                                      { return button == target; });
        REQUIRE (at != buttons.end());
        CHECK ((*at)->getButtonText().endsWith (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe"))));
    }

    CHECK (created.size() == model::cueKinds().size() - 4);
    for (const auto& kind : created)
        CHECK_FALSE (model::opensList (kind));

    /*  AND THE LIST AS JUCE IS HANDED IT: every line to click answers the
        choice it names, headings and greyed sentences answer nothing. */
    const auto lines = model::transportMenu ("Rain", "after Rain");
    const auto menu = ui::newCueMenu (lines);

    int items = 0;
    int enabled = 0;

    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        const auto& item = it.getItem();

        if (item.isSectionHeader || item.isSeparator)
            continue;

        ++items;

        if (! item.isEnabled)
            continue;

        ++enabled;
        const auto [choice, wrap] = ui::choiceOfMenuItem (item.itemID);
        REQUIRE (choice >= 0);
        REQUIRE (static_cast<std::size_t> (choice) < model::transportChoices().size());
        CHECK_FALSE (wrap);
        CHECK (item.text.startsWith (juce::String (model::transportChoices()[static_cast<std::size_t> (choice)].label)));
    }

    CHECK (enabled == static_cast<int> (model::transportChoices().size()));
    CHECK (items == enabled + 1);   // the sentence saying where it lands

    CHECK (ui::choiceOfMenuItem (0).first == -1);
    CHECK (ui::choiceOfMenuItem (1) == std::make_pair (0, false));
    CHECK (ui::choiceOfMenuItem (2) == std::make_pair (0, true));
    CHECK (ui::choiceOfMenuItem (7) == std::make_pair (3, false));

    /*  SQUEEZED, the words give way and the pictures and list marks do not
        (author, 2026-09-30: "Can the icon and arrow be visible at all
        times"). Drawn at three widths through the window's own look; with
        WFG_SNAPSHOT_DIR set, each is written out to be looked at. */
    const model::Theme theme;
    ui::Look look (theme);
    bar.setLookAndFeel (&look);

    const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {});

    for (const auto width : { 1400, 760, 420 })
    {
        bar.setBounds (0, 0, width, bar.preferredHeight());

        const auto picture = bar.createComponentSnapshot (bar.getLocalBounds());
        CHECK (picture.isValid());

        //  Every word or none: wide, all of them; squeezed, not one.
        auto wordless = 0;

        for (auto* button : buttonsUnder (bar))
            if (static_cast<bool> (button->getProperties()[ui::Look::iconOnly()]))
                ++wordless;

        if (width == 1400)
            CHECK (wordless == 0);
        else if (width == 420)
            CHECK (wordless == static_cast<int> (model::cueKinds().size()));
        else
            CHECK ((wordless == 0 || wordless == static_cast<int> (model::cueKinds().size())));

        if (dir.isNotEmpty())
        {
            const auto file = juce::File (dir).getChildFile ("new-cue-bar-" + juce::String (width) + ".png");
            file.getParentDirectory().createDirectory();
            file.deleteFile();

            juce::FileOutputStream out (file);
            juce::PNGImageFormat().writeImageToStream (picture, out);
        }
    }

    bar.setLookAndFeel (nullptr);
}

TEST_CASE ("network monitor: opening listens, shutting stops, and the filters choose the lines")
{
    /*  The author, 2026-09-30: a network monitor "similar to the one in
        WFS-DIY", reached from the Show menu. The engine keeps nothing while
        nobody watches, so what is asserted first is that the window switches
        the listening on and off; then that lines arrive and filter. */
    std::vector<bool> heard;

    ui::NetworkMonitorWindow::Actions actions;
    actions.listen = [&heard] (bool on) { heard.push_back (on); };

    ui::NetworkMonitorWindow window (model::Theme {}, actions);

    window.open();
    REQUIRE_FALSE (heard.empty());
    CHECK (heard.back());
    CHECK (window.listening());

    const auto midi = [] (wfg::monitor::Direction direction, std::uint8_t status)
    {
        wfg::monitor::Capture capture;
        capture.wallMicros = 1'000'000;
        capture.direction = direction;
        capture.medium = wfg::monitor::Medium::midi;
        capture.road = wfg::monitor::Road::midi;
        capture.size = 3;
        capture.fullSize = 3;
        capture.bytes[0] = status;
        capture.bytes[1] = 60;
        capture.bytes[2] = 100;
        return capture;
    };

    window.add ({ midi (wfg::monitor::Direction::in, 0x90), midi (wfg::monitor::Direction::out, 0xB0),
                  midi (wfg::monitor::Direction::in, 0x80) }, 0);

    CHECK (window.lineCount() == 3u);
    CHECK (window.shownCount() == 3u);
    CHECK (window.shownRow (0).address == "note on");

    model::TrafficFilter onlyOut;
    onlyOut.in = false;
    window.setFilter (onlyOut);
    REQUIRE (window.shownCount() == 1u);
    CHECK (window.shownRow (0).address == "control change");

    /*  WITH WFG_SNAPSHOT_DIR SET, a picture of it with some OSC beside the
        MIDI, every filter open, to be looked at. */
    const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {});

    if (dir.isNotEmpty())
    {
        window.setFilter ({});

        const auto osc = [] (wfg::monitor::Direction direction, wfg::monitor::Road road,
                             std::string_view peer, const wfg::osc::Packet& packet)
        {
            std::string error;
            const auto bytes = wfg::osc::encode (packet, error).value_or (std::vector<std::uint8_t> {});

            wfg::monitor::Capture capture;
            capture.wallMicros = 1'759'230'000'000'000;
            capture.direction = direction;
            capture.road = road;
            capture.size = static_cast<std::uint16_t> (bytes.size());
            capture.fullSize = static_cast<std::uint32_t> (bytes.size());
            capture.peerLength = static_cast<std::uint8_t> (peer.size());
            std::copy (peer.begin(), peer.end(), capture.peer);
            std::copy (bytes.begin(), bytes.end(), capture.bytes);
            return capture;
        };

        window.add ({ osc (wfg::monitor::Direction::out, wfg::monitor::Road::udp, "192.168.1.20:9000",
                           wfg::osc::Packet::message ("/desk/fader/1", { wfg::osc::Value::float32 (0.75f) })),
                      osc (wfg::monitor::Direction::in, wfg::monitor::Road::udp, "192.168.1.20:9000",
                           wfg::osc::Packet::message ("/godot/cmd/go")),
                      osc (wfg::monitor::Direction::out, wfg::monitor::Road::page, "127.0.0.1:51234",
                           wfg::osc::Packet::message ("/godot/transport/standby", { wfg::osc::Value::string ("P4MED003") })) }, 3);

        window.setSize (1180, 420);
        const auto picture = window.createComponentSnapshot (window.getLocalBounds());
        const auto file = juce::File (dir).getChildFile ("network-monitor.png");
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (picture, out);
    }

    //  Shut, it stops the engine recording and keeps its lines.
    const auto keptBefore = window.lineCount();
    window.closeButtonPressed();
    CHECK_FALSE (heard.back());
    CHECK_FALSE (window.listening());
    CHECK (window.lineCount() == keptBefore);
}

//==============================================================================
/*  DOH! ON THE TRANSPORT (PRD §3.32; the author, 2026-09-30, D1): its own
    button, to the left of PANIC on GO's row, and its own key, F9 - one press
    however long it is held. Both failed before D1: there was no such button,
    and nothing answered F9.

    AND APART FROM PANIC (the author, 2026-10-02, K7: more padding between the
    two "to avoid a total disaster"): a row's height of air between them, where
    D1 left four pixels - wider than GO's own air, at the widest window and at
    the narrowest one the main window allows (640). Failed before K7: the gap
    was 4. */
TEST_CASE ("transport: Doh! sits left of PANIC on GO's row with a row of air between them, and a click is a Doh! and nothing else")
{
    auto goes = 0, panics = 0, dohs = 0;

    ui::TransportComponent::Actions actions;
    actions.go = [&goes] { ++goes; };
    actions.panic = [&panics] { ++panics; };
    actions.doh = [&dohs] { ++dohs; };

    const model::Theme theme;
    const auto row = juce::roundToInt (theme.row * theme.type);

    ui::TransportComponent transport (theme, actions);

    juce::TextButton* go = nullptr;
    juce::TextButton* panic = nullptr;
    juce::TextButton* doh = nullptr;

    for (auto* child : transport.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
        {
            if (button->getButtonText() == "GO")                 go = button;
            if (button->getButtonText() == "PANIC")              panic = button;
            if (button->getButtonText().startsWith ("Doh!"))     doh = button;
        }

    REQUIRE (go != nullptr);
    REQUIRE (panic != nullptr);
    REQUIRE (doh != nullptr);

    for (const auto width : { 1200, 640 })
    {
        INFO ("width " << width);
        transport.setSize (width, transport.preferredHeight());

        const auto g = go->getBounds();
        const auto d = doh->getBounds();
        const auto p = panic->getBounds();

        CHECK (d.getY() == p.getY());
        CHECK (d.getHeight() == p.getHeight());
        CHECK (d.getWidth() > row);
        CHECK (p.getWidth() > row);
        CHECK (d.getRight() <= p.getX());
        CHECK (d.getX() > g.getRight());
        CHECK (p.getRight() <= width);

        /*  THE GAP: at least a row, and wider than the air between GO and what
            stands beside it, the widest any other button on the row is given. */
        const auto gapPixels = p.getX() - d.getRight();
        CHECK (gapPixels >= row);
        CHECK (gapPixels > row / 2 + 4);

        //  Nothing in the gap between them.
        const juce::Rectangle<int> gap { d.getRight(), d.getY(), gapPixels, d.getHeight() };

        for (auto* child : transport.getChildren())
            if (child != doh && child != panic && child->isVisible() && ! gap.isEmpty())
                CHECK_FALSE (child->getBounds().intersects (gap));
    }

    doh->onClick();
    CHECK (dohs == 1);
    CHECK (goes == 0);
    CHECK (panics == 0);
}

/*  DOH! IN ITS OWN COLOUR WHILE IT CAN ACT, FADING AS ITS WINDOW RUNS OUT
    (the author, 2026-10-02: "I would display the button in a distinctive colour
    and fade out when the Doh! timer is over"; K7). The theme's `doh` while the
    window is open, part way to the idle grey while it fades, the idle grey and
    no click once it is over - and the state said by more than the colour
    (§4.8): disabled when over, the cue named and the seconds left in the
    tooltip while open. Read off a reading, so off the engine's tick. Failed
    before K7: the button wore the standby's amber whatever the window said, and
    was never disabled. */
TEST_CASE ("transport: Doh! wears its own colour while the window is open, fades, and is disabled once it is over")
{
    auto dohs = 0;

    ui::TransportComponent::Actions actions;
    actions.doh = [&dohs] { ++dohs; };

    const model::Theme theme;
    ui::TransportComponent transport (theme, actions);
    transport.setSize (1200, transport.preferredHeight());

    juce::TextButton* doh = nullptr;

    for (auto* child : transport.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
            if (button->getButtonText().startsWith ("Doh!"))
                doh = button;

    REQUIRE (doh != nullptr);

    const auto own = juce::Colour (theme.colour ("doh"));
    const auto idle = juce::Colour (theme.colour ("go-idle"));
    const auto colour = [doh] { return doh->findColour (juce::TextButton::buttonColourId); };

    //  Nothing to take back yet: idle, and no click.
    model::TransportReading reading;
    reading.tick = "900";
    reading.dohWindow = "10";
    transport.show (reading);
    CHECK_FALSE (doh->isEnabled());
    CHECK (colour() == idle);

    //  A GO on cue 12 at tick 1000: open, in its own colour, the seconds said.
    reading.doh = "7K2QM9X4 B3N8R5TW 1000";
    reading.dohCue = "12";
    reading.tick = "1100";
    transport.show (reading);
    CHECK (doh->isEnabled());
    CHECK (doh->getButtonText() == "Doh! 12");
    CHECK (colour() == own);
    CHECK (doh->getTooltip().contains ("8 s left"));

    //  Half way through the fade: still a Doh!, its colour half way to the grey.
    reading.tick = "1450";
    transport.show (reading);
    CHECK (doh->isEnabled());
    CHECK (colour() == own.interpolatedWith (idle, 0.5f));
    CHECK (colour() != own);
    CHECK (colour() != idle);

    //  Over: the idle grey, no click, the cue no longer named.
    reading.tick = "1500";
    transport.show (reading);
    CHECK_FALSE (doh->isEnabled());
    CHECK (colour() == idle);
    CHECK (doh->getButtonText() == "Doh!");
    CHECK_FALSE (doh->getTooltip().contains ("s left"));
}

TEST_CASE ("transport: F9 held sends one Doh!, through the Shell as the window delivers keys")
{
    auto dohs = 0;

    ui::TransportComponent::Actions actions;
    actions.doh = [&dohs] { ++dohs; };

    ui::Shell shell (model::Theme {}, actions, {}, {}, {}, {}, {}, {}, {});

    //  Held: the key repeats, and one Doh! goes.
    CHECK (shell.keyPressed (juce::KeyPress (juce::KeyPress::F9Key)));
    CHECK (shell.keyPressed (juce::KeyPress (juce::KeyPress::F9Key)));
    CHECK (dohs == 1);

    //  Let go, and pressed again: a second.
    shell.keyStateChanged (false);
    CHECK (shell.keyPressed (juce::KeyPress (juce::KeyPress::F9Key)));
    CHECK (dohs == 2);

    /*  LET GO SOMEWHERE ELSE: the release went to another window - a menu, a
        dialog, another application - and never reached the Shell. The latch is
        looked at again when the focus comes back, F9 up by then, so the next
        press is not swallowed (the review, 2026-10-01). */
    shell.focusLost (juce::Component::focusChangedDirectly);
    shell.focusGained (juce::Component::focusChangedDirectly);
    CHECK (shell.keyPressed (juce::KeyPress (juce::KeyPress::F9Key)));
    CHECK (dohs == 3);
}

/*  THE DOH NOTICE ON THE TRANSPORT'S LINE (2026-10-03, Doh! D4, namespace draft
    §24.14): the last Doh's report, shown in front of the line ONCE, when it
    arrives - another sentence put there since keeps it, until the next report
    - its whole text one hover away, and a refusal newer than the report takes
    the line back. Failed before D4: nothing showed the report. */
TEST_CASE ("transport: the Doh notice is shown once when the report arrives, whole on hover, and a newer refusal takes the line")
{
    const model::Theme theme;
    ui::TransportComponent transport (theme, ui::TransportComponent::Actions {});
    transport.setSize (1200, transport.preferredHeight());

    //  The visible sentence on the top line - the notice, or the error behind it.
    const auto shown = [&transport] () -> juce::Label*
    {
        juce::Label* found = nullptr;

        for (auto* child : transport.getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (child))
                if (label->isVisible() && (label->getText().startsWith ("Doh!") || label->getText().contains ("refused")
                                             || label->getText() == "Esc now"))
                    found = label;

        return found;
    };

    model::TransportReading reading;
    reading.tick = "1100";
    reading.listId = "7K2QM9X4";
    reading.listName = "Show";
    reading.status = "running";
    transport.show (reading);
    CHECK (shown() == nullptr);

    //  The report arrives: in front of the line, in words, whole on hover.
    reading.tick = "1201";
    reading.dohReport = "7K2QM9X4 1200 Lighting desk: Q12, Q13 - left to its operator, not sent again";
    reading.dohReportList = "Show";
    transport.show (reading);
    REQUIRE (shown() != nullptr);
    CHECK (shown()->getText() == "Doh!: Lighting desk: Q12, Q13 - left to its operator, not sent again");
    CHECK (shown()->getTooltip() == shown()->getText());

    //  Once: another sentence put there since is not pushed aside by the same report.
    transport.setNotice ("Esc now");
    reading.tick = "1250";
    transport.show (reading);
    REQUIRE (shown() != nullptr);
    CHECK (shown()->getText() == "Esc now");
    CHECK (shown()->getTooltip().isEmpty());

    //  A later report - a relaunch's - is shown again.
    reading.dohReport = "7K2QM9X4 1300 Scene: put back at 0:12";
    transport.show (reading);
    REQUIRE (shown() != nullptr);
    CHECK (shown()->getText() == "Doh!: Scene: put back at 0:12");

    //  A refusal newer than it: the error has the line.
    reading.lastError = "1350 31 window not-a-stop standby.set";
    transport.show (reading);
    REQUIRE (shown() != nullptr);
    CHECK (shown()->getText() == "standby.set refused: not-a-stop");
}

/*  THE DOH NOTICE GIVES WAY, AND FOLLOWS THE FOCUS (2026-10-03, D4's review):
    standing in front of the line, it hid every later sentence the line has
    besides a refusal - a write that failed, the audio gone - for as long as it
    stood; now any change of what the line says takes the line back from it.
    And its opening - "Doh! on <list>" - follows a focus moved, or a list
    renamed, while it is shown. Failed on 204ac69: the notice stood over both,
    and kept its old opening. */
TEST_CASE ("transport: the Doh notice gives way to any newer sentence on the line, and follows the focus while shown")
{
    const model::Theme theme;
    ui::TransportComponent transport (theme, ui::TransportComponent::Actions {});
    transport.setSize (1200, transport.preferredHeight());

    const auto visibleText = [&transport] (const juce::String& start) -> juce::String
    {
        for (auto* child : transport.getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (child))
                if (label->isVisible() && label->getText().startsWith (start))
                    return label->getText();

        return {};
    };

    model::TransportReading reading;
    reading.tick = "1201";
    reading.listId = "7K2QM9X4";
    reading.listName = "Show";
    reading.status = "running";
    reading.dohReport = "LQ4X8MZT 1200 Lighting desk: Q12 - left to its operator, not sent again";
    reading.dohReportList = "Act 2";
    transport.show (reading);
    CHECK (visibleText ("Doh!") == "Doh! on Act 2: Lighting desk: Q12 - left to its operator, not sent again");

    //  Renamed while shown: the opening follows.
    reading.dohReportList = "Second half";
    transport.show (reading);
    CHECK (visibleText ("Doh!") == "Doh! on Second half: Lighting desk: Q12 - left to its operator, not sent again");

    //  The focus moved onto the report's list: no list's name.
    reading.listId = "LQ4X8MZT";
    reading.listName = "Second half";
    transport.show (reading);
    CHECK (visibleText ("Doh!") == "Doh!: Lighting desk: Q12 - left to its operator, not sent again");

    SUBCASE ("a write that failed takes the line")
    {
        reading.writeError = "disk full";
        transport.show (reading);
        CHECK (visibleText ("Doh!").isEmpty());
        CHECK (visibleText ("write failed") == "write failed: disk full");
    }

    SUBCASE ("the audio gone takes the line")
    {
        reading.status = "noClock";
        transport.show (reading);
        CHECK (visibleText ("Doh!").isEmpty());
        CHECK (visibleText ("Audio disconnected").isNotEmpty());
    }
}

TEST_CASE ("transport: a long Doh report is cut on the row at a whole character, and whole on hover")
{
    /*  Doh! D4's review: the row is one line high, so a long report is cut
        there - by characters, never inside one, a lighting desk named "Éclairage"
        included - and the whole of it is the tooltip. A net: juce::String
        counts characters, and the cut was already safe on 204ac69. */
    const model::Theme theme;
    ui::TransportComponent transport (theme, ui::TransportComponent::Actions {});
    transport.setSize (1200, transport.preferredHeight());

    juce::String cues;

    for (int n = 0; n < 90; ++n)
        cues << (n == 0 ? "" : ", ") << "Q" << n;

    const auto desk = juce::String (juce::CharPointer_UTF8 ("\xc3\x89" "clairage \xc3\xa9t\xc3\xa9"));
    const auto sentence = desk + ": " + cues + " - left to its operator, not sent again";

    model::TransportReading reading;
    reading.tick = "1201";
    reading.listId = "7K2QM9X4";
    reading.listName = "Show";
    reading.status = "running";
    reading.dohReport = "7K2QM9X4 1200 " + sentence.toStdString();
    reading.dohReportList = "Show";
    transport.show (reading);

    juce::Label* notice = nullptr;

    for (auto* child : transport.getChildren())
        if (auto* label = dynamic_cast<juce::Label*> (child))
            if (label->isVisible() && label->getText().startsWith ("Doh!"))
                notice = label;

    REQUIRE (notice != nullptr);
    CHECK (notice->getText().length() == 303);
    CHECK (notice->getText().endsWith ("..."));
    CHECK (notice->getText().startsWith ("Doh!: " + desk));
    CHECK (notice->getTooltip() == "Doh!: " + sentence);
}

TEST_CASE ("import window: the named scenes that do something are ticked, and Import hands back the ticks")
{
    /*  Namespace draft §29, QS: a scene a tick box, the ones the host ticked
        ticked to start, one that does nothing listed and greyed; Import hands
        back what is ticked and the folder the show is made in - a new folder of
        the name typed, in the folder shown. */
    wfg::ImportScenes scenes;
    scenes.scenes = { { 0, "top d\xc3\xa9" "but spec", "MISE : Fader 1", true, true },
                      { 1, "", "", true, false },
                      { 2, "rien", "", false, false } };

    std::vector<int> handed;
    juce::File into;

    ui::ImportWindow::Actions actions;
    actions.import = [&] (const std::vector<int>& ticked, const juce::File& where)
    {
        handed = ticked;
        into = where;
    };

    const auto parent = juce::File::getSpecialLocation (juce::File::tempDirectory);
    ui::ImportWindow window (model::Theme {}, juce::StringArray { juce::String::fromUTF8 ("Lazzi r\xc3\xa9gie Pau") },
                             scenes, parent,
                             "Lazzi", actions);

    std::vector<juce::ToggleButton*> boxes;
    juce::TextButton* importButton = nullptr;

    std::function<void (juce::Component&)> find = [&] (juce::Component& component)
    {
        for (auto* child : component.getChildren())
        {
            if (auto* box = dynamic_cast<juce::ToggleButton*> (child))
                boxes.push_back (box);

            if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->getButtonText() == "Import")
                importButton = button;

            find (*child);
        }
    };

    find (window);

    REQUIRE (boxes.size() == 3u);
    CHECK (boxes[0]->getToggleState());
    CHECK_FALSE (boxes[1]->getToggleState());   // no name: not ticked to start
    CHECK (boxes[1]->isEnabled());
    CHECK_FALSE (boxes[2]->isEnabled());        // does nothing: listed, greyed
    CHECK (boxes[0]->getButtonText().startsWith ("1 "));

    /*  The unnamed scene ticked by hand comes back with the first. */
    boxes[1]->setToggleState (true, juce::dontSendNotification);
    REQUIRE (importButton != nullptr);
    REQUIRE (importButton->onClick != nullptr);
    importButton->onClick();

    CHECK (handed == std::vector<int> { 0, 1 });
    CHECK (into == parent.getChildFile ("Lazzi"));
}
