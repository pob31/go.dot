/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/client/ui/ShowSettingsWindow.h>
#include <wfg/client/ui/InspectorComponent.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/ui/TransportComponent.h>
#include <wfg/client/model/Pair.h>
#include <wfg/client/model/Problems.h>
#include <wfg/client/model/SettingsPages.h>
#include <wfg/client/model/Transport.h>
#include <wfg/engine/pair/Leadership.h>
#include <wfg/engine/pair/PeerLink.h>
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
#include <wfg/engine/midi/PortTable.h>
#include <wfg/engine/cue/Run.h>
#include <spatcore/ui/patch/PatchMatrixComponent.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <optional>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <tuple>
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
    juce::Component* withId (juce::Component& root, const juce::String& id)
    {
        if (root.getComponentID() == id) return &root;
        for (auto* child : root.getChildren())
            if (auto* found = withId (*child, id)) return found;
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
        selected before anything in it can be found. Found by its name, as
        every page is: where each stands is the layout case's to say. */
    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    REQUIRE (tabs->getTabNames().contains ("Outputs"));
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Outputs"));

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

    //  Before Plugins, among the audio's pages (2026-10-15).
    const auto names = tabs->getTabNames();
    CHECK (names.indexOf ("Rack") == names.indexOf ("Plugins") - 1);
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

    //  The first page of the Show section (2026-10-15; the layout case has the order).
    const auto names = tabs->getTabNames();
    REQUIRE (names.contains ("Playback"));
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
TEST_CASE ("show settings UI: the window is called Show settings, and its pages stand in sections down the left")
{
    /*  Author, 2026-10-15: "The window is still called Audio Settings. We
        renamed the menu item to Show Settings. The window should say this
        too", and "It's a bit crowded in the tabs ... a vertical tab list to
        the left ... a few sections Show, Audio, Video, ... Controllers,
        Getting started" - Network among the controllers, and the sections in
        a list down the left rather than a second strip of tabs. */
    Rig rig;
    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    CHECK (panel.getName() == "Show settings");

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    //  Every page, in the author's order, and no strip of tabs to show them.
    const juce::StringArray pages { "Playback", "Templates", "Redundancy",
                                    "Audio interface", "Outputs", "Output patch", "Inputs", "Input patch", "Rack", "Plugins",
                                    "Video",
                                    "Network", "MIDI", "Surfaces", "Timecode", "Serial", "Bindings",
                                    "Getting started" };
    CHECK (tabs->getTabNames() == pages);
    CHECK_FALSE (tabs->getTabbedButtonBar().isVisible());

    //  It opens on the audio interface, what a show is set up with first, though its section is the second.
    CHECK (tabs->getCurrentTabName() == "Audio interface");

    //  THE LIST DOWN THE LEFT: each section's heading over its pages; a section of one page is its heading alone.
    auto* list = withId (panel, "pageList");
    REQUIRE (list != nullptr);

    std::vector<juce::Button*> rows;

    const std::function<void (juce::Component&)> collect = [&] (juce::Component& at)
    {
        if (auto* b = dynamic_cast<juce::Button*> (&at))
            rows.push_back (b);

        for (auto* child : at.getChildren())
            collect (*child);
    };

    collect (*list);

    std::vector<juce::String> words;
    for (auto* row : rows)
        words.push_back (row->getButtonText());

    CHECK (words == std::vector<juce::String> { "Show", "Playback", "Templates", "Redundancy",
                                                "Audio", "Audio interface", "Outputs", "Output patch", "Inputs",
                                                "Input patch", "Rack", "Plugins",
                                                "Video",
                                                "Controllers", "Network", "MIDI", "Surfaces", "Timecode", "Serial",
                                                "Bindings", "Getting started" });

    //  The page showing is marked, and nothing else is.
    const auto marked = [&rows]
    {
        std::vector<juce::String> on;

        for (auto* row : rows)
            if (row->getToggleState())
                on.push_back (row->getButtonText());

        return on;
    };

    CHECK (marked() == std::vector<juce::String> { "Audio interface" });

    const auto press = [&rows] (const juce::String& text)
    {
        for (auto* row : rows)
            if (row->getButtonText() == text)
            {
                REQUIRE (row->onClick != nullptr);
                row->onClick();
                return;
            }

        FAIL ("no row reads " << text);
    };

    //  A page goes there, and the mark moves with it.
    press ("MIDI");
    CHECK (tabs->getCurrentTabName() == "MIDI");
    CHECK (marked() == std::vector<juce::String> { "MIDI" });

    //  A heading goes to its section's first page.
    press ("Show");
    CHECK (tabs->getCurrentTabName() == "Playback");
    CHECK (marked() == std::vector<juce::String> { "Playback" });
    press ("Audio");
    CHECK (tabs->getCurrentTabName() == "Audio interface");
    press ("Controllers");
    CHECK (tabs->getCurrentTabName() == "Network");

    //  And a section of one page is that page, its heading marked.
    press ("Video");
    CHECK (tabs->getCurrentTabName() == "Video");
    CHECK (marked() == std::vector<juce::String> { "Video" });

    //  Moved from anywhere else - Getting started's buttons - the list follows.
    tabs->setCurrentTabIndex (pages.indexOf ("Rack"));
    CHECK (marked() == std::vector<juce::String> { "Rack" });

    /*  A LOOK, with no screen: the window at the size it opens at, on the
        audio interface and on Getting started, painted into PNGs when
        WFG_SNAPSHOT_DIR is set and skipped otherwise. */
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        panel.setSize (1400, 740);

        for (const auto& [page, name] : { std::pair { "Audio interface", "show-settings.png" },
                                          std::pair { "Getting started", "show-settings-getting-started.png" } })
        {
            tabs->setCurrentTabIndex (pages.indexOf (page));
            panel.refresh (*rig.publish());

            const auto picture = panel.createComponentSnapshot (panel.getLocalBounds());
            const juce::File file { juce::File (dir).getChildFile (name) };
            file.getParentDirectory().createDirectory();
            file.deleteFile();

            juce::FileOutputStream out { file };
            REQUIRE (out.openedOk());

            juce::PNGImageFormat png;
            CHECK (png.writeImageToStream (picture, out));
            MESSAGE ("wrote " << file.getFullPathName().toStdString());
        }
    }

    //  Under the lock the list goes dead with the pages, as the strip of tabs did.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());
    CHECK_FALSE (tabs->isEnabled());
    CHECK_FALSE (list->isEnabled());
}

TEST_CASE ("show settings UI: the interface menus look again as they open, and keep an interface this machine lacks")
{
    /*  The author, 2026-10-15: "Can the screen detection, MIDI detection,
        Audio device detection happen in the Show Settings panel when clicking
        on the drop down menu again and not just when the program starts?"
        Opening either menu fills it again from what the system has - the
        path Rescan interfaces takes, which opens no device. An interface the
        show names and this machine lacks stays in its menu, marked, and Apply
        still sends its own name. */
    Rig rig;
    audio::AudioSettings settings;
    settings.enabled = true;
    settings.deviceType = "Windows Audio";
    settings.outputDevice = "Nowhere Interface";
    REQUIRE (rig.document.configureAudio (settings).ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* output = dynamic_cast<juce::ComboBox*> (withId (panel, "outputInterface"));
    auto* input = dynamic_cast<juce::ComboBox*> (withId (panel, "inputInterface"));
    REQUIRE (output != nullptr);
    REQUIRE (input != nullptr);

    CHECK (output->getText() == "Nowhere Interface  (not on this machine)");

    //  Emptied by hand, its words kept, then opened: the menu is filled again before it shows.
    const auto words = output->getText();
    output->clear (juce::dontSendNotification);
    output->setText (words, juce::dontSendNotification);
    REQUIRE (output->getNumItems() == 0);

    output->showPopup();
    output->hidePopup();
    CHECK (output->getNumItems() >= 2);    // "System default", the interfaces, the one this machine lacks

    input->showPopup();
    input->hidePopup();
    CHECK (input->getNumItems() >= 1);

    CHECK (output->getText() == "Nowhere Interface  (not on this machine)");
    CHECK (input->getSelectedId() == 1);    // "None", as the show says

    //  And Apply names the interface without the mark.
    auto* apply = button (panel, "Apply while stopped");
    REQUIRE (apply != nullptr);
    apply->onClick();
    REQUIRE_FALSE (rig.sent.empty());
    CHECK (rig.sent.back().command == "audio.setup");
    CHECK (rig.sent.back().args[2].getString() == "Nowhere Interface");
}

TEST_CASE ("show settings UI: a page with a problem is marked, its heading too, and the mark goes with the problem (§64)")
{
    /*  The author, 2026-10-15: "In the Show settings, mark sections or tabs
        with problems." The page's row wears the warning and a count, its
        section's heading the sum; the sentences are the tooltip. */
    Rig rig;
    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* list = withId (panel, "pageList");
    REQUIRE (list != nullptr);

    const auto row = [list] (const juce::String& words) -> juce::Button*
    {
        return button (*list, words);
    };

    namespace page = client::model::settingsPages;
    using client::model::Icon;

    std::vector<client::model::PageProblems> pages {
        { page::midi, Icon::midi, { "MIDI port Lights: no such device - Show settings, MIDI.",
                                    "MIDI port Desk: no such device - Show settings, MIDI." }, {} },
        { page::serial, Icon::serial, { "Serial port Arduino: in use - Show settings, Serial." }, {} },
        { page::video, Icon::video, { "Output Front: no such display - Show settings, Video." }, {} },
        { "", Icon::missing, { "Missing: thunder.wav ..." }, { "SND00001" } },
    };

    const auto picture = [list]
    {
        juce::Image canvas (juce::Image::ARGB, list->getWidth(), list->getHeight(), true, juce::SoftwareImageType());
        juce::Graphics g (canvas);
        list->paintEntireComponent (g, false);
        return canvas;
    };

    const auto before = picture();
    panel.showProblems (pages);
    const auto after = picture();

    //  The rows that changed are the pages with problems and their headings - and nothing else.
    REQUIRE (row ("MIDI") != nullptr);
    CHECK (row ("MIDI")->getTooltip().contains ("MIDI port Lights"));
    CHECK (row ("MIDI")->getTooltip().contains ("MIDI port Desk"));
    CHECK (row ("Serial")->getTooltip().contains ("Arduino"));
    CHECK (row ("Controllers")->getTooltip().contains ("MIDI port Lights"));
    CHECK (row ("Controllers")->getTooltip().contains ("Arduino"));
    CHECK (row ("Video")->getTooltip().contains ("Output Front"));
    CHECK (row ("Network")->getTooltip().isEmpty());
    CHECK (row ("Audio")->getTooltip().isEmpty());

    const auto changedIn = [&before, &after] (const juce::Button& entry)
    {
        const auto area = entry.getBoundsInParent();
        auto changed = 0;

        for (int y = area.getY(); y < area.getBottom(); ++y)
            for (int x = area.getX(); x < area.getRight(); ++x)
                if (before.getPixelAt (x, y) != after.getPixelAt (x, y))
                    ++changed;

        return changed;
    };

    CHECK (changedIn (*row ("MIDI")) > 0);
    CHECK (changedIn (*row ("Controllers")) > 0);
    CHECK (changedIn (*row ("Network")) == 0);
    CHECK (changedIn (*row ("Playback")) == 0);

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        panel.setSize (1400, 740);
        panel.showPage (page::midi);

        const auto shot = panel.createComponentSnapshot (panel.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile ("show-settings-problems.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());
        CHECK (juce::PNGImageFormat().writeImageToStream (shot, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }

    //  Put right, the marks go.
    panel.showProblems ({});
    CHECK (row ("MIDI")->getTooltip().isEmpty());
    CHECK (row ("Controllers")->getTooltip().isEmpty());
}

TEST_CASE ("show settings UI: a page asked for from outside is shown, by the model's own names")
{
    /*  Namespace draft §64: a warning in the main window opens Show settings
        on the page its problem is put right on, so the window turns to a page
        named from outside - by the names the std-only model knows them by,
        which are the window's own. */
    Rig rig;
    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    namespace page = client::model::settingsPages;

    for (const auto* name : { page::playback, page::templates, page::redundancy, page::audioInterface, page::outputs,
                              page::outputPatch, page::inputs, page::inputPatch, page::rack, page::plugins, page::video,
                              page::network, page::midi, page::surfaces, page::timecode, page::serial, page::bindings,
                              page::gettingStarted })
    {
        CAPTURE (name);
        REQUIRE (tabs->getTabNames().contains (name));

        panel.showPage (name);
        CHECK (tabs->getCurrentTabName() == name);
    }

    //  A name the window has not leaves it where it was.
    panel.showPage (page::midi);
    panel.showPage ("Nowhere");
    CHECK (tabs->getCurrentTabName() == page::midi);
}

TEST_CASE ("show settings UI: Getting started names every other page under its section, in order, and goes to it")
{
    /*  Author, 2026-09-30: "Should we add a 'Getting started' tab in the
        settings window explaining what people should do? And what each tab is
        for..." - as the last page. Then 2026-10-15: "Getting started should
        be reordered and the links should match the new layout". */
    Rig rig;
    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    const auto names = tabs->getTabNames();
    REQUIRE (names[names.size() - 1] == "Getting started");

    tabs->setCurrentTabIndex (names.size() - 1);
    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    /*  A button for every other page, in the list's own order, each under its
        section's heading - the headings in capitals, as down the left; the
        sentences beside the buttons are the only other words, and none of
        them is in capitals. */
    std::vector<juce::String> listed, buttons;

    const std::function<void (juce::Component&)> walk = [&] (juce::Component& at)
    {
        if (auto* b = dynamic_cast<juce::Button*> (&at))
        {
            listed.push_back (b->getButtonText());
            buttons.push_back (b->getButtonText());
        }
        else if (auto* label = dynamic_cast<juce::Label*> (&at);
                 label != nullptr && label->getText() == label->getText().toUpperCase())
            listed.push_back (label->getText());

        for (auto* child : at.getChildren())
            walk (*child);
    };

    walk (*page);

    CHECK (listed == std::vector<juce::String> { "SHOW", "Playback", "Templates", "Redundancy",
                                                 "AUDIO", "Audio interface", "Outputs", "Output patch", "Inputs",
                                                 "Input patch", "Rack", "Plugins",
                                                 "VIDEO", "Video",
                                                 "CONTROLLERS", "Network", "MIDI", "Surfaces", "Timecode", "Serial", "Bindings" });

    //  The buttons alone are every other page, in the window's own order.
    std::vector<juce::String> others;

    for (int at = 0; at < names.size() - 1; ++at)
        others.push_back (names[at]);

    CHECK (buttons == others);

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
    CHECK (tabs->getTabNames().contains ("Audio interface"));
    CHECK (tabs->getTabNames().contains ("Network"));

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

    //  Nothing but a reading of your folder (namespace draft §76, ARZ) until one is chosen.
    REQUIRE (rig.sent.size() == beforePreset + 1);
    CHECK (rig.sent.back().command == "preset.rescan");
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
    REQUIRE (rig.sent.size() == beforePreset + 2);
    CHECK (rig.sent.back().command == "mount.createFromPreset");
    REQUIRE (rig.sent.back().args.size() == 1u);
    CHECK (rig.sent.back().args[0].getString() == "adm-osc");
    CHECK_FALSE (chooser->isVisible());

    /*  FOUND ASKS THE NETWORK AGAIN as its menu opens (namespace draft §76,
        ARW): one network.browse; what is picked from it is mount.describeAt. */
    auto* found = button (panel, "FOUND");
    REQUIRE (found != nullptr);
    CHECK (found->isVisible());
    const auto beforeFound = rig.sent.size();
    found->onClick();
    REQUIRE (rig.sent.size() == beforeFound + 1);
    CHECK (rig.sent.back().command == "network.browse");
    juce::PopupMenu::dismissAllActiveMenus();

    //  And the folder of your presets is there to open.
    CHECK (button (panel, "Show presets folder") != nullptr);

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
    //  After Network, among the controllers (2026-10-15).
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
        else tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Audio interface"));
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

    /*  Per tick, left of Doh! (namespace draft §88): 62 wide. And Bundles, left
        of it (namespace draft 45): 62 wide. */
    int networkPerTickAt (const juce::ListBox& list) { return list.getWidth() - 8 - 24 - 150 - 150 - 54 - 54 - 150 - 80 - 62 / 2; }
    int networkBundlesAt (const juce::ListBox& list) { return list.getWidth() - 8 - 24 - 150 - 150 - 54 - 54 - 150 - 80 - 62 - 62 / 2; }

    /*  The label a click opened its editor on, anywhere under `root`. */
    juce::Label* labelBeingEdited (juce::Component& root)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&root); label != nullptr && label->isBeingEdited())
            return label;

        for (auto* child : root.getChildren())
            if (auto* found = labelBeingEdited (*child))
                return found;

        return nullptr;
    }
    int midiDohAt (const juce::ListBox& list)    { return list.getWidth() - 8 - 24 - 190 - 150 - 80 / 2; }

    /*  The MIDI tab's "Sends on", left of Listens to (210 each) and Rx and Tx
        (42 each) and Sound; the Serial tab's Where and Speed: the cross 24,
        the state 240, heard 180, tx and rx 42, reads 60, then speed 90 and
        where 240 (namespace draft §64's menus). */
    int midiSendsAt (const juce::ListBox& list)  { return list.getWidth() - 8 - 24 - 190 - 150 - 80 - 56 - 42 - 42 - 210 - 210 / 2; }
    int serialWhereAt (const juce::ListBox& list) { return list.getWidth() - 8 - 24 - 240 - 180 - 42 - 42 - 60 - 90 - 240 / 2; }
    int serialSpeedAt (const juce::ListBox& list) { return list.getWidth() - 8 - 24 - 240 - 180 - 42 - 42 - 60 - 90 / 2; }
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

    //  The tab coming up reads your presets again (namespace draft §76); that is not this case's.
    rig.sent.clear();

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

    //  The tab coming up reads your presets again (namespace draft §76); that is not this case's.
    rig.sent.clear();

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

/*  HOW MANY DATAGRAMS A DEVICE IS GIVEN A TICK (namespace draft §88, AUT): a
    number after Bundles, 20 until somebody types another, typed in place. */
TEST_CASE ("show settings UI: a network device's Per tick cell is typed in place and sent as its row (§88)")
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
    rig.sent.clear();

    auto* list = listOn (*tabs->getCurrentContentComponent());
    REQUIRE (list != nullptr);
    CHECK (client::model::readDevices (*first).front().perTick == 20);

    clickAt (*list, networkPerTickAt (*list));

    auto* label = labelBeingEdited (*tabs->getCurrentContentComponent());
    REQUIRE (label != nullptr);
    REQUIRE (label->getCurrentTextEditor() != nullptr);
    CHECK (label->getCurrentTextEditor()->getText() == "20");

    /*  RETURN, DELIVERED: a TextEditor posts it as a command message
        (`TextEditorDefs::returnKeyMessageId`, juce_TextEditor.cpp) and a test
        runs no message loop, so the message is handed over here as the loop
        would - the label then takes the typed text and closes its editor. Not
        `Label::setText`, which closes the editor first and commits what was
        there before. */
    const auto pressReturn = [] (juce::Label& editing)
    {
        constexpr int returnKeyMessageId = 0x10003002;
        static_cast<juce::Component&> (*editing.getCurrentTextEditor()).handleCommandMessage (returnKeyMessageId);
    };

    //  Unchanged is not a write.
    pressReturn (*label);
    CHECK (rig.sent.empty());

    clickAt (*list, networkPerTickAt (*list));
    label = labelBeingEdited (*tabs->getCurrentContentComponent());
    REQUIRE (label != nullptr);
    label->getCurrentTextEditor()->setText ("5", juce::dontSendNotification);
    pressReturn (*label);

    CHECK (theOneSet (rig.sent) == std::pair<std::string, std::string> { "/godot/mount/QX7DESK0/perTick", "5" });
}

/*  THE BOX SHOWS WHAT THE CLICK DID (namespace draft §88). The author ticked
    Bundles on WFS-DIY's device three times, 2026-10-18, and each time the
    second click a second later turned it off again: the tab redrew only when
    a field on its list moved, and Bundles and Per tick were not on it, so the
    show changed and the box did not.

    A REDRAW ASKED FOR, SEEN WITHOUT A SCREEN: a test has no window to paint
    into, so the list is given a cached image that counts what it is asked to
    make again - which is where a repaint lands before it reaches a window. */
TEST_CASE ("show settings UI: the Network tab redraws when only a device's Bundles or Per tick changes (§88)")
{
    struct RedrawsAsked final : juce::CachedComponentImage
    {
        int asked = 0;
        void paint (juce::Graphics&) override {}
        bool invalidateAll() override { ++asked; return true; }
        bool invalidate (const juce::Rectangle<int>&) override { ++asked; return true; }
        void releaseResources() override {}
    };

    Rig rig;
    REQUIRE (rig.document.createMount ("/wfs", "", "QX7DESK0").ok);

    const auto first = rig.publish();
    client::ui::ShowSettingsWindow panel (rig.theme, *first, [] (Event) {});
    panel.setSize (1400, 800);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Network"));

    auto* list = listOn (*tabs->getCurrentContentComponent());
    REQUIRE (list != nullptr);

    auto* redraws = new RedrawsAsked();
    list->setCachedComponentImage (redraws);        // the list owns it from here

    //  The tab's first refresh since it came up settles it; after that,
    //  nothing moved is nothing redrawn, so the count means something.
    panel.refresh (*first);
    redraws->asked = 0;
    panel.refresh (*first);
    CHECK (redraws->asked == 0);

    REQUIRE (rig.document.setAttribute ("/godot/mount/QX7DESK0/bundles", "true").ok);
    panel.refresh (*rig.publish());
    CHECK (redraws->asked > 0);

    redraws->asked = 0;
    REQUIRE (rig.document.setAttribute ("/godot/mount/QX7DESK0/perTick", "60").ok);
    panel.refresh (*rig.publish());
    CHECK (redraws->asked > 0);
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

TEST_CASE ("show settings UI: the MIDI and Serial menus look for devices again before they open (§64)")
{
    /*  The author, 2026-10-15: "Can the screen detection, MIDI detection,
        Audio device detection happen in the Show Settings panel when clicking
        on the drop down menu again and not just when the program starts?" A
        device menu asks the engine to look again and opens once it has - the
        count of looks moved past what the menu saw - so a device plugged in a
        moment ago is in it; or half a second on, with what there is. */
    Rig rig;
    midi::PortTable ports;
    ports.countScan();
    rig.parameters.setMidiPorts (&ports);
    rig.state.serialScans = 1;

    REQUIRE (rig.document.createPort ("Lights").ok);
    REQUIRE (rig.document.createSerial ("Arduino").ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });
    panel.setSize (1400, 740);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    const auto offers = [] (const juce::ComboBox& box, const juce::String& words)
    {
        for (auto at = 0; at < box.getNumItems(); ++at)
            if (box.getItemText (at).contains (words))
                return true;

        return false;
    };

    //  MIDI: Sends on asks first, and nothing opens yet.
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("MIDI"));
    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);
    auto* list = listOn (*page);
    REQUIRE (list != nullptr);
    auto* chooser = component<juce::ComboBox> (*page);
    REQUIRE (chooser != nullptr);

    rig.sent.clear();
    clickAt (*list, midiSendsAt (*list));
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().command == "midi.rescan");
    CHECK_FALSE (chooser->isVisible());

    panel.refresh (*rig.publish());
    CHECK_FALSE (chooser->isVisible());     // the engine has not looked yet

    //  The engine looks: a synth plugged in since, and one look more. The menu opens with it.
    ports.setDevices ({}, { { "New synth", "usb#9" } });
    ports.countScan();
    panel.refresh (*rig.publish());
    CHECK (chooser->isVisible());
    CHECK (offers (*chooser, "New synth"));
    chooser->hidePopup();
    chooser->setVisible (false);

    //  And a look that never comes: the menu opens half a second on, with what there is.
    rig.sent.clear();
    clickAt (*list, midiSendsAt (*list));
    REQUIRE (rig.sent.size() == 1u);
    panel.refresh (*rig.publish());
    CHECK_FALSE (chooser->isVisible());
    juce::Thread::sleep (600);
    panel.refresh (*rig.publish());
    CHECK (chooser->isVisible());
    chooser->hidePopup();

    //  SERIAL: Where asks first, the same way.
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Serial"));
    page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);
    list = listOn (*page);
    REQUIRE (list != nullptr);
    chooser = component<juce::ComboBox> (*page);
    REQUIRE (chooser != nullptr);

    rig.sent.clear();
    clickAt (*list, serialWhereAt (*list));
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().command == "serial.rescan");
    CHECK_FALSE (chooser->isVisible());

    rig.state.serialPorts = "COM7\tUSBSER001";
    rig.state.serialScans = 2;
    panel.refresh (*rig.publish());
    CHECK (chooser->isVisible());
    CHECK (offers (*chooser, "COM7"));
    chooser->hidePopup();
    chooser->setVisible (false);

    //  A speed needs no look: its menu opens at once and nothing is sent.
    rig.sent.clear();
    clickAt (*list, serialSpeedAt (*list));
    CHECK (rig.sent.empty());
    CHECK (chooser->isVisible());
    chooser->hidePopup();
}

TEST_CASE ("show settings UI: the Serial tab declares a port and says what this machine has")
{
    Rig rig;
    rig.state.serialPorts = "COM3\tUSBSER000";

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    //  The last of the controllers (2026-10-15).
    const auto index = tabs->getTabNames().indexOf ("Serial");
    REQUIRE (index == tabs->getTabNames().indexOf ("Timecode") + 1);
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

TEST_CASE ("show settings UI: the Timecode tab sits before Playback, and each of its rows is a node.set on /godot/sync")
{
    /*  Namespace draft §59 (AHA, AHW; TC.8): the rate, the MIDI port MTC
        leaves on, the one-channel output LTC plays on and its level - each a
        `node.set` that lands at once, nothing waiting for Apply. The words
        were the implementer's, accepted by the author as offered (2026-10-12). */
    Rig rig;

    const auto port = rig.document.createPort ("Desk");
    REQUIRE (port.ok);
    const auto mono = rig.document.createBus ("direct", 1);
    const auto stereo = rig.document.createBus ("direct", 2);
    REQUIRE (mono.ok);
    REQUIRE (stereo.ok);
    REQUIRE (rig.document.setAttribute ("/godot/bus/" + mono.id + "/name", "Code").ok);
    REQUIRE (rig.document.setAttribute ("/godot/bus/" + stereo.id + "/name", "Main").ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    //  Among the controllers, after Surfaces and before Serial (2026-10-15).
    const auto names = tabs->getTabNames();
    REQUIRE (names.contains ("Timecode"));
    CHECK (names.indexOf ("Timecode") == names.indexOf ("Surfaces") + 1);
    CHECK (names.indexOf ("Timecode") == names.indexOf ("Serial") - 1);
    tabs->setCurrentTabIndex (names.indexOf ("Timecode"));

    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    std::vector<juce::ComboBox*> menus;
    std::vector<juce::TextEditor*> boxes;
    std::vector<juce::Label*> labels;

    std::function<void (juce::Component&)> collect = [&] (juce::Component& at)
    {
        if (auto* menu = dynamic_cast<juce::ComboBox*> (&at))
            menus.push_back (menu);
        else if (auto* box = dynamic_cast<juce::TextEditor*> (&at))
            boxes.push_back (box);
        else if (auto* label = dynamic_cast<juce::Label*> (&at))
            labels.push_back (label);

        for (auto* child : at.getChildren())
            collect (*child);
    };
    collect (*page);

    /*  Three menus and a number, in the order the rows stand - then, since
        TG.10, the Coming in part's two menus and four numbers after them
        (namespace draft §60; their own case below). */
    REQUIRE (menus.size() == 5u);
    REQUIRE (boxes.size() == 5u);
    auto* rate = menus[0];
    auto* mtc = menus[1];
    auto* ltc = menus[2];
    auto* level = boxes[0];

    const auto shownLabel = [&labels] (const juce::String& text) -> juce::Label*
    {
        for (auto* label : labels)
            if (label->getText() == text)
                return label;

        return nullptr;
    };

    for (const auto* row : { "Frame rate", "MTC leaves on", "LTC plays on", "LTC level", "dBFS" })
    {
        INFO (row);
        CHECK (shownLabel (row) != nullptr);
    }

    //  What a show nobody has set shows: 25, no port, no output, -18 dBFS - and no line about the output.
    CHECK (rate->getText() == "25");
    CHECK (mtc->getText() == "None");
    CHECK (ltc->getText() == "None");
    CHECK (level->getText().getDoubleValue() == doctest::Approx (-18.0));

    auto* carriesOnly = shownLabel ("This output carries timecode only: no sound is heard on it.");
    REQUIRE (carriesOnly != nullptr);
    CHECK_FALSE (carriesOnly->isVisible());

    //  The rates, worded; the ports by name; every output, the ones wider than a channel disabled and saying why.
    REQUIRE (rate->getNumItems() == 6);
    CHECK (rate->getItemText (3) == "29.97 drop-frame");
    REQUIRE (mtc->getNumItems() == 2);
    CHECK (mtc->getItemText (1) == "Desk");

    auto codeItem = 0;
    auto mainItem = 0;

    for (int at = 0; at < ltc->getNumItems(); ++at)
    {
        const auto text = ltc->getItemText (at);
        const auto item = ltc->getItemId (at);
        INFO (text);

        if (text == "None")
            CHECK (ltc->isItemEnabled (item));
        else if (text == "Code")
        {
            codeItem = item;
            CHECK (ltc->isItemEnabled (item));
        }
        else if (text == "Main (2 channels: LTC needs one)")
        {
            mainItem = item;
            CHECK_FALSE (ltc->isItemEnabled (item));
        }
        else
        {
            //  Anything else the show has is enabled only if it is one channel.
            CHECK (text.contains ("channels: LTC needs one") != ltc->isItemEnabled (item));
        }
    }

    REQUIRE (codeItem > 0);
    REQUIRE (mainItem > 0);

    //  Each control sends its own row: the rate's stored word, the port's identifier, the output's, the level.
    rate->setSelectedId (4, juce::sendNotificationSync);
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().command == "node.set");
    CHECK (rig.sent.back().args[0].getString() == "/godot/sync/frameRate");
    CHECK (rig.sent.back().args[1].getString() == "29.97df");

    mtc->setSelectedId (2, juce::sendNotificationSync);
    REQUIRE (rig.sent.size() == 2u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/sync/mtcOut");
    CHECK (rig.sent.back().args[1].getString() == port.id);

    ltc->setSelectedId (codeItem, juce::sendNotificationSync);
    REQUIRE (rig.sent.size() == 3u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/sync/ltcOut");
    CHECK (rig.sent.back().args[1].getString() == mono.id);

    //  A disabled output is never sent, however it came to be picked.
    ltc->setSelectedId (mainItem, juce::sendNotificationSync);
    CHECK (rig.sent.size() == 3u);

    //  A comma is a decimal point, as a French booth types it.
    level->setText ("-20,5", juce::dontSendNotification);
    level->onReturnKey();
    REQUIRE (rig.sent.size() == 4u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/sync/ltcLevel");
    CHECK (rig.sent.back().args[1].getString() == "-20.5");

    //  Once the show says so, the tab shows it - and the line under the LTC row appears.
    REQUIRE (rig.document.setAttribute ("/godot/sync/frameRate", "29.97df").ok);
    REQUIRE (rig.document.setAttribute ("/godot/sync/mtcOut", port.id).ok);
    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcOut", mono.id).ok);
    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcLevel", "-20.5").ok);
    rig.state.mtcLate = 2;
    rig.state.mtcWorstMs = 3.5;
    rig.state.ltcLate = 0;
    panel.refresh (*rig.publish());

    CHECK (rate->getText() == "29.97 drop-frame");
    CHECK (mtc->getText() == "Desk");
    CHECK (ltc->getText() == "Code");
    CHECK (carriesOnly->isVisible());

    //  The machine's counts, a quiet line.
    CHECK (shownLabel (juce::String::fromUTF8 ("MTC late: 2 (worst 3.5 ms) \xc2\xb7 LTC late: 0")) != nullptr);

    //  The value a row already has is not an edit; None is a choice that sends the empty identifier.
    rig.sent.clear();
    rate->setSelectedId (4, juce::sendNotificationSync);
    level->onFocusLost();
    CHECK (rig.sent.empty());

    mtc->setSelectedId (1, juce::sendNotificationSync);
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/sync/mtcOut");
    CHECK (rig.sent.back().args[1].getString().empty());

    //  Locked, nothing on the tab can be changed.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());
    CHECK_FALSE (rate->isEnabled());
    CHECK_FALSE (mtc->isEnabled());
    CHECK_FALSE (ltc->isEnabled());
    CHECK_FALSE (level->isEnabled());
}

TEST_CASE ("show settings UI: the Timecode tab's Coming in part sends a node.set for each row, LTC offered one-channel inputs only")
{
    /*  Namespace draft §60 (AIM, AIN; TG.10): the input LTC comes in on, the
        port MTC comes in on, and the four chase settings - each a `node.set`
        on its /godot/sync row that lands at once - and a line for each code
        the show names. The words were the implementer's, accepted by the
        author as offered (2026-10-12/13). */
    Rig rig;

    const auto port = rig.document.createPort ("Desk");
    const auto mono = rig.document.createInput (1);
    const auto stereo = rig.document.createInput (2);
    REQUIRE (port.ok);
    REQUIRE (mono.ok);
    REQUIRE (stereo.ok);
    REQUIRE (rig.document.setAttribute ("/godot/input/" + mono.id + "/name", "Code in").ok);
    REQUIRE (rig.document.setAttribute ("/godot/input/" + stereo.id + "/name", "Keys").ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    const auto names = tabs->getTabNames();
    REQUIRE (names.contains ("Timecode"));
    tabs->setCurrentTabIndex (names.indexOf ("Timecode"));

    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    std::vector<juce::ComboBox*> menus;
    std::vector<juce::TextEditor*> boxes;
    std::vector<juce::Label*> labels;

    std::function<void (juce::Component&)> collect = [&] (juce::Component& at)
    {
        if (auto* menu = dynamic_cast<juce::ComboBox*> (&at))
            menus.push_back (menu);
        else if (auto* box = dynamic_cast<juce::TextEditor*> (&at))
            boxes.push_back (box);
        else if (auto* label = dynamic_cast<juce::Label*> (&at))
            labels.push_back (label);

        for (auto* child : at.getChildren())
            collect (*child);
    };
    collect (*page);

    //  After the outgoing rows: two menus and four numbers, in the order the rows stand.
    REQUIRE (menus.size() == 5u);
    REQUIRE (boxes.size() == 5u);
    auto* ltcIn = menus[3];
    auto* mtcIn = menus[4];
    auto* tolerance = boxes[1];
    auto* jumpPast = boxes[2];
    auto* catchUp = boxes[3];
    auto* crossfade = boxes[4];

    const auto shownLabel = [&labels] (const juce::String& text) -> juce::Label*
    {
        for (auto* label : labels)
            if (label->getText() == text)
                return label;

        return nullptr;
    };

    for (const auto* row : { "Coming in", "LTC comes in on", "MTC comes in on", "in step within", "jumps past",
                             "catches up by at most", "jump crossfade", "ms", "s", "%" })
    {
        INFO (row);
        CHECK (shownLabel (row) != nullptr);
    }

    //  What a show nobody has set shows: no input, no port, the engine's four defaults.
    CHECK (ltcIn->getText() == "None");
    CHECK (mtcIn->getText() == "None");
    CHECK (tolerance->getText().getDoubleValue() == doctest::Approx (10.0));
    CHECK (jumpPast->getText().getDoubleValue() == doctest::Approx (0.25));
    CHECK (catchUp->getText().getDoubleValue() == doctest::Approx (1.0));
    CHECK (crossfade->getText().getDoubleValue() == doctest::Approx (10.0));

    //  The inputs: one channel enabled, wider listed and disabled, saying why - as the LTC output menu does.
    auto codeItem = 0;
    auto keysItem = 0;

    for (int at = 0; at < ltcIn->getNumItems(); ++at)
    {
        const auto text = ltcIn->getItemText (at);
        const auto item = ltcIn->getItemId (at);
        INFO (text);

        if (text == "Code in")
        {
            codeItem = item;
            CHECK (ltcIn->isItemEnabled (item));
        }
        else if (text == "Keys (2 channels: LTC needs one)")
        {
            keysItem = item;
            CHECK_FALSE (ltcIn->isItemEnabled (item));
        }
        else if (text != "None")
        {
            //  Anything else the show has is enabled only if it is one channel.
            CHECK (text.contains ("channels: LTC needs one") != ltcIn->isItemEnabled (item));
        }
    }

    REQUIRE (codeItem > 0);
    REQUIRE (keysItem > 0);
    REQUIRE (mtcIn->getNumItems() == 2);
    CHECK (mtcIn->getItemText (1) == "Desk");

    //  Each control sends its own row.
    rig.sent.clear();
    ltcIn->setSelectedId (codeItem, juce::sendNotificationSync);
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().command == "node.set");
    CHECK (rig.sent.back().args[0].getString() == "/godot/sync/ltcIn");
    CHECK (rig.sent.back().args[1].getString() == mono.id);

    //  A disabled input is never sent, however it came to be picked.
    ltcIn->setSelectedId (keysItem, juce::sendNotificationSync);
    CHECK (rig.sent.size() == 1u);

    mtcIn->setSelectedId (2, juce::sendNotificationSync);
    REQUIRE (rig.sent.size() == 2u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/sync/mtcIn");
    CHECK (rig.sent.back().args[1].getString() == port.id);

    //  The four numbers, a comma read as a point, each to its row.
    for (auto [box, typed, address, sent] : { std::tuple { tolerance, "12,5", "/godot/sync/chaseTolerance", "12.5" },
                                              std::tuple { jumpPast, "0,5", "/godot/sync/chaseJump", "0.5" },
                                              std::tuple { catchUp, "2", "/godot/sync/chaseRate", "2" },
                                              std::tuple { crossfade, "20", "/godot/sync/chaseCrossfade", "20" } })
    {
        INFO (address);
        const auto before = rig.sent.size();
        box->setText (typed, juce::dontSendNotification);
        box->onReturnKey();
        REQUIRE (rig.sent.size() == before + 1);
        CHECK (rig.sent.back().args[0].getString() == address);
        CHECK (rig.sent.back().args[1].getString() == sent);
    }

    //  The value a row already has is not an edit.
    {
        const auto before = rig.sent.size();
        tolerance->setText ("10", juce::dontSendNotification);
        tolerance->onFocusLost();
        CHECK (rig.sent.size() == before);
    }

    //  Once the show names both sources, the tab shows them - and a line for each, as the machine hears it.
    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcIn", mono.id).ok);
    REQUIRE (rig.document.setAttribute ("/godot/sync/mtcIn", port.id).ok);
    REQUIRE (rig.document.setAttribute ("/godot/sync/chaseJump", "0.5").ok);
    rig.state.ltcIn = timecode::CodeReadout { "running", "01:02:03:04", "25", 1.000012 };
    rig.state.mtcIn = timecode::CodeReadout { "absent", {}, {}, 0.0 };
    panel.refresh (*rig.publish());

    CHECK (ltcIn->getText() == "Code in");
    CHECK (mtcIn->getText() == "Desk");
    CHECK (jumpPast->getText().getDoubleValue() == doctest::Approx (0.5));
    CHECK (shownLabel ("LTC in: running, 01:02:03:04 at 25, +12 ppm") != nullptr);
    CHECK (shownLabel ("MTC in: absent") != nullptr);

    //  Locked, nothing in the part can be changed.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());

    for (auto* control : std::initializer_list<juce::Component*> { ltcIn, mtcIn, tolerance, jumpPast, catchUp, crossfade })
        CHECK_FALSE (control->isEnabled());
}

//==============================================================================
/*  REDUNDANCY ON SCREEN (namespace draft §58, RD.11): the Redundancy tab of
    Show settings - after Network, the author's AGP - and the transport's words
    and banner, each state painted into a PNG when WFG_SNAPSHOT_DIR is set.

    THE PICTURES ARE PAINTED, NEVER CAPTURED: `createComponentSnapshot` draws a
    component into an image with no screen involved. The transport is a plain
    component that is never put on the desktop; the settings window, which
    shows itself as it is made (as every test in this file finds it), is hidden
    again at once, before anything is drawn. */
namespace
{
    /*  THE WINDOW'S OWN LOOK for the pictures, as Client.cpp sets it, and put
        back before the look itself goes. */
    struct DefaultLook
    {
        explicit DefaultLook (juce::LookAndFeel& look) { juce::Desktop::getInstance().setDefaultLookAndFeel (&look); }
        ~DefaultLook() { juce::Desktop::getInstance().setDefaultLookAndFeel (nullptr); }

        DefaultLook (const DefaultLook&) = delete;
        DefaultLook& operator= (const DefaultLook&) = delete;
    };

    void snapshotTo (juce::Component& component, const juce::String& name)
    {
        const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {});

        if (dir.isEmpty())
            return;

        const auto picture = component.createComponentSnapshot (component.getLocalBounds());
        const juce::File file { juce::File (dir).getChildFile (name) };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (picture, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }

    juce::Label* labelSaying (juce::Component& root, const juce::String& text)
    {
        if (auto* candidate = dynamic_cast<juce::Label*> (&root))
            if (candidate->getText() == text) return candidate;
        for (auto* child : root.getChildren())
            if (auto* found = labelSaying (*child, text)) return found;
        return nullptr;
    }

    /*  ONE STATE OF A PAIR, as serve fills `EngineState` from the link, the
        follower and the leadership - the alarm the engine's own sentence. */
    struct PairState
    {
        const char* name = "";
        const char* role = "none";
        const char* seat = "alone";
        bool linked = false;
        std::string alarm = {};
        std::function<void (tree::EngineState&)> more = {};
    };

    std::vector<PairState> pairStates()
    {
        using pair::Role;
        using pair::Seat;

        const pair::Situation nobody;
        const pair::NotInStep inStep;

        pair::NotInStep showDiffers;
        showDiffers.showDiffers = "the show differs: cue 4 Rain changed";
        showDiffers.showTextDiffers = true;

        pair::NotInStep apart;
        apart.differs = "cue Bed is running on the main, not here";

        /*  RD.12: A MAIN TOLD BY ITS BACKUP - not in step, or its show not this
            one's - the alarm the engine makes of it, and the rows it publishes. */
        pair::Situation backupThere;
        backupThere.role = Role::main;
        backupThere.linked = true;
        backupThere.peerSeat = Seat::following;

        pair::FollowerVerdict backupApart;
        backupApart.inStep = false;
        backupApart.differs = apart.differs;
        backupApart.showMatch = true;
        backupApart.seat = "following";

        pair::FollowerVerdict backupShow;
        backupShow.inStep = true;
        backupShow.showMatch = false;
        backupShow.showDiffers = showDiffers.showDiffers;
        backupShow.seat = "following";

        const auto timing = [] (tree::EngineState& state, bool follows)
        {
            state.pairLink.roundTrip = 0.0011;
            state.pairLink.received = 1840;
            state.pairFollow.delay = 2;
            state.pairFollow.outputLatency = follows ? 0.010 : 0.003771;
            state.pairFollow.peerOutputLatency = follows ? 0.003771 : 0.010;

            if (follows)
            {
                state.pairFollow.mapped = true;
                state.pairFollow.drift = 14.5;
                state.pairFollow.clockResidual = 341.0;
                state.pairFollow.relayed = 3;
                state.pairSending = false;
            }
        };

        return {
            { "not-paired", "none", "alone", false, {}, {} },
            { "main-sending", "main", "leading", true, {},
              [timing] (tree::EngineState& s) { timing (s, false); s.pairLink.peer = "ws:192.168.1.21:52344"; } },
            { "main-alone", "main", "alone", false, {},
              [] (tree::EngineState& s) { s.pairSettings.peers = { "192.168.1.21:5010" }; s.pairFollow.outputLatency = 0.003771; } },
            { "backup-following", "backup", "following", true, {},
              [timing] (tree::EngineState& s) { timing (s, true); s.pairLink.peer = "192.168.1.20:5010";
                                                 s.pairFollow.inStep = true; s.pairFollow.showMatch = true; } },
            { "backup-waiting", "backup", "waiting", false,
              pair::alarm ({ Role::backup, Seat::waiting, "started" }, nobody, inStep),
              [] (tree::EngineState& s) { s.pairSettings.peers = { "192.168.1.20:5010" }; s.pairSending = false;
                                          s.pairFollow.outputLatency = 0.010; } },
            { "two-mains", "backup", "bothSending", true,
              pair::alarm ({ Role::backup, Seat::bothSending, "both-sending" }, nobody, inStep),
              [timing] (tree::EngineState& s) { timing (s, false); s.pairLink.peer = "192.168.1.20:5010"; } },
            { "not-in-step", "backup", "following", true,
              pair::alarm ({ Role::backup, Seat::following, "main-found" }, nobody, apart),
              [timing, apart] (tree::EngineState& s) { timing (s, true); s.pairLink.peer = "192.168.1.20:5010";
                                                       s.pairFollow.differs = apart.differs; s.pairFollow.showMatch = true; } },
            { "show-differs", "backup", "following", true,
              pair::alarm ({ Role::backup, Seat::following, "main-found" }, nobody, showDiffers),
              [timing, showDiffers] (tree::EngineState& s) { timing (s, true); s.pairLink.peer = "192.168.1.20:5010";
                                                             s.pairFollow.showDiffers = showDiffers.showDiffers; } },
            { "backup-took-over", "backup", "leading", false,
              pair::alarm ({ Role::backup, Seat::leading, "link-lost" }, nobody, inStep),
              [] (tree::EngineState& s) { s.pairSettings.peers = { "192.168.1.20:5010" }; s.pairFollow.outputLatency = 0.010; } },
            { "main-following", "main", "following", true, {},
              [timing] (tree::EngineState& s) { timing (s, true); s.pairLink.peer = "ws:192.168.1.21:52344";
                                                 s.pairFollow.inStep = true; s.pairFollow.showMatch = true; } },
            { "main-told-apart", "main", "leading", true,
              pair::alarm ({ Role::main, Seat::leading, "follower-linked" }, backupThere, inStep, backupApart),
              [timing, backupApart] (tree::EngineState& s) { timing (s, false); s.pairLink.peer = "ws:192.168.1.21:52344";
                                                             pair::verdictRowsInto (s.pairFollow, &backupApart); } },
            { "main-told-show-differs", "main", "leading", true,
              pair::alarm ({ Role::main, Seat::leading, "follower-linked" }, backupThere, inStep, backupShow),
              [timing, backupShow] (tree::EngineState& s) { timing (s, false); s.pairLink.peer = "ws:192.168.1.21:52344";
                                                            pair::verdictRowsInto (s.pairFollow, &backupShow); } },
        };
    }

    void seatThe (tree::EngineState& state, const PairState& pair)
    {
        state.pairSettings = {};
        state.pairLink = {};
        state.pairFollow = {};
        state.pairSending = true;

        state.pairSettings.role = pair.role;
        state.pairFollow.state = pair.seat;

        //  The other computer's address, as a paired machine would have it.
        if (std::string (pair.role) == "main")
            state.pairSettings.peers = { "192.168.1.21:5010" };
        else if (std::string (pair.role) == "backup")
            state.pairSettings.peers = { "192.168.1.20:5010" };
        state.pairLink.linked = pair.linked;
        state.pairFollow.alarm = pair.alarm;

        if (pair.more)
            pair.more (state);
    }
}

TEST_CASE ("show settings UI: the Redundancy tab sits after Network, and its three settings are this machine's own commands")
{
    Rig rig;
    client::ui::Look look (rig.theme);
    const DefaultLook guard (look);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });
    panel.setVisible (false);       // painted, never shown: see above
    panel.setSize (1200, 740);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    //  The author's word for the tab (AGP), the last of the Show section's (2026-10-15).
    const auto names = tabs->getTabNames();
    REQUIRE (names.contains ("Redundancy"));
    CHECK (names.indexOf ("Redundancy") == names.indexOf ("Templates") + 1);

    tabs->setCurrentTabIndex (names.indexOf ("Redundancy"));
    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);

    //  It says first that it belongs to this computer and not the show.
    std::vector<juce::TextEditor*> boxes;
    std::vector<juce::Label*> labels;
    std::function<void (juce::Component&)> collect = [&] (juce::Component& at)
    {
        if (auto* box = dynamic_cast<juce::TextEditor*> (&at)) boxes.push_back (box);
        if (auto* label = dynamic_cast<juce::Label*> (&at)) labels.push_back (label);
        for (auto* child : at.getChildren()) collect (*child);
    };
    collect (*page);

    CHECK (std::any_of (labels.begin(), labels.end(), [] (const juce::Label* l)
                        { return l->getText().startsWith ("These settings belong to this computer, not to the show"); }));

    auto* role = component<juce::ComboBox> (*page);
    REQUIRE (role != nullptr);
    REQUIRE (boxes.size() == 2u);
    auto* peers = boxes[0];
    auto* timeout = boxes[1];

    //  Unpaired, as a machine with no pairing.xml is.
    CHECK (role->getText() == "Not paired");
    CHECK (timeout->getText() == "1");

    /*  EACH SETTING IS ITS OWN MACHINE-LOCAL COMMAND (AGC), never a node.set,
        and a value already in force is not sent again. */
    role->setSelectedId (3, juce::sendNotificationSync);
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().command == "pair.setRole");
    CHECK (rig.sent.back().origin == "window");
    CHECK (rig.sent.back().args[0].getString() == "backup");

    peers->setText ("192.168.1.20 10.0.0.20:5011", juce::dontSendNotification);
    peers->onReturnKey();
    REQUIRE (rig.sent.size() == 2u);
    CHECK (rig.sent.back().command == "pair.setPeers");
    CHECK (rig.sent.back().args[0].getString() == "192.168.1.20 10.0.0.20:5011");

    //  A comma is a decimal point, as a French booth types it; the float is the command's own type.
    timeout->setText ("1,5", juce::dontSendNotification);
    timeout->onReturnKey();
    REQUIRE (rig.sent.size() == 3u);
    CHECK (rig.sent.back().command == "pair.setTimeout");
    REQUIRE (rig.sent.back().args[0].isFloat32());
    CHECK (rig.sent.back().args[0].getFloat32() == doctest::Approx (1.5f));

    timeout->setText ("1", juce::dontSendNotification);
    timeout->onFocusLost();
    CHECK (rig.sent.size() == 3u);

    /*  THE OPERATOR'S FIVE, GREYED WITH WHY: unpaired, every one refused and a
        sentence beside each; a backup following, Take over open - and pressed,
        its one command. */
    for (const auto* words : { "Take over", "Hand over", "Make this the main", "Realign", "Take the main's show" })
    {
        INFO (words);
        auto* act = button (*page, words);
        REQUIRE (act != nullptr);
        CHECK_FALSE (act->isEnabled());
    }

    CHECK (labelSaying (*page, "This computer is not paired: choose Main or Backup above.") != nullptr);

    for (const auto& state : pairStates())
    {
        INFO (state.name);
        seatThe (rig.state, state);
        panel.refresh (*rig.publish());

        const auto reading = client::model::readPair (*rig.publish());
        CHECK (role->getText() == (std::string (state.role) == "main" ? "Main"
                                   : std::string (state.role) == "backup" ? "Backup" : "Not paired"));

        for (const auto act : { client::model::PairAct::takeOver, client::model::PairAct::handOver,
                                client::model::PairAct::keep, client::model::PairAct::realign,
                                client::model::PairAct::adoptShow })
        {
            const auto offered = reading.offer (act);
            auto* pressed = button (*page, reading.buttonWords (act));
            REQUIRE (pressed != nullptr);
            CHECK (pressed->isEnabled() == offered.open);
            CHECK (labelSaying (*page, offered.words) != nullptr);
        }

        /*  THE WHOLE TAB in the picture: at the window's own height the
            readout under the five scrolls. */
        panel.setSize (1200, 1240);
        snapshotTo (panel, "redundancy-tab-" + juce::String (state.name) + ".png");
        panel.setSize (1200, 740);
    }

    /*  RD.12: THE MAIN'S READOUT SAYS WHAT ITS BACKUP JUDGED, in the backup's
        words, where RD.11's said only to look at the backup's screen. */
    for (const auto& state : pairStates())
        if (std::string (state.name) == "main-told-show-differs")
            seatThe (rig.state, state);

    panel.refresh (*rig.publish());
    CHECK (labelSaying (*page, "Yes - the backup has the same standby and the same cues, at the same place") != nullptr);
    CHECK (labelSaying (*page, "No - the backup says the show differs: cue 4 Rain changed") != nullptr);
    CHECK (labelSaying (*page, "Checked on the backup - look at its screen") == nullptr);

    //  The readout of a backup following: the jump a changeover makes, said with what makes up for it - nothing.
    seatThe (rig.state, pairStates()[3]);
    panel.refresh (*rig.publish());
    CHECK (labelSaying (*page, "Changeover jump") != nullptr);
    CHECK (labelSaying (*page, "6.2 ms - the main's sound comes out 6.2 ms earlier than this computer's. Nothing makes up"
                               " for it: both start each sound on the same sample, and each interface takes its own"
                               " time") != nullptr);

    auto* takeOver = button (*page, "Take over");
    REQUIRE (takeOver != nullptr);
    REQUIRE (takeOver->isEnabled());

    rig.sent.clear();
    takeOver->onClick();
    REQUIRE (rig.sent.size() == 1u);
    CHECK (rig.sent.back().command == "pair.takeOver");
    CHECK (rig.sent.back().args.empty());

    //  And Getting started lists it, with what it is for.
    tabs->setCurrentTabIndex (names.indexOf ("Getting started"));
    auto* started = tabs->getCurrentContentComponent();
    REQUIRE (started != nullptr);
    auto* go = button (*started, "Redundancy");
    REQUIRE (go != nullptr);
    go->onClick();
    CHECK (tabs->getCurrentTabIndex() == names.indexOf ("Redundancy"));
}

TEST_CASE ("transport UI: a button per page with a problem, and pressing it asks for that page (§64)")
{
    /*  The author, 2026-10-15: "Add warnings in the main window for each type
        of error. Clicking on one of these icons opens the show settings to
        the relevant tab." The missing files' button, which has no page, asks
        for the next such cue instead (the author's choice); the pair's alarm
        is left to its banner. */
    Rig rig;
    client::ui::Look look (rig.theme);
    const DefaultLook guard (look);
    std::vector<std::string> opened;
    int revealed = 0;

    client::ui::TransportComponent::Actions actions;
    actions.openSettings = [&opened] (const std::string& page) { opened.push_back (page); };
    actions.revealMissing = [&revealed] { ++revealed; };

    client::ui::TransportComponent transport (rig.theme, std::move (actions));
    transport.setSize (1180, transport.preferredHeight());
    transport.show (client::model::readTransport (*rig.publish()));

    namespace page = client::model::settingsPages;
    using client::model::Icon;

    transport.showProblems ({
        { page::redundancy, Icon::warning, { "Two mains! - Show settings, Redundancy." }, {} },
        { page::audioInterface, Icon::sound, { "No audio interface is running - Show settings, Audio interface." }, {} },
        { page::midi, Icon::midi, { "MIDI port Lights: no such device - Show settings, MIDI.",
                                    "MIDI port Desk: no such device - Show settings, MIDI." }, {} },
        { "", Icon::missing, { "Missing: thunder.wav", "Missing: rain.wav" }, { "SND00001", "SND00002" } },
    });

    auto* audio = dynamic_cast<juce::Button*> (withId (transport, "problem:Audio interface"));
    auto* midi = dynamic_cast<juce::Button*> (withId (transport, "problem:MIDI"));
    auto* files = dynamic_cast<juce::Button*> (withId (transport, "problem:"));
    REQUIRE (audio != nullptr);
    REQUIRE (midi != nullptr);
    REQUIRE (files != nullptr);
    CHECK (withId (transport, "problem:Redundancy") == nullptr);

    //  How many, in the button's words; the page and every sentence in its tooltip.
    CHECK (midi->getButtonText() == "2");
    CHECK (files->getButtonText() == "2");
    CHECK (midi->getTooltip().startsWith ("MIDI - press to open Show settings on it"));
    CHECK (midi->getTooltip().contains ("MIDI port Desk"));
    CHECK (midi->getWidth() > 0);

    //  In the settings window's order, from the left, before the clock.
    CHECK (audio->getX() < midi->getX());
    CHECK (midi->getX() < files->getX());

    midi->onClick();
    audio->onClick();
    CHECK (opened == std::vector<std::string> { page::midi, page::audioInterface });

    files->onClick();
    CHECK (revealed == 1);
    CHECK (opened.size() == 2u);

    snapshotTo (transport, "transport-problems.png");

    //  Put right, the buttons go.
    transport.showProblems ({});
    CHECK (withId (transport, "problem:MIDI") == nullptr);
}

TEST_CASE ("transport UI: what this computer is beside the show's name, and the banner when the pair needs a hand")
{
    Rig rig;

    //  A show to stand the transport on: a list, two cues, the standby on the first.
    const auto list = rig.document.createList ("Main");
    REQUIRE (list.ok);
    const auto rain = rig.document.createCue (list.id, 0, "memo", "Rain");
    REQUIRE (rain.ok);
    REQUIRE (rig.document.createCue (list.id, 1, "memo", "Thunder").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rain.id + "/notes", "When the lights go blue").ok);
    REQUIRE (rig.document.setAttribute ("/godot/list/" + list.id + "/standby", rain.id).ok);
    rig.state.documentName = "Storm";
    rig.state.sampleRate = 48000;
    rig.state.blockSize = 256;
    rig.state.clock = "device";
    rig.state.audioStatus = "running";

    client::ui::Look look (rig.theme);
    const DefaultLook guard (look);

    std::vector<client::model::PairAct> pressed;
    client::ui::TransportComponent::Actions actions;
    actions.pair = [&pressed] (client::model::PairAct act) { pressed.push_back (act); };

    //  A plain component, never put on the desktop.
    client::ui::TransportComponent transport (rig.theme, std::move (actions));
    transport.setSize (1180, transport.preferredHeight());

    const auto showIt = [&transport, &rig]
    {
        transport.show (client::model::readTransport (*rig.publish()));
        transport.setSize (1180, transport.preferredHeight());
    };

    //  Not paired: no words, no banner, and not a pixel of height for either.
    seatThe (rig.state, pairStates()[0]);
    showIt();
    const auto unpairedHeight = transport.preferredHeight();
    snapshotTo (transport, "transport-not-paired.png");

    struct Wanted { const char* name = ""; const char* words = ""; const char* heading = nullptr;
                    const char* button = nullptr; std::optional<client::model::PairAct> act = {}; };

    for (const auto& wanted : { Wanted { "main-sending", "Main - sending", nullptr, nullptr, std::nullopt },
                                Wanted { "main-alone", "Main - alone", nullptr, nullptr, std::nullopt },
                                Wanted { "backup-following", "Backup - following", nullptr, nullptr, std::nullopt },
                                Wanted { "backup-waiting", "Backup - waiting", "Backup - waiting", "Take over",
                                         client::model::PairAct::takeOver },
                                Wanted { "two-mains", "Backup - sending", "Two mains!", "Make this the main",
                                         client::model::PairAct::keep },
                                Wanted { "not-in-step", "Backup - following", "Backup - following", "Realign",
                                         client::model::PairAct::realign },
                                Wanted { "show-differs", "Backup - following", "Backup - following",
                                         "Take the main's show", client::model::PairAct::adoptShow },
                                Wanted { "backup-took-over", "Backup - sending", "Backup - sending", nullptr, std::nullopt },
                                Wanted { "main-following", "Main - following", nullptr, nullptr, std::nullopt },
                                Wanted { "main-told-apart", "Main - sending", "Main - sending", nullptr, std::nullopt },
                                Wanted { "main-told-show-differs", "Main - sending", "Main - sending", nullptr,
                                         std::nullopt } })
    {
        INFO (wanted.name);

        for (const auto& state : pairStates())
            if (std::string (state.name) == wanted.name)
                seatThe (rig.state, state);

        showIt();

        //  The words on the title line, always while a role is set.
        auto* seat = labelSaying (transport, wanted.words);
        REQUIRE (seat != nullptr);
        CHECK (seat->isVisible());
        CHECK_FALSE (seat->getBounds().isEmpty());

        if (wanted.heading == nullptr)
        {
            //  Nothing to act on: no banner, and the strip as tall as an unpaired one.
            CHECK (transport.preferredHeight() == unpairedHeight);
        }
        else
        {
            CHECK (transport.preferredHeight() > unpairedHeight);

            //  The heading - a word, never a colour alone - and the engine's sentence under it.
            const auto headings = [&transport, &wanted]
            {
                int found = 0;
                std::function<void (juce::Component&)> walk = [&] (juce::Component& at)
                {
                    if (auto* label = dynamic_cast<juce::Label*> (&at))
                        if (label->getText() == wanted.heading && label->isVisible()) ++found;
                    for (auto* child : at.getChildren()) walk (*child);
                };
                walk (transport);
                return found;
            }();

            CHECK (headings >= 1);

            if (wanted.button != nullptr)
            {
                auto* answer = button (transport, wanted.button);
                REQUIRE (answer != nullptr);
                CHECK (answer->isVisible());
                CHECK (answer->isEnabled());

                pressed.clear();
                answer->onClick();
                REQUIRE (pressed.size() == 1u);
                CHECK (pressed.back() == *wanted.act);
            }
        }

        snapshotTo (transport, "transport-" + juce::String (wanted.name) + ".png");
    }

    /*  A BUTTON THE ENGINE WOULD REFUSE IS GREYED, AND THE BANNER SAYS WHY:
        the main's show offered while a cue runs here. */
    for (const auto& state : pairStates())
        if (std::string (state.name) == "show-differs")
            seatThe (rig.state, state);

    rig.runs.create ("RUNLIVE1", rain.id, "memo");
    auto* run = rig.runs.find ("RUNLIVE1");
    REQUIRE (run != nullptr);
    run->state = cue::runState::playing;
    run->launchRequestedAtTick = 120;

    showIt();

    auto* adopt = button (transport, "Take the main's show");
    REQUIRE (adopt != nullptr);
    CHECK_FALSE (adopt->isEnabled());
    CHECK (adopt->getTooltip() == "Not while a cue runs here: stop it first.");

    std::vector<juce::String> sentences;
    std::function<void (juce::Component&)> gather = [&] (juce::Component& at)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&at))
            if (label->isVisible()) sentences.push_back (label->getText());
        for (auto* child : at.getChildren()) gather (*child);
    };
    gather (transport);

    CHECK (std::any_of (sentences.begin(), sentences.end(), [] (const juce::String& said)
                        { return said.endsWith ("Not while a cue runs here: stop it first."); }));

    snapshotTo (transport, "transport-show-differs-busy.png");
}

//==============================================================================
/*  MQ.6: GO.DOT'S OWN MQTT BROKER ON THE NETWORK TAB (namespace draft §67,
    AND, ANJ): the switch says which setting is in force in words and sends
    the one row; the port box shows while it is on. */
TEST_CASE ("show settings UI: the Network tab's MQTT broker switch sends the row, and says in words what the broker is doing")
{
    Rig rig;

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("Network"));

    auto* off = button (panel, "MQTT broker: Off");
    REQUIRE (off != nullptr);

    const auto before = rig.sent.size();
    off->onClick();
    REQUIRE (rig.sent.size() == before + 1);
    CHECK (rig.sent.back().command == "node.set");
    REQUIRE (rig.sent.back().args.size() == 2u);
    CHECK (rig.sent.back().args[0].getString() == "/godot/network/broker");
    CHECK (rig.sent.back().args[1].getString() == "true");

    //  On: the word changes, and the switch sends it off again.
    REQUIRE (rig.document.setAttribute ("/godot/network/broker", "true").ok);
    panel.refresh (*rig.publish());
    auto* on = button (panel, "MQTT broker: On");
    REQUIRE (on != nullptr);
    on->onClick();
    CHECK (rig.sent.back().args[0].getString() == "/godot/network/broker");
    CHECK (rig.sent.back().args[1].getString() == "false");
}

//==============================================================================
/*  THE BINDINGS TAB (namespace draft §74, BD.11): Learn on a click of Moved by
    and off on a second, How going round, the switches, the cross, ADD. Its
    cells from the right: the cross 24, Feedback 76, Pick up 64, On 42, Live
    200, How 84; from the left Binding 160, then Moved by 260. */
namespace
{
    int bindingMovedByAt (const juce::ListBox&)        { return 8 + 160 + 260 / 2; }
    int bindingHowAt (const juce::ListBox& list)       { return list.getWidth() - 8 - 24 - 76 - 64 - 42 - 200 - 84 / 2; }
    int bindingOnAt (const juce::ListBox& list)        { return list.getWidth() - 8 - 24 - 76 - 64 - 42 / 2; }
    int bindingFeedbackAt (const juce::ListBox& list)  { return list.getWidth() - 8 - 24 - 76 / 2; }
    int bindingCrossAt (const juce::ListBox& list)     { return list.getWidth() - 8 - 24 / 2; }
}

TEST_CASE ("show settings: the Bindings tab learns a binding, and edits it in place")
{
    Rig rig;
    const auto list = rig.document.createList ("Main").id;
    const auto cue = rig.document.createCue (list, 0, "media", "Door").id;
    const auto port = rig.document.createPort ("Desk").id;

    REQUIRE (rig.document.createBinding ("", "BND00001", { { "port", port }, { "number", "7" },
                                                           { "target", "/godot/cue/" + cue + "/level" } }).ok);
    REQUIRE (rig.document.createBinding (cue, "BND00002", { { "source", "osc" }, { "address", "/desk/fader/1" },
                                                            { "target", "level" } }).ok);

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });
    panel.setSize (1400, 740);
    panel.showPage (client::model::settingsPages::bindings);

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    CHECK (tabs->getCurrentTabName() == "Bindings");
    auto* page = tabs->getCurrentContentComponent();
    REQUIRE (page != nullptr);
    auto* rows = listOn (*page);
    REQUIRE (rows != nullptr);
    CHECK (rows->getListBoxModel()->getNumRows() == 2);

    const auto last = [&rig] () -> const Event& { return rig.sent.back(); };

    //  Moved by: Learn on it; once the engine listens, a second click stops it.
    rig.sent.clear();
    clickAt (*rows, bindingMovedByAt (*rows));
    REQUIRE (rig.sent.size() == 1u);
    CHECK (last().command == "binding.learn");
    CHECK (last().args[0].getString() == "BND00001");

    rig.state.bindingLearn = "BND00001";
    panel.refresh (*rig.publish());
    clickAt (*rows, bindingMovedByAt (*rows));
    CHECK (last().command == "binding.learn");
    CHECK (last().args[0].getString().empty());

    //  How goes round; the switches flip; the cross deletes.
    clickAt (*rows, bindingHowAt (*rows));
    CHECK (last().command == "node.set");
    CHECK (last().args[0].getString() == "/godot/binding/BND00001/control");
    CHECK (last().args[1].getString() == "relative");

    clickAt (*rows, bindingOnAt (*rows));
    CHECK (last().args[0].getString() == "/godot/binding/BND00001/enabled");
    CHECK (last().args[1].getString() == "false");

    clickAt (*rows, bindingFeedbackAt (*rows));
    CHECK (last().args[0].getString() == "/godot/binding/BND00001/feedback");
    CHECK (last().args[1].getString() == "true");

    clickAt (*rows, bindingCrossAt (*rows));
    CHECK (last().command == "object.delete");
    CHECK (last().args[0].getString() == "BND00001");

    //  ADD makes one of the show's.
    auto* add = button (*page, "ADD");
    REQUIRE (add != nullptr);
    add->onClick();
    CHECK (last().command == "binding.create");
    CHECK (last().args[0].getString().empty());

    //  Locked: no ADD, and a click changes nothing.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    panel.refresh (*rig.publish());
    CHECK_FALSE (add->isVisible());
    rig.sent.clear();
    clickAt (*rows, bindingOnAt (*rows));
    CHECK (rig.sent.empty());
}
