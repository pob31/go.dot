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
#include <wfg/client/model/NewCue.h>
#include <wfg/client/model/Panic.h>
#include <wfg/client/model/Reorder.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/Selection.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/ui/MainWindow.h>
#include <wfg/client/ui/Shell.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_audio_formats/juce_audio_formats.h>
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
        /*  THE MENU'S ITEMS, one number each. A menu item is a gesture like a
            button, and ends in the same one command; the menu exists because
            some gestures are about which show is on screen at all rather than
            about this one (author, 2026-09-18: "I would put these in a menu
            in the top bar"). */
        enum MenuItem
        {
            menuNew = 1, menuOpen, menuSave, menuSaveAs, menuRevert,
            menuUndo, menuRedo, menuCopy, menuPaste, menuSelectAll, menuDeleteCue,
            menuLock
        };

        class Window final : public wfg::Client,
                             public juce::MenuBarModel,
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
                actions.panic           = [this] { panic(); };
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
                /*  WHAT IS PICKED IS THIS CLIENT'S (model/Selection.h): a
                    click, with shift or ctrl/⌘, over the rows as drawn. */
                listActions.pick            = [this] (const std::string& id, bool extend, bool toggle)
                                              {
                                                  if (id.empty())
                                                      selection.clear();
                                                  else
                                                      selection.click (id, extend, toggle, show.rows());
                                              };
                listActions.pickAll         = [this] { selection.all (show.rows()); };
                listActions.removeChosen    = [this] { removeChosen(); };

                listActions.importMedia     = [this] (const std::string& parent, int index,
                                                      const juce::StringArray& files)
                                              { importMedia (parent, index, files); };
                listActions.linkMedia       = [this] (const std::string& cueId,
                                                      const juce::String& file)
                                              { linkMedia (cueId, file); };

                /*  A ROW DRAGGED IN THE LIST: one `object.move`, or one write to
                    the aimed cue's target. The document holds the identifier;
                    the pane already has it. */
                listActions.move            = [this] (const std::string& id, const std::string& parent,
                                                      int index)
                                              {
                                                  if (! refusedWhileLocked())
                                                      send (gesture::moveObject (id, parent, index));
                                              };
                listActions.setTarget       = [this] (const std::string& aimed, const std::string& at)
                                              {
                                                  if (! refusedWhileLocked())
                                                      send (gesture::setNode ("/godot/cue/" + aimed + "/target", at));
                                              };

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
                inspectorActions.close = [this] { selection.clear(); };

                inspectorActions.chooseFile = [this] (const std::string& cueId)
                                              { chooseFile (cueId); };

                /*  A TARGET TYPED AS A NUMBER OR A NAME is resolved to the
                    identifier the document stores (model/Reorder.h), against
                    the rows this window is drawing. An empty field clears the
                    target; a name nobody has, or two cues have, writes nothing
                    and says so. */
                inspectorActions.setCueRef  = [this] (const std::string& address, const std::string& text)
                                              {
                                                  if (text.empty())
                                                  {
                                                      send (gesture::setNode (address, text));
                                                      return;
                                                  }

                                                  const auto id = model::resolveCueRef (text, show.rows());

                                                  if (id.empty())
                                                      shell->transport.setNotice ("no one cue is numbered or named "
                                                                                    + juce::String (text));
                                                  else
                                                      send (gesture::setNode (address, id));
                                              };

                /*  THE NEW-CUE ROW: one button per kind, one `cue.create` each,
                    landing where the pick says (model/NewCue.h). The bar
                    knows the kind; this knows the place. */
                ui::NewCueBarComponent::Actions newCueActions;

                newCueActions.create = [this] (const std::string& kind) { createCue (kind); };

                auto content = std::make_unique<ui::Shell> (theme, std::move (actions),
                                                            std::move (listActions),
                                                            std::move (runActions),
                                                            std::move (inspectorActions),
                                                            std::move (newCueActions));
                shell = content.get();

                window = std::make_unique<ui::MainWindow> (titleFor (""),
                                                            ui::Look::colour (theme, "ground"),
                                                            [this] { closeRequested(); });
                window->setContentOwned (content.release(), false);

                /*  THE MENU, in the window's own bar under its title - and on
                    the Mac at the top of the screen, where a menu lives. Its
                    keys are the classical ones (author, 2026-09-18), and each
                    key does exactly what its item does, enabled or not. */
                window->setMenuBar (this);
                shell->menuKeys = [this] (const juce::KeyPress& key)
                {
                    const auto item = menuItemForKey (key);

                    if (item == 0)
                        return false;

                    if (menuItemEnabled (static_cast<MenuItem> (item)))
                        menuItemSelected (item, 0);

                    return true;
                };
               #if JUCE_MAC
                juce::MenuBarModel::setMacMainMenu (this);
               #endif

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
               #if JUCE_MAC
                juce::MenuBarModel::setMacMainMenu (nullptr);
               #endif
                if (window != nullptr)
                    window->setMenuBar (nullptr);
                juce::Desktop::getInstance().setDefaultLookAndFeel (nullptr);
                window.reset();
            }

            //======================================================================
            //  The menu (juce::MenuBarModel)
            juce::StringArray getMenuBarNames() override
            {
                return { "File", "Edit", "Show" };
            }

            /*  THE KEY EACH ITEM IS PRINTED BESIDE, and the one it answers to:
                one table, so the menu cannot show a key the window ignores.
                Ctrl/⌘-S, -Z, -shift-Z and -Backspace are the two panes' first
                (Shell::keyPressed asks them before this), so they are printed
                here and answered there; the rest are answered here. */
            static juce::KeyPress keyFor (MenuItem item)
            {
                const auto mod = juce::ModifierKeys::commandModifier;
                const auto shift = juce::ModifierKeys::shiftModifier;

                switch (item)
                {
                    case menuNew:       return { 'n', mod, 0 };
                    case menuOpen:      return { 'o', mod, 0 };
                    case menuSave:      return { 's', mod, 0 };
                    case menuSaveAs:    return { 's', mod | shift, 0 };
                    case menuUndo:      return { 'z', mod, 0 };
                    case menuRedo:      return { 'z', mod | shift, 0 };
                    case menuCopy:      return { 'c', mod, 0 };
                    case menuPaste:     return { 'v', mod, 0 };
                    case menuSelectAll: return { 'a', mod, 0 };
                    case menuDeleteCue: return { juce::KeyPress::backspaceKey, mod, 0 };
                    case menuLock:      return { 'l', mod, 0 };
                    case menuRevert:    break;
                }

                return {};
            }

            static int menuItemForKey (const juce::KeyPress& key)
            {
                for (const auto item : { menuNew, menuOpen, menuSaveAs, menuCopy, menuPaste, menuLock })
                    if (key == keyFor (item))
                        return item;

                //  Ctrl/⌘-Y is redo everywhere but the Mac, and costs nothing to honour.
                if (key == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0))
                    return menuRedo;

                return 0;
            }

            /*  THE MENU FOLLOWS THE READING, as the keys do: what the show does
                not allow, the menu does not offer, and a key for it does nothing. */
            bool menuItemEnabled (MenuItem item) const
            {
                const auto unlocked = ! model::isYes (last.locked);

                switch (item)
                {
                    case menuNew:
                    case menuOpen:      return host.openWindow != nullptr;
                    case menuSave:      return last.mayOfferSave() && last.hasSomethingToSave();
                    case menuSaveAs:    return last.mayOfferSave();
                    case menuRevert:    return last.mayOfferSave();
                    case menuUndo:      return unlocked && last.canUndo == model::Flag::yes;
                    case menuRedo:      return unlocked && last.canRedo == model::Flag::yes;
                    case menuCopy:      return ! selection.empty();
                    case menuPaste:     return unlocked && ! last.listId.empty();
                    case menuSelectAll: return true;
                    case menuDeleteCue: return unlocked && ! selection.empty();
                    case menuLock:      return last.locked != model::Flag::unsaid;
                }

                return false;
            }

            void addMenuItem (juce::PopupMenu& menu, MenuItem item, const juce::String& words) const
            {
                juce::PopupMenu::Item entry { words };
                entry.itemID = item;
                entry.isEnabled = menuItemEnabled (item);

                if (const auto key = keyFor (item); key.isValid())
                    entry.shortcutKeyDescription = key.getTextDescription();

                menu.addItem (entry);
            }

            juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
            {
                juce::PopupMenu menu;

                if (index == 0)
                {
                    addMenuItem (menu, menuNew, "New show...");
                    addMenuItem (menu, menuOpen, "Open show...");
                    menu.addSeparator();
                    addMenuItem (menu, menuSave, "Save");
                    addMenuItem (menu, menuSaveAs, "Save as...");
                    addMenuItem (menu, menuRevert, "Revert to saved...");
                }
                else if (index == 1)
                {
                    addMenuItem (menu, menuUndo, "Undo");
                    addMenuItem (menu, menuRedo, "Redo");
                    menu.addSeparator();
                    addMenuItem (menu, menuCopy, selection.size() > 1
                                                   ? "Copy " + juce::String (static_cast<int> (selection.size())) + " cues"
                                                   : "Copy cue");
                    addMenuItem (menu, menuPaste, "Paste");
                    menu.addSeparator();
                    addMenuItem (menu, menuSelectAll, "Select all cues");
                    addMenuItem (menu, menuDeleteCue, selection.size() > 1
                                                        ? "Delete " + juce::String (static_cast<int> (selection.size())) + " cues"
                                                        : "Delete cue");
                }
                else if (index == 2)
                {
                    addMenuItem (menu, menuLock, model::isYes (last.locked) ? "Unlock the show"
                                                                            : "Lock the show");
                }

                return menu;
            }

            void menuItemSelected (int itemId, int) override
            {
                switch (itemId)
                {
                    case menuNew:       chooseShowFolder (true); break;
                    case menuOpen:      chooseShowFolder (false); break;
                    case menuSave:      send (gesture::save()); break;
                    case menuSaveAs:    chooseSaveAsFolder(); break;
                    case menuRevert:    shell->transport.askThenRevert(); break;
                    case menuUndo:      send (gesture::undo()); break;
                    case menuRedo:      send (gesture::redo()); break;
                    case menuCopy:      copyChosen(); break;
                    case menuPaste:     pasteFromClipboard(); break;
                    case menuSelectAll: selection.all (show.rows()); break;
                    case menuDeleteCue: removeChosen(); break;
                    case menuLock:      send (gesture::setLocked (! model::isYes (last.locked))); break;
                    default: break;
                }
            }

            /*  ONE `object.delete`, from the key and from the menu alike. It does
                not ask, since undo is one keystroke; and it unpicks, so the
                panel is not left describing a cue that is gone. */
            /*  COPY AND PASTE, ACROSS WINDOWS. Copy asks the engine for the
                fragment (one `document.copy`); the fragment comes back through
                the tree on a later pass and `pass` puts it on the operating
                system's clipboard, which is the only thing two processes
                share. Paste reads that clipboard and hands what it finds to
                `document.paste`, landing where a new cue would - after the
                anchor, or at the end of the list. Text that is not a fragment
                is not pasted, and the foot says so. */
            void copyChosen()
            {
                if (selection.empty())
                    return;

                send (gesture::copyCues (selection.ids()));
                shell->transport.setNotice (juce::String (static_cast<int> (selection.size()))
                                              + (selection.size() == 1 ? " cue copied" : " cues copied"));
            }

            void pasteFromClipboard()
            {
                if (refusedWhileLocked())
                    return;

                const auto text = juce::SystemClipboard::getTextFromClipboard();

                if (! text.trimStart().startsWith ("<Fragment"))
                {
                    shell->transport.setNotice ("the clipboard holds no cues");
                    return;
                }

                const auto [parent, index] = destination();

                if (parent.empty())
                {
                    shell->transport.setNotice ("no list to paste into");
                    return;
                }

                const auto members = static_cast<int> (model::words (orderOf (parent)).size());
                const auto at = index < 0 ? members : juce::jlimit (0, members, index);

                send (gesture::pasteCues (parent, at, text.toStdString()));
            }

            /*  The engine's clipboard, mirrored to the system's when it moves:
                what `document.copy` made is what ctrl/⌘-V in any window reads. */
            void mirrorClipboard (const tree::TreeSnapshot& snapshot)
            {
                const auto fragment = model::text (snapshot, "/godot/document/clipboard");

                if (fragment.empty() || fragment == clipboardSeen)
                    return;

                clipboardSeen = fragment;
                juce::SystemClipboard::copyTextToClipboard (juce::String (fragment));
            }

            void removeChosen()
            {
                if (selection.empty() || refusedWhileLocked())
                    return;

                /*  ONE `object.delete` EACH, in the order they were picked:
                    N decisions, N records, N presses of undo to take back -
                    which the page's own delete says in its title. A copy of
                    the ids, since the selection is cleared under them. */
                const auto ids = selection.ids();

                for (const auto& id : ids)
                    send (gesture::deleteObject (id));

                selection.clear();
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

                /*  THE MENU'S ENABLED STATES FOLLOW THE READING, rebuilt only
                    when one of the things they read has moved. */
                if (reading.locked != last.locked || reading.canUndo != last.canUndo
                      || reading.canRedo != last.canRedo || reading.dirty != last.dirty
                      || selection.size() != chosenAtLastMenu)
                {
                    chosenAtLastMenu = selection.size();
                    menuItemsChanged();
                }

                /*  THE NEW-CUE ROW STANDS UNLESS THE SHOW SAID IT IS LOCKED.
                    `isYes`, not a truth test: before the node is published
                    the show has not answered, and a row that vanished for a
                    tick at start would be a flicker with no meaning. */
                shell->setEditing (! model::isYes (reading.locked));

                /*  THE TWO RATES OUT OF ONE SNAPSHOT. The model walks the show
                    only when `/godot/document/revision` has moved or the
                    focused list has changed (M0); the pointer is read every
                    pass and costs two rows a repaint. Both from the same
                    pointer copy, so the list and the strip can never disagree
                    about which tick they are drawing. */
                show.refresh (*snapshot, reading.listId);

                //  What the show no longer has cannot stay picked.
                selection.retain (show.rows());
                shell->cues.show (show, reading.standbyId, selection.ids());

                /*  And the present tense, read fresh: runs have no revision to
                    key on, because a run is not a decision anybody recorded.

                    THE ANALYSER'S TABLE IS READ ONCE HERE TOO, beside the
                    tree's and for the same reason: one pointer copy per pass,
                    so the waveform under a row and the position on it cannot
                    come from two different moments. It is the table the HTTP
                    route serves the page from, published by the analyser
                    thread under a short mutex - not anything the tick thread
                    owns, which is what §14.16's second rule is about. */
                shell->runs.show (model::readRuns (*snapshot),
                                  host.media != nullptr ? host.media->snapshot()
                                                        : nullptr);

                //  And any import or create still waiting for the cue it made.
                finishImports (*snapshot, reading.revision);
                finishCreations (*snapshot, reading.revision);
                mirrorClipboard (*snapshot);

                //  Where the next new cue would land, said on the buttons.
                shell->newCues.setDestination (destinationSentence());

                /*  AND THE ONE CUE SOMEBODY ASKED ABOUT. The panel is built
                    from the tree when the picked cue changes and its values
                    updated otherwise, so typing is never overwritten by a
                    poll - which is the one thing a panel like this must not
                    do. */
                shell->setInspecting (! selection.empty());

                if (! selection.empty())
                    shell->inspector.show (model::inspectMany (*snapshot, selection.ids()));

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

            /*  PANIC AND ESC (PRD §4.4). The first press is the graceful
                abort and the footers run; a second within the window
                (model/Panic.h) drops everything and no footer runs. Each is
                one named command, so the log says which level was reached.
                The sentence on the foot says the same thing in words, since
                a hand that pressed Esc is not looking at the running pane to
                find out what it did. */
            void panic()
            {
                const auto now = static_cast<std::int64_t> (juce::Time::getMillisecondCounter());

                if (panicPresses.press (now))
                {
                    send (gesture::killAll());
                    shell->transport.setNotice ("double Esc: everything dropped, no footers");
                }
                else
                {
                    send (gesture::stopAll());
                    shell->transport.setNotice ("Esc: every cue stopping, footers run - Esc again drops everything");
                }
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

            /*  THE NATIVE OPEN, which is the other half of decision Y: a drop
                is what somebody does with a file they can already see, and this
                is what they do when they cannot. It ends in exactly the same
                place - `linkMedia`, with its copy and its confirmation - so the
                two gestures cannot come to mean different things.

                THE CHOOSER IS A MEMBER because `launchAsync` returns at once
                and the object must outlive the dialogue; one at a time, since
                a second click while one is open replaces it, which is what
                somebody clicking twice meant anyway. */
            void chooseFile (const std::string& cueId)
            {
                if (refusedWhileLocked())
                    return;

                /*  THE SAME READERS THE ENGINE USES, so the chooser cannot
                    offer a file the show would then fail on. */
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();

                chooser = std::make_unique<juce::FileChooser> (
                            "Choose the media this cue plays",
                            mediaFolder(), formats.getWildcardForAllFormats());

                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this, cueId] (const juce::FileChooser& answered)
                                      {
                                          const auto chosen = answered.getResult();

                                          if (safe != nullptr && chosen.existsAsFile())
                                              linkMedia (cueId, chosen.getFullPathName());
                                      });
            }

            /*  ANOTHER SHOW, IN ANOTHER WINDOW. New and Open both ask for a
                folder and hand it to the console, which starts a second
                process on it with this one's flags (Console.h, `openWindow`):
                one engine holds one document, and a window per show is what
                the author asked for. This window is untouched either way, so
                neither asks anything else. */
            void chooseShowFolder (bool createNew)
            {
                if (! host.openWindow)
                {
                    shell->transport.setNotice ("this build cannot open another window");
                    return;
                }

                chooser = std::make_unique<juce::FileChooser> (
                            createNew ? "Choose an empty folder for the new show"
                                      : "Choose a show's folder",
                            mediaFolder().getParentDirectory().getParentDirectory());

                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectDirectories,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this, createNew] (const juce::FileChooser& answered)
                                      {
                                          const auto folder = answered.getResult();

                                          if (safe == nullptr || folder == juce::File())
                                              return;

                                          const auto refused = host.openWindow (folder.getFullPathName().toStdString(),
                                                                                createNew);

                                          shell->transport.setNotice (refused.empty()
                                                                        ? "opening " + folder.getFileName() + " in a new window"
                                                                        : juce::String (refused));
                                      });
            }

            /*  SAVE AS: a folder, and one `document.saveAs` on it. The engine
                writes the copy and keeps this session on the show it opened,
                which is what the command was drawn to do (§14.10); the foot
                says where the copy went. */
            void chooseSaveAsFolder()
            {
                chooser = std::make_unique<juce::FileChooser> (
                            "Choose an empty folder for the copy",
                            mediaFolder().getParentDirectory().getParentDirectory());

                chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                        | juce::FileBrowserComponent::canSelectDirectories
                                        | juce::FileBrowserComponent::warnAboutOverwriting,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this] (const juce::FileChooser& answered)
                                      {
                                          const auto folder = answered.getResult();

                                          if (safe == nullptr || folder == juce::File())
                                              return;

                                          send (gesture::saveAs (folder.getFullPathName().toStdString()));
                                          shell->transport.setNotice ("copy of the show written to "
                                                                        + folder.getFileName());
                                      });
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

                shell->transport.setNotice ("the show is locked: unlock it to edit it");
                return true;
            }

            //======================================================================
            /*  A NEW CUE FROM THE ROW OF BUTTONS. Where it lands is the page's
                rule (model/NewCue.h): after the picked cue in the picked cue's
                parent, or at the end of the focused list when nothing is
                picked. `+ media` is the native Open followed by the import,
                so a media cue made here is never left without its file - the
                one thing the page's "+ media" cannot do (decision Y). */

            /** The container and member position the next new cue takes. Empty parent when there is no list. */
            std::pair<std::string, int> destination() const
            {
                const auto& picked = selection.anchor();

                if (latest != nullptr && ! picked.empty())
                {
                    const auto parent = model::text (*latest, "/godot/cue/" + picked + "/parent");

                    if (! parent.empty())
                        return { parent, model::positionAfter (orderOf (parent), picked) };
                }

                return { last.listId, -1 };
            }

            juce::String destinationSentence() const
            {
                const auto& picked = selection.anchor();

                if (latest == nullptr || picked.empty())
                    return "at the end of the list";

                const auto name = model::text (*latest, "/godot/cue/" + picked + "/name");

                return "after " + (name.empty() ? juce::String ("the picked cue") : juce::String (name));
            }

            void createCue (const std::string& kind)
            {
                if (refusedWhileLocked())
                    return;

                const auto [parent, index] = destination();

                if (parent.empty())
                {
                    shell->transport.setNotice ("no list to add a cue to");
                    return;
                }

                if (kind == "media")
                {
                    chooseMedia (parent, index);
                    return;
                }

                /*  The index is pinned to the member count here, as an
                    import's is, so what is asked for is what `createdAt`
                    will look at. */
                const auto members = static_cast<int> (model::words (orderOf (parent)).size());
                const auto at = index < 0 ? members : juce::jlimit (0, members, index);

                send (gesture::createCue (parent, at, kind, ""));
                creations.push_back ({ parent, at, kind, last.revision, 0 });
            }

            /*  `+ media` asks for the files first and imports them where the
                cue would have gone; nothing is made when the dialogue is
                cancelled. Several files make several cues, in the order
                chosen, exactly as a drop of several does. */
            void chooseMedia (const std::string& parent, int index)
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();

                chooser = std::make_unique<juce::FileChooser> (
                            "Choose the media for the new cue",
                            mediaFolder(), formats.getWildcardForAllFormats());

                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this, parent, index] (const juce::FileChooser& answered)
                                      {
                                          if (safe == nullptr)
                                              return;

                                          juce::StringArray files;

                                          for (const auto& file : answered.getResults())
                                              if (file.existsAsFile())
                                                  files.add (file.getFullPathName());

                                          if (! files.isEmpty())
                                              importMedia (parent, index, files);
                                      });
            }

            /*  THE OTHER HALF OF A CREATE, run every pass: find the cue and
                pick it, so the inspector opens on it and the name is the next
                thing typed. Picking is this client's own state, so a create
                that is never found costs nothing but a sentence. */
            void finishCreations (const tree::TreeSnapshot& snapshot, std::uint64_t revision)
            {
                if (creations.empty())
                    return;

                std::vector<model::Creation> waiting;

                for (auto& job : creations)
                {
                    const auto id = revision > job.askedAt
                                      ? model::createdAt (orderOf (job.parent), job.index)
                                      : std::string {};

                    if (! id.empty()
                          && model::madeByCreate (job,
                                                  model::text (snapshot, "/godot/cue/" + id + "/kind"),
                                                  model::text (snapshot, "/godot/cue/" + id + "/name")))
                    {
                        selection.set (id);
                        continue;
                    }

                    if (++job.waited < model::importPatience)
                        waiting.push_back (job);
                    else
                        shell->transport.setNotice ("the new " + juce::String (job.kind)
                                                      + " cue was refused");
                }

                creations = std::move (waiting);
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
            model::Selection selection;

            /*  THE POINTER THE LAST PASS DREW. Null until the first one, which
                is why every reader above checks. */
            std::shared_ptr<const tree::TreeSnapshot> latest;

            /** Files copied in, cues asked for, and the naming still to do. */
            std::vector<model::Import> pending;

            /** Creates sent from the new-cue row and not yet found, to be picked when they are. */
            std::vector<model::Creation> creations;

            /** Which level of stop the next Esc means. */
            model::Panic panicPresses;

            /** How many were picked when the menu was last rebuilt, so it rebuilds when that moves. */
            std::size_t chosenAtLastMenu = 0;

            /** The engine's clipboard as last mirrored to the system's. */
            std::string clipboardSeen;

            /** The open file dialogue, which must outlive the call that launched it. */
            std::unique_ptr<juce::FileChooser> chooser;
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
