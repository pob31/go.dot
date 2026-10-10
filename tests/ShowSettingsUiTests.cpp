/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/client/ui/ShowSettingsWindow.h>
#include <wfg/client/ui/InspectorComponent.h>
#include <wfg/client/model/Devices.h>
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/MidiPorts.h>
#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/Theme.h>
#include <wfg/engine/audio/DeviceLayer.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/PresetTable.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/cue/Run.h>
#include <spatcore/ui/patch/PatchMatrixComponent.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <functional>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

using namespace wfg;

namespace
{
    template<class T> T* component (juce::Component& root)
    {
        if (auto* found = dynamic_cast<T*> (&root)) return found;
        for (auto* child : root.getChildren())
            if (auto* found = component<T> (*child)) return found;
        return nullptr;
    }
    juce::Button* button (juce::Component& root, const juce::String& text)
    {
        if (auto* candidate = dynamic_cast<juce::Button*> (&root))
            if (candidate->getButtonText() == text) return candidate;
        for (auto* child : root.getChildren())
            if (auto* found = button (*child, text)) return found;
        return nullptr;
    }

    struct Rig
    {
        Engine engine;
        doc::ShowDocument document;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        client::model::Theme theme;
        std::vector<Event> sent;

        auto publish()
        {
            parameters.markStale();
            return parameters.publish (state.tick++, state);
        }

        void exercisePanel (const std::function<void()>& checkClock)
        {
            /*  WHAT APPLY SHOULD SEND AS THE OUTPUT PATCH, which depends on
                whether this show is still following its output list. A show
                with a patch already written has settled, and Apply sends it
                back; a show with none has not, and Apply must send none -
                writing the diagonal out in full would settle it, and somebody
                applying a buffer size did not ask for that. */
            const auto wantedPatch = document.getAttribute ("/godot/audio/outputPatch")
                                       .value_or (std::string {});
            client::ui::ShowSettingsWindow panel (theme, *publish(),
                [this] (Event event) { sent.push_back (std::move (event)); });
            checkClock();
            auto* rescan = button (panel, "Rescan interfaces");
            REQUIRE (rescan != nullptr);
            rescan->onClick();
            checkClock();
            auto* apply = button (panel, "Apply while stopped");
            REQUIRE (apply != nullptr);
            apply->onClick();
            REQUIRE (sent.size() == 1);
            REQUIRE (sent.back().command == "audio.setup");

            CHECK (sent.back().args[6].getString() == wantedPatch);
            CHECK_FALSE (apply->isEnabled());

            const auto& args = sent.back().args;
            audio::AudioSettings selected;
            selected.enabled = args[0].getBool(); selected.deviceType = args[1].getString();
            selected.outputDevice = args[2].getString(); selected.inputDevice = args[3].getString();
            selected.bufferSize = args[4].getInt32(); selected.inputPatch = args[5].getString();
            selected.outputPatch = args[6].getString();
            REQUIRE (document.configureAudio (selected).ok);
            // Complete between two UI passes, just as a fast restart can do.
            state.audioSettingsRevision++;
            state.documentDirty = true;
            panel.refresh (*publish());
            CHECK (apply->isEnabled());
            REQUIRE (sent.size() == 2);
            CHECK (sent.back().command == "document.save");
            panel.refresh (*publish());
            CHECK (sent.size() == 2); // completion saves only once
            checkClock();

            apply->onClick();
            state.errorCount++;
            state.lastError = "audio-busy audio.setup";
            panel.refresh (*publish());
            CHECK (apply->isEnabled()); // rejection must not leave Apply waiting
        }
    };
}

TEST_CASE ("audio settings UI: apply completion and refusal release the controls")
{
    Rig rig;
    rig.exercisePanel ([] {});
}

TEST_CASE ("audio settings UI: the Outputs tab makes outputs, and a hand patch settles the show")
{
    Rig rig;

    /*  Two outputs to look at, made through the commands so the channels are
        packed the way the tab will show them. */
    REQUIRE (rig.document.createBus ("direct", 1).ok);
    const auto mix = rig.document.createBus ("mix", 2);
    REQUIRE (mix.ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    /*  A TAB'S CONTENT IS ONLY A LIVE CHILD WHILE IT SHOWS, which is
        `juce::TabbedComponent`'s own arrangement - so the page has to be
        selected before anything in it can be found. Outputs is the second tab,
        between Interface and the two patches. */
    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    CHECK (tabs->getTabNames()[1] == "Outputs");
    tabs->setCurrentTabIndex (1);

    /*  THE FOUR ADD BUTTONS ARE THE WHOLE STRUCTURE GESTURE. Everything else
        on this tab - the name, the cross, the drag - is a click on a row,
        which a component test cannot reach without a mouse; what it CAN assert
        is that each control sends the named command it claims to.

        FOUR AND NOT TWO WITH A FLIP (author, 2026-09-22): a width is said when
        the output is made, because changing it afterwards repacks every
        channel below it, which reads from the patch as the rig having been
        re-wired. */
    struct Wanted { const char* label; const char* kind; int width; };

    for (const auto& one : { Wanted { "+ mono out",   "direct", 1 },
                             Wanted { "+ stereo out", "direct", 2 },
                             Wanted { "+ mono mix",   "mix",    1 },
                             Wanted { "+ stereo mix", "mix",    2 } })
    {
        INFO (one.label);

        auto* add = button (panel, one.label);
        REQUIRE (add != nullptr);

        const auto before = rig.sent.size();
        add->onClick();

        REQUIRE (rig.sent.size() == before + 1);
        CHECK (rig.sent.back().command == "bus.create");
        CHECK (rig.sent.back().args[0].getString() == one.kind);
        CHECK (rig.sent.back().args[1].getInt32() == one.width);
    }

    /*  AND THE FIRST HAND EDIT OF THE PATCH SETTLES THE SHOW, before the edit
        lands rather than after: the engine's layout rule materialises the patch
        it had before repacking, so it has to already know the show has stopped
        following its list. */
    /*  BY NAME since the Inputs tab (Phase 9b): the strip has grown a tab
        between Outputs and the patches, and a number would now open the input
        patch - whose first edit settles the inputs, not the outputs. */
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Output patch"));
    auto* matrix = component<spatcore::ui::patch::PatchMatrixComponent> (panel);
    REQUIRE (matrix != nullptr);

    const auto before = rig.sent.size();
    REQUIRE (matrix->onBeforeUserPatchEdit != nullptr);
    matrix->onBeforeUserPatchEdit();

    REQUIRE (rig.sent.size() == before + 1);
    CHECK (rig.sent.back().command == "node.set");
    CHECK (rig.sent.back().args[0].getString() == "/godot/audio/patchSettled");
    CHECK (rig.sent.back().args[1].getBool());

    //  Once, and not once per click: the flag is already true.
    matrix->onBeforeUserPatchEdit();
    CHECK (rig.sent.size() == before + 1);

    /*  AND THE OTHER HALF OF THE SAME RULE: once it has been touched, Apply
        sends the patch it has rather than nothing. An untouched patch on a
        show still following its list sends nothing at all, which is what
        `exercisePanel` pins. */
    auto* apply = button (panel, "Apply while stopped");
    REQUIRE (apply != nullptr);
    apply->onClick();

    REQUIRE (rig.sent.back().command == "audio.setup");
    CHECK_FALSE (rig.sent.back().args[6].getString().empty());
}

TEST_CASE ("show settings UI: the Inputs tab makes named inputs, and a hand patch of the inputs settles them")
{
    /*  Phase 9b (namespace draft 18.9): the output list's twin, beside it. Two
        named inputs to look at, made through the commands, and the engine's
        readings of them - one loud, one quiet - as a running interface would
        publish them. */
    Rig rig;

    REQUIRE (rig.document.createInput (1).ok);
    const auto keys = rig.document.createInput (2);
    REQUIRE (keys.ok);
    REQUIRE (rig.document.setAttribute ("/godot/input/" + keys.id + "/name", "Keys").ok);

    rig.state.logicalInputs = 3;
    rig.state.inputMetersDb = { -18.0, -3.0, -42.0 };

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    /*  EACH SIDE WHOLE, OUTPUTS FIRST (author, 2026-09-30): the outputs and
        their patch, then the inputs and theirs, in the same order. */
    const auto names = tabs->getTabNames();
    CHECK (names.indexOf ("Output patch") == names.indexOf ("Outputs") + 1);
    CHECK (names.indexOf ("Inputs") == names.indexOf ("Output patch") + 1);
    CHECK (names.indexOf ("Input patch") == names.indexOf ("Inputs") + 1);
    tabs->setCurrentTabIndex (names.indexOf ("Inputs"));

    for (const auto& [label, width] : { std::pair<const char*, int> { "+ mono input", 1 },
                                        std::pair<const char*, int> { "+ stereo input", 2 } })
    {
        INFO (label);

        auto* add = button (panel, label);
        REQUIRE (add != nullptr);

        const auto before = rig.sent.size();
        add->onClick();

        REQUIRE (rig.sent.size() == before + 1);
        CHECK (rig.sent.back().command == "input.create");
        CHECK (rig.sent.back().args[0].getInt32() == width);
        CHECK (rig.sent.back().args[1].getInt32() == -1);
    }

    /*  A LOOK, with no screen: the tab painted into a PNG when WFG_SNAPSHOT_DIR
        is set - the meters lit, the stereo one hot - and skipped otherwise. */
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        panel.setSize (880, 610);
        panel.refresh (*rig.publish());

        const auto picture = panel.createComponentSnapshot (panel.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("inputs-tab.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }

    /*  UNTOUCHED, THE INPUT PATCH FOLLOWS THE LIST and Apply sends none of it:
        writing the diagonal out would settle the inputs, and somebody
        applying a buffer size did not ask for that. */
    auto* apply = button (panel, "Apply while stopped");
    REQUIRE (apply != nullptr);
    apply->onClick();
    REQUIRE (rig.sent.back().command == "audio.setup");
    CHECK (rig.sent.back().args[5].getString().empty());

    //  Apply is waiting for its answer; complete it, as the engine would.
    rig.state.audioSettingsRevision++;
    panel.refresh (*rig.publish());

    /*  AND THE FIRST HAND EDIT OF THE INPUT PATCH SETTLES THE INPUTS - its own
        flag, never the outputs' - once. */
    tabs->setCurrentTabIndex (names.indexOf ("Input patch"));
    auto* matrix = component<spatcore::ui::patch::PatchMatrixComponent> (*tabs->getCurrentContentComponent());
    REQUIRE (matrix != nullptr);
    REQUIRE (matrix->onBeforeUserPatchEdit != nullptr);

    const auto before = rig.sent.size();
    matrix->onBeforeUserPatchEdit();

    REQUIRE (rig.sent.size() == before + 1);
    CHECK (rig.sent.back().command == "node.set");
    CHECK (rig.sent.back().args[0].getString() == "/godot/audio/inputPatchSettled");
    CHECK (rig.sent.back().args[1].getBool());

    matrix->onBeforeUserPatchEdit();
    CHECK (rig.sent.size() == before + 1);
}

TEST_CASE ("show settings UI: the Rack tab offers a recorder, says what it sets aside, and puts a plugin before or after it")
{
    /*  Phase 9c, stage 9c.2 (namespace draft 19.2): a sampling channel, one
        plugin before its recorder and one after, and the graph's record of
        what it set aside as serve's host would write it. The channel's row
        says its recorder as a menu, each plugin says its side, and the foot
        says the memory - set aside, or still to set aside at Load now. */
    Rig rig;
    plugin::PluginTable table;
    rig.parameters.setPlugins (&table);
    rig.state.sampleRate = 48000;

    const auto loops = rig.document.createRackChannel ("mono");
    REQUIRE (loops.ok);
    REQUIRE (rig.document.setAttribute ("/godot/slot/" + loops.id + "/name", "Loops").ok);
    REQUIRE (rig.document.setAttribute ("/godot/slot/" + loops.id + "/takeSeconds", "60").ok);

    const auto drive = rig.document.createChannelPlugin (loops.id, "Drive", "VST3-0badf00d-drve", "VST3", "C:/plugins/drive.vst3");
    const auto delay = rig.document.createChannelPlugin (loops.id, "Delay", "VST3-0badf00d-dlay", "VST3", "C:/plugins/delay.vst3");
    REQUIRE (drive.ok);
    REQUIRE (delay.ok);
    REQUIRE (rig.document.setAttribute ("/godot/plugin/" + drive.id + "/side", "before").ok);

    plugin::PluginTable::BuiltTake built;
    built.seconds = 60.0;
    built.layers = 4;
    built.bytes = 115219200;
    built.before = { drive.id };
    table.setBuiltTakes ({ { loops.id, built } });

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Rack"));

    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    const auto saying = [] (juce::Component& root, const juce::String& words)
    {
        std::function<bool (juce::Component&)> find = [&] (juce::Component& at)
        {
            if (auto* label = dynamic_cast<juce::Label*> (&at); label != nullptr && label->getText().contains (words))
                return true;

            for (auto* child : at.getChildren())
                if (find (*child))
                    return true;

            return false;
        };

        return find (root);
    };

    CHECK (saying (*page, "It records up to 1 min with 4 layers on top: 115.2 MB set aside."));

    //  The picture first, while the graph agrees with the show.
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        panel.setSize (880, 610);
        panel.refresh (*rig.publish());

        const auto picture = panel.createComponentSnapshot (panel.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("rack-tab-sampling.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }

    //  A longer take asked for: the graph still holds the old one, and the foot says Load now.
    REQUIRE (rig.document.setAttribute ("/godot/slot/" + loops.id + "/takeSeconds", "120").ok);
    panel.refresh (*rig.publish());
    CHECK (saying (*page, "It records up to 2 min with 4 layers on top: 230.4 MB to set aside at Load now."));
}

TEST_CASE ("show settings UI: the Rack tab makes channels, and says each chain's worst case against the budget")
{
    /*  Phase 9b (namespace draft 18.3): two channels to look at, made through
        the document, and their plugins' states as a sandbox would write them
        tonight - one chain over the budget, one plugin down. */
    Rig rig;
    plugin::PluginTable table;
    rig.parameters.setPlugins (&table);
    rig.state.sampleRate = 48000;

    const auto vox = rig.document.createRackChannel ("mono");
    REQUIRE (vox.ok);
    REQUIRE (rig.document.setAttribute ("/godot/slot/" + vox.id + "/name", "Vox 1").ok);

    const auto room = rig.document.createChannelPlugin (vox.id, "Room", "VST3-0badf00d-room", "VST3", "C:/plugins/room.vst3");
    const auto deEss = rig.document.createChannelPlugin (vox.id, "De-esser", "VST3-0badf00d-dess", "VST3", "C:/plugins/dess.vst3");
    REQUIRE (room.ok);
    REQUIRE (deEss.ok);

    const auto band = rig.document.createRackChannel ("stereo");
    REQUIRE (band.ok);
    REQUIRE (rig.document.setAttribute ("/godot/slot/" + band.id + "/name", "Band").ok);

    const auto eq = rig.document.createChannelPlugin (band.id, "Bus EQ", "VST3-0badf00d-beq", "VST3", "C:/plugins/beq.vst3");
    REQUIRE (eq.ok);

    plugin::PluginTable::Status loaded;
    loaded.state = "loaded";
    loaded.inputs = 2;
    loaded.outputs = 2;
    loaded.layout = "stereo in, stereo out";

    auto roomStatus = loaded;
    roomStatus.latencySamples = 256;
    auto deEssStatus = loaded;
    deEssStatus.latencySamples = 64;
    table.set (room.id, roomStatus);
    table.set (deEss.id, deEssStatus);

    plugin::PluginTable::Status failed;
    failed.state = "failed";
    failed.problem = "its child process stopped answering; Band is silent until it is back";
    table.set (eq.id, failed);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    const auto names = tabs->getTabNames();
    CHECK (names.indexOf ("Rack") == names.indexOf ("Plugins") + 1);
    tabs->setCurrentTabIndex (names.indexOf ("Rack"));

    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    for (const auto& [label, channelClass] : { std::pair<const char*, const char*> { "+ Mono", "mono" },
                                               std::pair<const char*, const char*> { "+ Mono to stereo", "monoToStereo" },
                                               std::pair<const char*, const char*> { "+ Stereo", "stereo" } })
    {
        INFO (label);

        auto* add = button (*page, label);
        REQUIRE (add != nullptr);

        const auto before = rig.sent.size();
        add->onClick();

        REQUIRE (rig.sent.size() == before + 1);
        CHECK (rig.sent.back().command == "channel.create");
        CHECK (rig.sent.back().args[0].getString() == channelClass);
    }

    /*  THE FIRST CHANNEL IS PICKED, so its chain is what the right side shows
        and Add... has somewhere to put a plugin. */
    auto* addPlugin = button (*page, "Add...");
    REQUIRE (addPlugin != nullptr);
    CHECK (addPlugin->isEnabled());

    /*  THE WORST CASE IS SAID: 320 samples at 48 kHz is 6.7 ms, over the 5 ms
        the show allows - in words at the foot, never a colour alone. */
    const auto saying = [] (juce::Component& root, const juce::String& words)
    {
        std::function<bool (juce::Component&)> find = [&] (juce::Component& at)
        {
            if (auto* label = dynamic_cast<juce::Label*> (&at); label != nullptr && label->getText().contains (words))
                return true;

            for (auto* child : at.getChildren())
                if (find (*child))
                    return true;

            return false;
        };

        return find (root);
    };

    CHECK (saying (*page, "Vox 1: 6.7 ms at worst, with every plugin in - over the 5 ms budget."));

    /*  A LOOK, with no screen: the tab painted into a PNG when WFG_SNAPSHOT_DIR
        is set, and skipped otherwise. */
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        panel.setSize (880, 610);
        panel.refresh (*rig.publish());

        const auto picture = panel.createComponentSnapshot (panel.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("rack-tab.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }

    /*  LOCKED, NOTHING IS OFFERED THAT WOULD EDIT THE SHOW - and a failed
        plugin can still be restarted, which is not an edit. */
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());
    CHECK_FALSE (button (*page, "+ Mono")->isVisible());
    CHECK_FALSE (addPlugin->isVisible());
    CHECK (button (*page, "Restart")->isVisible());
}

TEST_CASE ("show settings UI: the Playback tab sets the least time between GOs and the panic fade")
{
    /*  Author, 2026-09-28: "In the show settings, there should be a 'time
        between' Go's and a 'Panic' fade duration". Two numbers, each a
        `node.set` that lands at once - nothing on the tab waits for Apply. */
    Rig rig;
    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    //  The last tab that sets anything; only the Getting started words come after it.
    const auto names = tabs->getTabNames();
    REQUIRE (names.contains ("Playback"));
    CHECK (names.indexOf ("Playback") == names.size() - 2);
    CHECK (names[names.size() - 1] == "Getting started");
    tabs->setCurrentTabIndex (names.indexOf ("Playback"));

    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    std::vector<juce::TextEditor*> boxes;
    std::function<void (juce::Component&)> collect = [&] (juce::Component& at)
    {
        if (auto* box = dynamic_cast<juce::TextEditor*> (&at))
            boxes.push_back (box);

        for (auto* child : at.getChildren())
            collect (*child);
    };
    collect (*page);

    /*  DOH!'S WINDOW RIGHT AFTER THE GO DEBOUNCE (the author, 2026-09-30;
        D1), then the panic fade - and then the process cues' two (namespace
        draft §51, ACJ): five numbers, in that order. */
    REQUIRE (boxes.size() == 5u);

    auto* between = boxes[0];
    auto* doh = boxes[1];
    auto* fade = boxes[2];
    auto* budget = boxes[3];
    auto* stuck = boxes[4];

    //  What a show that says nothing has: half a second, ten, one, two and fifty.
    CHECK (between->getText().getDoubleValue() == doctest::Approx (0.5));
    CHECK (doh->getText().getDoubleValue() == doctest::Approx (10.0));
    CHECK (fade->getText().getDoubleValue() == doctest::Approx (1.0));
    CHECK (budget->getText().getDoubleValue() == doctest::Approx (2.0));
    CHECK (stuck->getText().getIntValue() == 50);

    doh->setText ("12,5", juce::dontSendNotification);
    doh->onReturnKey();
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().command == "node.set");
    CHECK (rig.sent.back().args[0].getString() == "/godot/list/dohWindow");
    CHECK (rig.sent.back().args[1].getString() == "12.5");
    rig.sent.clear();

    //  A comma is a decimal point, as a French booth types it.
    between->setText ("0,4", juce::dontSendNotification);
    between->onReturnKey();
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().command == "node.set");
    CHECK (rig.sent.back().args[0].getString() == "/godot/list/goDebounce");
    CHECK (rig.sent.back().args[1].getString() == "0.4");

    fade->setText ("2.5", juce::dontSendNotification);
    fade->onReturnKey();
    REQUIRE (rig.sent.size() == 2u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/audio/panicFade");
    CHECK (rig.sent.back().args[1].getString() == "2.5");

    budget->setText ("5", juce::dontSendNotification);
    budget->onReturnKey();
    REQUIRE (rig.sent.size() == 3u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/list/processBudget");
    CHECK (rig.sent.back().args[1].getString() == "5");

    stuck->setText ("100", juce::dontSendNotification);
    stuck->onReturnKey();
    REQUIRE (rig.sent.size() == 4u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/list/processStuckAfter");
    CHECK (rig.sent.back().args[1].getString() == "100");
    rig.sent.resize (2);

    //  The value it already has is not an edit.
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "2.5").ok);
    panel.refresh (*rig.publish());
    fade->onFocusLost();
    CHECK (rig.sent.size() == 2u);

    //  Locked, none can be typed into.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());
    CHECK_FALSE (between->isEnabled());
    CHECK_FALSE (doh->isEnabled());
    CHECK_FALSE (fade->isEnabled());
    CHECK_FALSE (budget->isEnabled());
    CHECK_FALSE (stuck->isEnabled());
}

//==============================================================================
/*  A ROW'S NAME CAN CHANGE NOW, AND THE PANEL HAS TO FOLLOW IT.

    Two cues of one kind share a panel: `shapeOf` keys on each row's name, its
    control and whether it is writable, so picking a second MIDI cue REFILLS
    the lines rather than rebuilding them. That was safe while a name was
    fixed. It stopped being safe when a MIDI cue's two payload rows started
    being called after the message they carry - note and velocity, controller
    and value, program - because that is a VALUE, and the refill was the one
    path that never touched a name.

    The author found it by looking: two program changes, then a note-on, and
    the note-on's words stuck to everything picked afterwards.
*/
TEST_CASE ("show settings UI: Getting started names every other tab, in order, and goes to it")
{
    /*  Author, 2026-09-30: "Should we add a 'Getting started' tab in the
        settings window explaining what people should do? And what each tab is
        for..." - as the last tab, Audio staying the first. */
    Rig rig;
    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    const auto names = tabs->getTabNames();
    CHECK (names[0] == "Audio");
    REQUIRE (names[names.size() - 1] == "Getting started");
    CHECK (tabs->getCurrentTabIndex() == 0);    // the window still opens on Audio

    tabs->setCurrentTabIndex (names.size() - 1);
    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    //  A button for every other tab, in the strip's own order.
    std::vector<juce::String> listed;

    const std::function<void (juce::Component&)> walk = [&] (juce::Component& at)
    {
        if (auto* b = dynamic_cast<juce::Button*> (&at))
            listed.push_back (b->getButtonText());

        for (auto* child : at.getChildren())
            walk (*child);
    };

    walk (*page);

    std::vector<juce::String> others;

    for (int at = 0; at < names.size() - 1; ++at)
        others.push_back (names[at]);

    CHECK (listed == others);

    //  And pressing one goes there.
    auto* rack = button (*page, "Rack");
    REQUIRE (rack != nullptr);
    rack->onClick();
    CHECK (tabs->getCurrentTabIndex() == names.indexOf ("Rack"));
}

TEST_CASE ("inspector UI: a MIDI cue's labels follow the type when the panel is reused")
{
    Rig rig;

    const auto list = rig.document.createList ("Cues");
    REQUIRE (list.ok);

    const auto program = rig.document.createCue (list.id, 0, "midi", "Snapshot");
    const auto note = rig.document.createCue (list.id, 1, "midi", "Stinger");
    REQUIRE (program.ok);
    REQUIRE (note.ok);

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + program.id + "/type",
                                        "programChange").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + note.id + "/type", "noteOn").ok);

    client::ui::InspectorComponent panel (rig.theme, {});
    panel.setSize (420, 800);

    const auto snapshot = rig.publish();

    /*  Every label the panel is drawing, which is what somebody reads. A row's
        name is a juce::Label and there is no other way in from outside - which
        is the point: this asserts what is on the screen, not what the model
        was asked for. */
    const auto drawn = [&panel]
    {
        std::vector<std::string> out;

        const std::function<void (juce::Component&)> walk = [&] (juce::Component& root)
        {
            if (auto* label = dynamic_cast<juce::Label*> (&root))
                if (label->isVisible() && label->getText().isNotEmpty())
                    out.push_back (label->getText().toStdString());

            for (auto* child : root.getChildren())
                walk (*child);
        };

        walk (panel);
        return out;
    };

    const auto shows = [] (const std::vector<std::string>& labels, const char* word)
    {
        for (const auto& label : labels)
            if (label == word)
                return true;

        return false;
    };

    panel.show (client::model::inspect (*snapshot, program.id));
    panel.resized();

    REQUIRE (shows (drawn(), "program"));
    CHECK_FALSE (shows (drawn(), "note"));

    /*  THE SECOND CUE, SAME SHAPE, DIFFERENT WORDS. This is the refill path:
        nothing is rebuilt, and before the fix the labels stayed as they were. */
    panel.show (client::model::inspect (*snapshot, note.id));
    panel.resized();

    CHECK (shows (drawn(), "note"));
    CHECK (shows (drawn(), "velocity"));
    CHECK_FALSE (shows (drawn(), "program"));

    //  And back again, because the author's report was that it stuck both ways.
    panel.show (client::model::inspect (*snapshot, program.id));
    panel.resized();

    CHECK (shows (drawn(), "program"));
    CHECK_FALSE (shows (drawn(), "velocity"));
}

TEST_CASE ("inspector UI: a cue's preset is drawn as a menu of the groups around it, and a pick writes the group")
{
    /*  Namespace draft §30, decision QZ - the author: "I could not see a
        header/preset toggle in the cues." The row was a bare box wanting a
        group's identifier. Asserted on the screen, as the case above is: the
        menu is drawn, says the groups by name, and a pick writes the
        identifier to the row it is drawn on. */
    Rig rig;

    const auto list = rig.document.createList ("Cues");
    REQUIRE (list.ok);

    const auto scene = rig.document.createCue (list.id, 0, "group", "Scene");
    REQUIRE (scene.ok);

    const auto bed = rig.document.createCue (scene.id, 0, "media", "Bed");
    REQUIRE (bed.ok);

    std::vector<std::pair<std::string, std::string>> written;

    client::ui::InspectorComponent::Actions actions;
    actions.set = [&written] (const std::string& address, const std::string& text)
                  { written.emplace_back (address, text); };

    client::ui::InspectorComponent panel (rig.theme, actions);
    panel.setSize (420, 2000);
    panel.show (client::model::inspect (*rig.publish(), bed.id));
    panel.resized();

    juce::ComboBox* menu = nullptr;

    const std::function<void (juce::Component&)> walk = [&] (juce::Component& root)
    {
        if (auto* box = dynamic_cast<juce::ComboBox*> (&root))
            if (box->getNumItems() > 0 && box->getItemText (0) == "not prepared ahead")
                menu = box;

        for (auto* child : root.getChildren())
            walk (*child);
    };

    walk (panel);

    REQUIRE (menu != nullptr);
    CHECK (menu->isVisible());
    REQUIRE (menu->getNumItems() == 2);
    CHECK (menu->getItemText (1) == "Scene");
    CHECK (menu->getSelectedId() == 1);                  // not prepared ahead, as it stands

    menu->setSelectedId (2, juce::sendNotificationSync);

    REQUIRE (written.size() == 1u);
    CHECK (written.front().first == "/godot/cue/" + bed.id + "/preset");
    CHECK (written.front().second == scene.id);
}

TEST_CASE ("inspector UI: over several cues a field is one write to all of them, and the bar opens the EQ and sends on the anchor")
{
    /*  Namespace draft §30.11: "Selecting multiple media cues did not show all
        the parameters and panels to adjust their sends all at once." A field
        committed over a selection is one gesture - one write naming every
        cue's row - and the panel bar is drawn for the selection. */
    Rig rig;

    const auto list = rig.document.createList ("Cues");
    REQUIRE (list.ok);

    const auto scene = rig.document.createCue (list.id, 0, "group", "Scene");
    REQUIRE (scene.ok);

    const auto bed = rig.document.createCue (scene.id, 0, "media", "Bed");
    const auto rain = rig.document.createCue (scene.id, 1, "media", "Rain");
    REQUIRE (bed.ok);
    REQUIRE (rain.ok);

    std::vector<std::pair<std::string, std::string>> written;
    std::vector<std::pair<std::vector<std::string>, std::string>> everywhere;
    std::vector<std::pair<std::string, std::string>> opened;

    client::ui::InspectorComponent::Actions actions;
    actions.set = [&written] (const std::string& address, const std::string& text)
                  { written.emplace_back (address, text); };
    actions.setAll = [&everywhere] (const std::vector<std::string>& addresses, const std::string& text)
                     { everywhere.emplace_back (addresses, text); };
    actions.openPanel = [&opened] (const std::string& cueId, const std::string& subject)
                        { opened.emplace_back (cueId, subject); };

    client::ui::InspectorComponent panel (rig.theme, actions);
    panel.setSize (420, 2000);
    panel.show (client::model::inspectMany (*rig.publish(), { bed.id, rain.id }, rain.id));
    panel.resized();

    juce::ComboBox* menu = nullptr;
    std::vector<juce::Button*> bar;

    const std::function<void (juce::Component&)> walk = [&] (juce::Component& root)
    {
        if (auto* box = dynamic_cast<juce::ComboBox*> (&root))
            if (box->getNumItems() > 0 && box->getItemText (0) == "not prepared ahead")
                menu = box;

        if (auto* button = dynamic_cast<juce::Button*> (&root))
            if (button->getTooltip().startsWith ("Opens at the foot of the window: "))
                bar.push_back (button);

        for (auto* child : root.getChildren())
            walk (*child);
    };

    walk (panel);

    //  The menu's pick, over both: one write naming both rows.
    REQUIRE (menu != nullptr);
    menu->setSelectedId (2, juce::sendNotificationSync);

    CHECK (written.empty());
    REQUIRE (everywhere.size() == 1u);
    CHECK (everywhere[0].first == std::vector<std::string> { "/godot/cue/" + bed.id + "/preset",
                                                             "/godot/cue/" + rain.id + "/preset" });
    CHECK (everywhere[0].second == scene.id);

    //  The bar: the EQ and the sends, saying how many, opening on the anchor.
    REQUIRE (bar.size() == 2u);
    CHECK (bar[0]->getTooltip().contains ("on all 2 cues at once"));

    //  A click, as the button delivers it (triggerClick posts, and this build runs no nested loop).
    REQUIRE (bar[1]->onClick != nullptr);
    bar[1]->onClick();

    REQUIRE (opened.size() == 1u);
    CHECK (opened[0].first == rain.id);
    CHECK (opened[0].second == "sends");
}

TEST_CASE ("show settings UI: the Network tab declares devices and switches the sender filter")
{
    Rig rig;
    tree::PresetTable presets;
    presets.scan (std::string (WFG_REPO_ROOT) + "/presets/devices");
    rig.parameters.setPresets (&presets);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    /*  THE WINDOW IS NOT ONLY ABOUT AUDIO ANY MORE (2026-09-22), and the tab
        strip is where that shows: the first one is named after the hardware it
        configures rather than after "Interface", which the network tab could
        equally have claimed. */
    CHECK (tabs->getTabNames()[0] == "Audio");
    CHECK (tabs->getTabNames().indexOf ("Network") > tabs->getTabNames().indexOf ("Input patch"));

    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Network"));

    /*  A TAB'S CONTENT IS ONLY A LIVE CHILD WHILE IT SHOWS - juce::
        TabbedComponent's own arrangement - so nothing below can be found until
        the line above has run. */

    /*  ADD MAKES A DEVICE, with no namespace file, which is what makes it an
        opaque one: a desk at an address, not a described tree. Everything else
        about it - the name, the host, the port - is written afterwards with
        `node.set`, which is why this is the only structure gesture the tab
        has. */
    auto* add = button (panel, "ADD");
    REQUIRE (add != nullptr);

    const auto beforeAdd = rig.sent.size();
    add->onClick();

    REQUIRE (rig.sent.size() == beforeAdd + 1);
    CHECK (rig.sent.back().command == "mount.create");
    REQUIRE (rig.sent.back().args.size() == 2u);
    CHECK (rig.sent.back().args[1].getString().empty());

    /*  THE FILTER SAYS WHICH SETTING IS IN FORCE IN WORDS, never as a light
        that is on or off (4.8) - and they are WFS-DIY's own two labels,
        because it is the same switch and the same person reading it. */
    auto* filter = button (panel, "OSC Filter: Accept All");
    REQUIRE (filter != nullptr);

    const auto beforeFilter = rig.sent.size();
    filter->onClick();

    REQUIRE (rig.sent.size() == beforeFilter + 1);
    CHECK (rig.sent.back().command == "node.set");
    REQUIRE (rig.sent.back().args.size() == 2u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/network/strictSenders");
    CHECK (rig.sent.back().args[1].getString() == "true");

    /*  PRESET OPENS THE CHOOSER of what is installed (namespace draft §57,
        AFO), a heading per vendor, and sends nothing until one is chosen;
        choosing sends the one command, with the slug. The same shape as the
        Surfaces tab's profile chooser. */
    auto* preset = button (panel, "PRESET");
    REQUIRE (preset != nullptr);
    const auto beforePreset = rig.sent.size();
    preset->onClick();
    CHECK (rig.sent.size() == beforePreset);
    auto* chooser = component<juce::ComboBox> (panel);
    REQUIRE (chooser != nullptr);
    CHECK (chooser->isVisible());
    REQUIRE (chooser->getNumItems() >= 3);
    int admOsc = 0;
    for (auto item = 0; item < chooser->getNumItems(); ++item)
        if (chooser->getItemText (item).startsWith ("Any processor speaking ADM-OSC"))
            admOsc = chooser->getItemId (item);
    REQUIRE (admOsc != 0);
    chooser->setSelectedId (admOsc, juce::dontSendNotification);
    REQUIRE (chooser->onChange != nullptr);
    chooser->onChange();
    REQUIRE (rig.sent.size() == beforePreset + 1);
    CHECK (rig.sent.back().command == "mount.createFromPreset");
    REQUIRE (rig.sent.back().args.size() == 1u);
    CHECK (rig.sent.back().args[0].getString() == "adm-osc");
    CHECK_FALSE (chooser->isVisible());

    /*  AND UNDER THE LOCK THE STRIP GOES DEAD, devices included. A show in
        show mode is one nobody can restructure, and a device is structure. */
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());

    CHECK_FALSE (tabs->isEnabled());
}

TEST_CASE ("show settings UI: the MIDI tab declares ports and offers this machine's devices")
{
    Rig rig;

    REQUIRE (rig.document.createPort ("Lights").ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    CHECK (tabs->getTabNames().indexOf ("MIDI") == tabs->getTabNames().indexOf ("Network") + 1);

    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("MIDI"));

    /*  ADD DECLARES A PORT, and nothing about a cable: the name is the show's
        and which socket it is on is said afterwards, because the two are
        different kinds of fact (PRD 4.10). */
    auto* add = button (panel, "ADD");
    REQUIRE (add != nullptr);

    const auto before = rig.sent.size();
    add->onClick();

    REQUIRE (rig.sent.size() == before + 1);
    CHECK (rig.sent.back().command == "port.create");
    REQUIRE (rig.sent.back().args.size() == 1u);

    //  And it does not collide with the port already there.
    CHECK (rig.sent.back().args[0].getString() != "Lights");

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());

    CHECK_FALSE (tabs->isEnabled());
}

TEST_CASE ("show settings UI: the Surfaces tab declares a surface, its strips and a DCA")
{
    Rig rig;

    /*  A SHOW WITH ONE SURFACE AND ONE DCA already in it, made through the
        document as a replay would make them: the virtual panel's eight
        strips come with the surface. */
    std::vector<std::string> made;
    const auto desk = rig.document.createSurface ("virtual", "Desk", {}, {}, made);
    REQUIRE (desk.ok);
    REQUIRE (made.size() == 8u);
    REQUIRE (rig.document.createDca ("Band").ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    /*  FOUND BY ITS NAME, not by where it sits: the tab strip has grown one
        tab at a time and will again. After MIDI, because a surface is
        reached through the ports that tab declares. */
    const auto names = tabs->getTabNames();
    const auto index = names.indexOf ("Surfaces");

    REQUIRE (index >= 0);
    CHECK (index > names.indexOf ("MIDI"));

    tabs->setCurrentTabIndex (index);

    //  A tab's content is only a live child while it shows.
    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    /*  THREE LISTS, STACKED: the surfaces, the strips of the one picked -
        the first, until somebody picks another - and the DCAs. */
    std::vector<juce::ListBox*> lists;

    for (auto* child : page->getChildren())
        if (auto* list = dynamic_cast<juce::ListBox*> (child))
            lists.push_back (list);

    REQUIRE (lists.size() == 3u);
    CHECK (lists[0]->getListBoxModel()->getNumRows() == 1);
    CHECK (lists[1]->getListBoxModel()->getNumRows() == 8);
    CHECK (lists[2]->getListBoxModel()->getNumRows() == 1);

    /*  ADD SURFACE ASKS WHICH KIND FIRST, in a popup of the four profiles,
        and sends nothing until one is chosen. The popup is the page's one
        chooser, opened over the button; choosing in it is the gesture. */
    auto* addSurface = button (*page, "ADD SURFACE");
    REQUIRE (addSurface != nullptr);

    const auto beforeSurface = rig.sent.size();
    addSurface->onClick();

    CHECK (rig.sent.size() == beforeSurface);

    auto* chooser = component<juce::ComboBox> (*page);
    REQUIRE (chooser != nullptr);
    CHECK (chooser->isVisible());

    const auto profiles = client::model::profileChoices();
    REQUIRE (chooser->getNumItems() == static_cast<int> (profiles.size()));

    std::string d700Label;

    for (const auto& choice : profiles)
        if (choice.first == "d700")
            d700Label = choice.second;

    REQUIRE_FALSE (d700Label.empty());

    auto d700 = 0;

    for (auto item = 0; item < chooser->getNumItems(); ++item)
        if (chooser->getItemText (item) == juce::String (d700Label))
            d700 = chooser->getItemId (item);

    REQUIRE (d700 != 0);

    chooser->setSelectedId (d700, juce::dontSendNotification);
    REQUIRE (chooser->onChange != nullptr);
    chooser->onChange();

    REQUIRE (rig.sent.size() == beforeSurface + 1);
    CHECK (rig.sent.back().command == "surface.create");
    CHECK (rig.sent.back().origin == "window");
    REQUIRE_FALSE (rig.sent.back().args.empty());
    CHECK (rig.sent.back().args[0].getString() == "d700");

    //  And the chooser goes back out of the way of the button it covered.
    CHECK_FALSE (chooser->isVisible());

    /*  ADD STRIP ADDS TO THE PICKED SURFACE - here the only one, picked
        because it is first. */
    auto* addStrip = button (*page, "ADD STRIP");
    REQUIRE (addStrip != nullptr);

    const auto beforeStrip = rig.sent.size();
    addStrip->onClick();

    REQUIRE (rig.sent.size() == beforeStrip + 1);
    CHECK (rig.sent.back().command == "strip.create");
    REQUIRE_FALSE (rig.sent.back().args.empty());
    CHECK (rig.sent.back().args[0].getString() == desk.id);

    //  ADD DCA makes one under a name nothing else has.
    auto* addDca = button (*page, "ADD DCA");
    REQUIRE (addDca != nullptr);

    const auto beforeDca = rig.sent.size();
    addDca->onClick();

    REQUIRE (rig.sent.size() == beforeDca + 1);
    CHECK (rig.sent.back().command == "dca.create");
    REQUIRE_FALSE (rig.sent.back().args.empty());
    CHECK_FALSE (rig.sent.back().args[0].getString().empty());
    CHECK (rig.sent.back().args[0].getString() != "Band");

    /*  UNDER THE LOCK THE STRUCTURE GOES: no ADD of any kind, as on every
        other tab - a surface, a strip and a DCA are all structure. */
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());

    CHECK_FALSE (tabs->isEnabled());
    CHECK_FALSE (addSurface->isVisible());
    CHECK_FALSE (addStrip->isVisible());
    CHECK_FALSE (addDca->isVisible());
}

TEST_CASE ("audio settings UI: held output tests clear on tab exit and window close")
{
    Rig rig;
    juce::AudioDeviceManager manager;
    audio::AudioSettings settings;
    for (auto* type : manager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        const auto names = type->getDeviceNames (false);
        if (names.isEmpty()) continue;
        settings.deviceType = type->getTypeName().toStdString();
        settings.outputDevice = names[0].toStdString();
        break;
    }
    REQUIRE_FALSE (settings.outputDevice.empty());
    settings.enabled = true; settings.outputPatch = "0 1";
    REQUIRE (rig.document.configureAudio (settings).ok);
    rig.state.audioStatus = "running"; rig.state.audioDevice = settings.outputDevice;
    rig.state.hardwareOutputs = 2;
    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&] (Event event) { rig.sent.push_back (std::move (event)); });
    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    for (bool close : { false, true })
    {
        tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Output patch"));
        auto* page = tabs->getCurrentContentComponent();
        REQUIRE (page != nullptr);
        auto* test = button (*page, "Test");
        REQUIRE (test != nullptr);
        REQUIRE (test->isEnabled());
        test->setToggleState (true, juce::dontSendNotification); test->onClick();
        auto* signal = component<juce::ComboBox> (*page);
        REQUIRE (signal != nullptr);
        CHECK (signal->getSelectedId() == 1);
        signal->setSelectedId (3, juce::dontSendNotification); signal->onChange();
        // Exercise the real message queue and snapshot refresh, rather than
        // only directly invoking control callbacks in one uninterrupted pass.
        for (int frame = 0; frame < 100; ++frame)
        {
            MSG message {};
            while (PeekMessage (&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage (&message); DispatchMessage (&message); }
            panel.refresh (*rig.publish());
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }
        REQUIRE (signal->getSelectedId() == 3);
        auto* matrix = component<spatcore::ui::patch::PatchMatrixComponent> (*page);
        REQUIRE (matrix != nullptr);
        matrix->setSelectedCell ({ 0, 0 });
        matrix->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        CHECK (rig.sent.back().args[1].getInt32() == 0);
        rig.state.audioTest = { 2, 0, 1000, -40, false };
        panel.refresh (*rig.publish());
        matrix->keyStateChanged (false);
        CHECK (rig.sent.back().args[1].getInt32() == -1);
        // The UI can receive the last active snapshot after releasing. Its
        // subsequent idle snapshot must stop the output, not clear Tone.
        panel.refresh (*rig.publish());
        rig.state.audioTest.channel = -1;
        panel.refresh (*rig.publish());
        REQUIRE (signal->getSelectedId() == 3);
        for (int frame = 0; frame < 100; ++frame) panel.refresh (*rig.publish());
        CHECK (signal->getSelectedId() == 3); // idle selection stays ready
        const auto beforePanic = rig.sent.size();
        rig.state.audioTest = {}; // explicit engine stop still clears the UI
        panel.refresh (*rig.publish());
        CHECK (signal->getSelectedId() == 1);
        CHECK (rig.sent.size() == beforePanic); // no echo of a received reset
        signal->setSelectedId (3, juce::dontSendNotification); signal->onChange();
        auto* hold = button (*page, "Hold");
        REQUIRE (hold != nullptr);
        CHECK_FALSE (hold->getToggleState());
        hold->setToggleState (true, juce::dontSendNotification); hold->onClick();
        matrix->setSelectedCell ({ 0, 0 });
        matrix->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        REQUIRE_FALSE (rig.sent.empty());
        CHECK (rig.sent.back().command == "audio.testSignal");
        CHECK (rig.sent.back().args[0].getInt32() == 2);
        CHECK (rig.sent.back().args[1].getInt32() == 0);
        CHECK (rig.sent.back().args[4].getBool());
        rig.state.audioTest = { 2, 0, 1000, -40, true };
        for (int frame = 0; frame < 100; ++frame) panel.refresh (*rig.publish());
        CHECK (signal->getSelectedId() == 3);
        CHECK (hold->getToggleState());
        if (close) panel.closeButtonPressed();
        else tabs->setCurrentTabIndex (0);
        CHECK (rig.sent.back().command == "audio.testStop");
        CHECK (signal->getSelectedId() == 1);
        CHECK_FALSE (hold->getToggleState());
        rig.state.audioTest = {};
    }
}

TEST_CASE ("audio settings UI: opening and rescanning cannot stop the ASIO callback")
{
    auto devices = audio::availableDevices(); // Inspect before any driver is active.
    std::stable_sort (devices.begin(), devices.end(), [] (const auto& a, const auto& b)
    { return (a.type == "ASIO") > (b.type == "ASIO"); });
    const auto storage = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("godot-ui-test-" + juce::Uuid().toString());
    struct Cleanup { juce::File folder; ~Cleanup() { folder.deleteRecursively(); } } cleanup { storage };
    for (const auto& device : devices)
    {
        if (device.outputChannels < 2) continue;
        audio::DeviceAudioDriver driver (storage.getFullPathName().toStdString());
        audio::DeviceAudioDriver::Request request;
        request.deviceName = device.name; request.deviceType = device.type;
        request.inputDeviceName = std::string {};
        request.logicalOutputs = 2;
        request.edit.tracks = 1;
        if (! driver.open (request)) continue;
        MESSAGE ("native panel with live " << device.type << " / " << device.name);
        Rig rig;
        audio::AudioSettings settings;
        settings.enabled = true; settings.deviceType = device.type;
        settings.outputDevice = device.name; settings.inputPatch = "-1 -1"; settings.outputPatch = "0 1";
        REQUIRE (rig.document.configureAudio (settings).ok);
        rig.state.audioStatus = "running"; rig.state.audioDevice = driver.deviceName();
        rig.state.audioBufferSize = driver.settings().blockSize;
        rig.state.audioSampleRate = driver.settings().sampleRate;
        rig.state.audioAvailableBufferSizes = driver.availableBufferSizes();
        rig.state.hardwareInputs = driver.inputChannels();
        rig.state.hardwareOutputs = driver.outputChannels();
        rig.exercisePanel ([&]
        {
            const auto before = driver.blocksDelivered();
            for (int n = 0; n < 200 && driver.blocksDelivered() <= before; ++n)
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
            CHECK (driver.blocksDelivered() > before);
        });
        return;
    }
    MESSAGE ("no usable device; native live-callback portion did not run");
}

//==============================================================================
/*  DOH!'S SETTING ON THE DEVICE ROWS (PRD §3.32; the author, 2026-10-01, D1):
    a Doh! cell on every network device and every MIDI port - Meh, the
    default, or Undo(h), in words (the author's, 2026-10-02; Leave and Take
    back until then) - and on a port the "plays sound" switch.
    Each a `node.set` that lands at once; locked, nothing is sent. Failed before
    D1: there were no such cells.

    ONE CLICK, AT THE MIDDLE OF ONE CELL, and exactly one event from it (the
    review, 2026-10-01): the first version walked a click in from the row's
    right edge until some cell answered, sending the delete cross and every
    cell on the way - so a cell sending too much, or two cells' hits trading
    places, still passed. The words a cell paints are asserted off the rows the
    page reads, which is where the cell takes them from. */
namespace
{
    juce::ListBox* listOn (juce::Component& root)
    {
        if (auto* list = dynamic_cast<juce::ListBox*> (&root))
            return list;

        for (auto* child : root.getChildren())
            if (auto* found = listOn (*child))
                return found;

        return nullptr;
    }

    /*  A click on the first row at `x`, in the list's own coordinates - which
        is the component a row is clicked through, and the width its cells are
        carved from. */
    void clickAt (juce::ListBox& list, int x)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const juce::ModifierKeys left { juce::ModifierKeys::leftButtonModifier };
        const juce::Point<float> at { static_cast<float> (x), 10.0f };
        const auto now = juce::Time::getCurrentTime();

        list.getListBoxModel()->listBoxItemClicked (0, juce::MouseEvent (source, at, left,
            juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
            &list, &list, now, at, now, 1, false));
    }

    /*  THE MIDDLES OF THE CELLS, by the pages' own carve (`cellsFor`), from the
        right: the row's 8 px of padding, then each cell's width. Written out
        here so a column that moves fails a case rather than a show.

        The Network tab: the cross 24, the Kind column 150 (namespace draft 57,
        DP.4), the problem 150, the heard count 54 (O.8), the sent count 54, the
        rollback 150, then Doh! 80. The MIDI tab: the cross 24, the state 190,
        the rollback 150, then Doh! 80 and Sound 56 (2026-10-03: the rollback
        column, OV-OX, moved both). */
    int networkDohAt (const juce::ListBox& list) { return list.getWidth() - 8 - 24 - 150 - 150 - 54 - 54 - 150 - 80 / 2; }

    /*  And Bundles, left of Doh! (namespace draft 45): 62 wide. */
    int networkBundlesAt (const juce::ListBox& list) { return list.getWidth() - 8 - 24 - 150 - 150 - 54 - 54 - 150 - 80 - 62 / 2; }
    int midiDohAt (const juce::ListBox& list)    { return list.getWidth() - 8 - 24 - 190 - 150 - 80 / 2; }
    int midiSoundAt (const juce::ListBox& list)  { return list.getWidth() - 8 - 24 - 190 - 150 - 80 - 56 / 2; }

    /*  The one `node.set` a click sent, as address and value. */
    std::pair<std::string, std::string> theOneSet (const std::vector<Event>& sent)
    {
        REQUIRE (sent.size() == 1u);
        REQUIRE (sent.front().command == "node.set");
        REQUIRE (sent.front().args.size() == 2u);
        return { sent.front().args[0].getString(), sent.front().args[1].getString() };
    }
}

TEST_CASE ("show settings UI: a network device's Doh! cell switches between leaving it to its operator and taking back")
{
    Rig rig;
    REQUIRE (rig.document.createMount ("/lx", "", "QX7DESK0").ok);

    const auto first = rig.publish();
    client::ui::ShowSettingsWindow panel (rig.theme, *first,
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });
    panel.setSize (1400, 800);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Network"));

    auto* list = listOn (*tabs->getCurrentContentComponent());
    REQUIRE (list != nullptr);
    REQUIRE (list->getListBoxModel()->getNumRows() == 1);

    //  Absent is Meh (`leave`), in words: one click on the cell asks to take back.
    CHECK (client::model::readDevices (*first).front().dohWord() == "Meh");

    clickAt (*list, networkDohAt (*list));
    CHECK (theOneSet (rig.sent) == std::pair<std::string, std::string> { "/godot/mount/QX7DESK0/doh", "takeBack" });

    //  And back.
    REQUIRE (rig.document.setAttribute ("/godot/mount/QX7DESK0/doh", "takeBack").ok);
    const auto taking = rig.publish();
    panel.refresh (*taking);
    CHECK (client::model::readDevices (*taking).front().dohWord() == "Undo(h)");

    rig.sent.clear();
    clickAt (*list, networkDohAt (*list));
    CHECK (theOneSet (rig.sent) == std::pair<std::string, std::string> { "/godot/mount/QX7DESK0/doh", "leave" });

    //  Locked, a click sends nothing.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());
    rig.sent.clear();
    clickAt (*list, networkDohAt (*list));
    CHECK (rig.sent.empty());
}

/*  WHETHER ONE TICK'S MESSAGES LEAVE AS ONE BUNDLE (namespace draft 45, YQ):
    the device's own switch, a third ON or OFF after Rx and Tx. */
TEST_CASE ("show settings UI: a network device's Bundles cell switches its messages into bundles and back")
{
    Rig rig;
    REQUIRE (rig.document.createMount ("/wfs", "", "QX7DESK0").ok);

    const auto first = rig.publish();
    client::ui::ShowSettingsWindow panel (rig.theme, *first,
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });
    panel.setSize (1400, 800);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Network"));

    auto* list = listOn (*tabs->getCurrentContentComponent());
    REQUIRE (list != nullptr);
    REQUIRE (list->getListBoxModel()->getNumRows() == 1);

    //  Off by default, and one click asks for bundles.
    CHECK_FALSE (client::model::readDevices (*first).front().bundles);
    clickAt (*list, networkBundlesAt (*list));
    CHECK (theOneSet (rig.sent) == std::pair<std::string, std::string> { "/godot/mount/QX7DESK0/bundles", "true" });

    REQUIRE (rig.document.setAttribute ("/godot/mount/QX7DESK0/bundles", "true").ok);
    const auto bundling = rig.publish();
    panel.refresh (*bundling);
    CHECK (client::model::readDevices (*bundling).front().bundles);

    rig.sent.clear();
    clickAt (*list, networkBundlesAt (*list));
    CHECK (theOneSet (rig.sent) == std::pair<std::string, std::string> { "/godot/mount/QX7DESK0/bundles", "false" });
}

TEST_CASE ("show settings UI: a MIDI port says whether it plays sound and what Doh! does with it")
{
    Rig rig;
    const auto made = rig.document.createPort ("Keys");
    REQUIRE (made.ok);

    const auto first = rig.publish();
    client::ui::ShowSettingsWindow panel (rig.theme, *first,
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });
    panel.setSize (1400, 800);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("MIDI"));

    auto* list = listOn (*tabs->getCurrentContentComponent());
    REQUIRE (list != nullptr);
    REQUIRE (list->getListBoxModel()->getNumRows() == 1);

    //  Sound reads OFF and Doh! reads Meh until somebody says otherwise.
    const auto ports = client::model::readPorts (*first);
    REQUIRE (ports.size() == 1u);
    CHECK_FALSE (ports.front().audible);
    CHECK (ports.front().dohWord() == "Meh");

    clickAt (*list, midiDohAt (*list));
    CHECK (theOneSet (rig.sent) == std::pair<std::string, std::string> { "/godot/port/" + made.id + "/doh", "takeBack" });

    rig.sent.clear();
    clickAt (*list, midiSoundAt (*list));
    CHECK (theOneSet (rig.sent) == std::pair<std::string, std::string> { "/godot/port/" + made.id + "/audible", "true" });

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());
    rig.sent.clear();
    clickAt (*list, midiSoundAt (*list));
    clickAt (*list, midiDohAt (*list));
    CHECK (rig.sent.empty());
}

namespace
{
    /*  THE SURFACES TAB'S STRIP ROW, by the page's own carve (`stripCells`),
        from the left: the row's 8 px of padding, the number's 56 (its grip
        inside it), then the Role cell, at least 120 wide on any window this
        size (namespace draft §30, S4). */
    int stripRoleAt (const juce::ListBox&) { return 8 + 56 + 40; }

    //  A point over the gap above strip row `gap`, on the page that holds the list.
    juce::Point<int> stripGap (const juce::ListBox& list, int gap)
    {
        return { list.getX() + 40, list.getY() + gap * list.getRowHeight() };
    }

    /*  The chooser's item that reads `text`, by its id; 0 when none does. */
    int itemReading (const juce::ComboBox& chooser, const juce::String& text)
    {
        for (auto item = 0; item < chooser.getNumItems(); ++item)
            if (chooser.getItemText (item) == text)
                return chooser.getItemId (item);

        return 0;
    }
}

TEST_CASE ("show settings UI: a strip's Role is one menu of Sampler and the DCAs, and a strip dragged to a new place is moved")
{
    /*  Namespace draft §30, S4: "Removing sampler faders and adding some DCA in
        the surface parameters did not show the DCA anywhere and there is no
        way to rearrange the order." */
    Rig rig;

    std::vector<std::string> made;
    const auto desk = rig.document.createSurface ("virtual", "Desk", {}, {}, made);
    REQUIRE (desk.ok);
    REQUIRE (made.size() == 8u);

    const auto band = rig.document.createDca ("Band");
    const auto keys = rig.document.createDca ("Keys");
    REQUIRE (band.ok);
    REQUIRE (keys.ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });
    panel.setSize (1400, 800);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Surfaces"));

    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    std::vector<juce::ListBox*> lists;

    for (auto* child : page->getChildren())
        if (auto* list = dynamic_cast<juce::ListBox*> (child))
            lists.push_back (list);

    REQUIRE (lists.size() == 3u);
    auto& strips = *lists[1];
    REQUIRE (strips.getListBoxModel()->getNumRows() == 8);

    /*  ONE MENU IN THE ROLE CELL: Sampler - what the strip is now - then a
        DCA for each the show declares. */
    clickAt (strips, stripRoleAt (strips));

    auto* chooser = component<juce::ComboBox> (*page);
    REQUIRE (chooser != nullptr);
    CHECK (chooser->isVisible());
    REQUIRE (chooser->getNumItems() == 3);
    CHECK (chooser->getItemText (0) == "Sampler");
    CHECK (chooser->getItemText (1) == "DCA: Band");
    CHECK (chooser->getItemText (2) == "DCA: Keys");
    CHECK (chooser->getSelectedId() == itemReading (*chooser, "Sampler"));
    CHECK (rig.sent.empty());

    /*  Choosing a DCA sends the role, then the DCA: one choice, both writes -
        as ONE `node.setMany`, so one step of Undo (namespace draft §30.11,
        TA's two steps made one). */
    chooser->setSelectedId (itemReading (*chooser, "DCA: Keys"), juce::dontSendNotification);
    REQUIRE (chooser->onChange != nullptr);
    chooser->onChange();

    const auto base = "/godot/slot/" + made[0] + "/";

    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent[0].command == "node.setMany");
    REQUIRE (rig.sent[0].args.size() == 4u);
    CHECK (rig.sent[0].args[0].getString() == base + "role");
    CHECK (rig.sent[0].args[1].getString() == "dca");
    CHECK (rig.sent[0].args[2].getString() == base + "dca");
    CHECK (rig.sent[0].args[3].getString() == keys.id);
    CHECK_FALSE (chooser->isVisible());

    //  Landed, the menu opens on it - and choosing it again writes nothing.
    for (std::size_t at = 0; at + 1 < rig.sent[0].args.size(); at += 2)
        REQUIRE (rig.document.setAttribute (rig.sent[0].args[at].getString(), rig.sent[0].args[at + 1].getString()).ok);

    panel.refresh (*rig.publish());
    rig.sent.clear();

    clickAt (strips, stripRoleAt (strips));
    CHECK (chooser->getSelectedId() == itemReading (*chooser, "DCA: Keys"));
    chooser->onChange();
    CHECK (rig.sent.empty());

    //  Sampler: the role, and the DCA cleared.
    clickAt (strips, stripRoleAt (strips));
    chooser->setSelectedId (itemReading (*chooser, "Sampler"), juce::dontSendNotification);
    chooser->onChange();

    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent[0].command == "node.setMany");
    REQUIRE (rig.sent[0].args.size() == 4u);
    CHECK (rig.sent[0].args[0].getString() == base + "role");
    CHECK (rig.sent[0].args[1].getString() == "sampler");
    CHECK (rig.sent[0].args[2].getString() == base + "dca");
    CHECK (rig.sent[0].args[3].getString().empty());

    for (std::size_t at = 0; at + 1 < rig.sent[0].args.size(); at += 2)
        REQUIRE (rig.document.setAttribute (rig.sent[0].args[at].getString(), rig.sent[0].args[at + 1].getString()).ok);

    rig.sent.clear();

    /*  DRAGGED: the strip's identifier rides the drag, and letting go under
        the fourth strip moves the first to the fourth place - one
        `object.move` into its own surface. */
    const auto description = strips.getListBoxModel()->getDragSourceDescription (juce::SparseSet<int> {});
    CHECK (description.isVoid());

    juce::SparseSet<int> first;
    first.addRange ({ 0, 1 });
    const auto dragged = strips.getListBoxModel()->getDragSourceDescription (first);
    CHECK (dragged.toString() == juce::String ("strip:" + made[0]));

    auto* target = dynamic_cast<juce::DragAndDropTarget*> (page);
    REQUIRE (target != nullptr);

    const juce::DragAndDropTarget::SourceDetails toFourth { dragged, &strips, stripGap (strips, 4) };
    CHECK (target->isInterestedInDragSource (toFourth));

    target->itemDropped (toFourth);

    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent[0].command == "object.move");
    REQUIRE (rig.sent[0].args.size() == 3u);
    CHECK (rig.sent[0].args[0].getString() == made[0]);
    CHECK (rig.sent[0].args[1].getString() == desk.id);
    CHECK (rig.sent[0].args[2].getInt32() == 3);

    //  Let go either side of itself, it has not moved, and nothing is sent.
    rig.sent.clear();
    target->itemDropped ({ dragged, &strips, stripGap (strips, 0) });
    target->itemDropped ({ dragged, &strips, stripGap (strips, 1) });
    CHECK (rig.sent.empty());

    //  Anything else dropped here is not a strip.
    CHECK_FALSE (target->isInterestedInDragSource ({ juce::var ("plugin:PG7N0001"), &strips, stripGap (strips, 2) }));

    /*  A SHOW WITH NO DCA: the menu says so in a greyed item that points at
        ADD DCA, and offers no DCA strip that would ride nothing. */
    REQUIRE (rig.document.remove (band.id).ok);
    REQUIRE (rig.document.remove (keys.id).ok);
    panel.refresh (*rig.publish());

    clickAt (strips, stripRoleAt (strips));
    REQUIRE (chooser->getNumItems() == 2);
    CHECK (chooser->getItemText (0) == "Sampler");
    CHECK (chooser->getItemText (1) == "No DCA yet: ADD DCA below makes one");
    CHECK (chooser->isItemEnabled (chooser->getItemId (0)));
    CHECK_FALSE (chooser->isItemEnabled (chooser->getItemId (1)));

    //  Locked: no drag, and nothing dropped is taken.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());

    CHECK (strips.getListBoxModel()->getDragSourceDescription (first).isVoid());
    CHECK_FALSE (target->isInterestedInDragSource (toFourth));

    rig.sent.clear();
    target->itemDropped (toFourth);
    CHECK (rig.sent.empty());
}

TEST_CASE ("show settings UI: the SpaceMouse's line, and the driver's button only while the driver holds the puck")
{
    /*  (O.11, ZF) The engine's word, read from the tree every refresh: the
        button to close 3Dconnexion's driver is there only while it holds the
        puck - and asks before it sends anything, which an async alert does. */
    Rig rig;

    const auto first = rig.publish();
    client::ui::ShowSettingsWindow panel (rig.theme, *first,
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });
    panel.setSize (1400, 800);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Surfaces"));

    auto* closeDriver = button (panel, "Close 3DxWare...");
    REQUIRE (closeDriver != nullptr);
    CHECK_FALSE (closeDriver->isVisible());

    rig.state.spaceMouse = "driver";
    panel.refresh (*rig.publish());
    CHECK (closeDriver->isVisible());

    rig.state.spaceMouse = "connected";
    rig.state.spaceMouseName = "SpaceMouse Compact";
    panel.refresh (*rig.publish());
    CHECK_FALSE (closeDriver->isVisible());
    CHECK (rig.sent.empty());
}

TEST_CASE ("show settings UI: the Serial tab declares a port and says what this machine has")
{
    Rig rig;
    rig.state.serialPorts = "COM3\tUSBSER000";

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    const auto index = tabs->getTabNames().indexOf ("Serial");
    REQUIRE (index == tabs->getTabNames().indexOf ("MIDI") + 1);
    tabs->setCurrentTabIndex (index);

    auto* page = tabs->getTabContentComponent (index);
    REQUIRE (page != nullptr);

    //  ADD DECLARES A PORT by a name, and nothing about where it is.
    auto* add = button (*page, "ADD");
    REQUIRE (add != nullptr);
    const auto before = rig.sent.size();
    add->onClick();
    REQUIRE (rig.sent.size() == before + 1);
    CHECK (rig.sent.back().command == "serial.create");
    REQUIRE (rig.sent.back().args.size() == 1u);
    CHECK (rig.sent.back().args[0].getString() == "Serial 1");

    //  WHAT THIS MACHINE HAS, in words under the list.
    bool said = false;
    std::function<void (juce::Component&)> look = [&] (juce::Component& at)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&at))
            if (label->getText().contains ("1 serial port on this machine"))
                said = true;
        for (auto* child : at.getChildren())
            look (*child);
    };
    look (*page);
    CHECK (said);

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());
    CHECK_FALSE (add->isVisible());
}
