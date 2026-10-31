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

#include <wfg/client/ui/TouchPanels.h>

#if JUCE_MAC
 #include <wfg/client/model/Pointers.h>
 #include <wfg/client/model/Surfaces.h>
 #include <wfg/client/ui/Look.h>
 #include <wfg/client/ui/SurfacePanelComponent.h>
 #include <spatcore/controllers/touch/TouchInjector.h>
 #include <spatcore/controllers/touch/macos/MacHidTouchManager.h>

 #include <algorithm>
 #include <cmath>
 #include <iostream>
 #include <set>
 #include <string>
 #include <vector>
#endif

namespace wfg::client::ui
{
   #if JUCE_MAC
    namespace
    {
        using Manager = spatcore::controllers::MacHidTouchManager;

        //==========================================================================
        /*  "TOUCH THE CIRCLE ON THIS SCREEN" (BFG; WFS-DIY's TouchIdentifySession,
            in Go.dot's look). A panel cannot say which display it lies on, so
            every display shows a circle in a place of its own, left to right in
            the order the displays are arranged, and while it runs the reader
            reports where a finger lands instead of routing it: where it lands on
            its panel names the circle it aimed at. Ends when every panel
            plugged in is placed, on a click or Esc, or after thirty seconds. */
        class Identify final : private juce::Timer
        {
        public:
            Identify (Manager& managerToUse, const model::Theme& themeToUse, std::function<void()> finished)
                : manager (managerToUse), theme (themeToUse), onFinished (std::move (finished))
            {
                displays = Manager::getDisplays();
                std::sort (displays.begin(), displays.end(),
                           [] (const auto& a, const auto& b) { return a.bounds.getX() < b.bounds.getX(); });

                for (std::size_t at = 0; at < displays.size(); ++at)
                {
                    const auto across = (static_cast<float> (at) + 0.5f) / static_cast<float> (displays.size());
                    auto overlay = std::make_unique<Overlay> (*this, across);
                    overlay->setBounds (displays[at].bounds);
                    overlay->addToDesktop (juce::ComponentPeer::windowIsTemporary);
                    overlay->setAlwaysOnTop (true);
                    overlay->setVisible (true);
                    overlays.push_back (std::move (overlay));
                }

                if (! overlays.empty())
                    overlays.front()->grabKeyboardFocus();

                manager.onIdentifyTouch = [this] (const juce::String& key, float x, float) { touched (key, x); };
                startTimer (30000);
            }

            ~Identify() override
            {
                manager.onIdentifyTouch = nullptr;
            }

        private:
            struct Overlay final : public juce::Component
            {
                Overlay (Identify& ownerToUse, float acrossToUse) : owner (ownerToUse), across (acrossToUse)
                {
                    setWantsKeyboardFocus (true);
                }

                void paint (juce::Graphics& g) override
                {
                    const auto& look = owner.theme;
                    g.fillAll (Look::colour (look, "ground").withAlpha (0.9f));

                    const auto area = getLocalBounds().toFloat();
                    const auto centre = juce::Point<float> (area.getX() + across * area.getWidth(), area.getCentreY());
                    const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.06f;

                    g.setColour (Look::colour (look, "accent"));
                    g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre), 4.0f);
                    g.fillEllipse (juce::Rectangle<float> (radius * 0.5f, radius * 0.5f).withCentre (centre));

                    g.setColour (Look::colour (look, "ink"));
                    g.setFont (Look::font (look, juce::jmax (18.0f, area.getHeight() * 0.03f)));
                    g.drawText ("Touch the circle on this screen",
                                juce::Rectangle<float> (area.getWidth(), radius * 1.5f)
                                    .withCentre ({ area.getCentreX(), centre.y - radius * 2.2f }),
                                juce::Justification::centred);

                    g.setColour (Look::colour (look, "ink-dim"));
                    g.setFont (Look::font (look, juce::jmax (14.0f, area.getHeight() * 0.018f)));
                    g.drawText ("Go.dot is finding which touch screen covers which display. A click or Esc stops.",
                                area.withTrimmedTop (area.getHeight() * 0.88f), juce::Justification::centred);

                    if (! placed.isEmpty())
                    {
                        g.setColour (Look::colour (look, "ink"));
                        g.setFont (Look::font (look, juce::jmax (18.0f, area.getHeight() * 0.025f)));
                        g.drawText ("This screen: " + placed.joinIntoString (", "),
                                    juce::Rectangle<float> (area.getWidth(), radius * 1.5f)
                                        .withCentre ({ area.getCentreX(), centre.y + radius * 2.2f }),
                                    juce::Justification::centred);
                    }
                }

                void mouseDown (const juce::MouseEvent&) override  { owner.finishIn (0); }

                bool keyPressed (const juce::KeyPress& key) override
                {
                    if (key == juce::KeyPress::escapeKey)
                        owner.finishIn (0);

                    return true;
                }

                Identify& owner;
                float across;
                juce::StringArray placed;
            };

            void touched (const juce::String& key, float panelX)
            {
                if (displays.empty() || finishing)
                    return;

                std::size_t best = 0;

                for (std::size_t at = 1; at < overlays.size(); ++at)
                    if (std::abs (panelX - overlays[at]->across) < std::abs (panelX - overlays[best]->across))
                        best = at;

                auto name = key;
                auto known = false;

                for (const auto& panel : manager.getPanels())
                    if (panel.key == key)
                    {
                        auto settings = panel.settings;
                        settings.displayId = displays[best].id;
                        settings.displayIndex = -1;
                        manager.setPanelSettings (key, settings);
                        name = panel.name.isNotEmpty() ? panel.name : key;
                        known = true;
                    }

                if (! known)
                    return;

                placedPanels.insert (key);
                overlays[best]->placed.addIfNotAlreadyThere (name);
                overlays[best]->repaint();

                const auto panels = manager.getPanels();
                const auto plugged = std::count_if (panels.begin(), panels.end(),
                                                    [] (const auto& panel) { return panel.connected; });

                //  Long enough to read which screen it took.
                if (static_cast<long> (placedPanels.size()) >= static_cast<long> (plugged))
                    finishIn (900);
            }

            void finishIn (int milliseconds)
            {
                finishing = true;
                manager.onIdentifyTouch = nullptr;
                startTimer (juce::jmax (1, milliseconds));
            }

            void timerCallback() override
            {
                stopTimer();
                manager.onIdentifyTouch = nullptr;

                //  The owner destroys this, never from inside its own callback.
                juce::MessageManager::callAsync ([done = onFinished] { if (done) done(); });
            }

            Manager& manager;
            const model::Theme& theme;
            std::function<void()> onFinished;
            std::vector<Manager::DisplayInfo> displays;
            std::vector<std::unique_ptr<Overlay>> overlays;
            std::set<juce::String> placedPanels;
            bool finishing = false;
        };

        //==========================================================================
        /*  THE SELF-TEST (`WFG_TEST_TOUCH_INJECT`): a desk of two sampler strips
            in a window of its own, on top, and two fingers injected on its two
            pads the way the panel reader injects them - each must be its own
            touch source and its own hand: press A, press B, release A, release
            B. Then a line on stderr. */
        class SelfTest final : private juce::Timer
        {
        public:
            SelfTest (const model::Theme& theme, std::function<void()> closeAfter)
                : close (std::move (closeAfter))
            {
                desk = std::make_unique<SurfacePanelComponent> (theme, [this] (Event event)
                {
                    said.push_back (event.command + " " + (event.args.empty() ? std::string {} : event.args[0].getString()));
                });
                desk->setSize (480, 360);

                model::SurfaceRow surface;
                surface.id = "TCHSRF01";
                surface.name = "Touch self-test";
                surface.profile = "virtual";
                surface.strips = 2;
                surface.connected = true;

                std::vector<model::StripRow> strips;

                for (int at = 0; at < 2; ++at)
                {
                    model::StripRow strip;
                    strip.id = "TCHSTP0" + std::to_string (at + 1);
                    strip.surface = surface.id;
                    strip.index = at;
                    strip.role = "sampler";
                    strip.endpoint = "absolute";
                    strip.word = "armed";
                    strips.push_back (strip);
                }

                desk->show ({ surface }, strips);
                desk->setAlwaysOnTop (true);
                desk->addToDesktop (juce::ComponentPeer::windowHasTitleBar);
                desk->centreWithSize (480, 360);
                desk->setVisible (true);
                desk->toFront (true);

                //  A second for the window to be on the screen and frontmost.
                startTimer (1000);
            }

        private:
            void timerCallback() override
            {
                stopTimer();

                const auto onScreen = [this] (juce::Point<float> local)
                {
                    return desk->localPointToGlobal (local);
                };

                const auto a = onScreen (desk->padCentre (0));
                const auto b = onScreen (desk->padCentre (1));

                injector.touchDown ("selftest", 1, a);
                injector.touchDown ("selftest", 2, b);

                auto sources = 0;

                for (const auto& each : juce::Desktop::getInstance().getMouseSources())
                    sources += each.isTouch() && each.isDragging() ? 1 : 0;

                injector.touchUp ("selftest", 1, a);
                injector.touchUp ("selftest", 2, b);

                const std::vector<std::string> wanted { "strip.press TCHSTP01", "strip.press TCHSTP02",
                                                        "strip.release TCHSTP01", "strip.release TCHSTP02" };

                if (injector.isTouchDropped())
                    std::cerr << "touch self-test: FAILED - JUCE dropped the touch: is patches/JUCE-local on?" << std::endl;
                else if (said != wanted || sources != 2)
                {
                    std::cerr << "touch self-test: FAILED - " << sources << " touch sources down, the desk said:";

                    for (const auto& line : said)
                        std::cerr << " [" << line << "]";

                    std::cerr << std::endl;
                }
                else
                {
                    std::cerr << "touch self-test: passed - two fingers, two touch sources, two hands" << std::endl;
                }

                desk->removeFromDesktop();

                if (close)
                    juce::MessageManager::callAsync ([done = close] { done(); });
            }

            std::function<void()> close;
            std::unique_ptr<SurfacePanelComponent> desk;
            spatcore::controllers::TouchInjector injector;
            std::vector<std::string> said;
        };

        //==========================================================================
        class MacTouchPanels final : public TouchPanels
        {
        public:
            MacTouchPanels (const model::Theme& themeToUse, std::function<void (const juce::String&)> sayToUse,
                            std::function<void()> closeToUse)
                : theme (themeToUse), say (std::move (sayToUse))
            {
                manager = std::make_unique<Manager> ("Go.dot");

                //  On the log, as the engine's own lines are: never periodic, only what happened.
                manager->onLog = [] (const juce::String& line) { std::cerr << "touch: " << line << std::endl; };
                manager->traceContacts = juce::SystemStats::getEnvironmentVariable ("WFG_TOUCH_TRACE", {}).isNotEmpty();

                /*  A PANEL ARRIVING, GOING OR CHANGING: with two displays or more and
                    a panel nobody has placed, the circles once a panel and session,
                    a moment after things settle; and a sentence when macOS gives no
                    touches or JUCE drops them. */
                listening = manager->addChangeListener ([this] { changed(); });

                if (const auto test = juce::SystemStats::getEnvironmentVariable ("WFG_TEST_TOUCH_INJECT", {});
                    test.isNotEmpty())
                    selfTest = std::make_unique<SelfTest> (theme, test == "exit" ? std::move (closeToUse)
                                                                                 : std::function<void()> {});
            }

            ~MacTouchPanels() override
            {
                //  The circles first, then the reader: fingers lift while the windows exist.
                identify.reset();
                selfTest.reset();

                if (manager != nullptr)
                    manager->removeChangeListener (listening);

                manager.reset();
            }

        private:
            void changed()
            {
                if (manager == nullptr)
                    return;

                auto plugged = false;

                for (const auto& panel : manager->getPanels())
                    plugged = plugged || panel.connected;

                if (plugged && Manager::getInputMonitoringAccess() == Manager::Access::denied && ! saidAccess)
                {
                    saidAccess = true;
                    say ("A touch screen is plugged in, but macOS gives Go.dot none of its touches: allow Go.dot in "
                         "System Settings, Privacy & Security, Input Monitoring, then open it again.");
                }

                if (manager->isTouchDropped() && ! saidDropped)
                {
                    saidDropped = true;
                    say ("A finger was dropped: this Go.dot was built without patches/JUCE-local.");
                }

                //  A moment after the panel settles, as WFS-DIY waits.
                juce::Timer::callAfterDelay (1500, [this, token = std::weak_ptr<bool> (alive)]
                {
                    if (! token.expired())
                        offerIdentify();
                });
            }

            void offerIdentify()
            {
                if (manager == nullptr || identify != nullptr || Manager::getDisplays().size() < 2)
                    return;

                /*  WHAT ANOTHER READER HAS PLACED (BFH): a screen identified in
                    WFS-DIY is placed here too, and is not offered again. */
                manager->reloadSettings();

                for (const auto& panel : manager->getPanels())
                    if (panel.connected && panel.problem.isEmpty() && ! manager->hasDisplay (panel.settings)
                          && offered.insert (panel.key).second)
                    {
                        identify = std::make_unique<Identify> (*manager, theme, [this, token = std::weak_ptr<bool> (alive)]
                        {
                            if (! token.expired())
                                identify.reset();
                        });
                        return;
                    }
            }

            const model::Theme& theme;
            std::function<void (const juce::String&)> say;
            std::unique_ptr<Manager> manager;
            std::unique_ptr<Identify> identify;
            std::unique_ptr<SelfTest> selfTest;
            std::set<juce::String> offered;
            int listening = 0;
            bool saidAccess = false, saidDropped = false;
            std::shared_ptr<bool> alive = std::make_shared<bool> (true);
        };
    }
   #endif

    std::unique_ptr<TouchPanels> makeTouchPanels (const model::Theme& theme,
                                                  std::function<void (const juce::String&)> say,
                                                  std::function<void()> close)
    {
       #if JUCE_MAC
        return std::make_unique<MacTouchPanels> (theme, std::move (say), std::move (close));
       #else
        juce::ignoreUnused (theme, say, close);
        return nullptr;
       #endif
    }
}
