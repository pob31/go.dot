/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/client/ui/ShowSettingsWindow.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Devices.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>
#include <spatcore/ui/patch/PatchMatrixComponent.h>
#include <spatcore/io/TestSignalGenerator.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

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
            TestMatrix (spatcore::ui::patch::PatchMatrixConfig configToUse, bool input,
                        spatcore::io::TestSignalGenerator* generator)
                : Matrix (std::move (configToUse), input, generator), draftGenerator (generator) {}
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
                /*  THE OUTPUT SIDE HAS NO CHANNEL COUNT TO TYPE (author,
                    2026-09-22: "I think we can remove the logical channel.
                    Adding and removing channels is redundant with this").

                    The show's OUTPUT LIST is the count: every bus declares a
                    width, `firstChannel` is the running sum, and the four
                    `bus.*` commands keep it packed. A second number beside it
                    was a second answer to one question, and the two could
                    disagree - a patch row past the last bus went nowhere and
                    said nothing about it.

                    THE INPUT SIDE KEEPS IT, because nothing else declares
                    inputs. There is no list of them to read a count off, so
                    the box is the only place the number can come from. */
                if (isInput)
                {
                    addAndMakeVisible (countLabel);
                    countLabel.setText ("Logical channels", juce::dontSendNotification);
                    addAndMakeVisible (count); count.setInputRestrictions (3, "0123456789");
                    count.setText (juce::String (static_cast<int> (mapping.size())));
                    count.onReturnKey = [this] { changeRows(); };
                    count.onFocusLost = [this] { changeRows(); };
                }
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
                    /*  BACK TO FOLLOWING THE LIST, for an output patch: the
                        straight diagonal is what an unsettled show already
                        has, so the honest answer to this button is to hand the
                        outputs back to the order rather than to freeze a copy
                        of it. `follow` sends the flag; the diagonal below is
                        what the operator sees either way. */
                    mapping = audio::identityPatch (static_cast<int> (mapping.size()));
                    for (auto& channel : mapping) if (channel >= hardware) channel = -1;
                    touched = false;
                    rebuild();

                    if (! isInput && follow)
                        follow();
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

                /*  A SHOW STILL FOLLOWING ITS LIST SENDS NO PATCH AT ALL.
                    Empty is what the device layer reads as identity and what
                    keeps the outputs following the output list; writing the
                    diagonal out in full would settle the show - and somebody
                    who pressed Apply to change a buffer size did not mean to
                    freeze their patch. Only a hand edit does that, and a hand
                    edit sets `touched`. */
                if (! isInput && ! touched && ! settled)
                    return {};

                return audio::writePatch (mapping);
            }

            /** Whether the show has stopped following its output list. */
            void setSettled (bool now) noexcept { settled = now; }
            void setHardware (int channels)
            {
                if (hardware == channels) return;
                readMatrix(); hardware = channels; rebuild();
            }

            /*  THE OUTPUT LIST, ARRIVING FROM THE DOCUMENT. Two things follow
                from it: the rows are named after the outputs, and how many
                there are is the layout's, so the number box on this page has
                nothing left to decide.

                THE TWO MOVE SEPARATELY, and that is the point of the split. A
                RENAMED output must reach the rows whatever the operator has
                drawn - the draft is read back first, so their patch survives
                the rebuild - while RESIZING the draft to a new output count is
                held off once they have started patching, because that would
                move the rows under their hand. */
            void setOutputs (std::vector<std::string> names, int channelCount)
            {
                /*  THE LAYOUT IS A FLOOR AND NOT THE COUNT. It says how many
                    logical outputs the show's buses need; it does not say how
                    many rows the operator has declared, and a patch written
                    with more of them than the buses span is theirs. Resizing
                    DOWN to the bus span would drop those rows out of the draft,
                    and the next Apply would write the shortened patch - every
                    output past the buses silently unpatched. Grow only. */
                const auto renamed = names != labels;
                const auto resized = channelCount > 0 && ! touched
                                       && channelCount > static_cast<int> (mapping.size());

                if (! renamed && ! resized)
                    return;

                labels = std::move (names);

                //  Whatever is drawn now, kept: `rebuild` draws from `mapping`.
                readMatrix();

                if (resized)
                {
                    const auto was = mapping.size();
                    mapping.resize (static_cast<std::size_t> (channelCount), -1);

                    /*  A NEW OUTPUT CONTINUES THE DIAGONAL where that channel
                        is free, rather than arriving disconnected: the show is
                        following its list, so what the operator should see is
                        what the engine is doing. A channel already spoken for
                        is left at -1 for them to place. */
                    for (auto row = was; row < mapping.size(); ++row)
                    {
                        const auto wanted = static_cast<int> (row);
                        const auto taken = std::find (mapping.begin(), mapping.end(), wanted) != mapping.end();
                        mapping[row] = taken ? -1 : wanted;
                    }

                    minimum = channelCount;

                    //  Only the input side has one to keep in step.
                    if (isInput)
                        count.setText (juce::String (channelCount), false);
                }

                rebuild();
            }

            /*  WHETHER THE OPERATOR HAS TOUCHED IT. The first hand edit is what
                settles the patch (WFS-DIY's latch, spent at the edit rather
                than at the window opening), and it is also what stops a
                refresh from overwriting a draft somebody is working in. */
            std::function<void()> edited;

            /** The "1:1" button on the output page: follow the list again. */
            std::function<void()> follow;

            void resized() override
            {
                auto area = getLocalBounds().reduced (10);
                auto bar = area.removeFromTop (30);
                if (isInput)
                {
                    countLabel.setBounds (bar.removeFromLeft (145));
                    count.setBounds (bar.removeFromLeft (65));
                    bar.removeFromLeft (20);
                }
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
                /*  AT LEAST AS MANY COLUMNS AS ROWS, always. A show with
                    eight outputs and a device reporting none - which is every
                    session on the dummy clock - drew six columns and left the
                    last two outputs with nowhere to go. The diagonal has to be
                    reachable whatever the interface says it has. */
                int columns = std::max (std::max (hardware, static_cast<int> (mapping.size())), 2);
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
                {
                    /*  A ROW SAYS WHICH OUTPUT IT IS, not which number:
                        "Main L/R · L" is what somebody wiring the rig would
                        say, and a matrix of rows reading "Output 7" is one
                        nobody can patch without counting. The labels come from
                        the output list and fall back to the number for a
                        channel no output claims. */
                    if (! isInput && row >= 0 && static_cast<std::size_t> (row) < labels.size())
                        return juce::String (labels[static_cast<std::size_t> (row)]);

                    return juce::String (isInput ? "Input " : "Output ") + juce::String (row + 1);
                };
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

                /*  FIRED BEFORE A USER EDIT LANDS, and not by the programmatic
                    rebuilds above - spatcore's own contract for this hook, and
                    the reason WFS-DIY latches on it rather than on the window
                    opening. Merely LOOKING at a patch that follows the list
                    must leave it following. */
                matrix->onBeforeUserPatchEdit = [this]
                {
                    touched = true;

                    if (edited)
                        edited();
                };
                matrix->setMode (test.getToggleState() ? Matrix::Mode::Testing : patch.getToggleState() ? Matrix::Mode::Patching : Matrix::Mode::Scrolling);
                test.setEnabled (hardware > 0);
                addAndMakeVisible (*matrix); resized();
            }
            const model::Theme& theme;
            bool isInput;
            int minimum, hardware = 0;
            bool touched = false, settled = false;
            std::vector<std::string> labels;
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

        /*  THE OUTPUT LIST: what the show sends to, in the order the interface
            is wired. One list of mixed kinds rather than two (the author's
            choice, 2026-09-21), because a rig interleaves them - PRD §6.2's
            own example is mono direct outs among stereo mixes - and a list
            that can be interleaved comes out one-for-one with no patching at
            all.

            EVERY ROW IS A COMMAND. Renaming is a `node.set`; the width combo,
            the delete cross, the two add buttons and a dragged row are
            `bus.width`, `bus.delete`, `bus.create` and `bus.move`. Nothing
            here edits a draft the way the patch matrix does, so nothing here
            needs applying: the document changes as the designer works, and
            Ctrl-Z takes it back. */
        class OutputPage final : public juce::Component,
                                 public juce::DragAndDropContainer,
                                 public juce::DragAndDropTarget,
                                 private juce::ListBoxModel
        {
        public:
            OutputPage (const model::Theme& themeToUse, std::function<void (Event)> dispatch)
                : theme (themeToUse), send (std::move (dispatch))
            {
                list.setModel (this);
                list.setRowHeight (34);
                list.setOutlineThickness (0);

                /*  THE GROUND UNDER THE ROWS IS THE PANEL'S, not JUCE's own
                    pale default: a list with three outputs in it is mostly
                    empty, and an empty slab of the wrong colour reads as a
                    second surface that is not there. */
                list.setColour (juce::ListBox::backgroundColourId, Look::colour (themeToUse, "panel-in"));
                addAndMakeVisible (list);

                for (auto* button : { &addMonoDirect, &addStereoDirect, &addMonoMix, &addStereoMix })
                    addAndMakeVisible (*button);

                /*  HIDDEN UNTIL A NAME IS CLICKED, and a child of the page
                    rather than of the list so that scrolling cannot leave it
                    drawn over the wrong row: it is placed from the row's live
                    position each time it opens. */
                addChildComponent (nameEditor);
                nameEditor.setEditable (false, true, false);
                nameEditor.setColour (juce::Label::backgroundColourId,
                                      Look::colour (themeToUse, "panel-in"));
                nameEditor.setColour (juce::Label::textColourId, Look::colour (themeToUse, "ink"));
                nameEditor.onEditorHide = [this] { commitName(); };

                addAndMakeVisible (regime);
                addAndMakeVisible (summary);
                regime.setJustificationType (juce::Justification::topLeft);
                summary.setJustificationType (juce::Justification::topLeft);

                /*  FOUR BUTTONS AND NO FLIP (author, 2026-09-22: "I'd rather
                    have fixed add mono and add stereo direct out or mix
                    channels than to flip").

                    A width is decided when an output is made, and changing it
                    afterwards moves every channel below it - which is the
                    repack working correctly and reads, from the patch, as the
                    rig having been re-wired. Saying it once, at the moment
                    the thing is created, is both fewer gestures and fewer
                    surprises; an output of the wrong width is deleted and
                    made again, which is one more click than a flip and says
                    what it is doing. */
                addMonoDirect.setTooltip ("A direct out is where one cue's own channels land. "
                                          "Mono: one interface channel.");
                addStereoDirect.setTooltip ("A direct out is where one cue's own channels land. "
                                            "Stereo: two consecutive interface channels.");
                addMonoMix.setTooltip ("A mix channel is one many cues send into, each at its own "
                                       "level. Mono: one interface channel.");
                addStereoMix.setTooltip ("A mix channel is one many cues send into, each at its own "
                                         "level. Stereo: two consecutive interface channels.");

                addMonoDirect.onClick   = [this] { if (send) send (gesture::createBus ("direct", 1, -1)); };
                addStereoDirect.onClick = [this] { if (send) send (gesture::createBus ("direct", 2, -1)); };
                addMonoMix.onClick      = [this] { if (send) send (gesture::createBus ("mix", 1, -1)); };
                addStereoMix.onClick    = [this] { if (send) send (gesture::createBus ("mix", 2, -1)); };

                /*  HOW MANY CUES CAN SOUND AT ONCE, said in those words rather
                    than as "tracks": the number IS the track count, and what
                    somebody setting it wants to know is how many things they
                    can hear together. It belongs on this tab because it is a
                    decision about the shape of the show's audio, which is what
                    this tab is; the Interface tab beside it is about the
                    device. */
                addAndMakeVisible (polyphony);
                addAndMakeVisible (polyphonyLabel);
                polyphonyLabel.setText ("Cues that can sound at once", juce::dontSendNotification);
                polyphony.setTooltip ("The polyphony ceiling. Applies when you apply the audio"
                                      " settings, as a buffer size does.");
                polyphony.setJustification (juce::Justification::centredLeft);
                polyphony.setInputRestrictions (3, "0123456789");

                polyphony.onReturnKey = [this] { commitPolyphony(); };
                polyphony.onFocusLost = [this] { commitPolyphony(); };
            }

            void show (std::vector<model::OutputRow> outputs, bool settled, bool editable,
                       int hardwareOutputs, int tracks)
            {
                const auto sameRows = outputs.size() == rows.size()
                                        && std::equal (outputs.begin(), outputs.end(), rows.begin(),
                                                       [] (const model::OutputRow& a, const model::OutputRow& b)
                                                       {
                                                           return a.id == b.id && a.name == b.name
                                                               && a.kind == b.kind && a.width == b.width
                                                               && a.firstChannel == b.firstChannel;
                                                       });

                const auto sameLock = locked == ! editable;

                rows = std::move (outputs);
                locked = ! editable;

                for (auto* button : { &addMonoDirect, &addStereoDirect, &addMonoMix, &addStereoMix })
                    button->setVisible (editable);

                regime.setText (juce::String (model::outputRegime (settled)), juce::dontSendNotification);

                const auto wanted = model::outputChannelCount (rows);

                /*  WHAT THE INTERFACE CAN ACTUALLY CARRY, said here rather than
                    found out at the next GO. Applying the settings is what
                    widens the graph, and a designer who has just added two
                    outputs is exactly the person who needs telling. */
                /*  AND WHETHER THIS SHOW CAN PLAY ANYTHING AT ALL, which is
                    the question the two numbers answer between them: a show
                    with no outputs has nowhere to send sound, and a show that
                    can play nothing at once has nothing to send. Either way
                    every GO ends `no-track` or `bad-route`, and finding that
                    out at the first cue is the failure this line exists to
                    prevent. */
                summary.setText (wanted == 0
                                   ? juce::String ("No outputs yet, so nothing can be heard."
                                                   " Add a direct out above.")
                                   : tracks <= 0
                                     ? juce::String ("No cues can sound at once, so nothing will play."
                                                     " Set a number above.")
                                     : juce::String (wanted) + " output channel" + (wanted == 1 ? "" : "s")
                                         + (hardwareOutputs > 0 && wanted > hardwareOutputs
                                              ? ", and the interface is open at " + juce::String (hardwareOutputs)
                                                + ". Apply while stopped to use the rest."
                                              : juce::String {}),
                                 juce::dontSendNotification);

                /*  REDRAWN WHEN SOMETHING DRAWN HAS MOVED, and not otherwise.
                    This is refreshed with the rest of the window, twenty-five
                    times a second, and a list that repainted on every one of
                    them would be asking the message thread to draw the same
                    rows over and over for nothing. */
                /*  Not while it is being typed into, or a refresh twenty-five
                    times a second would take the caret back on every keystroke. */
                if (! polyphony.hasKeyboardFocus (true) && polyphony.getText() != juce::String (tracks))
                    polyphony.setText (juce::String (tracks), juce::dontSendNotification);

                polyphony.setEnabled (editable);

                if (! sameRows)
                    list.updateContent();

                if (! sameRows || ! sameLock)
                    list.repaint();
            }

            void paintOverChildren (juce::Graphics& g) override
            {
                if (dropRow < 0)
                    return;

                /*  WHERE IT WOULD LAND, drawn between two rows rather than on
                    one: the gap is what a move names, and a line on a row
                    would read as "replace this". */
                const auto y = list.getY() + dropRow * list.getRowHeight()
                                 - list.getViewport()->getViewPositionY();

                g.setColour (Look::colour (theme, "picked"));
                g.fillRect (list.getX(), y - 1, list.getWidth(), 2);
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (10);
                auto bar = area.removeFromTop (30);
                for (auto* button : { &addMonoDirect, &addStereoDirect, &addMonoMix, &addStereoMix })
                    button->setBounds (bar.removeFromLeft (128).reduced (3, 0));
                bar.removeFromLeft (20);
                polyphonyLabel.setBounds (bar.removeFromLeft (220));
                polyphony.setBounds (bar.removeFromLeft (60).reduced (0, 2));
                area.removeFromTop (8);
                regime.setBounds (area.removeFromTop (34));
                summary.setBounds (area.removeFromBottom (26));
                area.removeFromTop (4);
                list.setBounds (area);
            }

        private:
            int getNumRows() override { return static_cast<int> (rows.size()); }

            void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
            {
                if (row < 0 || static_cast<std::size_t> (row) >= rows.size())
                    return;

                const auto& entry = rows[static_cast<std::size_t> (row)];
                const auto ink = Look::colour (theme, "ink");

                g.setColour (Look::colour (theme, row % 2 == 0 ? "panel" : "panel-in"));
                g.fillRect (0, 0, width, height - 1);

                auto area = juce::Rectangle<int> (0, 0, width, height).reduced (8, 0);

                /*  THE GRIP FIRST, because a row that can be dragged has to
                    look like one - and the cross last, at the far end, where a
                    delete belongs rather than beside the name it would take
                    away. Both are left out entirely under the lock rather than
                    dimmed, which is the window's rule for structure (the
                    new-cue bar does the same). */
                if (! locked)
                {
                    g.setColour (Look::colour (theme, "ink-off"));
                    g.setFont (Look::font (theme, 13.0f));
                    g.drawText (juce::String::fromUTF8 ("\xe2\x89\xa1"), area.removeFromLeft (18),
                                juce::Justification::centred);

                    g.setColour (Look::colour (theme, "ink-dim"));
                    g.drawText (juce::String::fromUTF8 ("\xc3\x97"), area.removeFromRight (24),
                                juce::Justification::centred);
                }
                else
                {
                    area.removeFromLeft (18);
                    area.removeFromRight (24);
                }

                g.setFont (Look::font (theme, 12.0f));
                g.setColour (Look::colour (theme, "ink-dim"));
                g.drawText (juce::String (entry.channelWord()), area.removeFromRight (74),
                            juce::Justification::centredLeft);
                g.drawText (juce::String (entry.widthWord()), area.removeFromRight (80),
                            juce::Justification::centredLeft);
                g.drawText (juce::String (entry.kindWord()), area.removeFromRight (96),
                            juce::Justification::centredLeft);

                g.setFont (Look::font (theme, 13.0f));
                g.setColour (ink);
                g.drawText (juce::String (entry.name), area, juce::Justification::centredLeft, true);
            }

            void listBoxItemClicked (int row, const juce::MouseEvent& event) override
            {
                if (locked || row < 0 || static_cast<std::size_t> (row) >= rows.size() || ! send)
                    return;

                const auto& entry = rows[static_cast<std::size_t> (row)];

                /*  THE ROW'S WIDTH, NOT THE LIST'S. A scrollbar makes the two
                    differ, and every cell carved from the wrong one is off by
                    its width - which the cue list learned the hard way. */
                const auto width = event.eventComponent != nullptr ? event.eventComponent->getWidth()
                                                                   : list.getWidth();

                /*  Carved from the right in the painter's own order, so the
                    two stay in step: the cross, then the channels, then the
                    width word.

                    THE WIDTH CELL NO LONGER FLIPS. A width is said when the
                    output is made - there are four buttons for it - and the
                    word here reports rather than offers. `bus.width` is still
                    a command and still tested; nothing in this window sends
                    it. */
                if (event.x > width - 32)
                    send (gesture::deleteBus (entry.id));
                else
                    renameAt (row, event);
            }

            /*  A DOUBLE CLICK IS THE SAME THING TWICE now that one click
                opens the editor. Kept rather than removed, because a hand that
                has learned to double-click a name should not be punished for
                it - the second click lands in the editor the first one
                opened. */
            void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override {}

            /*  WHERE THE NAME IS DRAWN, carved exactly as the painter carves
                it: the grip off the left, and the cross, channels, width and
                kind off the right. One arithmetic, so a click cannot land
                somewhere the eye says is a name. */
            static juce::Rectangle<int> nameCellOf (juce::Rectangle<int> row)
            {
                auto area = row.reduced (8, 0);
                area.removeFromLeft (18);
                area.removeFromRight (24 + 74 + 80 + 96);
                return area;
            }

            /*  RENAMED IN PLACE (author, 2026-09-22: "can we directly rename
                in the output list rather than double click, validate and
                all?"). One click on the name opens an editor over it, Return
                or clicking away commits, Escape puts it back. One floating
                editor rather than a component per row: a list of outputs is a
                handful of rows and only one of them can be being typed into.  */
            void renameAt (int row, const juce::MouseEvent& event)
            {
                if (row < 0 || static_cast<std::size_t> (row) >= rows.size())
                    return;

                const auto width = event.eventComponent != nullptr ? event.eventComponent->getWidth()
                                                                   : list.getWidth();

                const auto cell = nameCellOf (juce::Rectangle<int> (0, 0, width, list.getRowHeight()));

                if (event.x < cell.getX() || event.x >= cell.getRight())
                    return;

                auto place = list.getRowPosition (row, true);
                place.translate (list.getX(), list.getY());

                editing = rows[static_cast<std::size_t> (row)].id;

                nameEditor.setBounds (nameCellOf (place));
                nameEditor.setText (juce::String (rows[static_cast<std::size_t> (row)].name),
                                    juce::dontSendNotification);
                nameEditor.setVisible (true);
                nameEditor.showEditor();
            }

            bool isInterestedInDragSource (const SourceDetails& details) override
            {
                return ! locked && details.description.isString();
            }

            void itemDragMove (const SourceDetails& details) override
            {
                const auto row = rowAt (details.localPosition);

                if (row != dropRow)
                {
                    dropRow = row;
                    list.repaint();
                }
            }

            void itemDragExit (const SourceDetails&) override
            {
                dropRow = -1;
                list.repaint();
            }

            void itemDropped (const SourceDetails& details) override
            {
                const auto row = rowAt (details.localPosition);
                const auto id = details.description.toString().toStdString();

                dropRow = -1;
                list.repaint();

                if (locked || ! send || id.empty())
                    return;

                /*  A POSITION IN THE LIST AS IT STANDS, the dragged output
                    still counted - `bus.move`'s convention, which is
                    `object.move`'s, which is `juce::ValueTree::moveChild`'s. A
                    drop BELOW a row means after it, and dropping an output on
                    itself is not a move at all. */
                const auto from = indexOf (id);

                if (from < 0 || row < 0)
                    return;

                const auto to = row > from ? row - 1 : row;

                if (to != from)
                    send (gesture::moveBus (id, to));
            }

            juce::var getDragSourceDescription (const juce::SparseSet<int>& selected) override
            {
                /*  THE IDENTIFIER, never the row number - a drag that carried a
                    position would name a row that may have moved by the time
                    it lands. The cue list settled this; the rule is the same
                    here. */
                if (locked || selected.isEmpty())
                    return {};

                const auto row = selected[0];

                if (row < 0 || static_cast<std::size_t> (row) >= rows.size())
                    return {};

                return juce::var (juce::String (rows[static_cast<std::size_t> (row)].id));
            }

            /*  Which gap in the list a point is nearest, counted in rows: 0 is
                above the first output and `rows.size()` is past the last. Half
                a row down is what turns "on this row" into "after it". */
            int rowAt (juce::Point<int> where) const
            {
                const auto inList = where - list.getPosition();
                const auto height = juce::jmax (1, list.getRowHeight());
                const auto row = (inList.y + height / 2) / height;

                return juce::jlimit (0, static_cast<int> (rows.size()), row);
            }

            int indexOf (const std::string& id) const
            {
                for (std::size_t at = 0; at < rows.size(); ++at)
                    if (rows[at].id == id)
                        return static_cast<int> (at);

                return -1;
            }

            void commitPolyphony()
            {
                const auto wanted = polyphony.getText().trim();

                if (! send || wanted.isEmpty())
                    return;

                send (gesture::setNode ("/godot/audio/tracks",
                                        juce::String (wanted.getIntValue()).toStdString()));
            }

            void commitName()
            {
                const auto typed = nameEditor.getText().trim().toStdString();
                const auto id = editing;

                editing.clear();
                nameEditor.setVisible (false);

                if (id.empty() || typed.empty() || send == nullptr)
                    return;

                send (gesture::setNode ("/godot/bus/" + id + "/name", typed));
            }

            const model::Theme& theme;
            std::function<void (Event)> send;
            std::vector<model::OutputRow> rows;
            bool locked = false;
            int dropRow = -1;
            juce::ListBox list;
            juce::TextButton addMonoDirect { "+ mono out" }, addStereoDirect { "+ stereo out" },
                             addMonoMix { "+ mono mix" }, addStereoMix { "+ stereo mix" };
            /*  The one floating name editor, and which output it is over.
                Empty when nothing is being typed into. */
            juce::Label nameEditor;
            std::string editing;

            juce::Label regime, summary, polyphonyLabel;
            juce::TextEditor polyphony;
        };

        /*  THE BOXES THIS SHOW TALKS TO.

            The author's shape for this is their own desk's (WFS-DIY's Network
            tab): a table of connections, each with a name, an address, a port
            and a pair of ON/OFF switches for whether it is heard and whether
            it is spoken to - and one button under it deciding whether messages
            from anybody else are obeyed at all. The words here are that tab's
            words on purpose, because it is the same person reading them.

            WHAT IS DIFFERENT, and why. There is no fixed ceiling of six rows:
            a device is a document object and `ADD` makes one. There is a
            PREFIX column, which WFS-DIY has no need of - a Go.dot cue carries
            the whole address it writes, so the prefix is the root of every cue
            aimed here, and it is what the inspector's target menu swaps. And
            there is no Protocol column yet: every device this window makes
            speaks plain OSC (the author, 2026-09-22: *"let's start with vanilla
            OSC"*), and a column offering one choice is a column that teaches
            nothing. Described devices - the ones with an OSCQuery namespace
            file - still appear here and read exactly like the rest; what they
            know about themselves is not edited from this tab.

            EVERY CELL IS A `node.set` AND NOTHING IS APPLIED. A device has no
            hardware to reopen, so there is no Apply button and no `applying`
            state: a retyped port reaches the socket on the next tick, through
            the after-tick's re-read of the declarations. That is the whole
            difference between this tab and the Interface tab beside it.
        */
        class NetworkPage final : public juce::Component,
                                  private juce::ListBoxModel
        {
        public:
            NetworkPage (const model::Theme& themeToUse, std::function<void (Event)> dispatch)
                : theme (themeToUse), send (std::move (dispatch))
            {
                list.setModel (this);
                list.setRowHeight (34);
                list.setOutlineThickness (0);
                list.setColour (juce::ListBox::backgroundColourId,
                                Look::colour (themeToUse, "panel-in"));
                addAndMakeVisible (list);

                addAndMakeVisible (addDevice);
                addDevice.setTooltip ("Declare a box this show sends to: a lighting desk, a"
                                      " processor, anything that listens for OSC.");
                addDevice.onClick = [this]
                {
                    if (send)
                        send (gesture::createDevice (freePrefix()));
                };

                /*  HIDDEN UNTIL A CELL IS CLICKED, and a child of the page
                    rather than of the list, so scrolling cannot leave it drawn
                    over the wrong row: it is placed from the row's live
                    position each time it opens. One editor rather than one per
                    cell - only one cell can be being typed into. */
                addChildComponent (cellEditor);
                cellEditor.setEditable (false, true, false);
                cellEditor.setColour (juce::Label::backgroundColourId,
                                      Look::colour (themeToUse, "panel-in"));
                cellEditor.setColour (juce::Label::textColourId, Look::colour (themeToUse, "ink"));
                cellEditor.onEditorHide = [this] { commitCell(); };

                addAndMakeVisible (filter);
                filter.setTooltip ("Filter incoming OSC: accept every sender, or only the devices"
                                   " above whose Rx is on. Refused messages are written to the log"
                                   " with the address that sent them.");
                filter.onClick = [this]
                {
                    if (send)
                        send (gesture::setNode ("/godot/network/strictSenders",
                                                strict ? "false" : "true"));
                };

                addAndMakeVisible (summary);
                summary.setJustificationType (juce::Justification::centredLeft);
            }

            void show (std::vector<model::DeviceRow> devices, bool strictNow,
                       int refusedNow, bool editable)
            {
                const auto sameRows = devices.size() == rows.size()
                                        && std::equal (devices.begin(), devices.end(), rows.begin(),
                                                       [] (const model::DeviceRow& a,
                                                           const model::DeviceRow& b)
                                                       {
                                                           return a.id == b.id && a.name == b.name
                                                               && a.prefix == b.prefix
                                                               && a.host == b.host && a.port == b.port
                                                               && a.rx == b.rx && a.tx == b.tx
                                                               && a.sent == b.sent
                                                               && a.problem == b.problem;
                                                       });

                const auto sameLock = locked == ! editable;

                rows = std::move (devices);
                locked = ! editable;
                strict = strictNow;

                addDevice.setVisible (editable);

                /*  TWO SETTINGS, AND THE WORD SAYS WHICH ONE IS IN FORCE -
                    never a light that is on or off (4.8). WFS-DIY's own two
                    labels, because it is the same switch. */
                filter.setButtonText (strict ? "OSC Filter: Registered Only"
                                             : "OSC Filter: Accept All");
                filter.setEnabled (editable);

                summary.setText (strict
                                   ? juce::String (refusedNow) + " message"
                                       + (refusedNow == 1 ? "" : "s") + " refused since the show"
                                         " opened. Only the devices above with Rx on are heard."
                                   : juce::String ("Every sender is heard. Switch to Registered"
                                                   " Only to take messages from the devices above"
                                                   " alone."),
                                 juce::dontSendNotification);

                if (! sameRows)
                    list.updateContent();

                if (! sameRows || ! sameLock || refusedNow != refused)
                    list.repaint();

                refused = refusedNow;
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (10);

                auto bar = area.removeFromTop (30);
                addDevice.setBounds (bar.removeFromLeft (128).reduced (3, 0));

                auto foot = area.removeFromBottom (32);
                filter.setBounds (foot.removeFromLeft (220).reduced (3, 2));
                foot.removeFromLeft (10);
                summary.setBounds (foot);

                area.removeFromTop (6);
                heading = area.removeFromTop (20);
                area.removeFromTop (2);
                list.setBounds (area);
            }

            void paint (juce::Graphics& g) override
            {
                /*  THE COLUMN NAMES, painted rather than a header component:
                    the list has no header of its own and a row of labels would
                    have to be kept in step with the painter's arithmetic by
                    hand. One carve, used three times.

                    CARVED FROM THE ROW'S WIDTH AND NOT THE PAGE'S. A scrollbar
                    makes the two differ, and a heading carved from the wrong
                    one slides away from the column it names the moment a show
                    has more devices than the list can show at once - which is
                    the same arithmetic mistake the cue list made, one pane
                    over. */
                g.setFont (Look::font (theme, 11.0f));
                g.setColour (Look::colour (theme, "ink-off"));

                auto cells = cellsFor (heading.withWidth (rowWidth()));

                const char* names[] { "Name", "Prefix", "IPv4 Address", "Tx Port",
                                      "Rx", "Tx", "Sent" };

                for (auto at = 0; at < 7; ++at)
                    g.drawText (names[at], cells[static_cast<std::size_t> (at)],
                                juce::Justification::centredLeft);
            }

        private:
            /*  How wide a row actually is, which is the list's width less
                whatever the scrollbar is taking. Asked of the viewed component
                rather than the list, because that is the component a row is
                laid out in and the one a click's coordinates come from. */
            int rowWidth() const
            {
                if (const auto* viewport = list.getViewport())
                    if (const auto* viewed = viewport->getViewedComponent())
                        if (viewed->getWidth() > 0)
                            return viewed->getWidth();

                return list.getWidth();
            }

            /*  ONE CARVE, USED BY THE PAINTER AND BY THE HIT TEST, so a click
                cannot land somewhere the eye says is another column. The cue
                list learned this the hard way and says so in its own header.

                Order from the right: the cross, then the readouts, then the
                two switches, then the numbers; the name takes what is left,
                because it is the one that wants room. */
            static std::array<juce::Rectangle<int>, 9> cellsFor (juce::Rectangle<int> row)
            {
                auto area = row.reduced (8, 0);

                const auto cross = area.removeFromRight (24);
                const auto problem = area.removeFromRight (150);
                const auto sent = area.removeFromRight (54);
                const auto tx = area.removeFromRight (42);
                const auto rx = area.removeFromRight (42);
                const auto port = area.removeFromRight (70);
                const auto host = area.removeFromRight (130);
                /*  WIDE ENOUGH FOR SEVERAL ROOTS. A desk reached directly can
                    answer at three - "/channel /console /digico" - and a column
                    that clipped the third would hide the fact that it is there
                    at all. Taken from the name, which has the rest of the row. */
                const auto prefix = area.removeFromRight (200);

                return { area, prefix, host, port, rx, tx, sent, problem, cross };
            }

            /*  What a click at this x is on, by the same arithmetic. Named
                rather than an index, because a column moving should break a
                compile and not a gesture. */
            enum class Cell { name, prefix, host, port, rx, tx, none, problem, cross };

            static Cell cellAt (int x, int width)
            {
                const auto cells = cellsFor (juce::Rectangle<int> (0, 0, width, 34));
                const Cell order[] { Cell::name, Cell::prefix, Cell::host, Cell::port,
                                     Cell::rx, Cell::tx, Cell::none, Cell::problem, Cell::cross };

                for (auto at = 0; at < 9; ++at)
                    if (x >= cells[static_cast<std::size_t> (at)].getX()
                          && x < cells[static_cast<std::size_t> (at)].getRight())
                        return order[at];

                return Cell::none;
            }

            int getNumRows() override { return static_cast<int> (rows.size()); }

            void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
            {
                if (row < 0 || static_cast<std::size_t> (row) >= rows.size())
                    return;

                const auto& entry = rows[static_cast<std::size_t> (row)];

                g.setColour (Look::colour (theme, row % 2 == 0 ? "panel" : "panel-in"));
                g.fillRect (0, 0, width, height - 1);

                const auto cells = cellsFor (juce::Rectangle<int> (0, 0, width, height));

                g.setFont (Look::font (theme, 13.0f));
                g.setColour (Look::colour (theme, "ink"));
                g.drawText (juce::String (entry.name), cells[0],
                            juce::Justification::centredLeft, true);

                g.setFont (Look::font (theme, 12.0f));
                g.setColour (Look::colour (theme, "ink-dim"));
                g.drawText (juce::String (entry.prefix), cells[1], juce::Justification::centredLeft, true);
                g.drawText (juce::String (entry.host), cells[2], juce::Justification::centredLeft, true);

                /*  A PORT OF NOUGHT IS NOT A PORT. The row's own default is
                    nothing at all, deliberately - no number could be right for
                    every device - so an empty cell is the honest drawing and a
                    bold nought would look like a decision somebody took. */
                g.drawText (entry.port > 0 ? juce::String (entry.port) : juce::String(),
                            cells[3], juce::Justification::centredLeft);

                /*  THE TWO SWITCHES, AS WORDS. WFS-DIY's ON and OFF, and the
                    word carries it rather than the colour (4.8). */
                for (auto at = 0; at < 2; ++at)
                {
                    const auto on = at == 0 ? entry.rx : entry.tx;

                    g.setColour (Look::colour (theme, on ? "ink" : "ink-off"));
                    g.drawText (on ? "ON" : "OFF", cells[static_cast<std::size_t> (4 + at)],
                                juce::Justification::centredLeft);
                }

                g.setColour (Look::colour (theme, "ink-dim"));
                g.drawText (juce::String (entry.sent), cells[6], juce::Justification::centredLeft);

                /*  AND WHAT IS WRONG WITH IT, in the engine's own sentence.
                    This is the whole reason the row exists rather than a line
                    on a terminal at startup: a device that can never work used
                    to look exactly like one that works, in every client, until
                    a cue failed during the show. */
                if (! entry.problem.empty())
                {
                    g.setColour (Look::colour (theme, "failed"));
                    g.drawText (juce::String (entry.problem), cells[7],
                                juce::Justification::centredLeft, true);
                }

                if (! locked)
                {
                    g.setColour (Look::colour (theme, "ink-dim"));
                    g.drawText (juce::String::fromUTF8 ("\xc3\x97"), cells[8],
                                juce::Justification::centred);
                }
            }

            void listBoxItemClicked (int row, const juce::MouseEvent& event) override
            {
                if (locked || row < 0 || static_cast<std::size_t> (row) >= rows.size() || ! send)
                    return;

                const auto& entry = rows[static_cast<std::size_t> (row)];

                /*  THE ROW'S WIDTH, NOT THE LIST'S: a scrollbar makes the two
                    differ and every cell carved from the wrong one is off by
                    its width. */
                const auto width = event.eventComponent != nullptr ? event.eventComponent->getWidth()
                                                                   : list.getWidth();

                const auto base = "/godot/mount/" + entry.id + "/";

                switch (cellAt (event.x, width))
                {
                    case Cell::cross:  send (gesture::deleteObject (entry.id)); return;
                    case Cell::rx:     send (gesture::setNode (base + "rx", entry.rx ? "false" : "true")); return;
                    case Cell::tx:     send (gesture::setNode (base + "tx", entry.tx ? "false" : "true")); return;

                    case Cell::name:
                    case Cell::prefix:
                    case Cell::host:
                    case Cell::port:   editAt (row, cellAt (event.x, width), width); return;

                    case Cell::problem:
                    case Cell::none:   return;
                }
            }

            void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override {}

            /*  EDITED IN PLACE, the output list's gesture exactly: one click
                opens an editor over the cell, Return or clicking away commits,
                Escape puts it back. */
            void editAt (int row, Cell cell, int width)
            {
                const auto& entry = rows[static_cast<std::size_t> (row)];

                auto place = list.getRowPosition (row, true);
                place.translate (list.getX(), list.getY());

                const auto cells = cellsFor (place.withWidth (width).withX (list.getX()));
                const auto index = cell == Cell::name ? 0 : cell == Cell::prefix ? 1
                                 : cell == Cell::host ? 2 : 3;

                editing = entry.id;
                editingCell = cell;

                cellEditor.setBounds (cells[static_cast<std::size_t> (index)]);
                cellEditor.setText (cell == Cell::name   ? juce::String (entry.name)
                                  : cell == Cell::prefix ? juce::String (entry.prefix)
                                  : cell == Cell::host   ? juce::String (entry.host)
                                  : entry.port > 0 ? juce::String (entry.port) : juce::String(),
                                    juce::dontSendNotification);
                cellEditor.setVisible (true);
                cellEditor.showEditor();
            }

            void commitCell()
            {
                const auto id = editing;
                const auto cell = editingCell;

                editing.clear();
                cellEditor.setVisible (false);

                if (id.empty() || ! send)
                    return;

                const auto typed = cellEditor.getText().trim();

                for (const auto& entry : rows)
                {
                    if (entry.id != id)
                        continue;

                    /*  UNCHANGED IS NOT A WRITE. Every one of these is an undo
                        step and a line in the log, so a click that opened an
                        editor and a click that closed it must not leave a
                        record of somebody deciding nothing. */
                    const auto was = cell == Cell::name   ? juce::String (entry.name)
                                   : cell == Cell::prefix ? juce::String (entry.prefix)
                                   : cell == Cell::host   ? juce::String (entry.host)
                                   : entry.port > 0 ? juce::String (entry.port) : juce::String();

                    if (typed == was)
                        return;

                    const auto name = cell == Cell::name   ? "name"
                                    : cell == Cell::prefix ? "prefix"
                                    : cell == Cell::host   ? "host" : "port";

                    send (gesture::setNode ("/godot/mount/" + id + "/" + name,
                                            typed.toStdString()));
                    return;
                }
            }

            /*  A PREFIX NOTHING ELSE IS USING, so `ADD` always makes a device
                rather than a refusal. `/device1`, `/device2`: a word a person
                can read in an address, and one they will rename anyway. */
            std::string freePrefix() const
            {
                for (auto at = 1; at < 1000; ++at)
                {
                    const auto candidate = "/device" + std::to_string (at);
                    auto taken = false;

                    for (const auto& entry : rows)
                        if (entry.prefix == candidate)
                            taken = true;

                    if (! taken)
                        return candidate;
                }

                return "/device";
            }

            const model::Theme& theme;
            std::function<void (Event)> send;

            juce::ListBox list;
            juce::TextButton addDevice { "ADD" };
            juce::TextButton filter { "OSC Filter: Accept All" };
            juce::Label summary;
            juce::Rectangle<int> heading;

            juce::Label cellEditor;
            std::string editing;
            Cell editingCell = Cell::name;

            std::vector<model::DeviceRow> rows;
            bool locked = false, strict = false;
            int refused = 0;
        };
    }

    class ShowSettingsWindow::Panel final : public juce::Component
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
            outputList = std::make_unique<OutputPage> (theme, send);
            network = std::make_unique<NetworkPage> (theme, send);

            /*  THE FIRST HAND EDIT OF THE OUTPUT PATCH IS WHAT SETTLES IT
                (PRD §6.2). Sent BEFORE the edit lands, so that the engine's own
                rule - a layout command materialises the patch it had before
                repacking - sees a settled show and keeps every output where it
                is. The other way it settles is the engine's: the first media
                run that launches with audio says so. */
            outputs->edited = [this]
            {
                if (! settled)
                {
                    settled = true;
                    send (gesture::setPatchSettled (true));
                }
            };

            outputs->follow = [this]
            {
                settled = false;
                send (gesture::setPatchSettled (false));
            };

            addAndMakeVisible (tabs);
            const auto background = Look::colour (theme, "panel");
            /*  "Audio" and not "Interface", since this window stopped being
                only about audio (2026-09-22): the tab at the end is about the
                network, and two tabs named after the thing they configure read
                better than one named after a word both could use. */
            tabs.addTab ("Audio", background, &interfacePage, false);
            tabs.addTab ("Outputs", background, outputList.get(), false);
            tabs.addTab ("Input patch", background, inputs.get(), false);
            tabs.addTab ("Output patch", background, outputs.get(), false);
            tabs.addTab ("Network", background, network.get(), false);
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

            /*  THE OUTPUT LIST IS THE DOCUMENT'S, so it is re-read every pass
                rather than held as a draft: `bus.create` and the rest land at
                once and are undoable, unlike the patch below them, which is
                applied. The same rows name the patch matrix's rows and set how
                many there are. */
            {
                const auto rows = model::readOutputs (snapshot);
                const auto lockedShow = model::isYes (model::flag (snapshot, "/godot/document/locked"));
                const auto hardware = juce::String (model::text (snapshot, "/godot/audio/hardwareOutputs")).getIntValue();

                settled = model::patchHasSettled (snapshot);
                outputs->setSettled (settled);
                outputList->show (rows, settled, ! lockedShow, hardware,
                                  juce::String (model::text (snapshot, "/godot/audio/tracks")).getIntValue());

                /*  Unconditional: `setOutputs` reads the draft back before it
                    redraws, so a rename reaches the rows without disturbing a
                    patch somebody is halfway through. */
                outputs->setOutputs (model::channelLabels (rows, 0),
                                     model::outputChannelCount (rows));
            }
            /*  THE DEVICES ARE THE DOCUMENT'S, so they are re-read every pass
                like the outputs above them rather than held as a draft:
                `mount.create` and every `node.set` under it land at once and
                are undoable. Nothing on that tab is applied - a device has no
                hardware to reopen, so a retyped port reaches the socket on the
                next tick through the engine's own re-read. */
            network->show (model::readDevices (snapshot),
                           model::isYes (model::flag (snapshot, "/godot/network/strictSenders")),
                           juce::String (model::text (snapshot, "/godot/network/refused")).getIntValue(),
                           ! model::isYes (model::flag (snapshot, "/godot/document/locked")));

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
            if (locked) status.setText ("Unlock the show to change the show settings.", juce::dontSendNotification);
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
        std::unique_ptr<OutputPage> outputList;
        std::unique_ptr<NetworkPage> network;
        bool settled = false;
        juce::TabbedComponent tabs;
        juce::ComboBox type, output, input, buffer;
        juce::Label typeLabel, outputLabel, inputLabel, bufferLabel, rate, explanation, status;
        juce::ToggleButton enabled { "Enable audio interface" }, save { "Save show for next launch" }, defaults { "Use as defaults for new shows" };
        juce::TextButton rescan { "Rescan interfaces" }, apply { "Apply while stopped" }, cancel { "Close" };
        bool waiting = false, sawApplying = false, saveAfterApply = false, defaultsAfterApply = false;
        std::string errorCount, errorCountAtApply, sequence, sequenceAtApply;
    };

    ShowSettingsWindow::ShowSettingsWindow (const model::Theme& theme, const tree::TreeSnapshot& snapshot,
                                             std::function<void (Event)> send, std::function<void()> onPanic)
        : DocumentWindow ("Audio settings", Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          panic (std::move (onPanic))
    {
        panel = std::make_unique<Panel> (theme, snapshot, std::move (send), [this] { closeButtonPressed(); });
        setUsingNativeTitleBar (true); setResizable (true, false); setResizeLimits (740, 550, 1500, 1100);
        setContentNonOwned (panel.get(), true); centreWithSize (880, 650); setVisible (true);
    }
    ShowSettingsWindow::~ShowSettingsWindow() { panel->stopTest(); clearContentComponent(); }
    void ShowSettingsWindow::refresh (const tree::TreeSnapshot& snapshot) { panel->refresh (snapshot); }
    void ShowSettingsWindow::closeButtonPressed() { panel->stopTest(); setVisible (false); }
    bool ShowSettingsWindow::keyPressed (const juce::KeyPress& key)
    {
        if (key != juce::KeyPress (juce::KeyPress::escapeKey)) return false;
        panel->stopTest();
        if (panic) panic();
        return true;
    }
}
