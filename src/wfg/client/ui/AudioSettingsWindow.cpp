/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/client/ui/AudioSettingsWindow.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>
#include <spatcore/ui/patch/PatchMatrixComponent.h>
#include <spatcore/io/TestSignalGenerator.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <map>

namespace wfg::client::ui
{
    namespace
    {
        using Matrix = spatcore::ui::patch::PatchMatrixComponent;
        // The matrix owns only a UI draft generator. Audio receives commands,
        // never this object (its phase setters are not thread safe).
        class TestMatrix final : public Matrix
        {
        public:
            TestMatrix (spatcore::ui::patch::PatchMatrixConfig config, bool input,
                        spatcore::io::TestSignalGenerator* generator)
                : Matrix (std::move (config), input, generator), draftGenerator (generator) {}
            std::function<void()> changed;
            void mouseDown (const juce::MouseEvent& event) override
            { Matrix::mouseDown (event); if (changed) changed(); }
            void mouseUp (const juce::MouseEvent& event) override
            { Matrix::mouseUp (event); if (changed) changed(); }
            bool keyPressed (const juce::KeyPress& key) override
            { const auto handled = Matrix::keyPressed (key); if (changed) changed(); return handled; }
            bool keyStateChanged (bool down) override
            { const auto handled = Matrix::keyStateChanged (down); if (changed) changed(); return handled; }
            void focusLost (FocusChangeType cause) override
            {
                Matrix::focusLost (cause);
                if (draftGenerator && ! draftGenerator->isHoldEnabled())
                { clearActiveTestChannel(); if (changed) changed(); }
            }
        private:
            spatcore::io::TestSignalGenerator* draftGenerator;
        };
        // These trees are LOCAL EDITOR DRAFTS, never a document or an engine
        // tree. spatcore edits them; Apply sends their values as one command.
        class PatchPage final : public juce::Component
        {
        public:
            PatchPage (const model::Theme& colours, bool input, std::vector<int> initial, int minimumRows,
                       std::function<void (Event)> dispatch = {})
                : theme (colours), isInput (input), minimum (minimumRows), mapping (std::move (initial)), send (std::move (dispatch))
            {
                addAndMakeVisible (countLabel); countLabel.setText ("Logical channels", juce::dontSendNotification);
                addAndMakeVisible (count); count.setInputRestrictions (3, "0123456789");
                count.setText (juce::String (static_cast<int> (mapping.size())));
                count.onReturnKey = [this] { changeRows(); };
                count.onFocusLost = [this] { changeRows(); };
                for (auto* button : { &scroll, &patch, &identity, &clear }) addAndMakeVisible (*button);
                scroll.onClick = [this] { stopTest(); matrix->setMode (Matrix::Mode::Scrolling); resized(); };
                patch.onClick = [this] { stopTest(); matrix->setMode (Matrix::Mode::Patching); resized(); };
                scroll.setClickingTogglesState (true); patch.setClickingTogglesState (true);
                scroll.setRadioGroupId (1); patch.setRadioGroupId (1);
                patch.setToggleState (true, juce::dontSendNotification);
                if (! isInput)
                {
                    addAndMakeVisible (test);
                    test.setClickingTogglesState (true); test.setRadioGroupId (1);
                    test.onClick = [this] { matrix->setMode (Matrix::Mode::Testing); resized(); };
                    for (auto* component : std::initializer_list<juce::Component*> { &signal, &hold, &level, &frequency, &hint })
                        addChildComponent (*component);
                    signal.addItemList ({ "Off", "Pink noise", "Tone", "Sweep", "Pulse" }, 1);
                    signal.setSelectedId (1, juce::dontSendNotification);
                    signal.setName ("Test signal");
                    signal.onChange = [this]
                    {
                        testDraft.setSignalType (static_cast<spatcore::io::TestSignalGenerator::SignalType> (signal.getSelectedId() - 1));
                        if (signal.getSelectedId() == 1) matrix->clearActiveTestChannel();
                        frequency.setEnabled (signal.getSelectedId() == 3); publishTest();
                    };
                    hold.onClick = [this]
                    {
                        testDraft.setHoldEnabled (hold.getToggleState());
                        if (! hold.getToggleState()) matrix->clearActiveTestChannel();
                        publishTest();
                    };
                    level.setName ("Test level"); level.setRange (-92, 0, 0.1); level.setValue (-40);
                    level.setTextValueSuffix (" dB"); level.setSliderStyle (juce::Slider::LinearHorizontal);
                    level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 85, 24);
                    level.onValueChange = [this] { testDraft.setLevel (static_cast<float> (level.getValue())); publishTest(); };
                    frequency.setName ("Tone frequency"); frequency.setRange (20, 20000, 1);
                    frequency.setSkewFactorFromMidPoint (1000); frequency.setValue (1000);
                    frequency.setTextValueSuffix (" Hz"); frequency.setSliderStyle (juce::Slider::LinearHorizontal);
                    frequency.setTextBoxStyle (juce::Slider::TextBoxRight, false, 95, 24);
                    frequency.setEnabled (false);
                    frequency.onValueChange = [this] { testDraft.setFrequency (static_cast<float> (frequency.getValue())); publishTest(); };
                    hint.setText ("Choose a signal, then press an output. Hold latches it. Tests replace audio on that output.", juce::dontSendNotification);
                }
                identity.onClick = [this]
                {
                    mapping = audio::identityPatch (static_cast<int> (mapping.size()));
                    for (auto& channel : mapping) if (channel >= hardware) channel = -1;
                    rebuild();
                };
                clear.onClick = [this] { matrix->clearAllPatches(); };
                rebuild();
            }

            ~PatchPage() override { stopTest(); }
            void visibilityChanged() override { if (! isShowing()) stopTest(); }
            void stopTest (bool notifyEngine = true)
            {
                if (isInput) return;
                const bool changed = lastTest.type != 0 || lastTest.channel != -1 || lastTest.hold;
                testDraft.setSignalType (spatcore::io::TestSignalGenerator::SignalType::Off);
                testDraft.setHoldEnabled (false);
                if (matrix) matrix->clearActiveTestChannel();
                signal.setSelectedId (1, juce::dontSendNotification);
                hold.setToggleState (false, juce::dontSendNotification);
                frequency.setEnabled (false);
                lastTest.type = 0; lastTest.channel = -1; lastTest.hold = false;
                sawConfiguredTest = false;
                if (changed && notifyEngine && send) send ({ "window", "audio.testStop", {} });
            }
            void observeTest (int type)
            {
                // Releasing a momentary output leaves its signal configured.
                // A delayed active snapshot followed by channel=-1 is not a
                // reset. Only an explicit Off (e.g. panic) clears the controls.
                if (type != 0) sawConfiguredTest = true;
                else if (sawConfiguredTest) stopTest (false);
            }

            std::string value()
            {
                changeRows();
                readMatrix();
                return audio::writePatch (mapping);
            }
            void setHardware (int channels)
            {
                if (hardware == channels) return;
                readMatrix(); hardware = channels; rebuild();
            }
            void resized() override
            {
                auto area = getLocalBounds().reduced (10);
                auto bar = area.removeFromTop (30);
                countLabel.setBounds (bar.removeFromLeft (145)); count.setBounds (bar.removeFromLeft (65));
                bar.removeFromLeft (20);
                for (auto* button : { &scroll, &patch, &identity, &clear })
                { button->setBounds (bar.removeFromLeft (80).reduced (3, 0)); }
                if (! isInput) test.setBounds (bar.removeFromLeft (80).reduced (3, 0));
                const bool testing = ! isInput && test.getToggleState();
                for (auto* component : std::initializer_list<juce::Component*> { &signal, &hold, &level, &frequency, &hint })
                    component->setVisible (testing);
                if (testing)
                {
                    area.removeFromTop (8);
                    auto controls = area.removeFromTop (30);
                    signal.setBounds (controls.removeFromLeft (130)); hold.setBounds (controls.removeFromLeft (70));
                    level.setBounds (controls.removeFromLeft (controls.getWidth() / 2)); frequency.setBounds (controls);
                    hint.setBounds (area.removeFromTop (30));
                }
                area.removeFromTop (10); matrix->setBounds (area);
            }
        private:
            void publishTest()
            {
                if (isInput || ! send) return;
                const audio::OutputTestSettings next { static_cast<int> (testDraft.getSignalType()),
                    testDraft.getOutputChannel(), static_cast<int> (frequency.getValue()), level.getValue(), hold.getToggleState() };
                if (next == lastTest) return;
                lastTest = next;
                if (next.type == 0) sawConfiguredTest = false;
                using V = osc::Value;
                send ({ "window", "audio.testSignal", { V::int32 (next.type), V::int32 (next.channel),
                    V::int32 (next.frequency), V::float64 (next.level), V::boolean (next.hold) } });
            }
            void readMatrix()
            {
                if (! matrix) return;
                for (std::size_t row = 0; row < mapping.size(); ++row)
                    mapping[row] = matrix->getHardwareChannelForWFS (static_cast<int> (row));
            }
            void changeRows()
            {
                const auto rows = juce::jlimit (minimum, audio::maximumPatchChannels, count.getText().getIntValue());
                count.setText (juce::String (rows), false);
                if (rows == static_cast<int> (mapping.size())) return;
                readMatrix(); mapping.resize (static_cast<std::size_t> (rows), -1); rebuild();
            }
            void rebuild()
            {
                stopTest();
                matrix.reset();
                int columns = std::max (hardware, 2);
                for (auto channel : mapping) columns = std::max (columns, channel + 1);
                juce::StringArray rows;
                for (auto channel : mapping)
                {
                    juce::StringArray cells;
                    for (int col = 0; col < columns; ++col) cells.add (col == channel ? "1" : "0");
                    rows.add (cells.joinIntoString (","));
                }
                draft.setProperty ("rows", static_cast<int> (mapping.size()), nullptr);
                draft.setProperty ("cols", columns, nullptr);
                draft.setProperty ("activeHardwareChannels", hardware, nullptr);
                draft.setProperty ("patchData", rows.joinIntoString (";"), nullptr);
                spatcore::ui::patch::PatchMatrixConfig config;
                config.patchTree = draft;
                config.channelsTree = channelsDraft;
                config.hostManagesRows = true;
                config.maxHardwareChannels = audio::maximumPatchChannels;
                config.numChannelsProvider = [this] { return static_cast<int> (mapping.size()); };
                config.recomputeColumns = [] {};
                config.channelNameProvider = [this] (int row)
                { return juce::String (isInput ? "Input " : "Output ") + juce::String (row + 1); };
                config.rowColourProvider = [this] (int) { return Look::colour (theme, "standby"); };
                config.paletteProvider = [this]
                {
                    return spatcore::ui::patch::PatchMatrixPalette {
                        Look::colour (theme, "ground"), Look::colour (theme, "panel"), Look::colour (theme, "panel-in"),
                        Look::colour (theme, "rule"), Look::colour (theme, "ink"),
                        Look::colour (theme, "ink-dim"), Look::colour (theme, "ink-off") };
                };
                config.translate = [] (const char* key)
                {
                    const juce::String name (key);
                    if (name.endsWith ("interfaceInput")) return juce::String ("Hardware inputs");
                    if (name.endsWith ("interfaceOutput")) return juce::String ("Hardware outputs");
                    if (name.endsWith ("processorInputs")) return juce::String ("Show inputs");
                    if (name.endsWith ("processorOutputs")) return juce::String ("Show outputs");
                    return name.fromLastOccurrenceOf (".", false, false);
                };
                matrix = std::make_unique<TestMatrix> (std::move (config), isInput, isInput ? nullptr : &testDraft);
                matrix->changed = [this] { publishTest(); };
                matrix->setMode (test.getToggleState() ? Matrix::Mode::Testing : patch.getToggleState() ? Matrix::Mode::Patching : Matrix::Mode::Scrolling);
                test.setEnabled (hardware > 0);
                addAndMakeVisible (*matrix); resized();
            }
            const model::Theme& theme;
            bool isInput;
            int minimum, hardware = 0;
            std::vector<int> mapping;
            juce::ValueTree draft { "PatchDraft" }, channelsDraft { "ChannelsDraft" };
            std::function<void (Event)> send;
            spatcore::io::TestSignalGenerator testDraft;
            audio::OutputTestSettings lastTest;
            bool sawConfiguredTest = false;
            std::unique_ptr<TestMatrix> matrix;
            juce::TextButton test { "Test" };
            juce::ComboBox signal;
            juce::ToggleButton hold { "Hold" };
            juce::Slider level, frequency;
            juce::Label hint;
            juce::Label countLabel;
            juce::TextEditor count;
            juce::TextButton scroll { "Scroll" }, patch { "Patch" }, identity { "1:1" }, clear { "Unpatch all" };
        };
    }

    class AudioSettingsWindow::Panel final : public juce::Component
    {
    public:
        Panel (const model::Theme& theme, const tree::TreeSnapshot& snapshot,
               std::function<void (Event)> dispatch, std::function<void()> close)
            : send (std::move (dispatch)), tabs (juce::TabbedButtonBar::TabsAtTop)
        {
            initial = audio::readAudioSettings ([&] (const std::string& name)
            { return model::text (snapshot, "/godot/audio/" + name); });
            readCapabilities (snapshot);
            std::vector<int> in, out;
            audio::readPatch (initial.inputPatch, in); audio::readPatch (initial.outputPatch, out);
            auto minimumOutputs = 2;
            for (const auto* node : snapshot.all())
                if (node->address.starts_with ("/godot/bus/") && node->address.ends_with ("/width"))
                {
                    const auto base = node->address.substr (0, node->address.size() - 6);
                    const auto first = juce::String (model::text (snapshot, base + "/firstChannel")).getIntValue();
                    minimumOutputs = std::max (minimumOutputs, first + juce::String (model::text (node)).getIntValue());
                }
            if (in.empty()) in = initial.inputDevice.empty() ? std::vector<int> (2, -1)
                                                             : audio::identityPatch (2);
            if (out.empty()) out = audio::identityPatch (minimumOutputs);
            out.resize (std::max (out.size(), static_cast<std::size_t> (minimumOutputs)), -1);
            inputs = std::make_unique<PatchPage> (theme, true, in, 0);
            outputs = std::make_unique<PatchPage> (theme, false, out, minimumOutputs, send);
            addAndMakeVisible (tabs);
            const auto background = Look::colour (theme, "panel");
            tabs.addTab ("Interface", background, &interfacePage, false);
            tabs.addTab ("Input patch", background, inputs.get(), false);
            tabs.addTab ("Output patch", background, outputs.get(), false);
            for (auto* component : std::initializer_list<juce::Component*> { &enabled, &type, &output, &input,
                     &buffer, &typeLabel, &outputLabel, &inputLabel, &bufferLabel, &rate, &explanation, &rescan })
                interfacePage.addAndMakeVisible (*component);
            enabled.setToggleState (initial.enabled, juce::dontSendNotification);
            typeLabel.setText ("Audio system", juce::dontSendNotification);
            outputLabel.setText ("Output interface", juce::dontSendNotification);
            inputLabel.setText ("Input interface", juce::dontSendNotification);
            bufferLabel.setText ("Buffer size", juce::dontSendNotification);
            buffer.setTooltip ("Use the device default, a supported size from the active interface, or enter a requested size in samples.");
            explanation.setText ("Sample rate follows the interface clock.\nInput patching supplies the engine's inputs; live-input monitoring and rack processing are not available yet.", juce::dontSendNotification);
            explanation.setJustificationType (juce::Justification::topLeft);
            type.onChange = [this] { fillDevices ({}, {}); };
            output.onChange = [this] { capabilities(); };
            input.onChange = [this] { capabilities(); };
            rescan.onClick = [this] { fillTypes (type.getText(), deviceName (output), deviceName (input)); };
            fillTypes (juce::String (initial.deviceType), juce::String (initial.outputDevice), juce::String (initial.inputDevice));
            buffer.setText (initial.bufferSize == 0 ? "Device default" : juce::String (initial.bufferSize), juce::dontSendNotification);
            for (auto* component : std::initializer_list<juce::Component*> { &save, &defaults, &status, &apply, &cancel })
                addAndMakeVisible (*component);
            save.setToggleState (true, juce::dontSendNotification);
            save.setTooltip ("Save the show, including any other unsaved edits, after audio settings apply successfully.");
            apply.onClick = [this] { applySettings(); };
            cancel.onClick = std::move (close);
            setSize (880, 610);
            refresh (snapshot);
        }

        void refresh (const tree::TreeSnapshot& snapshot)
        {
            outputs->observeTest (juce::String (model::text (snapshot, "/godot/audio/testType")).getIntValue());
            if (readCapabilities (snapshot)) capabilities();
            const auto state = model::text (snapshot, "/godot/audio/settingsStatus");
            const auto error = model::text (snapshot, "/godot/audio/settingsError");
            const auto errors = model::text (snapshot, "/godot/engine/errorCount");
            if (waiting && errors != errorCountAtApply
                && model::text (snapshot, "/godot/engine/lastError").find ("audio.setup") != std::string::npos)
            {
                waiting = false; saveAfterApply = false; defaultsAfterApply = false;
                status.setText (juce::String (model::text (snapshot, "/godot/engine/lastError")), juce::dontSendNotification);
            }
            if (waiting && state == "applying") sawApplying = true;
            if (waiting && (sawApplying || model::text (snapshot, "/godot/audio/settingsRevision") != sequenceAtApply)
                && state != "applying")
            {
                // Also require the submitted settings to be visible: a UI pass
                // before the command landed must not announce a successful save.
                const auto current = audio::readAudioSettings ([&] (const std::string& name)
                { return model::text (snapshot, "/godot/audio/" + name); });
                if (current == submitted)
                {
                    waiting = false;
                    if (! error.empty()) { saveAfterApply = false; defaultsAfterApply = false; }
                    if (error.empty())
                    {
                        if (saveAfterApply) send ({ "window", "document.save", {} });
                        if (defaultsAfterApply) send ({ "window", "audio.defaults", {} });
                        status.setText (saveAfterApply ? "Audio settings applied. Saving show..." : "Audio settings applied. Save the show to keep them.", juce::dontSendNotification);
                    }
                }
            }
            errorCount = errors;
            sequence = model::text (snapshot, "/godot/audio/settingsRevision");
            if (! error.empty()) status.setText (juce::String (error), juce::dontSendNotification);
            else if (state == "applying") status.setText ("Restarting audio...", juce::dontSendNotification);
            const auto writeError = model::text (snapshot, "/godot/document/writeError");
            if (! writeError.empty()) status.setText (juce::String (writeError), juce::dontSendNotification);
            else if (error.empty() && ! waiting && saveAfterApply && model::text (snapshot, "/godot/document/dirty") == "false")
            { status.setText ("Audio settings applied and show saved.", juce::dontSendNotification); saveAfterApply = false; }
            rate.setText ("Current device: " + juce::String (model::text (snapshot, "/godot/audio/device"))
                + "   " + juce::String (model::text (snapshot, "/godot/audio/actualSampleRate")) + " Hz / "
                + juce::String (model::text (snapshot, "/godot/audio/actualBufferSize")) + " samples", juce::dontSendNotification);
            const auto locked = model::isYes (model::flag (snapshot, "/godot/document/locked"));
            apply.setEnabled (! waiting && state != "applying" && ! locked);
            tabs.setEnabled (! waiting && state != "applying" && ! locked);
            if (locked) status.setText ("Unlock the show to change audio settings.", juce::dontSendNotification);
        }

        void stopTest() { outputs->stopTest(); }

        void resized() override
        {
            auto area = getLocalBounds().reduced (14);
            auto buttons = area.removeFromBottom (34);
            cancel.setBounds (buttons.removeFromRight (110)); buttons.removeFromRight (8);
            apply.setBounds (buttons.removeFromRight (150));
            status.setBounds (area.removeFromBottom (50));
            auto options = area.removeFromBottom (32);
            save.setBounds (options.removeFromLeft (340)); defaults.setBounds (options);
            tabs.setBounds (area);
            auto form = interfacePage.getLocalBounds().reduced (22);
            enabled.setBounds (form.removeFromTop (34)); form.removeFromTop (12);
            auto row = [&form] (juce::Label& label, juce::ComboBox& box)
            { auto line = form.removeFromTop (42); label.setBounds (line.removeFromLeft (170)); box.setBounds (line.reduced (0, 5)); };
            row (typeLabel, type); row (outputLabel, output); row (inputLabel, input); row (bufferLabel, buffer);
            rescan.setBounds (form.removeFromTop (34).removeFromRight (150));
            rate.setBounds (form.removeFromTop (42)); explanation.setBounds (form);
        }
    private:
        bool readCapabilities (const tree::TreeSnapshot& snapshot)
        {
            // Only the engine owns an open device. Constructing a second ASIO
            // device even just to inspect it may stop the live driver's clock.
            // Read granted capabilities through the existing snapshot door.
            const auto active = model::text (snapshot, "/godot/audio/status") == "running"
                             && model::text (snapshot, "/godot/audio/settingsStatus") == "ready";
            const auto settings = audio::readAudioSettings ([&] (const std::string& name)
            { return model::text (snapshot, "/godot/audio/" + name); });
            const auto device = model::text (snapshot, "/godot/audio/device");
            const auto ins = juce::String (model::text (snapshot, "/godot/audio/hardwareInputs")).getIntValue();
            const auto outs = juce::String (model::text (snapshot, "/godot/audio/hardwareOutputs")).getIntValue();
            const auto size = juce::String (model::text (snapshot, "/godot/audio/actualBufferSize")).getIntValue();
            const auto sizes = model::text (snapshot, "/godot/audio/availableBufferSizes");
            const auto changed = active != hasCapabilities || settings != activeSettings
                              || device != activeDevice || ins != activeInputs
                              || outs != activeOutputs || size != activeBuffer || sizes != activeBufferSizes;
            hasCapabilities = active; activeSettings = settings; activeDevice = device;
            activeInputs = ins; activeOutputs = outs; activeBuffer = size;
            activeBufferSizes = sizes;
            return changed;
        }
        static juce::String deviceName (const juce::ComboBox& box) { return box.getSelectedId() == 1 ? juce::String {} : box.getText(); }
        juce::AudioIODeviceType* selectedType()
        {
            for (auto* deviceType : devices.getAvailableDeviceTypes())
                if (deviceType->getTypeName() == type.getText()) return deviceType;
            return nullptr;
        }
        void fillTypes (juce::String selected, const juce::String& out, const juce::String& in)
        {
            type.clear (juce::dontSendNotification);
            int id = 1;
            for (auto* deviceType : devices.getAvailableDeviceTypes())
                type.addItem (deviceType->getTypeName(), id++);
            if (selected.isEmpty() && type.getNumItems() > 0) selected = type.getItemText (0);
            type.setText (selected, juce::dontSendNotification);
            fillDevices (out, in);
        }
        void fillDevices (const juce::String& out, const juce::String& in)
        {
            output.clear (juce::dontSendNotification); input.clear (juce::dontSendNotification);
            output.addItem ("System default", 1); input.addItem ("None", 1);
            if (auto* deviceType = selectedType())
            {
                deviceType->scanForDevices();
                output.addItemList (deviceType->getDeviceNames (false), 2);
                input.addItemList (deviceType->getDeviceNames (true), 2);
            }
            if (out.isEmpty()) output.setSelectedId (1, juce::dontSendNotification);
            else output.setText (out, juce::dontSendNotification);
            if (in.isEmpty()) input.setSelectedId (1, juce::dontSendNotification);
            else input.setText (in, juce::dontSendNotification);
            capabilities();
        }
        void capabilities()
        {
            auto previous = buffer.getText();
            buffer.clear (juce::dontSendNotification); buffer.addItem ("Device default", 1);
            int numInputs = 0, numOutputs = 0;
            if (auto* deviceType = selectedType())
            {
                auto out = deviceName (output);
                const auto names = deviceType->getDeviceNames (false);
                if (out.isEmpty() && ! names.isEmpty()) out = names[juce::jlimit (0, names.size() - 1, deviceType->getDefaultDeviceIndex (false))];
                auto in = deviceName (input);
                if (! deviceType->hasSeparateInputsAndOutputs())
                {
                    in = deviceType->getDeviceNames (true).contains (out) ? out : juce::String {};
                    if (in.isEmpty()) input.setSelectedId (1, juce::dontSendNotification);
                    else input.setText (in, juce::dontSendNotification);
                }
                input.setEnabled (deviceType->hasSeparateInputsAndOutputs());
                if (hasCapabilities && type.getText().toStdString() == activeSettings.deviceType
                    && out.toStdString() == activeDevice)
                {
                    numInputs = activeInputs;
                    numOutputs = activeOutputs;
                    auto sizes = juce::StringArray::fromTokens (juce::String (activeBufferSizes), " ", "");
                    if (activeBuffer > 0) sizes.addIfNotAlreadyThere (juce::String (activeBuffer));
                    int id = 2;
                    for (const auto& size : sizes)
                        if (size.getIntValue() > 0) buffer.addItem (size, id++);
                }
            }
            explanation.setText ("Sample rate follows the interface clock. Channel counts update after applying the interface.\nInput patching supplies the engine's inputs; live-input monitoring and rack processing are not available yet.", juce::dontSendNotification);
            inputs->setHardware (numInputs); outputs->setHardware (numOutputs);
            buffer.setEditableText (true);
            buffer.setText (previous.isEmpty() ? "Device default" : previous, juce::dontSendNotification);
        }
        void applySettings()
        {
            outputs->stopTest();
            submitted.enabled = enabled.getToggleState();
            submitted.deviceType = type.getText().toStdString();
            submitted.outputDevice = deviceName (output).toStdString();
            submitted.inputDevice = deviceName (input).toStdString();
            const auto bufferText = buffer.getText().trim();
            if (bufferText != "Device default" && (! bufferText.containsOnly ("0123456789") || bufferText.isEmpty()))
            { status.setText ("Enter a buffer size in samples, or choose Device default.", juce::dontSendNotification); return; }
            submitted.bufferSize = bufferText == "Device default" ? 0 : bufferText.getIntValue();
            submitted.inputPatch = inputs->value(); submitted.outputPatch = outputs->value();
            if (! audio::validAudioSettings (submitted))
            { status.setText ("Invalid buffer size or patch.", juce::dontSendNotification); return; }
            using V = osc::Value;
            errorCountAtApply = errorCount; sequenceAtApply = sequence; sawApplying = false; waiting = true;
            saveAfterApply = save.getToggleState(); defaultsAfterApply = defaults.getToggleState();
            send ({ "window", "audio.setup", { V::boolean (submitted.enabled), V::string (submitted.deviceType),
                V::string (submitted.outputDevice), V::string (submitted.inputDevice), V::int32 (submitted.bufferSize),
                V::string (submitted.inputPatch), V::string (submitted.outputPatch) } });
            apply.setEnabled (false); status.setText ("Applying audio settings...", juce::dontSendNotification);
        }
        std::function<void (Event)> send;
        audio::AudioSettings initial, submitted;
        audio::AudioSettings activeSettings;
        std::string activeDevice, activeBufferSizes;
        bool hasCapabilities = false;
        int activeInputs = 0, activeOutputs = 0, activeBuffer = 0;
        juce::AudioDeviceManager devices;
        juce::Component interfacePage;
        std::unique_ptr<PatchPage> inputs, outputs;
        juce::TabbedComponent tabs;
        juce::ComboBox type, output, input, buffer;
        juce::Label typeLabel, outputLabel, inputLabel, bufferLabel, rate, explanation, status;
        juce::ToggleButton enabled { "Enable audio interface" }, save { "Save show for next launch" }, defaults { "Use as defaults for new shows" };
        juce::TextButton rescan { "Rescan interfaces" }, apply { "Apply while stopped" }, cancel { "Close" };
        bool waiting = false, sawApplying = false, saveAfterApply = false, defaultsAfterApply = false;
        std::string errorCount, errorCountAtApply, sequence, sequenceAtApply;
    };

    AudioSettingsWindow::AudioSettingsWindow (const model::Theme& theme, const tree::TreeSnapshot& snapshot,
                                             std::function<void (Event)> send, std::function<void()> onPanic)
        : DocumentWindow ("Audio settings", Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          panic (std::move (onPanic))
    {
        panel = std::make_unique<Panel> (theme, snapshot, std::move (send), [this] { closeButtonPressed(); });
        setUsingNativeTitleBar (true); setResizable (true, false); setResizeLimits (740, 550, 1500, 1100);
        setContentNonOwned (panel.get(), true); centreWithSize (880, 650); setVisible (true);
    }
    AudioSettingsWindow::~AudioSettingsWindow() { panel->stopTest(); clearContentComponent(); }
    void AudioSettingsWindow::refresh (const tree::TreeSnapshot& snapshot) { panel->refresh (snapshot); }
    void AudioSettingsWindow::closeButtonPressed() { panel->stopTest(); setVisible (false); }
    bool AudioSettingsWindow::keyPressed (const juce::KeyPress& key)
    {
        if (key != juce::KeyPress (juce::KeyPress::escapeKey)) return false;
        panel->stopTest();
        if (panic) panic();
        return true;
    }
}
