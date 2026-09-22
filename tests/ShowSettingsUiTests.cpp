/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/client/ui/ShowSettingsWindow.h>
#include <wfg/client/ui/InspectorComponent.h>
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/Theme.h>
#include <wfg/engine/audio/DeviceLayer.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/cue/Run.h>
#include <spatcore/ui/patch/PatchMatrixComponent.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <functional>
#include <chrono>
#include <thread>
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
    tabs->setCurrentTabIndex (3);   // the output patch
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

TEST_CASE ("show settings UI: the Network tab declares devices and switches the sender filter")
{
    Rig rig;

    client::ui::ShowSettingsWindow panel (rig.theme, *rig.publish(),
        [&rig] (Event event) { rig.sent.push_back (std::move (event)); });

    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);

    /*  THE WINDOW IS NOT ONLY ABOUT AUDIO ANY MORE (2026-09-22), and the tab
        strip is where that shows: the first one is named after the hardware it
        configures rather than after "Interface", which the network tab could
        equally have claimed. */
    CHECK (tabs->getTabNames()[0] == "Audio");
    CHECK (tabs->getTabNames()[4] == "Network");

    tabs->setCurrentTabIndex (4);

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
    CHECK (tabs->getTabNames()[5] == "MIDI");

    tabs->setCurrentTabIndex (5);

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
        tabs->setCurrentTabIndex (3);   // the output patch, after Interface and Outputs
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
