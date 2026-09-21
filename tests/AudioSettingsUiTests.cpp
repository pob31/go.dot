/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/client/ui/AudioSettingsWindow.h>
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
            client::ui::AudioSettingsWindow panel (theme, *publish(),
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
            CHECK (sent.back().args[6].getString() == "0 1");
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
    client::ui::AudioSettingsWindow panel (rig.theme, *rig.publish(),
        [&] (Event event) { rig.sent.push_back (std::move (event)); });
    auto* tabs = component<juce::TabbedComponent> (panel);
    REQUIRE (tabs != nullptr);
    for (bool close : { false, true })
    {
        tabs->setCurrentTabIndex (2);
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
