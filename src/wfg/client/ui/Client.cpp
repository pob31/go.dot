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
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/Media.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/ui/MainWindow.h>
#include <wfg/client/ui/Shell.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

                ui::TransportComponent::Actions actions;

                /*  ONE GESTURE, ONE COMMAND, and the command is the model's to
                    name (model/Gestures.h): this file knows which button was
                    pressed and nothing about what it means. `submit` takes any
                    thread and never blocks, so a click returns at once and the
                    tick thread applies it in arrival order like a datagram. */
                actions.go              = [this] { send (gesture::go()); };
                actions.undo            = [this] { send (gesture::undo()); };
                actions.redo            = [this] { send (gesture::redo()); };
                actions.save            = [this] { send (gesture::save()); };
                actions.revert          = [this] { send (gesture::revert()); };
                actions.recover         = [this] { send (gesture::recover()); };
                actions.discardRecovery = [this] { send (gesture::discardRecovery()); };
                actions.setLocked       = [this] (bool locked) { send (gesture::setLocked (locked)); };
                actions.reloadTheme     = [this] { reloadTheme(); };

                ui::CueListComponent::Actions listActions;

                listActions.standbyNext     = [this] { send (gesture::standbyNext()); };
                listActions.standbyPrevious = [this] { send (gesture::standbyPrevious()); };
                listActions.park            = [this] (const std::string& id)
                                              { send (gesture::park (id)); };

                /*  The list has no line of its own to speak on, and the
                    transport's foot is where every other sentence about this
                    session already lands - a theme's refusal, the show's
                    warnings. One place to look is the point. */
                listActions.say             = [this] (const juce::String& sentence)
                                              { shell->transport.setNotice (sentence); };
                listActions.go              = [this] { send (gesture::go()); };

                /*  A FOLD IS THIS CLIENT'S AND NEVER THE ENGINE'S (§14.1): what
                    somebody collapsed on their screen is not something the show
                    decided, so it goes to the model and nowhere near `submit`. */
                listActions.fold            = [this] (const std::string& key)
                                              { show.toggle (key); };
                listActions.pick            = [this] (const std::string& id) { picked = id; };

                listActions.importMedia     = [this] (const std::string& parent, int index,
                                                      const juce::StringArray& files)
                                              { importMedia (parent, index, files); };
                listActions.linkMedia       = [this] (const std::string& cueId,
                                                      const juce::String& file)
                                              { linkMedia (cueId, file); };

                ui::RunPaneComponent::Actions runActions;

                runActions.kill = [this] (const std::string& id) { send (gesture::kill (id)); };

                ui::InspectorComponent::Actions inspectorActions;

                /*  ONE COMMITTED FIELD IS ONE `node.set`, carrying the address
                    the NODE gave rather than one assembled here - which is what
                    keeps this inspector generic and keeps the command reachable
                    from the page's (§14.16, rule 3). */
                inspectorActions.set = [this] (const std::string& address, const std::string& text)
                                       { send (gesture::setNode (address, text)); };

                /*  CLOSING THE PANEL IS PICKING NOTHING, which is client state
                    like the folds and never reaches the engine. */
                inspectorActions.close = [this] { picked.clear(); };

                auto content = std::make_unique<ui::Shell> (theme, std::move (actions),
                                                            std::move (listActions),
                                                            std::move (runActions),
                                                            std::move (inspectorActions));
                shell = content.get();

                window = std::make_unique<ui::MainWindow> (titleFor (""),
                                                            ui::Look::colour (theme, "ground"),
                                                            [this] { closeRequested(); });
                window->setContentOwned (content.release(), false);
                window->centreWithSize (juce::roundToInt (34 * theme.row * theme.type),
                                        juce::roundToInt (26 * theme.row * theme.type));

                pass();     // the first reading, before the window is seen

                window->setVisible (true);
                shell->grabKeyboardFocus();

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
                shell->transport.setNotice (juce::String (sentence));
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

                /*  Held for the gestures that answer a hand rather than a
                    tick: a drop arrives between passes and has no snapshot of
                    its own, and taking a second one would be a second call
                    site. This is the same pointer, kept until the next pass
                    replaces it. */
                latest = snapshot;

                if (reading.show != last.show)
                    window->setName (titleFor (reading.show));

                shell->transport.show (reading);

                /*  THE TWO RATES OUT OF ONE SNAPSHOT. The model walks the show
                    only when `/godot/document/revision` has moved or the
                    focused list has changed (M0); the pointer is read every
                    pass and costs two rows a repaint. Both from the same
                    pointer copy, so the list and the strip can never disagree
                    about which tick they are drawing. */
                show.refresh (*snapshot, reading.listId);
                shell->cues.show (show, reading.standbyId, picked);

                /*  And the present tense, read fresh: runs have no revision to
                    key on, because a run is not a decision anybody recorded. */
                shell->runs.show (model::readRuns (*snapshot));

                //  And any import still waiting for the cue its create made.
                finishImports (*snapshot, reading.revision);

                /*  AND THE ONE CUE SOMEBODY ASKED ABOUT. The panel is built
                    from the tree when the picked cue changes and its values
                    updated otherwise, so typing is never overwritten by a
                    poll - which is the one thing a panel like this must not
                    do. */
                shell->setInspecting (! picked.empty());

                if (! picked.empty())
                    shell->inspector.show (model::inspect (*snapshot, picked));

                last = reading;
            }

            void timerCallback() override
            {
                pass();
            }

            /*  THE ONE WAY OUT OF THIS CLIENT INTO THE SHOW (§14.16, rule 1).
                `submit` answers false when the queue was full and an older
                entry was dropped - which is not a rejection and not this
                window's business: a refused command comes back as
                `/godot/engine/lastError` on the next pass, where the operator
                reads it, and a full queue at four thousand entries is a
                machine in trouble that a dialogue here would not help. */
            void send (Event event)
            {
                host.engine.submit (std::move (event));
            }

            //======================================================================
            /*  MEDIA, which is decision Y and the reason this client is
                compiled rather than served: a browser is handed a dropped
                file's name and bytes and never its path, so it can only ever
                offer to upload one. This is handed the path.

                AN IMPORT IS THREE THINGS AND ONLY TWO OF THEM ARE THE SHOW'S
                (§14.16): the bytes arrive in the bundle's `media/`, which is a
                fact about a disk and is done here; then a cue is created and
                the cue names the file, which are decisions and go through the
                one door as `cue.create` and `node.set`.

                NOTHING BELOW READS THE TREE FOR ITSELF. `latest` is the
                pointer the timer copied, so rule 2's single call site stands:
                a drop is answered out of the same snapshot the window is
                currently drawing, which is also the only way a confirmation
                dialogue can quote a value the operator can actually see. */

            /** The bundle's media folder, or nothing when no show is open. */
            juce::File mediaFolder() const
            {
                if (latest == nullptr)
                    return {};

                const auto path = model::text (*latest, "/godot/document/path");

                return path.empty() ? juce::File()
                                    : juce::File (juce::String (path)).getChildFile ("media");
            }

            /*  A container's members, whichever kind of container it is. A
                list and a group both hold an `order` and hold it at different
                addresses, which is the document's own shape rather than an
                inconsistency: the two are asked in turn. */
            std::string orderOf (const std::string& container) const
            {
                if (latest == nullptr)
                    return {};

                const auto inList = model::text (*latest, "/godot/list/" + container + "/order");

                return inList.empty() ? model::text (*latest, "/godot/cue/" + container + "/order")
                                      : inList;
            }

            /*  COPYING THE BYTES IN. Answers the name the cue should carry, or
                nothing when the copy did not happen - and then NOTHING is
                sent, because a cue naming a file that is not there is worse
                than no cue at all. */
            std::string copyIn (const juce::File& source, bool replaceExisting)
            {
                const auto folder = mediaFolder();

                if (folder == juce::File() || ! source.existsAsFile())
                    return {};

                folder.createDirectory();

                const auto target = folder.getChildFile (source.getFileName());

                if (target == source)
                    return model::mediaNameFor (source.getFileName().toStdString());

                if (target.existsAsFile() && ! replaceExisting)
                    return {};

                return source.copyFileTo (target)
                         ? model::mediaNameFor (target.getFileName().toStdString())
                         : std::string {};
            }

            /*  A SHOW THAT DECLARES NO AUDIO TRACKS CANNOT PLAY MEDIA, and
                the operator should hear that from the drop rather than from
                the GO. `Audio/@tracks` is the polyphony ceiling and the show
                states it - PRD §3.9b, a width is stated and never inferred -
                so a show sitting at zero has nowhere to put a sound, and every
                run of an imported cue ends `no-track`.

                IT IS NOT A REASON TO REFUSE THE DROP. Making the cue is a
                decision somebody is entitled to take, and setting the ceiling
                afterwards is the obvious next thing they will do. So the
                import happens and the sentence says what is missing, which is
                the difference between this and the lock. */
            juce::String silenceWarning() const
            {
                if (latest == nullptr || model::text (*latest, "/godot/audio/tracks") != "0")
                    return {};

                return "; this show declares no audio tracks, so nothing will sound yet";
            }

            void finishLink (const std::string& cueId, const juce::File& source, bool replacing)
            {
                const auto name = copyIn (source, replacing);

                if (name.empty())
                {
                    shell->transport.setNotice ("could not copy " + source.getFileName()
                                                  + " into the show");
                    return;
                }

                send (gesture::setNode ("/godot/cue/" + cueId + "/file", name));

                if (const auto warning = silenceWarning(); ! warning.isEmpty())
                    shell->transport.setNotice (juce::String (name) + " is on the cue" + warning);
            }

            /*  A FILE DROPPED ONTO A MEDIA CUE NAMES THAT CUE'S FILE, and asks
                first when there is something to lose - the author's own
                condition on this gesture. Two different things can be at stake
                and the question says which: the cue's current choice, and
                bytes of the same name already in the bundle. With neither at
                stake there is no question, because a dialogue nobody needs is
                one people learn to dismiss unread. */
            void linkMedia (const std::string& cueId, const juce::String& path)
            {
                if (refusedWhileLocked())
                    return;

                const juce::File source { path };

                const auto already = latest == nullptr
                                       ? std::string {}
                                       : model::text (*latest, "/godot/cue/" + cueId + "/file");

                const auto target = mediaFolder().getChildFile (source.getFileName());
                const auto wouldOverwrite = target.existsAsFile() && target != source;

                if (already.empty() && ! wouldOverwrite)
                {
                    finishLink (cueId, source, false);
                    return;
                }

                juce::String question;

                if (! already.empty())
                    question << "This cue plays " << juce::String (already) << ".";

                if (wouldOverwrite)
                    question << (question.isEmpty() ? "" : "\n\n")
                             << "The show already has a file called " << source.getFileName()
                             << ", and it is not this one.";

                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                .withTitle ("Use " + source.getFileName() + "?")
                                                .withMessage (question)
                                                .withButton ("Replace")
                                                .withButton ("Leave it")
                                                .withAssociatedComponent (window.get()),
                                              [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                               this, cueId, source] (int result)
                                              {
                                                  if (result == 1 && safe != nullptr)
                                                      finishLink (cueId, source, true);
                                              });
            }

            /*  AND FILES DROPPED ANYWHERE ELSE MAKE CUES: one per file, in the
                order they were dropped, each named after its own.

                THE INDEX IS RESOLVED HERE AND NOT WHERE THE HAND LET GO. The
                pane knows the rows it drew; this knows the members the show
                has, and the two are different numbers. An index past the end -
                which -1 asks for outright - is pinned to the member count,
                after which each file is exactly one further along. Leaving it
                past the end would have worked for ONE file and quietly
                mismatched cue and file for two, because every create beyond
                the end lands at the same place.

                THE CREATE AND THE NAMING ARE TWO COMMANDS AND A TICK APART,
                because `cue.create` is applied on the tick thread and this
                window sees the result only in a later published tree - and the
                identifier is the engine's to draw, never a client's. So the
                import is remembered and finished on a later pass. */
            void importMedia (const std::string& parent, int index,
                              const juce::StringArray& files)
            {
                if (refusedWhileLocked())
                    return;

                if (parent.empty())
                {
                    shell->transport.setNotice ("no list to import into");
                    return;
                }

                const auto members = static_cast<int> (model::words (orderOf (parent)).size());
                auto at = index < 0 ? members : juce::jlimit (0, members, index);
                auto made = 0;

                for (const auto& path : files)
                {
                    const juce::File source { path };
                    const auto name = copyIn (source, false);

                    if (name.empty())
                    {
                        shell->transport.setNotice ("could not copy " + source.getFileName()
                                                      + " into the show");
                        continue;
                    }

                    const auto cueName = model::cueNameFor (name);

                    send (gesture::createCue (parent, at, "media", cueName));
                    pending.push_back ({ parent, at, cueName, name, last.revision, 0 });
                    ++at;
                    ++made;
                }

                if (const auto warning = silenceWarning(); made > 0 && ! warning.isEmpty())
                    shell->transport.setNotice (juce::String (made)
                                                  + (made == 1 ? " cue" : " cues") + warning);
            }

            /*  THE OTHER HALF OF AN IMPORT, run every pass: find the cue the
                create made and give it its file. The cue is CHECKED before it
                is written (model::madeByImport), and an import that never
                finds its cue is given up on with a sentence rather than
                waiting for ever - a create refused for a reason this window
                did not foresee must not leave a job in the queue behind it. */
            void finishImports (const tree::TreeSnapshot& snapshot, std::uint64_t revision)
            {
                if (pending.empty())
                    return;

                std::vector<model::Import> waiting;

                for (auto& job : pending)
                {
                    const auto id = revision > job.askedAt
                                      ? model::createdAt (orderOf (job.parent), job.index)
                                      : std::string {};

                    if (! id.empty()
                          && model::madeByImport (job,
                                                  model::text (snapshot, "/godot/cue/" + id + "/kind"),
                                                  model::text (snapshot, "/godot/cue/" + id + "/name"),
                                                  model::text (snapshot, "/godot/cue/" + id + "/file")))
                    {
                        send (gesture::setNode ("/godot/cue/" + id + "/file", job.mediaName));
                        continue;
                    }

                    if (++job.waited < model::importPatience)
                        waiting.push_back (job);
                    else
                        shell->transport.setNotice (juce::String (job.mediaName)
                                                      + " is in the show, but the cue for it was refused");
                }

                pending = std::move (waiting);
            }

            /*  A CLIENT DOES NOT OFFER A GESTURE IT COULD HAVE KNOWN WOULD BE
                REFUSED. Under the lock the engine turns down a create and a
                write to a show value alike, so a drop is answered here - and
                before anything is copied, since bytes left in `media/` for a
                cue that was never made are litter nobody asked for. */
            bool refusedWhileLocked()
            {
                /*  `isYes`, not a truth test: a node the engine has not
                    published yet reads `unsaid`, which is not the same as a
                    show that answered no, and only an answered yes refuses. */
                if (! model::isYes (last.locked))
                    return false;

                shell->transport.setNotice ("the show is locked: unlock it to bring media in");
                return true;
            }

            void reloadTheme()
            {
                if (themeFile == juce::File())
                {
                    shell->transport.setNotice ("no theme file: start with --theme=<file> to edit the look");
                    return;
                }

                model::Theme next = theme;

                if (const auto refused = next.apply (themeFile.loadFileAsString().toStdString());
                    ! refused.empty())
                {
                    shell->transport.setNotice (juce::String (refused));
                    return;
                }

                theme = next;
                look.apply (theme);
                shell->applyTheme (theme);
                shell->transport.setNotice ("theme read from " + themeFile.getFileName());
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

                /*  `isYes`, not "not no": a show whose lock the engine has not
                    published yet is not an unlocked show, and the safe reading
                    for a gesture is the one that offers less. Here that means
                    a window whose engine has said nothing still closes - the
                    refusal is for a show KNOWN to be in show mode. */
                if (model::isYes (last.locked))
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

            /*  So that a disabled button can say WHICH thing is unavailable
                (§4.8): without one, setTooltip is a value nothing reads. */
            juce::TooltipWindow tooltips { nullptr, 700 };
            std::unique_ptr<ui::MainWindow> window;
            ui::Shell* shell = nullptr;                     // owned by the window

            /*  The rows, cached against the show's revision. Declared after the
                window only because nothing in it points back: it is plain data
                the timer hands to the list. */
            model::ShowModel show;

            /*  WHICH CUE THE INSPECTOR IS ABOUT. Client state, like the folds:
                what somebody is looking at is not something the show decided,
                and §14.1 keeps it out of the document for that reason. */
            std::string picked;

            /*  THE POINTER THE LAST PASS DREW. Null until the first one, which
                is why every reader above checks. */
            std::shared_ptr<const tree::TreeSnapshot> latest;

            /** Files copied in, cues asked for, and the naming still to do. */
            std::vector<model::Import> pending;
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
