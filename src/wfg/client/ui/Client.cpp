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
#include <wfg/client/model/LoadToTime.h>
#include <wfg/client/model/UndoHistory.h>
#include <wfg/client/model/Media.h>
#include <wfg/client/model/NewCue.h>
#include <wfg/client/model/Panic.h>
#include <wfg/client/model/Reorder.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/DirectOuts.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Selection.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/ui/AudioSettingsWindow.h>
#include <wfg/client/ui/MainWindow.h>
#include <wfg/client/ui/Shell.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <memory>
#include <optional>
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
            menuUndo, menuRedo, menuCut, menuCopy, menuPaste, menuSelectAll, menuDeleteCue,
            menuLock, menuLoadToTime, menuUndoHistory, menuRecord, menuAudioSettings,
            menuWaveform
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
                actions.go              = [this] { send (gesture::go()); leaveLoadToTime(); };
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
                listActions.go              = [this] { send (gesture::go()); leaveLoadToTime(); };

                /*  A FOLD IS THIS CLIENT'S AND NEVER THE ENGINE'S (§14.1): what
                    somebody collapsed on their screen is not something the show
                    decided, so it goes to the model and nowhere near `submit`. */
                /*  A FOLD IS DRAWN AT ONCE AND RECORDED WITH THE SHOW (author,
                    2026-09-18: "fold state should be recorded in project
                    file"): the model flips it now, and the flag - a state
                    row, so a locked show takes it - is written for state.xml
                    to keep, and for the next opening to seed the model from. */
                listActions.fold            = [this] (const std::string& key)
                                              {
                                                  show.toggle (key);
                                                  send (gesture::setNode (show.foldAddress (key),
                                                                          show.isShut (key) ? "true" : "false"));
                                              };

                /*  THE PRESET GESTURES: alt-drop on an ancestor group, and
                    ctrl/⌘-arrows stepping through the ancestors. Both are one
                    write to the cue's `preset` (model/Reorder.h). */
                listActions.setPreset       = [this] (const std::string& cueId, const std::string& group)
                                              {
                                                  if (! refusedWhileLocked())
                                                      send (gesture::setNode ("/godot/cue/" + cueId + "/preset", group));
                                              };
                listActions.presetStep      = [this] (int direction) { ladderStep (direction); };
                listActions.moveToFooter    = [this] (const std::string& cueId, const std::string& group)
                                              { moveIntoRole (cueId, group, "footer"); };
                listActions.moveToHeader    = [this] (const std::string& cueId, const std::string& group)
                                              { moveIntoRole (cueId, group, "header"); };

                //  A cell edited in place is one `node.set`, as a field in the inspector is.
                listActions.setValue        = [this] (const std::string& address, const std::string& text)
                                              {
                                                  if (! refusedWhileLocked())
                                                      send (gesture::setNode (address, text));
                                              };
                /*  WHAT IS PICKED IS THIS CLIENT'S (model/Selection.h): a
                    click, with shift or ctrl/⌘, over the rows as drawn. */
                listActions.pick            = [this] (const std::string& id, bool extend, bool toggle)
                                              {
                                                  if (id.empty())
                                                      selection.clear();
                                                  else
                                                      selection.click (id, extend, toggle, show.rows());

                                                  /*  LOADING TO TIME, THE PICK IS THE AIM (author,
                                                      2026-09-18: "cue or group can be changed and
                                                      the load to time readjusts to it"). */
                                                  aimAtPick();

                                                  /*  THE INSPECTOR WAITS OUT THE DOUBLE-CLICK on a plain
                                                      click (author, 2026-09-18: "don't open the inspector
                                                      on a double click"): the row is picked at once, and
                                                      the panel opens only if no second click follows in
                                                      the system's double-click time - so the list is not
                                                      relaid out under the second click, which was what
                                                      moved the cells from under the pointer. */
                                                  inspectorHeld = false;
                                                  inspectorDueAt = juce::Time::getMillisecondCounter()
                                                                   + (extend || toggle || id.empty()
                                                                        ? 0u
                                                                        : static_cast<juce::uint32> (juce::MouseEvent::getDoubleClickTimeout()));
                                              };
                listActions.editingBegan    = [this] { inspectorHeld = true; };
                listActions.pickAll         = [this] { selection.all (show.rows()); inspectNow(); };
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
                                                  if (refusedWhileLocked())
                                                      return;

                                                  /*  WHAT THIS CUE'S OUTPUT LOOKED LIKE
                                                      BEFORE THE MOVE, so the pass after
                                                      it can say what the move did (PRD
                                                      3.9c: "the reorder warning is
                                                      liveness re-analysis on edit").

                                                      AFTER AND NOT BEFORE, deliberately:
                                                      the analysis runs on the document
                                                      and the document has not moved yet,
                                                      so the only honest way to know what
                                                      a move does is to make it and look.
                                                      Nothing is blocked, the two cues sum
                                                      in the meantime, and undo is one
                                                      gesture away. */
                                                  rememberOutsOf (id);
                                                  send (gesture::moveObject (id, parent, index));
                                              };
                listActions.setTarget       = [this] (const std::string& aimed, const std::string& at)
                                              {
                                                  if (! refusedWhileLocked())
                                                      send (gesture::setNode ("/godot/cue/" + aimed + "/target", at));
                                              };

                /*  A CLICK ON A STEP ROW under the aimed cue re-aims at that
                    moment; only while the panel is up, since the rows exist
                    only then. */
                listActions.reaim           = [this] (double offset)
                                              {
                                                  if (loadingToTime && ! aimCue.empty())
                                                      send (gesture::aim (last.listId, aimCue, offset));
                                              };

                ui::RunPaneComponent::Actions runActions;

                runActions.kill = [this] (const std::string& id) { send (gesture::kill (id)); };
                runActions.inspectError = [this] (const std::string& id) { inspectCueError (id); };

                /*  A SCRUB IS A HANDFUL OF SEEKS A SECOND AND ONE ON RELEASE,
                    each a record; the pane decides when the hand has settled
                    (model/Scrub.h), and this only sends. */
                runActions.seek = [this] (const std::string& id, double seconds)
                                  { send (gesture::seek (id, seconds)); };

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

                /*  THE PANEL AT THE FOOT, ASKED FOR FROM THE CUE ITSELF. The
                    inspector hands back a word; the words are the ones
                    `model::Subject` spells, and an unknown one opens nothing
                    rather than guessing. */
                inspectorActions.openPanel = [this] (const std::string& cueId,
                                                     const std::string& subject)
                {
                    if (shell == nullptr || cueId.empty())
                        return;

                    auto wanted = model::Subject::Kind::none;

                    if (subject == "waveform")
                        wanted = model::Subject::Kind::waveform;
                    else if (subject == "sends")
                        wanted = model::Subject::Kind::sends;
                    else if (subject == "timeline")
                        wanted = model::Subject::Kind::timeline;
                    else if (subject == "curve")
                        wanted = model::Subject::Kind::curve;

                    if (wanted == model::Subject::Kind::none)
                        return;

                    /*  A SECOND PRESS ON THE PANEL ALREADY OPEN SHUTS IT, which
                        is what the same button in a menu does and what a hand
                        expects of a control that has no other off switch. */
                    const auto already = shell->footSubject();

                    shell->setFoot (already.kind == wanted && already.objectId == cueId
                                      ? model::Subject {}
                                      : model::Subject { wanted, cueId });

                    menuItemsChanged();
                };

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

                /*  LOAD TO TIME'S OWN TWO GESTURES: the aim, asked on every
                    change, and the jump. Both go to the focused list, which
                    is the list the history and the answer are read from. */
                ui::HistoryPanelComponent::Actions historyActions;

                historyActions.aim  = [this] (const std::string& cueId, double offset)
                                      { send (gesture::aim (last.listId, cueId, offset)); };
                historyActions.load = [this] { send (gesture::loadToTime (last.listId)); };

                /*  THE UNDO HISTORY'S THREE GESTURES: stand after that many
                    transactions - which is that many `undo` or `redo`
                    records, each one the log replays - and the two buttons
                    that close the panel, OK keeping where it stands, Cancel
                    going back to where it was opened. */
                ui::UndoPanelComponent::Actions undoActions;

                undoActions.moveTo = [this] (int index) { standAt (index); };
                undoActions.ok     = [this] { leaveUndoHistory(); };
                undoActions.cancel = [this] { standAt (undoOpenedAt); leaveUndoHistory(); };

                /*  THE PANEL AT THE FOOT. It writes through the same door
                    every other gesture does - one `node.set` per edit - and
                    tells the Shell when its edge is dragged, because how much
                    of the window it may take is the window's question. */
                ui::FootPanelComponent::Actions footActions;
                footActions.set = [this] (const std::string& address, const std::string& value)
                { send (gesture::setNode (address, value)); };
                footActions.close = [this]
                {
                    /*  SHUT MEANS SHUT, for the one subject that opens itself:
                        remembered against the cue it was showing, so picking
                        the same fade again leaves it closed and picking a
                        different one opens it. */
                    if (shell->footSubject().kind == model::Subject::Kind::curve)
                        shutCurveFor = shell->footSubject().objectId;

                    shell->setFoot ({});
                };
                footActions.resizeBy = [this] (int pixels) { shell->growFoot (pixels); };

                footActions.createRange = [this] (const std::string& cueId, double in, double out)
                                          { send (gesture::createRange (cueId, in, out)); };

                footActions.removeObject = [this] (const std::string& objectId)
                                           { send (gesture::deleteObject (objectId)); };

                footActions.splitRange = [this] (const std::string& cueId, double at)
                                         { send (gesture::splitRange (cueId, at)); };

                footActions.createSend = [this] (const std::string& cueId, const std::string& busId)
                                         { send (gesture::createSend (cueId, busId)); };

                footActions.openTimelineOn = [this] (const std::string& groupId)
                {
                    if (shell != nullptr && ! groupId.empty())
                        shell->setFoot ({ model::Subject::Kind::timeline, groupId });
                };

                /*  THE PANEL'S TRANSPORT, through the ordinary doors: the cue
                    is fired by name, the run it made is killed by identifier,
                    and a drag in the ruler seeks that run. Every one of them
                    is a command a surface could send (4.11), and all three
                    show up in the running pane like anything else. */
                footActions.play = [this] (const std::string& cueId)
                                   { send (gesture::fireCue (cueId)); };

                footActions.stop = [this] (const std::string& runId)
                                   { send (gesture::kill (runId)); };

                footActions.seek = [this] (const std::string& runId, double seconds)
                                   { send (gesture::seek (runId, seconds)); };

                auto content = std::make_unique<ui::Shell> (theme, std::move (actions),
                                                            std::move (listActions),
                                                            std::move (runActions),
                                                            std::move (inspectorActions),
                                                            std::move (newCueActions),
                                                            std::move (historyActions),
                                                            std::move (undoActions),
                                                            std::move (footActions));
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

                /*  MOST OF THE SCREEN, not a fixed thirty-four rows by
                    twenty-six (author, 2026-09-19: "make the initial window
                    size larger too. It's cramped"): the panes have grown to
                    three beside each other, and a booth screen is there to
                    be used. Eighty-five hundredths of the working area, never
                    smaller than the size it opened at before, and never past
                    the area itself. */
                const auto least = juce::Point<int> (juce::roundToInt (34 * theme.row * theme.type),
                                                     juce::roundToInt (26 * theme.row * theme.type));
                auto wanted = least;

                if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                {
                    const auto area = display->userBounds.toNearestInt();
                    wanted.x = juce::jlimit (juce::jmin (least.x, area.getWidth()), area.getWidth(),
                                             juce::roundToInt (area.getWidth() * 0.85));
                    wanted.y = juce::jlimit (juce::jmin (least.y, area.getHeight()), area.getHeight(),
                                             juce::roundToInt (area.getHeight() * 0.85));
                }

                window->centreWithSize (wanted.x, wanted.y);

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
                    case menuCut:       return { 'x', mod, 0 };
                    case menuCopy:      return { 'c', mod, 0 };
                    case menuPaste:     return { 'v', mod, 0 };
                    case menuSelectAll: return { 'a', mod, 0 };
                    case menuDeleteCue: return { juce::KeyPress::backspaceKey, mod, 0 };
                    case menuLock:      return { 'l', mod, 0 };
                    case menuLoadToTime: return { 't', mod, 0 };
                    /*  W FOR THE WAVEFORM (author, 2026-09-22: "the waveform
                        editor should have a keyboard shortcut (Ctrl/Cmd+W)").
                        It is the one letter on this table another application
                        would spend on closing a window; Go.dot has no close
                        item to spend it on - a show window is shut by shutting
                        the show - so it goes to the thing it names. */
                    case menuWaveform:  return { 'w', mod, 0 };
                    case menuUndoHistory: return { 'u', mod | shift, 0 };
                    case menuRecord:     return { 'r', mod | shift, 0 };
                    //  No accelerator: both are reached through the menu only.
                    case menuRevert:
                    case menuAudioSettings: break;
                    
                }

                return {};
            }

            static int menuItemForKey (const juce::KeyPress& key)
            {
                for (const auto item : { menuNew, menuOpen, menuSaveAs, menuCut, menuCopy, menuPaste, menuLock,
                                         menuLoadToTime, menuUndoHistory, menuRecord, menuWaveform })
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
                    case menuCut:       return unlocked && ! selection.empty();
                    case menuCopy:      return ! selection.empty();
                    case menuPaste:     return unlocked && ! last.listId.empty();
                    case menuSelectAll: return true;
                    case menuDeleteCue: return unlocked && ! selection.empty();
                    case menuLock:      return last.locked != model::Flag::unsaid;
                    case menuLoadToTime: return ! last.listId.empty();
                    case menuUndoHistory: return unlocked;
                    case menuRecord:     return model::isYes (last.recording) ? unlocked : true;
                    case menuAudioSettings: return true;

                    /*  Offered for a media cue, and for shutting the panel
                        whatever is picked - a panel that could be opened and
                        not closed from the same place would be a trap. */
                    case menuWaveform:  return shell != nullptr
                                                 && (shell->footSubject().isOpen()
                                                     || ! selection.empty());
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

                    /*  AND THE HISTORY BESIDE THEM (author, 2026-09-21: "the
                        undo/redo history should be in the edit menu not the
                        show"). It was under Show, beside load-to-time, because
                        both open a panel where the inspector sits - but what a
                        menu groups is what a thing IS, not where it draws, and
                        this is the third way of saying Undo. */
                    addMenuItem (menu, menuUndoHistory, browsingUndo ? "Close the undo history"
                                                                     : "Undo history...");
                    menu.addSeparator();
                    addMenuItem (menu, menuCut, selection.size() > 1
                                                  ? "Cut " + juce::String (static_cast<int> (selection.size())) + " cues"
                                                  : "Cut cue");
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
                    menu.addSeparator();
                    addMenuItem (menu, menuLoadToTime, loadingToTime ? "Stop loading to time"
                                                                     : "Load to time...");

                    /*  THE PANEL AT THE FOOT, named for what it would show
                        rather than for the furniture: "editor panel" tells
                        nobody which of several things they are about to get,
                        and the author's shape for it is that whatever opens it
                        says what it is opening on. */
                    addMenuItem (menu, menuWaveform,
                                 shell != nullptr && shell->footSubject().isOpen()
                                   ? "Close the waveform" : "Waveform...");
                    menu.addSeparator();
                    addMenuItem (menu, menuRecord, model::isYes (last.recording) ? "Stop the live recorder"
                                                                                  : "Start the live recorder");
                    menu.addSeparator();
                    addMenuItem (menu, menuAudioSettings, "Audio settings...");
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
                    case menuCut:       copyChosen(); removeChosen(); break;
                    case menuCopy:      copyChosen(); break;
                    case menuPaste:     pasteFromClipboard(); break;
                    case menuSelectAll: selection.all (show.rows()); inspectNow(); break;
                    case menuDeleteCue: removeChosen(); break;
                    case menuLock:      send (gesture::setLocked (! model::isYes (last.locked))); break;
                    case menuLoadToTime: toggleLoadToTime(); break;
                    case menuUndoHistory: toggleUndoHistory(); break;
                    case menuWaveform:  toggleWaveform(); break;
                    case menuAudioSettings:
                        if (latest)
                        {
                            if (! audioSettings)
                                audioSettings = std::make_unique<ui::AudioSettingsWindow> (theme, *latest,
                                    [this] (Event event) { send (std::move (event)); }, [this] { panic(); });
                            audioSettings->setVisible (true);
                            audioSettings->toFront (true);
                        }
                        break;
                    case menuRecord:     send (model::isYes (last.recording) ? gesture::recordStop()
                                                                             : gesture::recordStart()); break;
                    default: break;
                }
            }

            /*  ONE `object.delete`, from the key and from the menu alike. It does
                not ask, since undo is one keystroke; and it unpicks, so the
                panel is not left describing a cue that is gone. */
            //======================================================================
            /*  THE LADDER (author, 2026-09-18): ctrl/⌘-up and -down walk a cue
                through where a group can hold it, from the outermost header
                down to the footer. Outward from none: prepared in the
                innermost group's header, then each group further out. Inward:
                back to none, and one step further puts the cue IN the
                innermost group's footer - a move, since a footer is a place
                and not a mark. From the footer, up is back among the members.
                Each step is one command and the foot says what happened. */
            void ladderStep (int direction)
            {
                if (selection.empty() || refusedWhileLocked() || latest == nullptr)
                    return;

                for (const auto& id : selection.ids())
                {
                    const model::Row* own = nullptr;

                    for (const auto& row : show.rows())
                        if (row.rowKind == model::RowKind::cue && row.id == id && ! row.derived)
                            own = &row;

                    if (own == nullptr)
                        continue;

                    //  In a footer: up is back among the group's members; down is the end.
                    if (own->section == model::Section::footer)
                    {
                        if (direction > 0 && ! own->parent.empty())
                        {
                            //  At the end of the group's members.
                            send (gesture::moveObject (id, own->parent,
                                                       static_cast<int> (model::words (orderOf (own->parent)).size())));
                            shell->transport.setNotice ("back among " + nameOf (own->parent) + "'s members");
                        }

                        continue;
                    }

                    const auto current = model::text (*latest, "/godot/cue/" + id + "/preset");

                    if (const auto next = model::presetStep (id, current, direction, show.rows()))
                    {
                        send (gesture::setNode ("/godot/cue/" + id + "/preset", *next));
                        shell->transport.setNotice (next->empty() ? juce::String ("no longer prepared ahead")
                                                                  : "prepared in " + nameOf (*next) + "'s header");
                        continue;
                    }

                    //  Down past none, inside a group: into that group's footer.
                    const auto ancestors = model::ancestorsOf (id, show.rows());

                    if (direction < 0 && current.empty() && ! ancestors.empty())
                        moveIntoRole (id, ancestors.front(), "footer");
                }
            }

            juce::String nameOf (const std::string& cueId) const
            {
                const auto name = latest != nullptr ? model::text (*latest, "/godot/cue/" + cueId + "/name")
                                                    : std::string {};
                return juce::String (name.empty() ? cueId : name);
            }

            /*  INTO A GROUP'S FOOTER: one `object.move` when the footer exists,
                and when it does not, `group.role` to make it and the move on
                a later pass once the tree names it (the import's own shape). */
            /*  INTO A GROUP'S HEADER OR ITS FOOTER, made first if it has
                none. One verb for both roles (2026-09-22): they differ by a
                word in three places and by nothing at all in the shape - ask
                whether the section exists, move into it if it does, otherwise
                make it and remember to move once it lands. Two copies of that
                would be two things to keep in step, and the header's was the
                one that did not exist. */
            void moveIntoRole (const std::string& cueId, const std::string& group,
                               const std::string& role)
            {
                if (refusedWhileLocked() || latest == nullptr)
                    return;

                const auto section = model::text (*latest, "/godot/cue/" + group + "/" + role);

                if (! section.empty())
                {
                    const auto members = static_cast<int> (model::words (
                        model::text (*latest, "/godot/cue/" + group + "/" + role + "Order")).size());

                    send (gesture::moveObject (cueId, section, members));
                    shell->transport.setNotice ("into " + nameOf (group) + "'s " + role);
                    return;
                }

                send (gesture::groupRole (group, role));
                footerMoves.push_back ({ cueId, group, role, 0 });
                shell->transport.setNotice ("making " + nameOf (group) + "'s " + role);
            }

            struct FooterMove
            {
                std::string cueId, group, role;
                int waited = 0;
            };

            void finishFooterMoves (const tree::TreeSnapshot& snapshot)
            {
                if (footerMoves.empty())
                    return;

                std::vector<FooterMove> waiting;

                for (auto& job : footerMoves)
                {
                    const auto footer = model::text (snapshot, "/godot/cue/" + job.group + "/" + job.role);

                    if (! footer.empty())
                    {
                        const auto members = static_cast<int> (model::words (model::text (snapshot, "/godot/cue/" + job.group + "/" + job.role + "Order")).size());
                        send (gesture::moveObject (job.cueId, footer, members));
                        shell->transport.setNotice ("into " + nameOf (job.group) + "'s footer");
                        continue;
                    }

                    if (++job.waited < model::importPatience)
                        waiting.push_back (job);
                    else
                        shell->transport.setNotice ("the footer for " + nameOf (job.group) + " was refused");
                }

                footerMoves = std::move (waiting);
            }

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
                /*  The dash as UTF-8 bytes, not a narrow literal: the title bar
                    showed "â□□" for it once the frame was the window's own. */
                return show.empty() ? juce::String ("Go.dot")
                                    : juce::String (juce::CharPointer_UTF8 ("Go.dot \xe2\x80\x94 "))
                                        + juce::String (show);
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
                if (audioSettings) audioSettings->refresh (*snapshot);

                if (reading.show != last.show)
                    window->setName (titleFor (reading.show));

                shell->transport.show (reading);

                /*  THE MENU'S ENABLED STATES FOLLOW THE READING, rebuilt only
                    when one of the things they read has moved. */
                if (reading.locked != last.locked || reading.canUndo != last.canUndo
                      || reading.canRedo != last.canRedo || reading.dirty != last.dirty
                      || reading.recording != last.recording
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
                if (! errorCueToInspect.empty()
                    && snapshot->find ("/godot/cue/" + errorCueToInspect + "/kind") == nullptr)
                    errorCueToInspect.clear();
                std::string revealedErrorCue;
                if (! errorCueToInspect.empty() && show.indexOf (errorCueToInspect) >= 0)
                {
                    selection.set (errorCueToInspect);
                    revealedErrorCue = errorCueToInspect;
                    errorCueToInspect.clear();
                    inspectNow();
                }

                /*  LOAD TO TIME, READ EVERY PASS WHILE THE PANEL IS UP: the
                    history, the aim and the engine's answer, and the steps
                    laid under the aimed cue's row as rows of their own. */
                if (loadingToTime)
                {
                    const auto ltt = model::readLoadToTime (*snapshot, reading.listId);
                    aimCue = ltt.aimCue;
                    aimOffset = ltt.aimOffset;
                    shell->history.show (ltt);

                    auto depth = 0;

                    for (const auto& row : show.rows())
                        if (row.rowKind == model::RowKind::cue && row.id == ltt.aimCue && ! row.derived)
                        {
                            depth = row.depth + 1;
                            break;
                        }

                    shell->cues.setSteps (ltt.aimCue,
                                          ltt.aimed ? model::stepRows (model::stepsUnder (ltt.steps, ltt.aimCue,
                                                                                          ltt.instant),
                                                                       ltt.aimOffset, ltt.names, depth)
                                                    : std::vector<model::Row> {});
                }
                else
                    shell->cues.setSteps ({}, {});

                /*  THE UNDO HISTORY, while its panel is up: where the show
                    stands, and what standing there changed against the
                    picture taken when the panel opened. */
                if (browsingUndo)
                {
                    lastUndo = model::readUndoHistory (*snapshot);
                    const auto changes = model::diff (undoPictureAtOpen, model::pictureOf (show.rows()));
                    shell->undoPanel.show (lastUndo, changes, undoOpenedAt);
                    shell->cues.setDiff (changes.changed, changes.added);
                }
                else
                    shell->cues.setDiff ({}, {});

                shell->cues.show (show, reading.standbyId, selection.ids());
                if (! revealedErrorCue.empty()) shell->cues.revealCue (revealedErrorCue);

                /*  And the present tense, read fresh: runs have no revision to
                    key on, because a run is not a decision anybody recorded.

                    THE ANALYSER'S TABLE IS READ ONCE HERE TOO, beside the
                    tree's and for the same reason: one pointer copy per pass,
                    so the waveform under a row and the position on it cannot
                    come from two different moments. It is the table the HTTP
                    route serves the page from, published by the analyser
                    thread under a short mutex - not anything the tick thread
                    owns, which is what §14.16's second rule is about. */
                const auto mediaTable = host.media != nullptr ? host.media->snapshot() : nullptr;

                shell->runs.show (model::readRuns (*snapshot), mediaTable);

                /*  AND THE PANEL AT THE FOOT, on the ONE subject it is open on.
                    Handed the table taken just above rather than asking for its
                    own: §14.16's second rule is one call site per door, and the
                    boundary check counts them.

                    The subject FOLLOWS THE PICK where that makes sense - the
                    waveform of the cue somebody just clicked is what they want
                    next - and `model::followsPick` is that rule, in one place,
                    so the panel does not have to guess per kind. */
                /*  A FADE OPENS ITS CURVE BY ITSELF (author, 2026-09-22:
                    "this opens automatically when selecting the fade cue").

                    THE ONE SUBJECT THAT IS NOT ASKED FOR, and the reason is
                    what a fade IS: its level, its duration and its curve word
                    are rows the inspector shows like any other, and its SHAPE
                    is not a row at all. A drawn fade whose panel had to be
                    opened by hand would be a list of numbers nobody can read
                    until they think to go looking.

                    AND IT CAN STILL BE SHUT. A panel that reopened every time
                    the pick came back would be one nobody could get rid of, so
                    closing it while a fade is picked is remembered against THAT
                    cue and cleared the moment the pick moves. The row in the
                    inspector is the way back. */
                if (! selection.anchor().empty()
                      && model::text (*snapshot, "/godot/cue/" + selection.anchor() + "/kind") == "fade"
                      && shutCurveFor != selection.anchor()
                      && shell->footSubject().kind != model::Subject::Kind::curve)
                {
                    shell->setFoot ({ model::Subject::Kind::curve, selection.anchor() });
                    menuItemsChanged();
                }

                if (shutCurveFor != selection.anchor())
                    shutCurveFor.clear();

                if (shell->footSubject().isOpen())
                {
                    auto subject = shell->footSubject();

                    /*  A TIMELINE IS ABOUT A CONTAINER, so picking a cue moves
                        it to that cue's GROUP rather than to the cue (author,
                        2026-09-22: "the group panel stays visible when
                        selecting other cues").

                        Three answers rather than two, and each one is what
                        somebody meant: pick a group and the panel arranges
                        THAT group; pick a cue inside one and it arranges the
                        group the cue is in, which is the scene they are
                        working on; pick a cue at the top of a list, where
                        there is no group to arrange, and the panel shuts
                        rather than sitting there showing somewhere else.

                        The last is the plan's own rule for every subject - "re-
                        points where that makes sense and closes where it does
                        not" - and it is the one a timeline had wrong. */
                    const auto picked = selection.anchor();

                    if (model::followsPick (subject.kind) && ! picked.empty())
                    {
                        auto wanted = picked;

                        /*  A PANEL THAT OPENS BY ITSELF MAY CLOSE BY ITSELF
                            (author, 2026-09-22: "collapse the fade foot panel
                            when selecting another type of cue").

                            The rule is not "every panel follows or shuts", it
                            is which of the two a subject was ASKED for. A
                            waveform and a send mixer were opened by hand and
                            stay until a hand shuts them - clicking a group to
                            check something must not cost somebody the zoom
                            they set. A fade's curve was never asked for: it
                            arrived because a fade was picked, so it leaves
                            when one is not, and picking a fade again brings it
                            straight back at no cost. */
                        if (subject.kind == model::Subject::Kind::curve
                              && model::text (*snapshot, "/godot/cue/" + picked + "/kind") != "fade")
                            wanted.clear();

                        if (subject.kind == model::Subject::Kind::timeline)
                        {
                            const auto isGroup = [&snapshot] (const std::string& id)
                            {
                                return ! id.empty()
                                         && model::text (*snapshot, "/godot/cue/" + id + "/kind") == "group";
                            };

                            if (! isGroup (wanted))
                                wanted = model::text (*snapshot, "/godot/cue/" + picked + "/parent");

                            /*  A CUE AT THE TOP OF A LIST HAS A LIST FOR A
                                PARENT, and a list is not a cue and has no
                                members to arrange in time. Asked rather than
                                assumed, because `parent` is never empty for a
                                placed cue and an unchecked climb would land
                                the panel on an identifier it cannot read. */
                            if (! isGroup (wanted))
                                wanted.clear();
                        }

                        if (wanted.empty())
                        {
                            shell->setFoot ({});
                            menuItemsChanged();
                        }
                        else if (wanted != subject.objectId)
                        {
                            subject.objectId = wanted;
                            shell->setFoot (subject);
                        }
                    }

                    shell->foot.show (model::readFoot (*snapshot, subject), mediaTable);
                }

                //  What a move just did to the moved cue's own output, if anything.
                sayIfTheMoveClashed (*snapshot, reading.revision);

                //  And any import or create still waiting for the cue it made.
                finishImports (*snapshot, reading.revision);
                finishCreations (*snapshot, reading.revision);
                finishFooterMoves (*snapshot);
                mirrorClipboard (*snapshot);

                //  Where the next new cue would land, said on the buttons.
                shell->newCues.setDestination (destinationSentence());

                /*  AND THE ONE CUE SOMEBODY ASKED ABOUT. The panel is built
                    from the tree when the picked cue changes and its values
                    updated otherwise, so typing is never overwritten by a
                    poll - which is the one thing a panel like this must not
                    do. */
                const auto inspecting = ! selection.empty() && ! inspectorHeld
                                     && juce::Time::getMillisecondCounter() >= inspectorDueAt;

                /*  THE SLOT BESIDE THE LIST: the history panel while loading
                    to time, the inspector when something is picked, nothing
                    otherwise. */
                shell->setPanel (browsingUndo  ? ui::Shell::Panel::undo
                                 : loadingToTime ? ui::Shell::Panel::history
                                 : inspecting    ? ui::Shell::Panel::inspector
                                                 : ui::Shell::Panel::none);

                if (inspecting && ! loadingToTime && ! browsingUndo)
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
                routeImportedMedia (cueId, name);

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
                        routeImportedMedia (id, job.mediaName);
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

            /*  WHAT AN IMPORTED FILE IS POINTED AT, and it is two plain
                writes rather than the matrix `route.default` used to build.

                The old command wrote a `Route` with its coefficients spelled
                out; it stays registered and tested so every log written before
                today still replays. What a cue says now is the same thing in
                the words a designer uses - how many channels the file has, and
                which direct out it lands on - and the coefficients are
                derived from those by `resolveRouting`. The difference matters
                at the menu: a matrix is not something a dropdown can show, and
                a designer changing where a cue goes should not be rewriting
                numbers.

                THE COUNT IS READ HERE AND WRITTEN DOWN rather than read off
                the disk when it is wanted, exactly as before: the file travels
                between machines and may be absent tonight, and a replay must
                not depend on it being present or readable. */
            /*  Held between a move being sent and the pass that sees its
                result. One cue at a time, because a hand moves one at a time
                and a second move before the first has landed is a sentence
                about the second. */
            struct MovedCue
            {
                std::string cue;
                std::string ownOut;
                std::vector<model::OutMark> before;
                std::uint64_t sentAt = 0;
            };

            std::optional<MovedCue> watchingMove;

            /*  The fade whose curve panel was shut by hand. Cleared when the
                pick moves, so the rule is "not this one, for now" rather than
                "never again". */
            std::string shutCurveFor;

            void rememberOutsOf (const std::string& cueId)
            {
                watchingMove.reset();

                if (latest == nullptr || cueId.empty())
                    return;

                const auto out = model::text (*latest, "/godot/cue/" + cueId + "/directOut");

                //  Only a cue that lands somewhere can start sharing it.
                if (out.empty())
                    return;

                MovedCue held;
                held.cue = cueId;
                held.ownOut = out;
                held.before = model::readOutMarks (*latest, cueId);
                held.sentAt = last.revision;

                watchingMove = std::move (held);
            }

            void sayIfTheMoveClashed (const tree::TreeSnapshot& snapshot,
                                      std::uint64_t revision)
            {
                if (! watchingMove.has_value())
                    return;

                /*  WAIT FOR THE DOCUMENT TO HAVE MOVED. The move is a command,
                    applied on the tick thread, and the pass that sent it sees
                    the tree from before. Compared too early this would report
                    the state it already knew. */
                if (revision == watchingMove->sentAt)
                    return;

                const auto held = *watchingMove;
                watchingMove.reset();

                const auto said = model::newClash (
                    held.before, model::readOutMarks (snapshot, held.cue), held.ownOut,
                    model::text (snapshot, "/godot/bus/" + held.ownOut + "/name"),
                    model::text (snapshot, "/godot/cue/" + held.cue + "/name"));

                if (said.has_value() && shell != nullptr)
                    shell->transport.setNotice (juce::String (*said));
            }

            void routeImportedMedia (const std::string& cueId, const std::string& name)
            {
                // Read the copied file's header off the tick/audio threads.
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                const std::unique_ptr<juce::AudioFormatReader> reader (
                    formats.createReaderFor (mediaFolder().getChildFile (juce::String (name))));
                if (! reader) return;

                send (gesture::setNode ("/godot/cue/" + cueId + "/channels",
                                        std::to_string (reader->numChannels)));

                /*  THE FIRST DIRECT OUT, which is a default and not an
                    assignment: PRD 3.9b's "no auto-assignment, ever" is about
                    nothing choosing a DESTINATION for a cue, and a show with
                    one direct out has no choice to take. Where there are
                    several the lowest is where a rig starts, and the menu is
                    one click away. Where there are none the cue keeps its
                    file and says nothing about where it goes, which is
                    honest - and the line below says why it is silent, because
                    a cue that plays nothing with no explanation is the worst
                    of the three. */
                const auto lowest = firstDirectOut();

                if (lowest.empty())
                {
                    shell->transport.setNotice ("This show has no direct out yet, so the cue has "
                                                "nowhere to play - Show, Audio settings, Outputs.");
                    return;
                }

                send (gesture::setNode ("/godot/cue/" + cueId + "/directOut", lowest));
            }

            /*  Read from the pass's own snapshot, which `latest` is holding for
                exactly this: an import arrives between passes and has no
                reading of its own, and taking a second one would be a second
                call site the boundary check counts. */
            std::string firstDirectOut() const
            {
                if (latest == nullptr)
                    return {};

                for (const auto& row : model::readOutputs (*latest))
                    if (row.kind == "direct")
                        return row.id;

                return {};
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

                if (kind == "group" && ! selection.empty() && latest)
                {
                    if (! groupingCue.empty()) return;
                    std::string ids;
                    for (const auto& id : selection.ids()) { if (! ids.empty()) ids += ' '; ids += id; }
                    groupingCue = selection.ids().front();
                    for (auto parent = model::text (*latest, "/godot/cue/" + groupingCue + "/parent");
                         ! parent.empty(); parent = model::text (*latest, "/godot/cue/" + parent + "/parent"))
                        if (selection.contains (parent)) groupingCue = parent;
                    groupingParent = model::text (*latest, "/godot/cue/" + groupingCue + "/parent");
                    groupingWait = 0;
                    send ({ "window", "group.wrap", { osc::Value::string (ids) } });
                    return;
                }

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
                if (! groupingCue.empty())
                {
                    const auto parent = model::text (snapshot, "/godot/cue/" + groupingCue + "/parent");
                    if (! parent.empty() && parent != groupingParent
                        && model::text (snapshot, "/godot/cue/" + parent + "/kind") == "group")
                    { selection.set (parent); inspectNow(); groupingCue.clear(); }
                    else if (++groupingWait >= model::importPatience)
                    { groupingCue.clear(); shell->transport.setNotice ("Grouping the selected cues was refused."); }
                }
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
                        inspectNow();
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
            std::unique_ptr<ui::AudioSettingsWindow> audioSettings;
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
            std::string groupingCue, groupingParent;
            int groupingWait = 0;

            /** Which level of stop the next Esc means. */
            model::Panic panicPresses;

            /** How many were picked when the menu was last rebuilt, so it rebuilds when that moves. */
            std::size_t chosenAtLastMenu = 0;

            /** The engine's clipboard as last mirrored to the system's. */
            std::string clipboardSeen;

            /*  When the inspector may open for the current pick, and whether a
                box opened in the list holds it shut until the next pick. */
            juce::uint32 inspectorDueAt = 0;
            bool inspectorHeld = false;

            /** A pick that is not a click - all, or a cue just made - opens the inspector at once. */
            void inspectNow()
            {
                inspectorHeld = false;
                inspectorDueAt = 0;
            }

            std::string errorCueToInspect;
            void inspectCueError (const std::string& id)
            {
                if (! latest || model::isYes (model::flag (*latest, "/godot/document/locked"))) return;
                if (latest->find ("/godot/cue/" + id + "/kind") == nullptr)
                {
                    shell->transport.setNotice ("This cue has been deleted.");
                    return;
                }
                // Reveal its containing list and folded ancestors without
                // moving standby or launching the cue.
                auto child = id;
                for (int depth = 0; depth < 64; ++depth)
                {
                    const auto parent = model::text (*latest, "/godot/cue/" + child + "/parent");
                    if (parent.empty()) break;
                    const bool isList = latest->find ("/godot/list/" + parent + "/order") != nullptr;
                    const auto base = (isList ? "/godot/list/" : "/godot/cue/") + parent + "/";
                    const auto unfold = [&] (const std::string& address, const std::string& key)
                    {
                        if (model::isYes (model::flag (*latest, address)))
                        {
                            send (gesture::setNode (address, "false"));
                            if (show.isShut (key)) show.toggle (key);
                        }
                    };
                    if (! isList) unfold (base + "folded", parent);
                    for (const auto* section : { "header", "footer", "persistent" })
                    {
                        const auto members = model::words (model::text (*latest, base + section + "Order"));
                        if (std::find (members.begin(), members.end(), child) != members.end())
                            unfold (base + section + "Folded", parent + "/" + section);
                    }
                    if (isList)
                    {
                        if (parent != last.listId)
                            send ({ "window", "list.focus", { osc::Value::string (parent) } });
                        break;
                    }
                    child = parent;
                }
                if (loadingToTime) leaveLoadToTime();
                browsingUndo = false;
                errorCueToInspect = id;
                inspectNow();
            }

            /*  LOAD TO TIME (PRD §3.13; author, 2026-09-18). While it is on,
                the history panel stands where the inspector would, the steps
                the list took after the aimed cue sit under its row, a pick
                moves the aim, and only a GO takes it down - the menu item is
                the other way out, for the hand that changed its mind. */
            bool loadingToTime = false;
            std::string aimCue;
            double aimOffset = -1.0;

            void toggleLoadToTime()
            {
                if (loadingToTime)
                {
                    leaveLoadToTime();
                    return;
                }

                if (last.listId.empty())
                    return;

                loadingToTime = true;
                menuItemsChanged();

                /*  Opened on the picked cue, else the standby: the aim has to
                    name something for the panel to have anything to say. */
                const auto cue = ! selection.empty() ? selection.anchor() : last.standbyId;

                if (! cue.empty())
                    send (gesture::aim (last.listId, cue, -1.0));
            }

            void leaveLoadToTime()
            {
                if (! loadingToTime)
                    return;

                loadingToTime = false;
                aimCue.clear();
                menuItemsChanged();
            }

            /*  THE UNDO HISTORY (author, 2026-09-18). While the panel is up
                the show can be stood anywhere in its history - each move is
                that many undo or redo records - and the list shows what
                standing there changed against the picture taken when the
                panel opened. OK keeps where it stands; Cancel goes back. */
            bool browsingUndo = false;
            int undoOpenedAt = 0;
            model::Picture undoPictureAtOpen;
            model::UndoReading lastUndo;

            /*  THE PANEL OPENS ON THE PICKED CUE and shuts from the same
                place. It is not a mode: nothing else changes, the cue list
                keeps its selection, and GO still fires. */
            void toggleWaveform()
            {
                if (shell == nullptr)
                    return;

                if (shell->footSubject().isOpen())
                {
                    shell->setFoot ({});
                    menuItemsChanged();
                    return;
                }

                const auto picked = selection.anchor();

                if (picked.empty())
                    return;

                shell->setFoot ({ model::Subject::Kind::waveform, picked });
                menuItemsChanged();
            }

            void toggleUndoHistory()
            {
                if (browsingUndo)
                {
                    leaveUndoHistory();
                    return;
                }

                if (latest == nullptr)
                    return;

                leaveLoadToTime();
                lastUndo = model::readUndoHistory (*latest);
                undoOpenedAt = lastUndo.position();
                undoPictureAtOpen = model::pictureOf (show.rows());
                browsingUndo = true;
                menuItemsChanged();
            }

            void leaveUndoHistory()
            {
                if (! browsingUndo)
                    return;

                browsingUndo = false;
                menuItemsChanged();
            }

            /*  Stand after `index` transactions: undo down to it, or redo up
                to it, one record each, in one drain. The reading the count
                is taken from is the last pass's, which is what the panel
                showed the hand. */
            void standAt (int index)
            {
                if (refusedWhileLocked())
                    return;

                const auto at = lastUndo.position();

                for (auto n = at; n > index; --n)
                    send (gesture::undo());

                for (auto n = at; n < index; ++n)
                    send (gesture::redo());
            }

            void aimAtPick()
            {
                if (! loadingToTime || selection.empty())
                    return;

                const auto cue = selection.anchor();

                if (! cue.empty() && cue != aimCue)
                    send (gesture::aim (last.listId, cue, aimOffset));
            }

            /** Moves into a footer that did not exist yet, waiting for the tree to name it. */
            std::vector<FooterMove> footerMoves;

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
