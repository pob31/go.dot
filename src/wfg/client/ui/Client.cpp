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

/*
    The compiled client: a window over the engine it runs inside.

    THREE RULES, from namespace draft §14.16, and where each one lives here:

      1. The show changes only through Engine::submit, origin `window`. Every
         gesture ends in `host.engine.submit (gesture::…())` and nothing else
         in this library can reach the show - it holds no ShowDocument and the
         boundary gate reads the source to say so.
      2. The client reads only ParameterTree::snapshot(), and it does so in
         ONE place: timerCallback, below. Everything on screen is derived from
         that one pointer copy per pass, on the message thread, with no lock
         held and no way to see a half-applied tick. The tick thread never
         touches the window; there is no after-tick hook and there never will
         be one.
      3. Every command the desktop sends stays reachable from the page's
         generic inspector - `go` is, trivially.

    WHAT CANNOT BE TESTED, and is written down as such rather than implied:
    that the window opens at all; the layout; colour on screen; hit-testing;
    keyboard focus; the close dialogue; timing; and the shutdown order. The
    first person to find a broken window is the author, on a build. What can
    be tested lives in model/ and is, in tests/ClientTests.cpp.

    SHUTDOWN, in order, because the order is the whole of what makes it safe:
    the console destroys this after the loop has returned and the clock has
    stopped, so no timer fires and no tick is in flight. The destructor stops
    the timer anyway, takes the look-and-feel back off the Desktop, then lets
    the window go, then the look-and-feel - which is declared first so it dies
    last, after every component that could still ask it for a colour.
*/

#include <wfg/client/Client.h>

#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/ui/MainWindow.h>
#include <wfg/client/ui/TransportComponent.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace wfg::client
{
    namespace
    {
        class Window final : public wfg::Client,
                             private juce::Timer
        {
        public:
            Window (const ClientHost& hostToUse, model::Theme themeToUse, juce::File themeFileToWatch)
                : host (hostToUse), themeFile (std::move (themeFileToWatch)),
                  theme (std::move (themeToUse)), look (theme)
            {
                juce::Desktop::getInstance().setDefaultLookAndFeel (&look);

                auto content = std::make_unique<ui::TransportComponent> (
                    theme, ui::TransportComponent::Actions { [this] { go(); },
                                                             [this] { reloadTheme(); } });
                transport = content.get();

                window = std::make_unique<ui::MainWindow> (titleFor (""),
                                                            ui::Look::colour (theme, "ground"),
                                                            [this] { closeRequested(); });
                window->setContentOwned (content.release(), false);
                window->centreWithSize (juce::roundToInt (30 * theme.row * theme.type),
                                        juce::roundToInt (7 * theme.row * theme.type));

                pass();     // the first reading, before the window is seen

                window->setVisible (true);
                transport->grabKeyboardFocus();

                startTimerHz (juce::jmax (1, juce::roundToInt (theme.refreshHz)));
            }

            ~Window() override
            {
                stopTimer();
                juce::Desktop::getInstance().setDefaultLookAndFeel (nullptr);
                window.reset();
            }

            /** For the factory, when a theme file was refused at start: shown where the author is looking. */
            void notice (const std::string& sentence)
            {
                transport->setNotice (juce::String (sentence));
            }

        private:
            static juce::String titleFor (const std::string& show)
            {
                return show.empty() ? juce::String ("Go.dot") : "Go.dot — " + juce::String (show);
            }

            /*  RULE 2's ONE CALL SITE. A pointer copy, never null, and the
                snapshot is only ever swapped whole. */
            void pass()
            {
                const auto snapshot = host.parameters.snapshot();
                const auto reading = model::readTransport (*snapshot);

                if (reading.show != last.show)
                    window->setName (titleFor (reading.show));

                transport->show (reading);
                last = reading;
            }

            void timerCallback() override
            {
                pass();
            }

            void go()
            {
                host.engine.submit (gesture::go());
            }

            void reloadTheme()
            {
                if (themeFile == juce::File())
                {
                    transport->setNotice ("no theme file: start with --theme=<file> to edit the look");
                    return;
                }

                model::Theme next = theme;

                if (const auto refused = next.apply (themeFile.loadFileAsString().toStdString());
                    ! refused.empty())
                {
                    transport->setNotice (juce::String (refused));
                    return;
                }

                theme = next;
                look.apply (theme);
                transport->applyTheme (theme);
                transport->setNotice ("theme read from " + themeFile.getFileName());
                window->setBackgroundColour (ui::Look::colour (theme, "ground"));
                window->sendLookAndFeelChange();
                window->repaint();

                startTimerHz (juce::jmax (1, juce::roundToInt (theme.refreshHz)));
            }

            /*  THE CLOSE BUTTON ASKS, because in process a closed window is a
                stopped engine (§14.16's price of the answer). Locked means
                show mode, and show mode means the forearm on the screen in
                the dark: no dialogue with a "yes" in it is offered at all. The
                callbacks hold a SafePointer, never `this` - the box outlives
                nothing, but JUCE_MODAL_LOOPS_PERMITTED=0 means every box here
                is asynchronous and the rule is cheaper than the exception. */
            void closeRequested()
            {
                using Options = juce::MessageBoxOptions;

                if (last.locked)
                {
                    juce::AlertWindow::showAsync (Options()
                                                    .withIconType (juce::MessageBoxIconType::InfoIcon)
                                                    .withTitle ("The show is locked")
                                                    .withMessage ("Unlock it before closing the window.")
                                                    .withButton ("OK")
                                                    .withAssociatedComponent (window.get()),
                                                  [] (int) {});
                    return;
                }

                juce::AlertWindow::showAsync (Options()
                                                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                .withTitle ("Close the window and stop the engine?")
                                                .withMessage ("Running cues will stop. The page and the "
                                                              "terminal stay the other ways out.")
                                                .withButton ("Stop the engine")
                                                .withButton ("Keep running")
                                                .withAssociatedComponent (window.get()),
                                              [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                               quit = host.quit] (int result)
                                              {
                                                  // Two buttons: the first answers 1 (AlertWindow::show's table).
                                                  if (result == 1 && safe != nullptr && quit)
                                                      quit();
                                              });
            }

            const ClientHost host;
            const juce::File themeFile;
            model::Theme theme;
            model::TransportReading last;

            ui::Look look;                                  // before the window: destroyed after it
            std::unique_ptr<ui::MainWindow> window;
            ui::TransportComponent* transport = nullptr;    // owned by the window
        };
    }

    ClientFactory factory()
    {
        return [] (const ClientHost& host) -> std::unique_ptr<wfg::Client>
        {
           #if JUCE_MAC
            /*  An unbundled binary is a background process to macOS: no dock
                icon, and no keyboard focus for its windows. Both are asked for
                here, before the window exists. */
            juce::Process::setDockIconVisible (true);
            juce::Process::makeForegroundProcess();
           #endif

            model::Theme theme;
            std::string refused;
            juce::File themeFile;

            if (! host.themePath.empty())
            {
                themeFile = juce::File (juce::String (host.themePath));
                refused = theme.apply (themeFile.loadFileAsString().toStdString());

                if (! refused.empty())
                {
                    std::cerr << "wfg serve --window: " << refused << " - opening with the defaults"
                              << std::endl;
                    theme = model::Theme {};
                }
            }

            auto client = std::make_unique<Window> (host, theme, themeFile);

            if (! refused.empty())
                client->notice (refused);

            return client;
        };
    }
}
