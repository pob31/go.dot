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

#include <wfg/engine/plugin/ProcessUtil.h>     // <windows.h>, lean, before JUCE on Windows
#include <wfg/engine/plugin/PluginEditorChild.h>
#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/plugin/EditorRegion.h>
#include <wfg/engine/plugin/PluginLoad.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if JUCE_MAC
/*  Declared here as PluginHostChild.cpp declares it, and for its reason:
    runDispatchLoop is [NSApp run], and there is no NSApp unless it is made. */
namespace juce { void initialiseNSApplication(); }
#endif

namespace wfg::plugin
{
    namespace
    {
        std::string optionFrom (const std::vector<std::string>& args, const char* name)
        {
            const std::string prefix = std::string (name) + "=";

            for (const auto& arg : args)
                if (arg.rfind (prefix, 0) == 0)
                    return arg.substr (prefix.size());

            return {};
        }

        bool hasFlag (const std::vector<std::string>& args, const char* name)
        {
            return std::find (args.begin(), args.end(), std::string (name)) != args.end();
        }

        int numberFrom (const std::vector<std::string>& args, const char* name, int otherwise)
        {
            const auto text = optionFrom (args, name);
            const auto value = text.empty() ? 0 : std::atoi (text.c_str());
            return value > 0 ? value : otherwise;
        }

        template <std::size_t size>
        std::string textOf (const char (&chars)[size])
        {
            return std::string (chars, static_cast<std::size_t> (std::find (chars, chars + size, '\0') - chars));
        }

        void reportFailure (editor::Region& region, const std::string& sentence)
        {
            std::memset (region.problem, 0, sizeof (region.problem));
            std::snprintf (region.problem, sizeof (region.problem), "%s", sentence.c_str());
            region.failed.store (1, std::memory_order_release);
        }

        /*  How long a value this window just moved wins over the tree's copy
            of it: long enough for the write to go round the tick thread and
            come back (two ticks, M33), so an older echo arriving mid-turn
            cannot drag the knob back. */
        constexpr std::uint32_t ownValueMs = 300;

        //======================================================================
        /*  THE TEST GAIN, AS A REAL PROCESSOR. The voice child plays it with
            no JUCE at all; here it has to have a window, so it is a processor
            with the catalogue's two parameters - Gain, resting at a half, its
            text in decibels, and the kill switch - and JUCE's generic editor.
            The kill switch does nothing in this process, which never plays a
            block: it is only a value the cue can hold. */
        class TestGainProcessor final : public juce::AudioProcessor
        {
        public:
            TestGainProcessor()
                : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                                   .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
            {
                auto decibels = juce::AudioParameterFloatAttributes().withStringFromValueFunction (
                    [] (float value, int)
                    {
                        return value <= 0.0f ? juce::String ("-inf dB")
                                             : juce::String (20.0 * std::log10 (static_cast<double> (value)), 1) + " dB";
                    });

                addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain",
                                                                    juce::NormalisableRange<float> (0.0f, 1.0f),
                                                                    0.5f, decibels));
                addParameter (die = new juce::AudioParameterBool (juce::ParameterID { "die", 1 },
                                                                  "Die (the test's kill switch)", false));
            }

            const juce::String getName() const override { return "Test gain"; }
            void prepareToPlay (double, int) override {}
            void releaseResources() override {}

            void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
            {
                buffer.applyGain (gain->get());
            }

            double getTailLengthSeconds() const override { return 0.0; }
            bool acceptsMidi() const override { return false; }
            bool producesMidi() const override { return false; }
            juce::AudioProcessorEditor* createEditor() override { return nullptr; }
            bool hasEditor() const override { return false; }
            int getNumPrograms() override { return 1; }
            int getCurrentProgram() override { return 0; }
            void setCurrentProgram (int) override {}
            const juce::String getProgramName (int) override { return {}; }
            void changeProgramName (int, const juce::String&) override {}

            void getStateInformation (juce::MemoryBlock& into) override
            {
                const auto text = "gain=" + juce::String (gain->get()) + "\ndie=" + juce::String (die->get() ? 1 : 0) + "\n";
                into.replaceAll (text.toRawUTF8(), text.getNumBytesAsUTF8());
            }

            void setStateInformation (const void* data, int size) override
            {
                const auto text = juce::String::fromUTF8 (static_cast<const char*> (data), size);

                for (const auto& line : juce::StringArray::fromLines (text))
                {
                    if (line.startsWith ("gain="))
                        gain->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, line.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
                    else if (line.startsWith ("die="))
                        die->setValueNotifyingHost (line.fromFirstOccurrenceOf ("=", false, false).getIntValue() != 0 ? 1.0f : 0.0f);
                }
            }

        private:
            juce::AudioParameterFloat* gain = nullptr;
            juce::AudioParameterBool* die = nullptr;
        };

        //======================================================================
        /*  WHAT THE PLUGIN'S WINDOW DID, caught as it happens. A plugin may
            report from whichever thread it likes, so a report is only stored
            - the latest value, a flag, the gesture's depth - and the message
            thread collects them a pass at a time: one value per parameter per
            pass, however many the plugin sent. `muted` is up while this
            process applies values itself, so what the cue says is never
            mistaken for what a hand did. */
        struct Watcher final : juce::AudioProcessorParameter::Listener
        {
            explicit Watcher (int countToUse)
                : count (countToUse),
                  latest (std::make_unique<std::atomic<float>[]> (static_cast<std::size_t> (std::max (1, countToUse)))),
                  dirty (std::make_unique<std::atomic<std::uint32_t>[]> (static_cast<std::size_t> (std::max (1, countToUse)))),
                  gestures (std::make_unique<std::atomic<int>[]> (static_cast<std::size_t> (std::max (1, countToUse))))
            {
            }

            void parameterValueChanged (int index, float value) override
            {
                if (muted.load (std::memory_order_acquire) || index < 0 || index >= count)
                    return;

                latest[static_cast<std::size_t> (index)].store (value, std::memory_order_relaxed);
                dirty[static_cast<std::size_t> (index)].store (1, std::memory_order_release);
                anyDirty.store (true, std::memory_order_release);
            }

            void parameterGestureChanged (int index, bool starting) override
            {
                if (index < 0 || index >= count)
                    return;

                auto& depth = gestures[static_cast<std::size_t> (index)];

                if (starting)
                    depth.fetch_add (1, std::memory_order_acq_rel);
                else if (depth.load (std::memory_order_acquire) > 0)
                    depth.fetch_sub (1, std::memory_order_acq_rel);
            }

            bool inGesture (int index) const
            {
                return gestures[static_cast<std::size_t> (index)].load (std::memory_order_acquire) > 0;
            }

            const int count;
            std::unique_ptr<std::atomic<float>[]> latest;
            std::unique_ptr<std::atomic<std::uint32_t>[]> dirty;
            std::unique_ptr<std::atomic<int>[]> gestures;
            std::atomic<bool> anyDirty { false };
            std::atomic<bool> muted { false };
        };

        //======================================================================
        /*  WHAT THE WINDOW HOLDS: a line of Go.dot's own across the top -
            which cue this is, in words - and the plugin's editor under it.
            When the pick has no such insert the editor is hidden and the
            reason is drawn in its place: a native plugin view covers
            anything drawn over it, so hiding is the one honest greying. The
            colours are the default theme's panel and inks. */
        struct Holder final : juce::Component
        {
            static constexpr int bannerHeight = 28;

            Holder (std::unique_ptr<juce::AudioProcessorEditor> viewToUse, std::function<void (editor::Key)> onKeyToUse)
                : view (std::move (viewToUse)), onKey (std::move (onKeyToUse))
            {
                addAndMakeVisible (*view);
                setWantsKeyboardFocus (true);
                fit();
            }

            ~Holder() override
            {
                view.reset();
            }

            void fit()
            {
                setSize (std::max (view->getWidth(), 420), view->getHeight() + bannerHeight);
            }

            void childBoundsChanged (juce::Component* child) override
            {
                if (child == view.get() && ! laying)
                    fit();
            }

            void resized() override
            {
                const juce::ScopedValueSetter<bool> guard (laying, true);

                if (view->isResizable())
                    view->setBounds (0, bannerHeight, getWidth(), getHeight() - bannerHeight);
                else
                    view->setTopLeftPosition (0, bannerHeight);
            }

            void paint (juce::Graphics& g) override
            {
                g.fillAll (juce::Colour (0xFF1D1D22));

                auto banner = getLocalBounds().removeFromTop (bannerHeight).reduced (10, 0);
                g.setColour (juce::Colour (0xFFE8E6E1));
                g.setFont (14.0f);
                g.drawText (heading, banner, juce::Justification::centredLeft, true);

                if (greyed)
                {
                    g.setColour (juce::Colour (0xFFC6C1B9));
                    g.setFont (15.0f);
                    g.drawFittedText (reason, getLocalBounds().withTrimmedTop (bannerHeight).reduced (24),
                                      juce::Justification::centred, 4);
                    return;
                }

                g.setColour (juce::Colour (0xFFA9A49C));
                g.setFont (12.0f);
                g.drawText (reason, banner, juce::Justification::centredRight, true);
            }

            void show (const juce::String& headingToUse, bool greyedToUse, const juce::String& reasonToUse)
            {
                heading = headingToUse;
                reason = reasonToUse;

                if (greyed != greyedToUse)
                {
                    greyed = greyedToUse;
                    view->setVisible (! greyed);
                }

                repaint();
            }

            /*  THE SHOW'S KEYS, when the plugin did not take them: whatever
                the focused control inside the editor leaves unconsumed comes
                up through here. */
            bool keyPressed (const juce::KeyPress& key) override
            {
                if (key == juce::KeyPress (juce::KeyPress::spaceKey))
                {
                    if (onKey) onKey (editor::Key::space);
                    return true;
                }

                if (key == juce::KeyPress (juce::KeyPress::escapeKey))
                {
                    if (onKey) onKey (editor::Key::escape);
                    return true;
                }

                return false;
            }

            std::unique_ptr<juce::AudioProcessorEditor> view;
            std::function<void (editor::Key)> onKey;
            juce::String heading, reason;
            bool greyed = false;
            bool laying = false;
        };

        struct EditorWindow final : juce::DocumentWindow
        {
            EditorWindow (const juce::String& title, std::unique_ptr<Holder> holder, std::function<void()> onCloseToUse)
                : DocumentWindow (title, juce::Colour (0xFF1D1D22),
                                  juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton, true),
                  onClose (std::move (onCloseToUse))
            {
                setUsingNativeTitleBar (true);
                const auto canResize = holder->view->isResizable();
                setContentOwned (holder.release(), true);
                setResizable (canResize, false);
                centreWithSize (getWidth(), getHeight());
            }

            void closeButtonPressed() override
            {
                if (onClose)
                    onClose();
            }

            std::function<void()> onClose;
        };

        //======================================================================
        /*  THE HELPER'S LIFE, a pass every ten milliseconds on the message
            thread: leave if asked or orphaned; hand what the window did to the
            parent under the subject it was done to; take a new subject; the
            test's hand; reconcile with values moved elsewhere; the window's
            visibility and place. */
        struct Session final : juce::Timer
        {
            Session (editor::Region& regionToUse, juce::AudioProcessor& processorToUse,
                     std::int64_t parentPidToUse, bool windowed, const juce::String& name)
                : region (regionToUse), processor (processorToUse), parentPid (parentPidToUse),
                  params (processorToUse.getParameters()),
                  count (std::min (params.size(), editor::maxParams)),
                  watcher (count)
            {
                baseline.assign (static_cast<std::size_t> (count), 0.0f);
                sentAt.assign (static_cast<std::size_t> (count), 0u);

                for (int i = 0; i < count; ++i)
                {
                    if (auto* p = params[i])
                    {
                        baseline[static_cast<std::size_t> (i)] = std::clamp (p->getValue(), 0.0f, 1.0f);
                        p->addListener (&watcher);
                    }
                }

                region.paramCount.store (static_cast<std::uint32_t> (count), std::memory_order_relaxed);
                mirror();

                if (windowed)
                {
                    auto* made = processor.hasEditor() ? processor.createEditorAndMakeActive() : nullptr;
                    std::unique_ptr<juce::AudioProcessorEditor> view (made != nullptr
                                                                          ? made
                                                                          : new juce::GenericAudioProcessorEditor (processor));

                    auto content = std::make_unique<Holder> (std::move (view), [this] (editor::Key key)
                    {
                        editor::push (region, editor::EventKind::key, static_cast<std::uint32_t> (key), 0.0f, taken);
                    });

                    holder = content.get();
                    holder->show (name, false, {});
                    window = std::make_unique<EditorWindow> (name, std::move (content), [this] { closePressed(); });
                }
            }

            ~Session() override
            {
                stopTimer();

                for (int i = 0; i < count; ++i)
                    if (auto* p = params[i])
                        p->removeListener (&watcher);

                /*  THE WINDOW BEFORE THE PROCESSOR: an editor outliving its
                    processor is the one ordering JUCE asserts on. */
                holder = nullptr;
                window.reset();
            }

            void timerCallback() override
            {
                ++ticks;

                if (region.shouldExit.load (std::memory_order_acquire) != 0
                      || (parentPid > 0 && ticks % 100 == 0 && ! process::isAlive (parentPid)))
                {
                    stopTimer();
                    juce::MessageManager::getInstance()->stopDispatchLoop();
                    return;
                }

                collect();
                takeSubject();
                poke();
                reconcile();
                place();

                if (ticks % 5 == 0)
                    mirror();
            }

            /*  WHAT THE HAND DID SINCE THE LAST PASS, one value a parameter,
                tagged with the subject in force when it was done - taken
                BEFORE a new subject is, so a turn made just as the pick moved
                goes to the cue it was made on. Greyed, it goes nowhere. */
            void collect()
            {
                if (! watcher.anyDirty.exchange (false, std::memory_order_acq_rel))
                    return;

                const auto now = juce::Time::getMillisecondCounter();

                for (int i = 0; i < count; ++i)
                {
                    const auto at = static_cast<std::size_t> (i);

                    if (watcher.dirty[at].exchange (0, std::memory_order_acq_rel) == 0)
                        continue;

                    if (greyed)
                        continue;

                    const auto value = watcher.latest[at].load (std::memory_order_relaxed);

                    if (editor::push (region, editor::EventKind::value, static_cast<std::uint32_t> (i), value, taken))
                        sentAt[at] = now | 1u;
                }
            }

            void takeSubject()
            {
                const auto seq = region.subjectSeq.load (std::memory_order_acquire);

                if (seq == taken)
                    return;

                const auto& s = region.subject;
                const auto title = juce::String::fromUTF8 (textOf (s.title).c_str());
                const auto reason = juce::String::fromUTF8 (textOf (s.reason).c_str());
                greyed = s.greyed.load (std::memory_order_relaxed) != 0;

                if (! greyed)
                {
                    const auto given = static_cast<int> (std::min<std::uint32_t> (s.valueCount.load (std::memory_order_relaxed),
                                                                                   static_cast<std::uint32_t> (count)));

                    apply ([&s, given] (int i)
                    {
                        return i < given ? s.values[i].load (std::memory_order_relaxed) : editor::restsAtPreset;
                    });
                }

                /*  A NEW CUE STARTS WITH NOTHING OF ITS OWN IN FLIGHT: the
                    last cue's protection is the last cue's. */
                std::fill (sentAt.begin(), sentAt.end(), 0u);

                taken = seq;
                region.subjectTaken.store (seq, std::memory_order_release);

                if (holder != nullptr)
                    holder->show (title, greyed, reason);

                if (window != nullptr && title.isNotEmpty())
                    window->setName (title);
            }

            /*  THE TEST'S HAND: what a click on the plugin's own window does,
                gesture and all, so the parent hears it the way it would hear
                a person. */
            void poke()
            {
                const auto seq = region.pokeSeq.load (std::memory_order_acquire);

                if (seq == pokeSeen)
                    return;

                pokeSeen = seq;
                const auto index = region.pokeIndex.load (std::memory_order_relaxed);
                const auto value = region.pokeValue.load (std::memory_order_relaxed);

                if (index == -2)
                {
                    closePressed();
                    return;
                }

                if (index >= 0 && index < count)
                    if (auto* p = params[index])
                    {
                        p->beginChangeGesture();
                        p->setValueNotifyingHost (std::clamp (value, 0.0f, 1.0f));
                        p->endChangeGesture();
                    }
            }

            /*  VALUES MOVED ELSEWHERE - an undo, the page, a surface - brought
                into the plugin, but never over a hand: a parameter in a
                gesture, or one this window moved a moment ago, keeps what
                the hand gave it until the tree has had time to agree. Only
                against values meant for the subject in force. */
            void reconcile()
            {
                if (greyed || region.liveSubject.load (std::memory_order_acquire) != taken)
                    return;

                const auto live = region.liveSeq.load (std::memory_order_acquire);

                if (live == liveSeen && ticks % 10 != 0)
                    return;

                liveSeen = live;
                const auto now = juce::Time::getMillisecondCounter();

                apply ([this] (int i) { return region.live[i].load (std::memory_order_relaxed); },
                       [this, now] (int i)
                       {
                           const auto at = static_cast<std::size_t> (i);
                           return watcher.inGesture (i) || (sentAt[at] != 0 && now - sentAt[at] < ownValueMs);
                       });
            }

            /*  THE PLUGIN PUT WHERE THE CUE SAYS, without a single report
                back: the watcher is muted while it happens. A value the cue
                does not mention rests where the preset left it. */
            template <typename ValueOf, typename Skip = bool (*) (int)>
            void apply (ValueOf valueOf, Skip skip = [] (int) { return false; })
            {
                watcher.muted.store (true, std::memory_order_release);

                for (int i = 0; i < count; ++i)
                {
                    auto* p = params[i];

                    if (p == nullptr || skip (i))
                        continue;

                    const auto given = valueOf (i);
                    const auto target = given < 0.0f ? baseline[static_cast<std::size_t> (i)] : std::clamp (given, 0.0f, 1.0f);

                    if (std::abs (p->getValue() - target) > 1.0e-6f)
                        p->setValueNotifyingHost (target);
                }

                watcher.muted.store (false, std::memory_order_release);
            }

            void closePressed()
            {
                closedByHand = true;

                if (window != nullptr)
                    window->setVisible (false);

                editor::push (region, editor::EventKind::windowClosed, 0, 0.0f, taken);
            }

            /*  SHOWN WHEN THE PARENT SAYS, forward when it asks, and above
                Go.dot's window while Go.dot is the one in front - never
                above everything, and never owned by Go.dot's window across
                processes: on Windows that joins the two processes' input,
                and a plugin window that hung would freeze the GO button. */
            void place()
            {
                if (window == nullptr)
                    return;

                const auto raise = region.raiseSeq.load (std::memory_order_acquire);
                const auto raised = raise != raiseSeen;
                raiseSeen = raise;

                if (raised)
                    closedByHand = false;

                const auto wanted = region.visible.load (std::memory_order_acquire) != 0 && ! closedByHand;

                if (wanted != window->isVisible())
                    window->setVisible (wanted);

                if (wanted && (raised || ! shownOnce))
                {
                    shownOnce = true;
                   #if JUCE_MAC
                    juce::Process::makeForegroundProcess();
                   #endif
                    window->toFront (true);

                    if (holder != nullptr)
                        holder->grabKeyboardFocus();
                }

               #if JUCE_WINDOWS
                if (ticks % 20 == 0 && window->isVisible())
                {
                    DWORD front = 0;

                    if (auto* foreground = ::GetForegroundWindow())
                        ::GetWindowThreadProcessId (foreground, &front);

                    const auto ours = static_cast<std::int64_t> (front) == parentPid
                                        || static_cast<std::int64_t> (front) == process::currentId();

                    if (ours != onTop)
                    {
                        onTop = ours;
                        window->setAlwaysOnTop (ours);
                    }
                }
               #endif
            }

            /** What the plugin has now, for the parent's tests and nothing else. */
            void mirror()
            {
                for (int i = 0; i < count; ++i)
                    if (auto* p = params[i])
                        region.current[i].store (p->getValue(), std::memory_order_relaxed);
            }

            editor::Region& region;
            juce::AudioProcessor& processor;
            const std::int64_t parentPid;
            const juce::Array<juce::AudioProcessorParameter*>& params;
            const int count;

            Watcher watcher;
            std::vector<float> baseline;
            std::vector<std::uint32_t> sentAt;

            std::unique_ptr<EditorWindow> window;
            Holder* holder = nullptr;

            std::uint32_t taken = 0, pokeSeen = 0, liveSeen = 0, raiseSeen = 0;
            std::uint64_t ticks = 0;
            bool greyed = false, closedByHand = false, shownOnce = false, onTop = false;
        };
    }

    //==============================================================================
    int runPluginEditor (const std::vector<std::string>& args)
    {
        const auto regionPath = optionFrom (args, "--region");
        const auto identifier = optionFrom (args, "--plugin");
        const auto parentPid = static_cast<std::int64_t> (std::atoll (optionFrom (args, "--parent-pid").c_str()));
        const auto windowed = ! hasFlag (args, "--no-window");

        if (regionPath.empty() || identifier.empty())
        {
            std::fprintf (stderr, "wfg plugin-editor: --region=<path> and --plugin=<identifier> are required\n");
            return 2;
        }

        const juce::File regionFile { juce::String (regionPath) };
        juce::MemoryMappedFile mapping (regionFile, juce::MemoryMappedFile::readWrite, false);

        if (mapping.getData() == nullptr || mapping.getSize() < editor::regionBytes())
        {
            std::fprintf (stderr, "wfg plugin-editor: could not map %s\n", regionPath.c_str());
            return 3;
        }

        auto& region = *static_cast<editor::Region*> (mapping.getData());

        if (! editor::looksValid (region))
        {
            std::fprintf (stderr, "wfg plugin-editor: the region at %s is not laid out as this build expects\n",
                          regionPath.c_str());
            return 3;
        }

        /*  JUCE up for the whole of the helper's life, on this thread, which
            is therefore the message thread: where the plugin is made, where
            its window lives, and where every pass below runs. */
        juce::ScopedJuceInitialiser_GUI juceForTheHelper;

       #if JUCE_MAC
        juce::initialiseNSApplication();

        /*  NO DOCK ICON, but a process that may own the key window: the
            plugin's window is part of Go.dot, not a second application. */
        if (windowed)
            juce::Process::setDockIconVisible (false);
       #endif

        const auto began = std::chrono::steady_clock::now();
        const auto sampleRate = static_cast<double> (numberFrom (args, "--sample-rate", 48000));
        const auto blockSize = numberFrom (args, "--block-size", 512);
        const auto channels = numberFrom (args, "--channels", 2);
        auto name = juce::String::fromUTF8 (optionFrom (args, "--name").c_str());

        juce::AudioPluginFormatManager manager;
        std::unique_ptr<juce::AudioProcessor> processor;
        std::string problem;

        if (identifier == Catalogue::testGainIdentifier())
        {
            processor = std::make_unique<TestGainProcessor>();
        }
        else
        {
            juce::addDefaultFormatsToManager (manager);
            juce::PluginDescription description;

            if (! readDescription (optionFrom (args, "--description"), description, problem))
            {
                reportFailure (region, problem);
                return 4;
            }

            if (description.createIdentifierString().toStdString() != identifier)
            {
                reportFailure (region, "the description does not name " + identifier);
                return 4;
            }

            const auto preset = optionFrom (args, "--preset");
            auto instance = makeInsertInstance (manager, description, channels, sampleRate, blockSize,
                                                preset.empty() ? juce::File() : juce::File (juce::String (preset)),
                                                problem);

            if (instance == nullptr)
            {
                reportFailure (region, problem);
                return 4;
            }

            processor = std::move (instance);
        }

        if (name.isEmpty())
            name = processor->getName();

        const auto micros = std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now() - began);
        region.loadMicros.store (static_cast<std::uint32_t> (std::min<long long> (micros.count(), 0xffffffffLL)),
                                 std::memory_order_relaxed);

        {
            Session session (region, *processor, parentPid, windowed, name);
            region.ready.store (1, std::memory_order_release);

            session.startTimer (10);
            juce::MessageManager::getInstance()->runDispatchLoop();
        }

        processor->releaseResources();
        processor.reset();
        return 0;
    }

    bool runPluginEditorIfAsked (int argc, char** argv, int& exitCode)
    {
        if (argc < 2 || std::strcmp (argv[1], pluginEditorVerb) != 0)
            return false;

        std::vector<std::string> args;

        for (int i = 2; i < argc; ++i)
            args.emplace_back (argv[i]);

        exitCode = runPluginEditor (args);
        return true;
    }
}
