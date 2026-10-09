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

#include <wfg/client/model/Video.h>
#include <wfg/client/model/Dual.h>
#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/LoadToTime.h>
#include <wfg/client/model/TemplateReview.h>
#include <wfg/client/model/UndoHistory.h>
#include <wfg/client/model/Media.h>
#include <wfg/client/model/NewCue.h>
#include <wfg/client/model/NewCueMenus.h>
#include <wfg/client/model/Panic.h>
#include <wfg/client/model/Readiness.h>
#include <wfg/client/model/Reorder.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/DirectOuts.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Selection.h>
#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/ui/NetworkMonitorWindow.h>
#include <wfg/client/ui/VideoMonitorWindow.h>
#include <wfg/client/ui/ImportWindow.h>
#include <wfg/client/ui/TemplateReviewWindow.h>
#include <wfg/client/ui/ShowSettingsWindow.h>
#include <wfg/client/ui/SurfacePanelComponent.h>
#include <wfg/client/ui/MainWindow.h>
#include <wfg/client/ui/MediaCopier.h>
#include <wfg/client/ui/NewCueMenu.h>
#include <wfg/client/ui/PluginEditors.h>
#include <wfg/client/ui/Shell.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/TakePictures.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/video/Ffmpeg.h>
#include <wfg/engine/video/FfmpegInstall.h>
#include <wfg/engine/video/Movie.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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
            menuLock, menuLoadToTime, menuUndoHistory, menuRecord, menuShowSettings,
            menuWaveform, menuSurfaces, menuNetworkMonitor, menuAssociate, menuGoDoh, menuNewPerformance, menuUpdateTemplate,
            menuImportAls, menuImportQlab,
            menuConvertUsed, menuConvertWhole, menuConvertUsedQuality, menuConvertWholeQuality,
            menuMovieSound, menuCancelConversion, menuDownloadFfmpeg,
            menuSaveTemplate, menuVideoMonitor, menuHideProjectors,

            /*  THE TEMPLATES APPLY TEMPLATE OFFERS, numbered from here in the
                order the show keeps them (namespace draft §38). */
            menuApplyTemplateFirst = 1000
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

                /*  DOH! (PRD §3.32): the command and nothing else (`doh`). */
                actions.doh             = [this] { doh(); };
                actions.undo            = [this] { send (gesture::undo()); };
                actions.redo            = [this] { send (gesture::redo()); };
                actions.save            = [this] { save(); };
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

                /*  THE PRESET GESTURES: alt-drop on an ancestor group, a
                    member dropped on its own group's header band (namespace
                    draft §30, QZ), a header line dragged, and ctrl/⌘-arrows
                    stepping through the ancestors. All are one write to the
                    cue's `preset` (model/Reorder.h).

                    AND THE FOOT SAYS WHAT HAPPENED, as the arrows' steps
                    always have: the band lights for a mark as it does for a
                    move, and a cue that stays where it was after a drop on a
                    header reads as a drop that failed unless somebody says it
                    was marked instead. */
                listActions.setPreset       = [this] (const std::string& cueId, const std::string& group)
                                              {
                                                  if (refusedWhileLocked())
                                                      return;

                                                  send (gesture::setNode ("/godot/cue/" + cueId + "/preset", group));
                                                  shell->transport.setNotice (group.empty()
                                                      ? juce::String ("no longer prepared ahead")
                                                      : "prepared in " + nameOf (group) + "'s header, and kept in its place");
                                              };
                listActions.presetStep      = [this] (int direction) { ladderStep (direction); };
                listActions.moveToFooter    = [this] (const std::string& cueId, const std::string& group)
                                              { moveIntoRole (cueId, group, "footer"); };
                listActions.moveToHeader    = [this] (const std::string& cueId, const std::string& group)
                                              { moveIntoRole (cueId, group, "header"); };
                listActions.moveToPersistent = [this] (const std::string& cueId, const std::string& listId)
                                               { moveIntoPersistent (cueId, listId); };

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
                                                      moved the cells from under the pointer.

                                                      AN INSPECTOR ALREADY OPEN STAYS OPEN (author,
                                                      2026-10-02): the list keeps its width, so there
                                                      is nothing to wait out, and closing it for the
                                                      wait only to open it again was a blink on every
                                                      change of pick. */
                                                  const auto open = shell->panel() == ui::Shell::Panel::inspector;
                                                  inspectorHeld = false;
                                                  inspectorDueAt = juce::Time::getMillisecondCounter()
                                                                   + (open || extend || toggle || id.empty()
                                                                        ? 0u
                                                                        : static_cast<juce::uint32> (juce::MouseEvent::getDoubleClickTimeout()));
                                              };
                //  A box opened in the list holds a closed inspector shut; an open one stays.
                listActions.editingBegan    = [this] { inspectorHeld = shell->panel() != ui::Shell::Panel::inspector; };
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

                /*  A CLICK ON A RUNNING CUE'S NAME AIMS THE SURFACES' ROTARIES
                    at it (author, 2026-09-25), and on the aimed one lets go. */
                runActions.aim = [this] (const std::string& cueId)
                                 { send (gesture::aimSurfaces (cueId)); };

                ui::InspectorComponent::Actions inspectorActions;

                /*  ONE COMMITTED FIELD IS ONE `node.set`, carrying the address
                    the NODE gave rather than one assembled here - which is what
                    keeps this inspector generic and keeps the command reachable
                    from the page's (§14.16, rule 3). */
                inspectorActions.set = [this] (const std::string& address, const std::string& text)
                                       { send (gesture::setNode (address, text)); };

                /*  AND ONE FIELD OVER SEVERAL CUES, ONE GESTURE (namespace
                    draft §30.11): one `node.setMany`, one step to undo. */
                inspectorActions.setAll = [this] (const std::vector<std::string>& addresses, const std::string& text)
                                          { send (gesture::setAll (addresses, text)); };

                /*  AN OSC CUE AND ITS MESSAGES MOVED TO ONE DEVICE (namespace
                    draft 45): one `node.setMany`, one step of Undo. */
                inspectorActions.setMany = [this] (const std::vector<std::pair<std::string, std::string>>& writes)
                                           { send (gesture::setNodes (writes)); };

                /*  CLOSING THE PANEL IS PICKING NOTHING, which is client state
                    like the folds and never reaches the engine. */
                inspectorActions.close = [this] { selection.clear(); };

                /*  A CLICK OR A TOUCH ON A NUMBER PUTS IT ON THE MASTER DIAL
                    (author, 2026-09-26) - here and at the foot. */
                inspectorActions.dial = [this] (const std::string& address) { dialTo (address); };

                /*  THE PANEL AT THE FOOT, ASKED FOR FROM THE CUE ITSELF. The
                    inspector hands back a word; the words are the ones
                    `model::Subject` spells, and an unknown one opens nothing
                    rather than guessing. */
                inspectorActions.openPanel = [this] (const std::string& cueId,
                                                     const std::string& subject)
                {
                    if (shell == nullptr || cueId.empty())
                        return;

                    const auto wanted = model::subjectKindFor (subject);

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

                inspectorActions.convertToHap = [this] (const std::string&, juce::Component& under)
                {
                    movieMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&under),
                                               [this] (int chosen)
                                               {
                                                   if (chosen != 0)
                                                       menuItemSelected (chosen, 0);
                                               });
                };

                inspectorActions.chooseFile = [this] (const std::string& cueId)
                                              { chooseFile (cueId); };

                /*  A TARGET TYPED AS A NUMBER OR A NAME is resolved to the
                    identifier the document stores (model/Reorder.h), against
                    the rows this window is drawing. An empty field clears the
                    target; a name nobody has, or two cues have, writes nothing
                    and says so. */
                inspectorActions.setCueRef  = [this] (const std::vector<std::string>& addresses, const std::string& text)
                                              {
                                                  if (addresses.empty())
                                                      return;

                                                  if (text.empty())
                                                  {
                                                      send (gesture::setAll (addresses, text));
                                                      return;
                                                  }

                                                  const auto id = model::resolveCueRef (text, show.rows());

                                                  if (id.empty())
                                                      shell->transport.setNotice ("no one cue is numbered or named "
                                                                                    + juce::String (text));
                                                  else
                                                      send (gesture::setAll (addresses, id));
                                              };

                /*  THE NEW-CUE ROW: one button per kind, one `cue.create` each,
                    landing where the pick says (model/NewCue.h). The bar
                    knows the kind; this knows the place. */
                ui::NewCueBarComponent::Actions newCueActions;

                newCueActions.create = [this] (const std::string& kind) { createCue (kind); };
                newCueActions.choose = [this] (const std::string& kind, juce::Component& button)
                {
                    chooseFromList (kind, button);
                };

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

                /*  AND A SEND MIXER OR AN EQ OVER SEVERAL PICKED CUES (namespace
                    draft §30.11): one `node.setMany` a frame, so a drag over six
                    cues is one record a frame and one step of Undo in all. */
                footActions.setMany = [this] (const std::vector<std::pair<std::string, std::string>>& writes)
                { send (gesture::setNodes (writes)); };

                /*  AN OSC CUE'S MESSAGES AND CURVES, from its table (namespace
                    draft 45). */
                footActions.createMessage = [this] (const std::string& cueId, const std::string& address,
                                                    const std::string& value)
                { send (gesture::createMessage (cueId, address, value)); };
                footActions.promoteMessage = [this] (const std::string& messageId)
                { send (gesture::promoteMessage (messageId)); };
                footActions.createCurve = [this] (const std::string& parentId, int arg)
                { send (gesture::createCurve (parentId, arg)); };
                footActions.curveArm = [this] (const std::string& cueId) { send (gesture::curveArm (cueId)); };
                footActions.curveFree = [this] { send (gesture::curveFree()); };
                footActions.curveRec = [this] (const std::string& curveId, bool on)
                { send (gesture::curveRec (curveId, on)); };
                footActions.curveRecord = [this] (double fromSeconds) { send (gesture::curveRecord (fromSeconds)); };
                footActions.curveStop = [this] { send (gesture::curveStop()); };
                footActions.close = [this]
                {
                    /*  SHUT MEANS SHUT, for the one subject that opens itself:
                        remembered against the cue it was showing, so picking
                        the same fade again leaves it closed and picking a
                        different one opens it. */
                    if (shell->footSubject().kind == model::Subject::Kind::curve
                          || shell->footSubject().kind == model::Subject::Kind::fade)
                        shutCurveFor = shell->footSubject().objectId;

                    shell->setFoot ({});
                };
                footActions.resizeBy = [this] (int pixels) { shell->growFoot (pixels); };
                footActions.dial = [this] (const std::string& address) { dialTo (address); };

                footActions.createRange = [this] (const std::string& cueId, double in, double out)
                                          { send (gesture::createRange (cueId, in, out)); };

                footActions.removeObject = [this] (const std::string& objectId)
                                           { send (gesture::deleteObject (objectId)); };

                footActions.splitRange = [this] (const std::string& cueId, double at)
                                         { send (gesture::splitRange (cueId, at)); };

                footActions.createSend = [this] (const std::string& cueId, const std::string& busId,
                                                 double level)
                                         { send (gesture::createSend (cueId, busId, level)); };

                /*  FLAT IS ONE COMMAND (Phase 9a): twenty-three rows back in one
                    transaction, which is one step to undo. */
                footActions.resetEq = [this] (const std::string& cueId)
                                      { send (gesture::eqReset (cueId)); };

                /*  COPY AND PASTE OF THE PART THE FOOT SHOWS (namespace draft
                    §38): its EQ, sends, chain, or time and loops. */
                footActions.copyPart = [this] (const std::string& part) { copyPartShown (part); };
                footActions.pastePart = [this] (const std::string& part) { pastePartShown (part); };

                /*  THE CHAIN'S TWO DOORS (author, 2026-09-25): switching an
                    entry of the set in for the first time is `fx.create`, and
                    the EQ box shows the same cue's EQ in this same foot. */
                footActions.createFx = [this] (const std::string& cueId, const std::string& pluginId)
                                       { send (gesture::createFx (cueId, pluginId)); };

                /*  THE TAKE PANEL'S FIVE PRESSES (Phase 9c): the take verbs,
                    as the D700's Rec and a transport cue send them. */
                footActions.pressTake = [this] (const std::string& verb, const std::string& channelId)
                                        { send (gesture::takePress (verb, channelId)); };

                footActions.keepTake = [this] (const std::string& channelId, bool asCue, const std::string& afterCue)
                                       { send (gesture::takeKeep (channelId, asCue, afterCue)); };

                footActions.openEqOn = [this] (const std::string& cueId)
                {
                    if (shell != nullptr && ! cueId.empty())
                        shell->setFoot ({ model::Subject::Kind::eq, cueId });
                };

                /*  A FADE'S MIXER OPENS ITS EQ AND ITS CURVE (namespace draft
                    §26): another subject on the same fade. */
                footActions.openSubject = [this] (const model::Subject& subject)
                {
                    if (shell != nullptr && subject.isOpen() && ! subject.objectId.empty())
                    {
                        shell->setFoot (subject);
                        menuItemsChanged();
                    }
                };

                footActions.openTakeOn = [this] (const std::string& cueId)
                {
                    if (shell != nullptr && ! cueId.empty())
                        shell->setFoot ({ model::Subject::Kind::take, cueId });
                };

                /*  EDIT... OPENS THE PLUGIN'S OWN WINDOW (author, 2026-09-25),
                    in a helper process the client keeps: a machine-local
                    thing, like a file chooser, so no command - what the window
                    then DOES is ordinary writes. Read against the last pass's
                    snapshot, as every gesture between passes is. */
                footActions.editPlugin = [this] (const std::string& cueId, const std::string& pluginId)
                {
                    if (editors != nullptr && latest != nullptr)
                        editors->edit (*latest, cueId, pluginId,
                                       model::isYes (model::flag (*latest, "/godot/document/locked")));
                };

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

                /*  THE LANE RECORDED FROM A FADER (§20.9), the same four named
                    commands a surface's Rec key and the network send. */
                footActions.laneArm = [this] (const std::string& cueId)
                                      { send (gesture::laneArm (cueId)); };

                footActions.laneFree = [this] { send (gesture::laneFree()); };

                footActions.laneRecord = [this] (double fromSeconds)
                                         { send (gesture::laneRecord (fromSeconds)); };

                footActions.laneStop = [this] { send (gesture::laneStop()); };

                auto content = std::make_unique<ui::Shell> (theme, std::move (actions),
                                                            std::move (listActions),
                                                            std::move (runActions),
                                                            std::move (inspectorActions),
                                                            std::move (newCueActions),
                                                            std::move (historyActions),
                                                            std::move (undoActions),
                                                            std::move (footActions));
                shell = content.get();

                /*  THE BAR'S TWO BUTTONS (2026-09-25): what a locked show rode
                    live, kept in the show as one undo step, or let go of. */
                shell->liveBar.setActions ({ [this] { send (gesture::keepLive()); },
                                             [this] { send (gesture::dropLive()); } });

                /*  THE PLUGINS' OWN WINDOWS: what they move is one node.set a
                    value, with origin window, like a slider; their close
                    buttons end their helpers; the show's keys pressed in them
                    come back to this window's own key handling. */
                ui::PluginEditors::Actions editing;
                editing.set = [this] (const std::string& address, const std::string& text)
                              { send (gesture::setNode (address, text)); };
                editing.createFx = [this] (const std::string& cueId, const std::string& pluginId)
                                   { send (gesture::createFx (cueId, pluginId)); };
                editing.capture = [this] (const std::string& fxId, const std::string& stateFile,
                                          const std::string& values)
                                  { send (gesture::captureFx (fxId, stateFile, values)); };
                editing.key = [this] (bool escape)
                {
                    if (shell != nullptr)
                        shell->keyPressed (juce::KeyPress (escape ? juce::KeyPress::escapeKey
                                                                  : juce::KeyPress::spaceKey));
                };
                editing.changed = [this]
                {
                    if (shell != nullptr && editors != nullptr)
                        shell->foot.setEditorWords (editors->words());
                };

                editors = std::make_unique<ui::PluginEditors> (std::move (editing), host.describePlugin,
                                                               host.pluginWorkFolder);

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

                /*  THE COPY UNDER WAY ENDS HERE, before anything it would
                    answer to goes: its part-file is removed, and what it would
                    have said is dropped (ui/MediaCopier.h). */
                copier.stop();
               #if JUCE_MAC
                juce::MenuBarModel::setMacMainMenu (nullptr);
               #endif
                if (window != nullptr)
                    window->setMenuBar (nullptr);
                juce::Desktop::getInstance().setDefaultLookAndFeel (nullptr);
                window.reset();
            }

            //======================================================================
            //  What the system asks of the window (Console.h, `Client`)
            void requestClose() override
            {
                closeRequested();
            }

            void bringToFront() override
            {
                if (window == nullptr)
                    return;

                if (window->isMinimised())
                    window->setMinimised (false);

                window->toFront (true);
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

                    /*  F9 FOR DOH! (PRD §3.32), printed here and answered by
                        the transport, which Shell::keyPressed asks first: it
                        holds the latch that makes a held F9 one Doh!. */
                    case menuGoDoh:      return { juce::KeyPress::F9Key, 0, 0 };
                    /*  No accelerator: these are reached through the menu only.
                        The surfaces window takes the number keys for its pads
                        once it is open, and needs no key of its own to open. */
                    case menuRevert:
                    case menuShowSettings:
                    case menuSurfaces:
                    case menuNetworkMonitor:
                    case menuVideoMonitor:
                    case menuHideProjectors:
                    case menuAssociate:
                    case menuNewPerformance:
                    case menuUpdateTemplate:
                    case menuImportAls:
                    case menuImportQlab:
                    case menuConvertUsed:
                    case menuConvertWhole:
                    case menuConvertUsedQuality:
                    case menuConvertWholeQuality:
                    case menuMovieSound:
                    case menuCancelConversion:
                    case menuDownloadFfmpeg:
                    case menuSaveTemplate:
                    case menuApplyTemplateFirst: break;
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

                    //  From the show, or from a performance used as a template.
                    case menuNewPerformance: return host.openWindow != nullptr && documentFolder().isDirectory();

                    //  A new show from an Ableton Live set (§29): nothing of this one is touched.
                    case menuImportAls: return host.importSets != nullptr && host.readImportScenes != nullptr
                                                 && host.openWindow != nullptr && ! importing;

                    //  And from a QLab workspace (§46), the same way.
                    case menuImportQlab: return host.importQlab != nullptr && host.readQlabLists != nullptr
                                                  && host.openWindow != nullptr && ! importing;

                    //  A performance of a show - with its template, or to be its first.
                    case menuUpdateTemplate: return host.compareWithTemplate != nullptr
                                                      && showAroundThisDocument().isNotEmpty()
                                                      && ! model::isYes (last.locked);
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
                    case menuShowSettings: return true;

                    /*  ALWAYS: under the lock - taking back a GO is running the
                        show, not editing it - and whether there is a GO it can
                        take back is the engine's to say, in its own sentence. */
                    case menuGoDoh:      return true;

                    /*  OFFERED UNDER THE LOCK TOO: riding a fader and pressing a
                        pad are playing the show, not editing it. */
                    case menuSurfaces:  return true;

                    /*  Whenever the engine has a tap to read: watching the wire
                        changes nothing, under the lock or not. */
                    case menuNetworkMonitor: return host.traffic != nullptr;
                    case menuVideoMonitor: return static_cast<bool> (host.canvasPictures);

                    /*  Whenever there is an engine to ask; under the lock too,
                        where it waits for the unlock rather than acting (§39). */
                    case menuHideProjectors: return latest != nullptr;

                    //  Linux's, where the console gives one (Console.h, `associate`).
                    case menuAssociate: return host.associate != nullptr;

                    /*  Offered for a media cue, and for shutting the panel
                        whatever is picked - a panel that could be opened and
                        not closed from the same place would be a trap. */
                    case menuWaveform:  return shell != nullptr
                                                 && (shell->footSubject().isOpen()
                                                     || ! selection.empty());

                    /*  A PICKED MOVIE, on a machine whose engine found FFmpeg
                        (namespace draft 37.5, WF-WJ); its conversion stopped
                        while it runs. */
                    case menuConvertUsed:
                    case menuConvertWhole:
                    case menuConvertUsedQuality:
                    case menuConvertWholeQuality:
                    case menuMovieSound:
                        return unlocked && ! pickedMovieFile().empty() && latest != nullptr
                                 && ! model::ffmpegPath (*latest).empty() && ! conversionRunning (pickedMovieFile());
                    case menuCancelConversion:
                        return ! pickedMovieFile().empty() && conversionRunning (pickedMovieFile());

                    //  Where nothing has it, and it is not on its way (37.5, WN).
                    case menuDownloadFfmpeg:
                        return latest != nullptr && model::ffmpegPath (*latest).empty()
                                 && ! model::readFfmpegInstall (*latest).running();

                    /*  A PICKED MEDIA OR VIDEO CUE, kept as a template, or the
                        templates of its kind stamped onto every picked cue
                        (namespace draft §38). */
                    case menuSaveTemplate:
                        return unlocked && ! templateKindOfAnchor().empty();
                    case menuApplyTemplateFirst:
                        return unlocked && ! selection.empty() && ! templatesForAnchor().empty();
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
                    addMenuItem (menu, menuNewPerformance, "New performance...");
                    addMenuItem (menu, menuUpdateTemplate, templateAroundThisDocument() ? "Update the show's template..."
                                                                                       : "Make this the show's template");
                    addMenuItem (menu, menuOpen, "Open show...");
                    addMenuItem (menu, menuImportAls, "Import Ableton Live set...");
                    addMenuItem (menu, menuImportQlab, "Import QLab workspace...");
                    menu.addSeparator();
                    addMenuItem (menu, menuSave, "Save");
                    addMenuItem (menu, menuSaveAs, "Save as...");
                    addMenuItem (menu, menuRevert, "Revert to saved...");

                    /*  LINUX ONLY: a tarball cannot tell the desktop what a .wfg
                        is, so this copy says so when asked (app/Associate.h).
                        Windows has its installer and the Mac its app. */
                   #if JUCE_LINUX
                    menu.addSeparator();
                    addMenuItem (menu, menuAssociate, "Open .wfg files with this Go.dot");
                   #endif
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

                    /*  CUE TEMPLATES (namespace draft §38): the picked cue's
                        settings kept under a name, and those of its kind
                        stamped onto the picked cues. */
                    addMenuItem (menu, menuSaveTemplate, "Save as template...");
                    {
                        juce::PopupMenu offered;
                        const auto rows = templatesForAnchor();

                        for (std::size_t at = 0; at < rows.size(); ++at)
                        {
                            juce::PopupMenu::Item entry { juce::String (rows[at].label()) };
                            entry.itemID = menuApplyTemplateFirst + static_cast<int> (at);
                            entry.isEnabled = menuItemEnabled (menuApplyTemplateFirst);
                            offered.addItem (entry);
                        }

                        menu.addSubMenu (selection.size() > 1 ? "Apply template to "
                                                                  + juce::String (static_cast<int> (selection.size())) + " cues"
                                                              : juce::String ("Apply template"),
                                         offered, ! rows.empty() && menuItemEnabled (menuApplyTemplateFirst));
                    }
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

                    /*  DOH! BESIDE THE LOCK, the two things here an operator
                        reaches for during a show rather than before it. */
                    addMenuItem (menu, menuGoDoh, "Doh! - take back the last GO");
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

                    /*  THE PICKED MOVIE TO HAP (namespace draft 37.5, WF-WJ):
                        the part its cues use or the whole file, Hap or Hap Q;
                        its sound brought in on its own; a conversion stopped. */
                    auto movie = movieMenu();
                    menu.addSubMenu ("Convert the movie to HAP", movie,
                                     ! pickedMovieFile().empty() || menuItemEnabled (menuDownloadFfmpeg));
                    menu.addSeparator();
                    addMenuItem (menu, menuRecord, model::isYes (last.recording) ? "Stop the live recorder"
                                                                                  : "Start the live recorder");
                    menu.addSeparator();
                    addMenuItem (menu, menuShowSettings, "Show settings...");

                    /*  THE VIRTUAL SURFACE: every strip the show declares, as a
                        desk the mouse can play (decision AB). Beside the
                        settings, where the surfaces it draws are declared. */
                    addMenuItem (menu, menuSurfaces, "Surfaces...");

                    /*  WHAT CROSSED THE WIRE (author, 2026-09-30): OSC and MIDI,
                        in and out, in a window of its own. */
                    menu.addSeparator();
                    addMenuItem (menu, menuNetworkMonitor, "Network monitor...");

                    /*  AND WHAT THE CANVASES SHOW (author, 2026-10-07: "a video
                        monitor window for the canvases ... opened by an item in
                        the show menu"). */
                    addMenuItem (menu, menuVideoMonitor, "Video monitor...");

                    /*  THE EXTERNAL SCREENS GIVEN BACK WHILE EDITING (author,
                        2026-10-08: "a toggle to not show the external screen
                        ... when the show is unlocked"; "once locked, in show
                        mode, then the video output has all its reasons" to
                        cover it): ticked while asked, and the lock overrules it. */
                    {
                        juce::PopupMenu::Item hide { "Hide the projectors while unlocked" };
                        hide.itemID = menuHideProjectors;
                        hide.isEnabled = menuItemEnabled (menuHideProjectors);
                        hide.isTicked = projectorsHidden();
                        menu.addItem (hide);
                    }
                }

                return menu;
            }

            void menuItemSelected (int itemId, int) override
            {
                switch (itemId)
                {
                    case menuNew:       chooseShowFolder (true); break;
                    case menuNewPerformance: askForANewPerformance(); break;
                    case menuImportAls: chooseLiveSets(); break;
                    case menuImportQlab: chooseQlabWorkspace(); break;
                    case menuUpdateTemplate:
                        if (templateAroundThisDocument())
                            reviewTemplate ({});
                        else if (host.makeTemplate)
                            shell->transport.setNotice (juce::String (host.makeTemplate().said));
                        break;
                    case menuOpen:      chooseShowFolder (false); break;
                    case menuSave:      save(); break;
                    case menuSaveAs:    if (host.emptyShowAtStart) chooseWhereTheEmptyShowLives();
                                        else chooseSaveAsFolder();
                                        break;
                    case menuRevert:    shell->transport.askThenRevert(); break;
                    case menuUndo:      send (gesture::undo()); break;
                    case menuRedo:      send (gesture::redo()); break;
                    case menuCut:       copyChosen(); removeChosen(); break;
                    case menuCopy:      copyChosen(); break;
                    case menuPaste:     pasteFromClipboard(); break;
                    case menuSelectAll: selection.all (show.rows()); inspectNow(); break;
                    case menuDeleteCue: removeChosen(); break;
                    case menuLock:      send (gesture::setLocked (! model::isYes (last.locked))); break;
                    case menuGoDoh:     doh(); break;
                    case menuLoadToTime: toggleLoadToTime(); break;
                    case menuUndoHistory: toggleUndoHistory(); break;
                    case menuWaveform:  toggleWaveform(); break;
                    case menuConvertUsed:         convertPicked ("used", "hap"); break;
                    case menuConvertWhole:        convertPicked ("whole", "hap"); break;
                    case menuConvertUsedQuality:  convertPicked ("used", "hapq"); break;
                    case menuConvertWholeQuality: convertPicked ("whole", "hapq"); break;
                    case menuMovieSound:
                        send (gesture::convertMovie (pickedMovieFile(), "whole", "none", true));
                        break;
                    case menuCancelConversion:
                        send (gesture::cancelConversion (pickedMovieFile()));
                        break;
                    case menuDownloadFfmpeg:
                        askToDownloadFfmpeg ({});
                        break;
                    case menuShowSettings:
                        openShowSettings();
                        break;
                    case menuNetworkMonitor:
                        openNetworkMonitor();
                        break;
                    case menuVideoMonitor:
                        openVideoMonitor();
                        break;
                    case menuHideProjectors:
                        send (gesture::hideProjectors (! projectorsHidden()));
                        break;
                    case menuAssociate:
                        if (host.associate)
                            shell->transport.setNotice (juce::String (host.associate()));
                        break;
                    case menuSurfaces:
                        /*  MADE ONCE AND KEPT, as the settings window is: closing it
                            hides it, and the next open is the same desk. Read at
                            once from the pass's own snapshot, so it opens drawn
                            rather than blank until the next pass. */
                        if (latest)
                        {
                            if (! surfaces)
                                surfaces = std::make_unique<ui::SurfaceWindow> (theme,
                                    [this] (Event event) { send (std::move (event)); }, [this] { panic(); });
                            surfaces->setVisible (true);
                            surfaces->toFront (true);
                            surfaces->refresh (*latest);
                        }
                        break;
                    case menuRecord:     send (model::isYes (last.recording) ? gesture::recordStop()
                                                                             : gesture::recordStart()); break;
                    case menuSaveTemplate: askToSaveTemplate(); break;
                    default:
                        if (itemId >= menuApplyTemplateFirst)
                        {
                            const auto rows = templatesForAnchor();
                            const auto at = static_cast<std::size_t> (itemId - menuApplyTemplateFirst);

                            if (at < rows.size() && ! refusedWhileLocked())
                            {
                                send (gesture::applyCueTemplate (rows[at].id, selection.ids()));
                                shell->transport.setNotice ("Template " + juce::String (rows[at].label()) + " applied to "
                                                              + juce::String (static_cast<int> (selection.size()))
                                                              + (selection.size() == 1 ? " cue" : " cues"));
                            }
                        }
                        break;
                }
            }

            /*  THE PICKED CUE'S TEMPLATE KIND (namespace draft §38): "media",
                a video cue's source, or empty for a cue no template is made of. */
            std::string templateKindOfAnchor() const
            {
                if (latest == nullptr || selection.anchor().empty())
                    return {};

                const auto base = "/godot/cue/" + selection.anchor() + "/";
                const auto kind = model::text (*latest, base + "kind");

                if (kind == "media")
                    return "media";

                if (kind == "video")
                {
                    const auto source = model::text (*latest, base + "source");
                    return source.empty() ? std::string ("fill") : source;
                }

                return {};
            }

            std::vector<model::CueTemplateRow> templatesForAnchor() const
            {
                if (latest == nullptr || selection.anchor().empty())
                    return {};

                const auto base = "/godot/cue/" + selection.anchor() + "/";
                return model::templatesFor (model::readCueTemplates (*latest), model::text (*latest, base + "kind"),
                                            model::text (*latest, base + "source"));
            }

            /*  SAVE AS TEMPLATE: a name asked for, the picked cue's own offered.
                A name a template of the same kind already has takes the cue's
                settings into that one - saving again is how a template is
                changed - and says so before it does. */
            void askToSaveTemplate()
            {
                if (refusedWhileLocked() || latest == nullptr)
                    return;

                const auto kind = templateKindOfAnchor();
                const auto cueId = selection.anchor();

                if (kind.empty())
                    return;

                auto* ask = new juce::AlertWindow ("Save as template",
                                                   "A template keeps this cue's settings - everything but its file,"
                                                   " its name and its times in the file - for new cues to be born"
                                                   " with. A cue made from it keeps what it was given: saving the"
                                                   " template again changes no cue already made.",
                                                   juce::MessageBoxIconType::NoIcon);
                ask->addTextEditor ("name", juce::String (model::text (*latest, "/godot/cue/" + cueId + "/name")),
                                    "Name");
                ask->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
                ask->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                ask->enterModalState (true, juce::ModalCallbackFunction::create (
                    [this, ask, kind, cueId, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int result)
                    {
                        const auto name = ask->getTextEditorContents ("name").trim().toStdString();

                        if (safe == nullptr || result != 1 || name.empty() || latest == nullptr)
                            return;

                        for (const auto& row : model::readCueTemplates (*latest))
                        {
                            if (row.name == name && row.kind == kind)
                            {
                                send (gesture::saveCueTemplate (row.id, cueId));
                                shell->transport.setNotice ("Template " + juce::String (name) + " saved again");
                                return;
                            }
                        }

                        send (gesture::createCueTemplate (name, cueId));
                        shell->transport.setNotice ("Template " + juce::String (name) + " saved");
                    }), true);
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

            /*  INTO A LIST'S PERSISTENT SECTION, made first if it has none
                (namespace draft §30, S5) - the same shape as a header or a
                footer, one level up: the section is the list's, so the tree
                names it at `/godot/list/<id>/persistent`, and it is made by
                `list.persistent` rather than `group.role`. The move waits in
                the same queue for the pass whose tree names the section.

                WHAT MAY GO IN WAS ASKED BEFORE THIS (`model::dropFor`, which
                refuses in words what the section would ignore), so nothing
                here makes a section for a cue that will not be put in it. */
            void moveIntoPersistent (const std::string& cueId, const std::string& listId)
            {
                if (refusedWhileLocked() || latest == nullptr || listId.empty())
                    return;

                const auto section = model::text (*latest, "/godot/list/" + listId + "/persistent");

                if (! section.empty())
                {
                    const auto members = static_cast<int> (model::words (
                        model::text (*latest, "/godot/list/" + listId + "/persistentOrder")).size());

                    send (gesture::moveObject (cueId, section, members));
                    shell->transport.setNotice ("into the persistent section");
                    return;
                }

                send (gesture::listPersistent (listId));
                footerMoves.push_back ({ cueId, listId, "persistent", 0 });
                shell->transport.setNotice ("making the persistent section");
            }

            /*  ONE QUEUE FOR THE THREE SECTIONS that are made on the way: a
                group's header and footer, and a list's persistent section,
                which `group` then names and whose node is the list's. */
            struct FooterMove
            {
                std::string cueId, group, role;
                int waited = 0;

                bool ofList() const { return role == "persistent"; }

                std::string address() const
                {
                    return (ofList() ? "/godot/list/" : "/godot/cue/") + group + "/" + role;
                }
            };

            juce::String movedWords (const FooterMove& job) const
            {
                return job.ofList() ? juce::String ("into the persistent section")
                                    : "into " + nameOf (job.group) + "'s " + juce::String (job.role);
            }

            juce::String refusedWords (const FooterMove& job) const
            {
                return job.ofList() ? juce::String ("the persistent section was refused")
                                    : "the " + juce::String (job.role) + " for " + nameOf (job.group)
                                        + " was refused";
            }

            void finishFooterMoves (const tree::TreeSnapshot& snapshot)
            {
                if (footerMoves.empty())
                    return;

                std::vector<FooterMove> waiting;

                for (auto& job : footerMoves)
                {
                    const auto footer = model::text (snapshot, job.address());

                    if (! footer.empty())
                    {
                        const auto members = static_cast<int> (model::words (model::text (snapshot, job.address() + "Order")).size());
                        send (gesture::moveObject (job.cueId, footer, members));
                        shell->transport.setNotice (movedWords (job));
                        continue;
                    }

                    if (++job.waited < model::importPatience)
                        waiting.push_back (job);
                    else
                        shell->transport.setNotice (refusedWords (job));
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

                /*  PART OF A CUE ON THE CLIPBOARD goes onto the picked cues
                    rather than in between them (namespace draft §38): the same
                    keys, the thing on the clipboard deciding what they do. */
                if (const auto clip = model::readPartClip (text.toStdString()); clip.isPart())
                {
                    pastePart (text.toStdString(), clip, selection.anchor());
                    return;
                }

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
                what `document.copy` made is what ctrl/⌘-V in any window reads.
                And its part clipboard the same way (namespace draft §38), so a
                part copied here pastes in another window - whichever of the two
                moved last is what the system's clipboard holds. */
            void mirrorClipboard (const tree::TreeSnapshot& snapshot)
            {
                const auto mirror = [] (const std::string& fragment, std::string& seen)
                {
                    if (fragment.empty() || fragment == seen)
                        return;

                    seen = fragment;
                    juce::SystemClipboard::copyTextToClipboard (juce::String (fragment));
                };

                mirror (model::text (snapshot, "/godot/document/clipboard"), clipboardSeen);
                mirror (model::text (snapshot, "/godot/document/partClipboard"), partClipboardSeen);
            }

            /*  PART OF A CUE, IN WORDS: what a notice and a tooltip call it. */
            static juce::String partWords (const std::string& part)
            {
                if (part == "eq")    return "EQ";
                if (part == "sends") return "sends";
                if (part == "fx")    return "effects";
                if (part == "time")  return "time and loops";

                return juce::String (part);
            }

            /*  COPY ON THE FOOT: the part of the cue the panel shows - the lead
                cue when it shows several. The engine keeps it and the tree
                brings it back, which is when it reaches the system's clipboard. */
            void copyPartShown (const std::string& part)
            {
                const auto cueId = shell->footSubject().objectId;

                if (cueId.empty())
                    return;

                send (gesture::copyPart (part, cueId));

                const auto name = latest != nullptr ? model::text (*latest, "/godot/cue/" + cueId + "/name")
                                                    : std::string {};
                shell->transport.setNotice (partWords (part) + " of " + (name.empty() ? juce::String ("the cue")
                                                                                      : juce::String (name))
                                              + " copied");
            }

            /*  PASTE ON THE FOOT: the part on the system's clipboard when it is
                this panel's part - copied here or in another window - else the
                one this engine holds, onto the panel's cues. */
            void pastePartShown (const std::string& part)
            {
                if (refusedWhileLocked() || latest == nullptr)
                    return;

                auto text = juce::SystemClipboard::getTextFromClipboard().toStdString();
                auto clip = model::readPartClip (text);

                if (clip.parts != part)
                {
                    text = model::text (*latest, "/godot/document/partClipboard");
                    clip = model::readPartClip (text);
                }

                if (clip.parts != part)
                {
                    shell->transport.setNotice ("no " + partWords (part) + " copied to paste");
                    return;
                }

                pastePart (text, clip, shell->footSubject().objectId);
            }

            /*  ONE PASTE OF A PART, onto the cues `model::pasteTargets` names
                for `panelCue` and the pick, said on the transport's line. */
            void pastePart (const std::string& text, const model::PartClip& clip, const std::string& panelCue)
            {
                if (refusedWhileLocked() || latest == nullptr)
                    return;

                const auto targets = model::pasteTargets (*latest, clip, panelCue, selection.ids());

                if (targets.empty())
                {
                    shell->transport.setNotice ("no picked cue takes " + partWords (clip.parts));
                    return;
                }

                send (gesture::pastePart (text, targets));
                shell->transport.setNotice (partWords (clip.parts) + " pasted onto "
                                              + juce::String (static_cast<int> (targets.size()))
                                              + (targets.size() == 1 ? " cue" : " cues"));
            }

            /*  WHETHER THE FOOT'S PASTE HAS ANYTHING TO PUT DOWN, and the
                sentence its tooltip says, from the part this engine holds. */
            void sayWhatPasteWouldDo (const tree::TreeSnapshot& snapshot, const model::Subject& subject)
            {
                const auto part = model::partForPanel (subject.kind);

                if (part.empty())
                    return;

                const auto clip = model::readPartClip (model::text (snapshot, "/godot/document/partClipboard"));

                if (clip.parts != part)
                {
                    shell->foot.setPasteable (false, "Nothing copied to paste here: Copy takes this cue's "
                                                       + partWords (part));
                    return;
                }

                const auto targets = model::pasteTargets (snapshot, clip, subject.objectId, selection.ids());
                const auto from = model::text (snapshot, "/godot/cue/" + clip.sourceId + "/name");
                const auto source = from.empty() ? juce::String ("a cue") : juce::String (from);

                if (targets.empty())
                {
                    shell->foot.setPasteable (false, "The " + partWords (part) + " of " + source
                                                       + " fits none of the cues here");
                    return;
                }

                shell->foot.setPasteable (true, "Paste the " + partWords (part) + " of " + source + " onto "
                                                  + juce::String (static_cast<int> (targets.size()))
                                                  + (targets.size() == 1 ? " cue, replacing its own"
                                                                         : " cues, replacing their own"));
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
            /*  "Go.dot - Hamlet", and for a performance its show's name before
                its own: "Go.dot - Hamlet - Paris" (§25, JJ). */
            static juce::String titleFor (const std::string& show, const juce::String& aroundShow = {})
            {
                /*  The dash as UTF-8 bytes, not a narrow literal: the title bar
                    showed "â□□" for it once the frame was the window's own. */
                const juce::String dash { juce::CharPointer_UTF8 (" \xe2\x80\x94 ") };

                return show.empty() ? juce::String ("Go.dot")
                                    : juce::String ("Go.dot") + dash
                                        + (aroundShow.isEmpty() ? juce::String() : aroundShow + dash)
                                        + juce::String (show);
            }

            /*  A NUMBER ON THE MASTER DIAL, from a click or a touch (author,
                2026-09-26): sent only where there is a dial to put it on and
                it is not on it already, so a click in a show with no surface,
                or a second click on the same box, writes nothing to the log.
                Read against the last pass's snapshot, as every gesture between
                passes is. */
            void dialTo (const std::string& address)
            {
                if (latest == nullptr || address.empty() || ! model::hasMasterDial (*latest))
                    return;

                if (model::text (*latest, "/godot/surface/dial") == address)
                    return;

                send (gesture::dial (address));
            }

            /*  THE SHOW SETTINGS WINDOW, made once and kept: closing it hides
                it, and the next open is the same window. From the menu, and
                once at start when the console was told to. */
            void openShowSettings()
            {
                if (! latest)
                    return;

                if (! audioSettings)
                    audioSettings = std::make_unique<ui::ShowSettingsWindow> (theme, *latest,
                        [this] (Event event) { send (std::move (event)); }, [this] { panic(); });

                audioSettings->setVisible (true);
                audioSettings->toFront (true);
            }

            bool openedSettingsAtStart = false;
            juce::Time firstPass;

            /*  THE NETWORK MONITOR, made once and kept like the settings: shut,
                it keeps its lines and the engine records nothing; opened, it
                switches the listening back on. */
            void openNetworkMonitor()
            {
                if (host.traffic == nullptr)
                    return;

                if (! networkMonitor)
                {
                    ui::NetworkMonitorWindow::Actions monitorActions;
                    monitorActions.listen = [tap = host.traffic] (bool on) { tap->setListening (on); };
                    monitorActions.panic = [this] { panic(); };

                    networkMonitor = std::make_unique<ui::NetworkMonitorWindow> (theme, std::move (monitorActions));
                }

                networkMonitor->open();
            }

            std::vector<wfg::monitor::Capture> trafficArrived;

            /*  THE VIDEO MONITOR (namespace draft 40), made once and kept like
                the network monitor: open, the renderer draws the canvases small
                for it; shut, it draws nothing for anybody. */
            void openVideoMonitor()
            {
                if (! host.canvasPictures)
                    return;

                if (videoMonitor == nullptr)
                {
                    ui::VideoMonitorWindow::Actions monitorActions;
                    monitorActions.monitor = host.monitorCanvases;
                    monitorActions.panic = [this] { panic(); };
                    videoMonitor = std::make_unique<ui::VideoMonitorWindow> (theme, std::move (monitorActions));
                }

                videoMonitor->open();
            }

            int videoMonitorPasses = 0;

            /*  THE MONITOR OPENED BY A PANEL (namespace draft §47, AAH): shown,
                but behind the show's window's keyboard. */
            void openVideoMonitorQuietly()
            {
                if (! host.canvasPictures)
                    return;

                if (videoMonitor == nullptr)
                {
                    ui::VideoMonitorWindow::Actions monitorActions;
                    monitorActions.monitor = host.monitorCanvases;
                    monitorActions.panic = [this] { panic(); };
                    videoMonitor = std::make_unique<ui::VideoMonitorWindow> (theme, std::move (monitorActions));
                }

                videoMonitor->openQuietly();
            }

            /*  THE PICKED CUE ON THE VIDEO MONITOR (namespace draft §47, AAH).
                The author, 2026-10-09: "We need to open the monitor window for
                the media when adjusting", and "When this panel is open the video
                monitor window could also be open"; asked, he chose the cue
                alone, large, as it will look on its canvas, playing or not.

                WHICH CUE: the one the picture panel is open on, or a movie
                whose strip is. WHICH SECOND of a movie: under an in or out
                point being dragged; else where it plays; else the strip's
                playhead; else where it starts. The monitor opens with the
                panel - without the keyboard, which stays where GO is - and
                shuts with it when the panel opened it; shut by a hand while the
                panel is open, it stays shut until the panel moves on. */
            void followCueTile (const tree::TreeSnapshot& snapshot)
            {
                if (! host.previewCue)
                    return;

                const auto subject = shell->footSubject();
                const auto base = "/godot/cue/" + subject.objectId + "/";
                const auto isVideo = subject.isOpen() && model::text (snapshot, base + "kind") == "video";
                const auto isMovie = isVideo && model::text (snapshot, base + "source") == "movie";
                const auto wanted = (subject.kind == model::Subject::Kind::picture && isVideo)
                                      || (subject.kind == model::Subject::Kind::waveform && isMovie);

                std::string cue;
                double seconds = 0.0;
                juce::String caption;

                if (wanted)
                {
                    cue = subject.objectId;
                    const auto number = model::text (snapshot, base + "number");
                    caption = juce::String (number.empty() ? std::string {} : "Cue " + number + " ")
                              + juce::String (model::text (snapshot, base + "name"));

                    if (isMovie)
                    {
                        const char* how = "where it starts";

                        if (const auto edge = shell->foot.heldEdge(); edge.has_value())
                        {
                            seconds = *edge;
                            how = nullptr;
                        }
                        else if (footReading.running && footReading.subject.objectId == cue)
                        {
                            seconds = footReading.position;
                            how = "playing";
                        }
                        else if (const auto head = shell->foot.stripPlayhead(); head.has_value() && *head > 0.0)
                        {
                            seconds = *head;
                            how = "at the playhead";
                        }
                        else
                        {
                            const auto ranges = model::readRanges (snapshot, cue);
                            seconds = ! ranges.empty() ? ranges.front().in
                                                       : osc::parseDouble (model::text (snapshot, base + "startOffset")).value_or (0.0);
                        }

                        const auto whole = static_cast<int> (std::floor (seconds / 60.0));
                        const auto rest = seconds - 60.0 * whole;
                        caption << " - " << juce::String (whole) << ":" << (rest < 10.0 ? "0" : "")
                                << juce::String (rest, 2) << ", "
                                << (how != nullptr ? juce::String (how)
                                                   : juce::String (shell->foot.heldEdgeWord()) + " being dragged");
                    }
                }

                if (cue != tileCueAsked || std::abs (seconds - tileSecondsAsked) > 0.0005)
                {
                    host.previewCue (cue, seconds);
                    tileCueAsked = cue;
                    tileSecondsAsked = seconds;
                }

                //  The monitor with the panel.
                if (! cue.empty())
                {
                    const auto key = model::wordFor (subject.kind) + "/" + cue;

                    if (videoMonitor != nullptr && monitorOpenedByPanel && ! videoMonitor->watching())
                    {
                        monitorShutFor = key;
                        monitorOpenedByPanel = false;
                    }

                    if ((videoMonitor == nullptr || ! videoMonitor->watching()) && monitorShutFor != key)
                    {
                        openVideoMonitorQuietly();
                        monitorOpenedByPanel = videoMonitor != nullptr && videoMonitor->watching();
                    }
                }
                else
                {
                    if (monitorOpenedByPanel && videoMonitor != nullptr && videoMonitor->watching())
                        videoMonitor->closeButtonPressed();

                    monitorOpenedByPanel = false;
                    monitorShutFor.clear();
                }

                if (videoMonitor == nullptr || ! videoMonitor->watching())
                    return;

                if (cue.empty())
                {
                    if (cueTileShown)
                    {
                        videoMonitor->showCue ({}, {});
                        cueTileShown = false;
                        cueTileBytes.clear();
                    }

                    return;
                }

                //  Ten times a second, as the canvases are read.
                if (! host.cueTile || ++cueTilePasses % std::max (1, juce::roundToInt (theme.refreshHz / 10.0)) != 0)
                    return;

                const auto tile = host.cueTile();

                if (tile.width > 0 && tile.height > 0 && tile.rgb != cueTileBytes)
                {
                    cueTileBytes = tile.rgb;
                    cueTileImage = juce::Image (juce::Image::RGB, tile.width, tile.height, false, juce::SoftwareImageType());
                    juce::Image::BitmapData pixels (cueTileImage, juce::Image::BitmapData::writeOnly);

                    for (int row = 0; row < tile.height; ++row)
                        for (int column = 0; column < tile.width; ++column)
                        {
                            const auto* rgb = tile.rgb.data() + 3 * (row * tile.width + column);
                            pixels.setPixelColour (column, row, juce::Colour (rgb[0], rgb[1], rgb[2]));
                        }

                    shell->foot.setCuePicture (cueTileImage);
                }

                videoMonitor->showCue (caption, tile.width > 0 ? cueTileImage : juce::Image());
                cueTileShown = true;
            }

            model::FootReading footReading;
            std::string tileCueAsked;
            double tileSecondsAsked = 0.0;
            bool monitorOpenedByPanel = false;
            std::string monitorShutFor;
            int cueTilePasses = 0;
            bool cueTileShown = false;
            std::vector<unsigned char> cueTileBytes;
            juce::Image cueTileImage;

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

                /*  THE FOURTH DOOR'S ONE CALL SITE (Console.h, `traffic`): what
                    crossed the wire since the last pass, while the monitor is
                    open and recording - and nothing read otherwise. */
                if (networkMonitor != nullptr && networkMonitor->listening() && host.traffic != nullptr)
                {
                    trafficArrived.clear();
                    host.traffic->drain (trafficArrived);
                    networkMonitor->add (trafficArrived, host.traffic->dropped());
                }

                /*  A SHOW THAT HAS JUST BEEN MADE, or the empty one a launcher
                    opens, starts on its settings (`--show-settings`,
                    Console.h) - once, on the first pass with a reading to
                    build them from. */
                if (host.openSettingsAtStart && ! openedSettingsAtStart)
                {
                    /*  BUT THE LAUNCHER'S EMPTY SHOW ONLY WHEN THERE IS
                        SOMETHING TO SET (namespace draft §39, the author's
                        niggle of 2026-10-07: "Opening a file shows the show
                        preferences systematically even when everything is
                        set"). Its interface open is everything an empty show
                        needs, so the window waits a few seconds for the engine
                        to open it and opens only if it did not - a first launch,
                        a device unplugged, sound turned off. A new show still
                        opens on its settings at once: its outputs are its own. */
                    if (firstPass == juce::Time())
                        firstPass = juce::Time::getCurrentTime();

                    const auto sounding = ! model::text (*snapshot, "/godot/audio/device").empty();
                    const auto waited = (juce::Time::getCurrentTime() - firstPass).inSeconds();

                    if (! host.emptyShowAtStart || (! sounding && waited > 4.0))
                    {
                        openedSettingsAtStart = true;
                        openShowSettings();
                    }
                    else if (sounding)
                    {
                        openedSettingsAtStart = true;
                    }
                }

                /*  THE CANVASES FOR THE VIDEO MONITOR, ten times a second while it
                    is open - as often as the renderer draws them (§40). */
                if (videoMonitor != nullptr && videoMonitor->watching() && host.canvasPictures
                      && ++videoMonitorPasses % std::max (1, juce::roundToInt (theme.refreshHz / 10.0)) == 0)
                {
                    std::vector<ui::VideoMonitorWindow::Tile> tiles;

                    for (const auto& canvas : model::readCanvases (*snapshot))
                        tiles.push_back ({ canvas.id, canvas.label(), canvas.width, canvas.height });

                    videoMonitor->show (std::move (tiles), host.canvasPictures());
                }

                /*  THE VIRTUAL SURFACE, from the same pointer: rule 2's one call
                    site feeds every window. It reads nothing while hidden. */
                if (surfaces) surfaces->refresh (*snapshot);

                if (reading.show != last.show)
                    window->setName (titleFor (reading.show, showAroundThisDocument()));

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

                //  How ready each row's cue is for GO (namespace draft §48, AAM).
                shell->cues.setReadiness (model::readinessOf (*snapshot, show.rows()));

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
                /*  AND SINCE 2026-10-03 IT OPENS ITS MIXER (namespace draft
                    §26, PG): what the fade moves and where to, as sliders, is
                    what a fade is now - the curve is a door from there. A fade
                    whose own EQ or curve is already open keeps it. */
                if (! selection.anchor().empty()
                      && model::text (*snapshot, "/godot/cue/" + selection.anchor() + "/kind") == "fade"
                      && shutCurveFor != selection.anchor()
                      && shell->footSubject().kind != model::Subject::Kind::fade
                      && shell->footSubject().kind != model::Subject::Kind::curve
                      && ! (shell->footSubject().kind == model::Subject::Kind::eq
                              && shell->footSubject().objectId == selection.anchor()))
                {
                    shell->setFoot ({ model::Subject::Kind::fade, selection.anchor() });
                    menuItemsChanged();
                }

                if (shutCurveFor != selection.anchor())
                    shutCurveFor.clear();

                /*  A PICK IN THE WINDOW AIMS THE SURFACES (author, 2026-10-09,
                    namespace draft §47, AAA: "when a cue is selected for
                    editing, the controller's EQ, sends, FX buttons should act
                    as physical short cuts ... Expand it to other media cue and
                    direct UI selection"). Until then only a strip's SELECT and
                    a running cue's name aimed them (2026-09-25's first answer,
                    which this overrules). When the pick MOVES, and only then,
                    so a SELECT pressed afterwards keeps its aim until the hand
                    picks again: the last hand to aim wins. A sound or a mic
                    aims at itself, a movie at its locked sound, anything else
                    leaves the aim alone (`model::aimForPick`). Sent only to a
                    show with a surface declared - the aim is a logged command,
                    and a show with no surface has nothing to aim. */
                if (selection.anchor() != pickAimSeen)
                {
                    pickAimSeen = selection.anchor();

                    if (! model::text (*snapshot, "/godot/surface/order").empty())
                    {
                        const auto aim = model::aimForPick (*snapshot, pickAimSeen);

                        if (! aim.empty() && aim != model::text (*snapshot, "/godot/surface/aim"))
                            send (gesture::aimSurfaces (aim));
                    }
                }

                /*  A SURFACE ADJUSTING A CUE HOLDS THE FOOT ON IT (author,
                    2026-09-25: "When adjusting either EQ or send levels
                    display the footer on screen"): the aimed cue's EQ panel
                    for an EQ page, its send mixer for a Send page, from the
                    press that puts the page up (2026-10-05, namespace draft
                    §30.5). Since 2026-10-09 the list's pick aims the surfaces
                    too (just above), so the held foot follows a new pick by
                    way of the aim. When the page comes down the foot goes back
                    to what it was showing. */
                const auto page = model::readSurfacePage (*snapshot);
                const auto held = model::footForSurface (page.up, page.word, page.aim);

                if (held.isOpen())
                {
                    if (! surfaceHoldsFoot)
                    {
                        footBeforeSurface = shell->footSubject();
                        surfaceHoldsFoot = true;
                    }

                    if (shell->footSubject() != held)
                    {
                        shell->setFoot (held);
                        menuItemsChanged();
                    }

                    //  And the band the rotary last turned, ringed on the panel.
                    if (held.kind == model::Subject::Kind::eq && page.edited != lastSurfaceEdit)
                        shell->foot.showEditedEqHandle (model::eqHandleForAddress (page.aim, page.edited));
                }
                else if (surfaceHoldsFoot && ! page.up)
                {
                    surfaceHoldsFoot = false;
                    shell->setFoot (footBeforeSurface);
                    menuItemsChanged();
                }

                lastSurfaceEdit = page.edited;

                /*  AND WHAT A LOCKED SHOW IS RIDING LIVE, in the bar under the
                    transport: said while locked, Keep or Discard once not. */
                shell->setLive (static_cast<int> (osc::parseDouble (model::text (*snapshot, "/godot/document/live"))
                                                     .value_or (0.0)),
                                model::isYes (reading.locked));

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

                    if (! surfaceHoldsFoot && model::followsPick (subject.kind) && ! picked.empty())
                    {
                        /*  A MOVIE AND ITS SOUND ARE ONE CUE (namespace draft
                            §47, AAD): an EQ follows a pick of either line to
                            the sound, a strip to the movie. */
                        auto wanted = model::footCueForPick (*snapshot, subject.kind, picked);

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
                        if ((subject.kind == model::Subject::Kind::curve || subject.kind == model::Subject::Kind::fade)
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

                    /*  THE THIRD DOOR, read here and only here, and only while
                        the take panel is what is open: a take's picture is
                        built on this thread when it is asked for, and nothing
                        else in the window draws one. */
                    const auto takePictures = subject.kind == model::Subject::Kind::take && host.takes != nullptr
                                                  ? host.takes->snapshot()
                                                  : nullptr;

                    /*  OVER THE SELECTION, for a send mixer or an EQ
                        (namespace draft §30.11): every media and mic cue
                        picked, the anchor's values drawn. Not while a
                        surface's page holds the foot - a page edits the one
                        cue it is aimed at, and the panel shows that one. */
                    footReading = model::readFoot (*snapshot, subject,
                                                   surfaceHoldsFoot ? std::vector<std::string> {} : selection.ids());
                    shell->foot.show (footReading, mediaTable, takePictures);

                    sayWhatPasteWouldDo (*snapshot, subject);
                }

                //  The picked cue on the video monitor, while its strip or its picture is open (§47, AAH).
                followCueTile (*snapshot);

                /*  AND EVERY OPEN PLUGIN WINDOW FOLLOWS THE PICK, from this
                    same snapshot; the lock closes them. */
                if (editors != nullptr)
                    editors->follow (*snapshot, selection.anchor(), model::isYes (reading.locked));

                //  What a move just did to the moved cue's own output, if anything.
                sayIfTheMoveClashed (*snapshot, reading.revision);

                //  And any import or create still waiting for the cue it made.
                followImports (*snapshot, reading.revision);
                followConversions (*snapshot);
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
                    shell->inspector.show (model::inspectMany (*snapshot, selection.ids(), selection.anchor()));

                /*  WHICH PANEL IS OPEN AT THE FOOT, and on which cue, so the
                    inspector's panel buttons are lit while theirs is. */
                shell->inspector.showFoot (model::wordFor (shell->footSubject().kind),
                                           shell->footSubject().objectId);

                /*  THE MASTER DIAL'S NUMBER, marked wherever it is drawn. */
                const auto dialed = model::text (*snapshot, "/godot/surface/dial");
                shell->inspector.showDial (dialed);
                shell->foot.showDial (dialed);

                //  The file row's way to HAP (§39).
                shell->inspector.showHap (hapOfferNow());

                last = reading;
            }

            void timerCallback() override
            {
                pass();
                followTheEmptyShowsSave();
                followTheNewPerformance();
                followTheSaveBeforeReview();
                followTheCopy();
                followTheClose();
            }

            /*  THE WINDOW GOES ONCE THE SHOW IS CLEAN (WW): saved, or put back
                to the folder by the revert - which is when the engine has taken
                `recovery/` away, so the exit leaves nothing to offer. A save that
                fails says so and the window stays; a show that is still not
                clean after a while closes anyway, its autosave kept and offered
                at the next open, which is the safe way round. */
            void followTheClose()
            {
                if (! closeOnceClean.has_value())
                    return;

                if (closeOnceClean->saving && ! last.writeError.empty() && last.writeError != closeOnceClean->errorBefore)
                {
                    shell->transport.setNotice ("the show was not saved, so the window stays: " + juce::String (last.writeError));
                    closeOnceClean.reset();
                    return;
                }

                const auto waited = (juce::Time::getCurrentTime() - closeOnceClean->since).inSeconds();

                if (last.dirty == model::Flag::no || waited > 10.0)
                {
                    closeOnceClean.reset();

                    if (host.quit)
                        host.quit();
                }
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

            /*  DOH! (PRD §3.32): the command and nothing else - the button,
                F9 and the Show menu's one door. A notice of its own would stand
                in front of the engine's refusal when there is one, and the
                refusal is the news; what the Doh did arrives as its report
                (D4). (2026-10-03, D4's review, OJ: in an audio outage too - the
                engine says what waits for the clock, on that same readout and
                only for a press it accepted; this said it for every press, a
                refused one included.) */
            void doh()
            {
                send (gesture::doh());
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

                    /*  HOW LONG IT WILL TAKE, said in the show's own number
                        (2026-09-28): a fade the operator did not know about
                        reads as an Esc that did not work. */
                    const auto fade = latest != nullptr ? model::text (*latest, "/godot/audio/panicFade")
                                                        : std::string {};
                    const auto seconds = juce::String (fade).getDoubleValue();

                    shell->transport.setNotice (seconds > 0.0
                        ? "Esc: every cue fading out over " + juce::String (fade) + " s, footers run"
                          " - Esc again cuts at once"
                        : juce::String ("Esc: every cue stopping, footers run - Esc again drops everything"));
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

            /*  A FILE DROPPED ONTO A MEDIA CUE NAMES THAT CUE'S FILE, and asks
                first when the cue already plays something - the author's own
                condition on this gesture. A name already in `media/` is the
                import's question now (namespace draft §30, S7): the same
                three answers, asked the same way, and silence when the file
                there is this one byte for byte. With nothing at stake there is
                no question, because a dialogue nobody needs is one people learn
                to dismiss unread. */
            void linkMedia (const std::string& cueId, const juce::String& path)
            {
                if (refusedWhileLocked())
                    return;

                const juce::File source { path };

                if (mediaFolder() == juce::File())
                {
                    shell->transport.setNotice (juce::String (model::noFolderWords (source.getFileName().toStdString())));
                    return;
                }

                const auto already = latest == nullptr
                                       ? std::string {}
                                       : model::text (*latest, "/godot/cue/" + cueId + "/file");

                if (already.empty())
                {
                    linkInto (cueId, source);
                    return;
                }

                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                .withTitle ("Use " + source.getFileName() + "?")
                                                .withMessage ("This cue plays " + juce::String (already) + ".")
                                                .withButton ("Replace")
                                                .withButton ("Leave it")
                                                .withAssociatedComponent (window.get()),
                                              [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                               this, cueId, source] (int result)
                                              {
                                                  if (result == 1 && safe != nullptr)
                                                      linkInto (cueId, source);
                                              });
            }

            /*  THE LINK, queued behind any import under way: looked at, copied
                off this thread, and the cue named when it lands. */
            void linkInto (const std::string& cueId, const juce::File& source)
            {
                imports.link (cueId, source.getFullPathName().toStdString());
                followImports();
            }

            /*  AND FILES DROPPED ANYWHERE ELSE MAKE CUES: one per file, in the
                order they were dropped, each named after its own.

                WHERE THEY GO IS READ HERE, out of the members the show has -
                the pane knows the rows it drew, which is a different number -
                and turned into the member the cues follow (model::MediaImports),
                so a long copy cannot move it.

                NOTHING IS COPIED HERE (§30, S7). The files are queued, a worker
                reads and copies them one at a time, and each cue is asked for
                when its file has landed - the create on this thread, as every
                command is - and named a tick later, once the tree shows it: the
                identifier is the engine's to draw, never a client's. */
            void importMedia (const std::string& parent, int index,
                              const juce::StringArray& files, const std::string& cueTemplate = {})
            {
                if (refusedWhileLocked())
                    return;

                if (parent.empty())
                {
                    shell->transport.setNotice ("no list to import into");
                    return;
                }

                if (files.isEmpty())
                    return;

                if (mediaFolder() == juce::File())
                {
                    shell->transport.setNotice (juce::String (model::noFolderWords (
                        model::mediaNameFor (files[0].toStdString()))));
                    return;
                }

                /*  A SAMPLER HOLDS SOUNDS AND PICTURES (§39, §49): what is
                    dropped among its members goes where it was put - a movie
                    with its sound locked to it, beside it. */
                std::vector<std::string> sources;

                for (const auto& path : files)
                    sources.push_back (path.toStdString());

                imports.add (parent, index, orderOf (parent), sources, cueTemplate);
                followImports();
            }

            /*  THE GO-BETWEEN, run on every pass and whenever the worker or a
                question answers: the cue asked for found and named, the next
                one asked for, each failure said in its own words, a file handed
                to the worker when it has none, the next question asked, and
                where the copying has got to on the foot - once per file, so
                anything else said meanwhile stays readable. */
            void followImports()
            {
                if (latest != nullptr)
                    followImports (*latest, last.revision);
            }

            void followImports (const tree::TreeSnapshot& snapshot, std::uint64_t revision)
            {
                if (imports.idle() || shell == nullptr)
                    return;

                const auto steps = imports.follow (snapshot, revision);

                for (const auto& naming : steps.namings)
                {
                    send (gesture::setNode ("/godot/cue/" + naming.cueId + "/file", naming.mediaName));

                    /*  A MOVIE IS ASKED ABOUT (namespace draft 37.5, WF, WJ):
                        once FFmpeg has said what it is. */
                    if (model::isMovieFile (naming.mediaName))
                    {
                        movieOffers.push_back (naming.mediaName);
                        probeNextMovie();
                        continue;
                    }

                    if (const auto nowhere = routeImportedMedia (naming.cueId, naming.mediaName); nowhere.isNotEmpty())
                        importWarning = nowhere;
                }

                //  Sent in this pass: the model has recorded it as asked.
                /*  A PICTURE MAKES A PICTURE CUE (namespace draft 36), on the
                    show's first canvas; a sound a media cue, as ever. */
                if (steps.create.has_value())
                {
                    if (model::isVisualFile (steps.create->mediaName))
                    {
                        const auto canvases = latest != nullptr ? model::readCanvases (*latest)
                                                                : std::vector<model::CanvasRow> {};
                        std::vector<std::pair<std::string, std::string>> bornWith {
                            { "source", model::isMovieFile (steps.create->mediaName) ? "movie" : "picture" } };

                        if (! canvases.empty())
                            bornWith.push_back ({ "canvas", canvases.front().id });

                        send (gesture::createCue (steps.create->parent, steps.create->index, "video",
                                                  steps.create->cueName, bornWith));
                    }
                    else if (! steps.create->cueTemplate.empty())
                    {
                        //  Born from the template the files were chosen for (namespace draft §38).
                        send (gesture::createCueFrom (steps.create->parent, steps.create->index,
                                                      steps.create->cueTemplate, steps.create->cueName));
                    }
                    else
                    {
                        send (gesture::createCue (steps.create->parent, steps.create->index, "media",
                                                  steps.create->cueName));
                    }
                }

                for (const auto& sentence : steps.said)
                {
                    shell->transport.setNotice (juce::String (sentence));
                    importSaid.clear();
                }

                /*  THE LAST WORD OF AN IMPORT, which stays: what it did, what
                    went wrong first, and what the show still lacks for the
                    cues to sound. */
                for (const auto& ended : steps.ended)
                {
                    auto sentence = juce::String (ended.sentence);

                    if (ended.made > 0)
                    {
                        sentence << silenceWarning();

                        if (importWarning.isNotEmpty())
                            sentence << ". " << importWarning;
                    }

                    importWarning.clear();
                    importSaid.clear();
                    shell->transport.setNotice (sentence);
                }

                handOutWork();
                askAboutClash (snapshot);

                if (const auto now = imports.progress(); ! now.empty() && now != importSaid)
                {
                    importSaid = now;
                    shell->transport.setNotice (juce::String (now));
                }
            }

            /*  ONE FILE TO THE WORKER WHEN IT HAS NONE, into the folder the show
                has now - a Save As while the files copy sends the rest to the
                new one. */
            void handOutWork()
            {
                while (! copier.busy())
                {
                    const auto job = imports.nextJob();

                    if (! job.has_value())
                        return;

                    const auto folder = mediaFolder();

                    if (folder != juce::File())
                    {
                        copier.start (*job, folder);
                        return;
                    }

                    model::MediaWork nowhere;
                    nowhere.found = model::Found::noFolder;
                    imports.worked (nowhere);
                }
            }

            /*  ANOTHER FILE OF THE SAME NAME IS IN THE SHOW: the three answers,
                Keep both on Return because it loses nothing, and "Use the one
                in the show" on Escape because it changes nothing (SJ). One
                question per file, in the order picked; while more of the same
                import may meet a name, a tick answers them all at once.

                IT COMES WITHIN MOMENTS OF THE DROP, never minutes into a copy:
                every file is looked at before any is copied (SI), because a box
                takes the keys GO is on. */
            void askAboutClash (const tree::TreeSnapshot& snapshot)
            {
                if (askingAboutClash || window == nullptr)
                    return;

                const auto asked = imports.asking();

                if (! asked.has_value())
                    return;

                /*  THE NAME KEEP BOTH WOULD GIVE, read off the folder as it is
                    now - the worker reads it again when it copies, and lands on
                    the same one unless something else took it first. */
                const auto folder = mediaFolder();
                std::vector<std::string> present;

                for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
                    present.push_back (file.getFileName().toStdString());

                const auto keptAs = model::freeName (asked->picked, [&present, &folder] (const std::string& candidate)
                {
                    return ! model::nameAmong (present, candidate).empty()
                        || folder.getChildFile (juce::String (candidate)).exists();
                });

                const auto question = model::clashWords (asked->picked, asked->met,
                                                         model::cuesPlaying (snapshot, asked->met), keptAs);

                auto* box = new juce::AlertWindow (juce::String (question.title), juce::String (question.message),
                                                   juce::MessageBoxIconType::QuestionIcon, window.get());
                box->addButton ("Keep both", 1, juce::KeyPress (juce::KeyPress::returnKey));
                box->addButton ("Replace it", 2);
                box->addButton ("Use the one in the show", 3, juce::KeyPress (juce::KeyPress::escapeKey));

                std::shared_ptr<juce::ToggleButton> forTheRest;

                if (asked->more)
                {
                    forTheRest = std::make_shared<juce::ToggleButton> ("The same for the other files whose names are taken");
                    forTheRest->setSize (360, 24);
                    box->addCustomComponent (forTheRest.get());
                }

                askingAboutClash = true;

                box->enterModalState (true, juce::ModalCallbackFunction::create (
                    [this, forTheRest, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int answer)
                    {
                        if (safe == nullptr)
                            return;

                        askingAboutClash = false;

                        const auto given = answer == 1 ? model::Clash::keepBoth
                                         : answer == 2 ? model::Clash::replace
                                                       : model::Clash::useTheShows;

                        imports.answer (given, forTheRest != nullptr && forTheRest->getToggleState());
                        followImports();
                    }), true);
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

            /*  A SURFACE HOLDING THE FOOT (2026-09-25): whether one is, what
                the foot showed before it took it, and the address its page
                last wrote - so the ringed band moves only when that does. */
            bool surfaceHoldsFoot = false;
            model::Subject footBeforeSurface;
            std::string lastSurfaceEdit;

            /*  The pick the surfaces were last aimed from (§47, AAA): the aim
                is sent when the pick moves, not every pass. */
            std::string pickAimSeen;

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

            /*  Answers what the cue still lacks to sound, in a sentence, or
                nothing - said at the end of the import rather than here, where
                the next file's progress would cover it (§30, S7). */
            //======================================================================
            /*  A MOVIE AND ITS HAP (namespace draft 37.5, WF-WJ).

                AN IMPORTED MOVIE IS ASKED ABOUT once FFmpeg has said what it
                is - on a thread of its own, since a file on a network share may
                take a while to answer. Not HAP: convert the whole file now, or
                later from the Show menu; Hap or Hap Q. With sound: bring it in
                as a cue locked to the movie. One question at a time, in the
                order the movies came. */
            void probeNextMovie()
            {
                if (probingMovie || askingAboutMovie || movieOffers.empty() || mediaFolder() == juce::File())
                    return;

                probingMovie = true;
                const auto name = movieOffers.front();
                const auto path = mediaFolder().getChildFile (juce::String::fromUTF8 (name.c_str())).getFullPathName().toStdString();
                const juce::Component::SafePointer<ui::MainWindow> safe (window.get());

                std::thread ([this, safe, name, path]
                {
                    const auto tools = video::ffmpeg::find();
                    auto probed = video::ffmpeg::probe (tools, path);

                    /*  NO FFMPEG HERE: Go.dot's own reader can still say whether
                        it is HAP, which plays as it is. */
                    if (! tools.found())
                    {
                        video::movie::MovieFile movie;
                        std::string why;
                        probed.ok = movie.open (path, why) && movie.info().isHap();
                        probed.codec = probed.ok ? "hap" : std::string {};
                    }

                    juce::MessageManager::callAsync ([this, safe, name, probed, found = tools.found()]
                    {
                        if (safe == nullptr)
                            return;

                        probingMovie = false;

                        if (! movieOffers.empty())
                            movieOffers.pop_front();

                        askAboutMovie (name, probed, found);
                        probeNextMovie();
                    });
                }).detach();
            }

            void askAboutMovie (const std::string& name, const video::ffmpeg::Probe& probed, bool ffmpeg)
            {
                const auto shown = juce::String::fromUTF8 (name.c_str());

                /*  NO FFMPEG HERE: offered once a session, and the movie asked
                    about again when it has come (37.5, WN). */
                if (! ffmpeg)
                {
                    if (! probed.isHap())
                    {
                        moviesWaitingForFfmpeg.push_back (name);

                        if (! ffmpegOffered)
                            askToDownloadFfmpeg (name);
                    }

                    return;
                }

                if (! probed.ok)
                {
                    shell->transport.setNotice (shown + ": " + juce::String::fromUTF8 (probed.why.c_str()));
                    return;
                }

                const auto convert = ! probed.isHap();

                if (! convert && ! probed.sound)
                    return;

                const auto title = convert ? juce::String ("Convert this movie to HAP?")
                                           : juce::String ("Bring in this movie's sound?");
                auto message = convert
                    ? shown + " is " + juce::String (probed.codec) + ", which Go.dot plays only as a preview. Converting"
                        " makes a HAP copy beside it in the show's media, in the background, and its cue then plays"
                        " the HAP. It can also be done later, from Show > Convert the movie to HAP."
                    : shown + " has sound.";

                if (probed.sound)
                    message << "\n\nIts sound can come in as a sound cue locked to the movie: they start, stop and"
                               " loop together, and the sound's level, routing and EQ are its own.";

                auto* box = new juce::AlertWindow (title, message, juce::MessageBoxIconType::QuestionIcon, window.get());

                std::shared_ptr<juce::ComboBox> format;

                if (convert)
                {
                    format = std::make_shared<juce::ComboBox>();
                    format->addItem ("Hap", 1);
                    format->addItem ("Hap Q - finer, about twice the size", 2);
                    format->setSelectedId (1, juce::dontSendNotification);
                    format->setSize (360, 24);
                    box->addCustomComponent (format.get());
                }

                std::shared_ptr<juce::ToggleButton> sound;

                if (probed.sound)
                {
                    sound = std::make_shared<juce::ToggleButton> ("Bring its sound in as a cue locked to the movie");
                    sound->setToggleState (true, juce::dontSendNotification);
                    sound->setSize (360, 24);
                    box->addCustomComponent (sound.get());
                }

                box->addButton (convert ? "Convert the whole file" : "Bring the sound in", 1,
                                juce::KeyPress (juce::KeyPress::returnKey));
                box->addButton (convert ? "Later" : "Not now", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                askingAboutMovie = true;

                box->enterModalState (true, juce::ModalCallbackFunction::create (
                    [this, name, convert, format, sound, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int answer)
                    {
                        if (safe == nullptr)
                            return;

                        askingAboutMovie = false;
                        const auto withSound = sound != nullptr && sound->getToggleState();

                        if (answer == 1 && convert)
                            send (gesture::convertMovie (name, "whole", format != nullptr && format->getSelectedId() == 2 ? "hapq" : "hap",
                                                         withSound));
                        else if (withSound && (answer == 1 || convert))
                            send (gesture::convertMovie (name, "whole", "none", true));

                        probeNextMovie();
                    }), true);
            }

            /*  FFMPEG DOWNLOADED ON FIRST USE (namespace draft 37.5, WN): what it
                is for, how large, where from, and that it is free software. */
            void askToDownloadFfmpeg (const std::string& forMovie)
            {
                ffmpegOffered = true;

                if (! video::ffmpeg::canDownload())
                {
                    shell->transport.setNotice ("FFmpeg was not found, and there is no build to download for this machine:"
                                                " install it, or put ffmpeg and ffprobe beside Go.dot.");
                    return;
                }

                auto message = juce::String (forMovie.empty() ? "" : juce::String::fromUTF8 (forMovie.c_str()) + " is not HAP. ")
                             + "Go.dot needs FFmpeg to convert movies to HAP and to preview them. It can download it now"
                               " - about 90 MB, from " + juce::String (video::ffmpeg::downloadSource())
                             + " - and keep it in its own folder. FFmpeg is free software, under the GPL.";

                auto* box = new juce::AlertWindow ("Download FFmpeg?", message, juce::MessageBoxIconType::QuestionIcon, window.get());
                box->addButton ("Download", 1, juce::KeyPress (juce::KeyPress::returnKey));
                box->addButton ("Not now", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                box->enterModalState (true, juce::ModalCallbackFunction::create (
                    [this, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int answer)
                    {
                        if (safe != nullptr && answer == 1)
                            send (gesture::installFfmpeg());
                    }), true);
            }

            /*  The file of the movie picked, or empty: a video cue showing one. */
            /*  THE PICKED MOVIE'S CONVERSIONS, as one list: the Show menu's
                submenu, and what the file row's "-> HAP" opens under itself
                (§39) - one list, so the two can never offer different things. */
            juce::PopupMenu movieMenu()
            {
                juce::PopupMenu movie;
                addMenuItem (movie, menuConvertUsed, "The part the cues use, as Hap");
                addMenuItem (movie, menuConvertWhole, "The whole file, as Hap");
                addMenuItem (movie, menuConvertUsedQuality, "The part the cues use, as Hap Q");
                addMenuItem (movie, menuConvertWholeQuality, "The whole file, as Hap Q");
                movie.addSeparator();
                addMenuItem (movie, menuMovieSound, "Bring its sound in, locked to it");
                addMenuItem (movie, menuCancelConversion, "Stop its conversion");
                movie.addSeparator();
                addMenuItem (movie, menuDownloadFfmpeg, "Download FFmpeg...");
                return movie;
            }

            /*  WHETHER A MOVIE PLAYS AS A PREVIEW - through FFmpeg, not as HAP
                (namespace draft 37.6) - which nothing in the tree says: the
                renderer decides it by opening the file. The window asks the
                same question of the file's header, once per file and again only
                when the file changes, since the panel asks it on every pass. A
                file not there is offered nothing. */
            bool playsAsPreview (const std::string& file)
            {
                if (file.empty() || mediaFolder() == juce::File())
                    return false;

                const auto path = mediaFolder().getChildFile (juce::String::fromUTF8 (file.c_str()));
                const auto stamp = path.getLastModificationTime().toMilliseconds();

                if (const auto known = previewAnswers.find (file);
                    known != previewAnswers.end() && known->second.first == stamp)
                    return known->second.second;

                auto preview = path.existsAsFile();

                if (preview && path.hasFileExtension ("mov"))
                {
                    video::movie::MovieFile movie;
                    std::string why;
                    preview = ! (movie.open (path.getFullPathName().toStdString(), why) && movie.info().isHap());
                }

                previewAnswers[file] = { stamp, preview };
                return preview;
            }

            std::map<std::string, std::pair<juce::int64, bool>> previewAnswers;

            /*  WHAT THE FILE ROW'S BUTTON SAYS (§39): "-> HAP" on a movie played
                as a preview, its conversion's progress while there is one, and
                nothing on anything else. Asked each pass, from this pass's
                snapshot. */
            ui::InspectorComponent::HapOffer hapOfferNow()
            {
                ui::InspectorComponent::HapOffer offer;
                const auto file = pickedMovieFile();

                if (file.empty() || latest == nullptr)
                    return offer;

                for (const auto& row : model::readConversions (*latest))
                    if (row.file == file && row.running())
                    {
                        offer.shown = true;
                        offer.words = row.state == "waiting" ? juce::String ("HAP: waiting")
                                                             : "HAP " + juce::String (row.percent) + " %";
                        offer.tooltip = "Converting to HAP. Press to stop it.";
                        return offer;
                    }

                if (! playsAsPreview (file))
                    return offer;

                offer.shown = true;
                offer.tooltip = "This movie plays as a preview. Convert it to HAP, which plays at full quality";

                for (const auto& row : model::readConversions (*latest))
                    if (row.file == file && ! row.problem.empty())
                        offer.tooltip = "The last conversion failed: " + juce::String (row.problem);

                return offer;
            }

            //  Whether the engine holds the projectors away while unlocked (§39).
            bool projectorsHidden() const
            {
                return latest != nullptr && model::isYes (model::flag (*latest, "/godot/videoOutput/projectorsHidden"));
            }

            std::string pickedMovieFile() const
            {
                const auto picked = selection.anchor();

                if (picked.empty() || latest == nullptr)
                    return {};

                const auto base = "/godot/cue/" + picked + "/";

                if (model::text (*latest, base + "kind") != "video" || model::text (*latest, base + "source") != "movie")
                    return {};

                return model::text (*latest, base + "file");
            }

            bool conversionRunning (const std::string& file) const
            {
                if (latest == nullptr || file.empty())
                    return false;

                for (const auto& row : model::readConversions (*latest))
                    if (row.file == file && row.running())
                        return true;

                return false;
            }

            void convertPicked (const std::string& scope, const std::string& format)
            {
                if (const auto file = pickedMovieFile(); ! file.empty())
                    send (gesture::convertMovie (file, scope, format, false));
            }

            /*  WHAT EACH CONVERSION IS DOING, said on the transport's line when
                it changes - started, each tenth of the way, done, failed. */
            void followConversions (const tree::TreeSnapshot& snapshot)
            {
                /*  FFMPEG ON ITS WAY, said as it comes; once it is here, the
                    movies that waited for it are asked about. */
                const auto installing = model::readFfmpegInstall (snapshot);

                if (const auto news = model::installNews (ffmpegInstallSeen, installing); ! news.empty() && shell != nullptr)
                    shell->transport.setNotice (juce::String::fromUTF8 (news.c_str()));

                if (installing.state == "done" && ffmpegInstallSeen.state != "done")
                {
                    for (const auto& name : moviesWaitingForFfmpeg)
                        movieOffers.push_back (name);

                    moviesWaitingForFfmpeg.clear();
                    probeNextMovie();
                }

                ffmpegInstallSeen = installing;

                for (const auto& row : model::readConversions (snapshot))
                {
                    const auto seen = conversionsSeen.find (row.file);
                    const auto* before = seen != conversionsSeen.end() ? &seen->second : nullptr;

                    if (const auto news = model::conversionNews (before, row); ! news.empty() && shell != nullptr)
                        shell->transport.setNotice (juce::String::fromUTF8 (news.c_str()));

                    conversionsSeen[row.file] = row;
                }
            }

            juce::String routeImportedMedia (const std::string& cueId, const std::string& name)
            {
                // Read the copied file's header off the tick/audio threads.
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                const std::unique_ptr<juce::AudioFormatReader> reader (
                    formats.createReaderFor (mediaFolder().getChildFile (juce::String (name))));
                if (! reader) return {};

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
                /*  A TEMPLATE THAT SAID WHERE IT PLAYS is not overruled
                    (namespace draft §38): the cue was born with its direct out. */
                if (latest != nullptr && ! model::text (*latest, "/godot/cue/" + cueId + "/directOut").empty())
                    return {};

                const auto lowest = firstDirectOut();

                if (lowest.empty())
                    return "This show has no direct out yet, so the cue has "
                           "nowhere to play - Show, Show settings, Outputs.";

                send (gesture::setNode ("/godot/cue/" + cueId + "/directOut", lowest));
                return {};
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
                    offer a file the show would then fail on - and a video
                    cue's are the renderer's picture readers (Phase 8a). */
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();

                const auto picture = latest != nullptr && model::text (*latest, "/godot/cue/" + cueId + "/kind") == "video";

                chooser = std::make_unique<juce::FileChooser> (
                            picture ? "Choose the picture or movie this cue shows" : "Choose the media this cue plays",
                            mediaDialogFolder(), picture ? juce::String (model::pictureWildcard())
                                                         : formats.getWildcardForAllFormats());

                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this, cueId] (const juce::FileChooser& answered)
                                      {
                                          const auto chosen = answered.getResult();

                                          if (safe == nullptr || ! chosen.existsAsFile())
                                              return;

                                          rememberMediaFolder (chosen);
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

                /*  OPEN TAKES THE SHOW'S `.wfg` AS WELL AS ITS FOLDER: the
                    console resolves either (Console.h, `openWindow`). Windows'
                    own picker chooses folders or files and never both, so
                    there it is a file picker - into the show's folder, then
                    its .wfg, as any document is opened on Windows. */
               #if JUCE_WINDOWS
                const auto openFlags = juce::FileBrowserComponent::canSelectFiles;
               #else
                const auto openFlags = juce::FileBrowserComponent::canSelectDirectories
                                         | juce::FileBrowserComponent::canSelectFiles;
               #endif

                chooser = std::make_unique<juce::FileChooser> (
                            createNew ? "Choose an empty folder for the new show"
                                      : "Choose a show (its folder or its .wfg)",
                            showsFolder(),
                            createNew ? juce::String() : juce::String ("*.wfg"));

                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | (createNew ? juce::FileBrowserComponent::canSelectDirectories
                                                     : openFlags),
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this, createNew] (const juce::FileChooser& answered)
                                      {
                                          const auto folder = answered.getResult();

                                          if (safe == nullptr || folder == juce::File())
                                              return;

                                          const auto refused = host.openWindow (folder.getFullPathName().toStdString(),
                                                                                createNew);

                                          shell->transport.setNotice (refused.empty()
                                                                        ? "opening " + (folder.hasFileExtension ("wfg") ? folder.getFileNameWithoutExtension()
                                                                                                                        : folder.getFileName())
                                                                            + " in a new window"
                                                                        : juce::String (refused));

                                          /*  THE LAUNCHER'S EMPTY SHOW GIVES WAY (Console.h,
                                              `emptyShowAtStart`): the show on its way takes
                                              this window's place while nothing has been done
                                              here - nothing to save, and nothing to undo, so
                                              an empty show somebody worked in and saved is
                                              never closed under them. A refusal leaves it. */
                                          if (refused.empty() && host.emptyShowAtStart && host.quit
                                                && ! last.hasSomethingToSave() && last.canUndo == model::Flag::no)
                                              host.quit();

                                          /*  AND ONE THAT STAYS TAKES ITS SETTINGS OUT OF THE
                                              WAY (§39): they were about its first moment, and
                                              left up they sat beside the show just opened as if
                                              they were that show's. */
                                          else if (refused.empty() && host.emptyShowAtStart && audioSettings != nullptr)
                                              audioSettings->closeButtonPressed();
                                      });
            }

            //======================================================================
            /*  A SHOW AND ITS PERFORMANCES (namespace draft §25). A Show is the
                piece and a Performance each event of it - a whole bundle folded
                inside the show's folder (JJ). Which this window's document is
                is a fact about the disk: a document whose folder sits in a
                folder holding a .wfg is a performance of that show. */
            juce::File documentFolder() const
            {
                if (latest == nullptr)
                    return {};

                const auto path = model::text (*latest, "/godot/document/path");
                return path.empty() ? juce::File() : juce::File (juce::String (path));
            }

            //  A template cue list: a .wfg of the show folder's own.
            static bool holdsATemplate (const juce::File& folder)
            {
                return folder.isDirectory()
                         && folder.getNumberOfChildFiles (juce::File::findFiles, "*.wfg") > 0;
            }

            /*  A show folder: a template, or the media its performances share -
                the template is optional (§25), and "around" is where a sound
                is found (audio/MediaInfo.h). */
            static bool holdsAShow (const juce::File& folder)
            {
                return holdsATemplate (folder) || folder.getChildFile ("media").isDirectory();
            }

            //  The show around this document, when it is a performance; empty when it is a show.
            juce::String showAroundThisDocument() const
            {
                const auto document = documentFolder();
                const auto around = document.getParentDirectory();
                return document.isDirectory() && around != document && holdsAShow (around) ? around.getFileName()
                                                                                          : juce::String();
            }

            bool templateAroundThisDocument() const
            {
                return showAroundThisDocument().isNotEmpty() && holdsATemplate (documentFolder().getParentDirectory());
            }

            /*  WHAT GOES BACK INTO THE SHOW'S TEMPLATE (§25, author 2026-10-02):
                on close and on demand, never on save. The comparison reads the
                files, so a performance with unsaved changes is offered "Save,
                then review" first. `afterwards` runs once the review is done -
                an update made, or nothing to bring - and is how closing goes on
                to its own question; a Cancel anywhere runs nothing. */
            void reviewTemplate (std::function<void()> afterwards)
            {
                if (last.dirty != model::Flag::yes)
                {
                    openTemplateReview (std::move (afterwards));
                    return;
                }

                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                .withTitle ("Save this performance first?")
                                                .withMessage ("What goes back into the show's template is read from what is "
                                                              "saved, and this performance has changes that are not.")
                                                .withButton ("Save, then review")
                                                .withButton ("Review what is saved")
                                                .withButton ("Cancel")
                                                .withAssociatedComponent (window.get()),
                                              [this, afterwards, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int answer)
                                              {
                                                  if (safe == nullptr)
                                                      return;

                                                  if (answer == 1)
                                                  {
                                                      send (gesture::save());
                                                      reviewAfterSave = ReviewAfterSave { afterwards, juce::Time::getCurrentTime(),
                                                                                          last.writeError };
                                                  }
                                                  else if (answer == 2)
                                                  {
                                                      openTemplateReview (afterwards);
                                                  }
                                              });
            }

            void followTheSaveBeforeReview()
            {
                if (! reviewAfterSave.has_value())
                    return;

                if (! last.writeError.empty() && last.writeError != reviewAfterSave->errorBefore)
                {
                    shell->transport.setNotice ("the performance was not saved: " + juce::String (last.writeError));
                    reviewAfterSave.reset();
                    return;
                }

                if (last.dirty == model::Flag::no)
                {
                    auto afterwards = reviewAfterSave->afterwards;
                    reviewAfterSave.reset();
                    openTemplateReview (std::move (afterwards));
                }
                else if ((juce::Time::getCurrentTime() - reviewAfterSave->since).inSeconds() > 20.0)
                {
                    shell->transport.setNotice ("the performance was not saved in time to review it");
                    reviewAfterSave.reset();
                }
            }

            void openTemplateReview (std::function<void()> afterwards)
            {
                if (! host.compareWithTemplate)
                    return;

                const auto comparison = host.compareWithTemplate();

                if (! comparison.ok)
                {
                    shell->transport.setNotice (juce::String (comparison.problem));
                    if (afterwards) afterwards();
                    return;
                }

                if (comparison.changes.empty())
                {
                    shell->transport.setNotice ("this performance and the show's template agree");
                    if (afterwards) afterwards();
                    return;
                }

                ui::TemplateReviewWindow::Actions actions;

                actions.update = [this, afterwards] (const model::TemplateReview& review)
                {
                    const auto picks = review.picks();
                    const auto sounds = review.localSounds();

                    if (sounds.empty())
                    {
                        finishTemplateUpdate (picks, false, afterwards);
                        return;
                    }

                    juce::String named;
                    for (const auto& sound : sounds)
                        named << (named.isEmpty() ? "" : ", ") << juce::String (sound);

                    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                    .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                    .withTitle ("Copy the sounds into the show?")
                                                    .withMessage ("Only this performance has " + named
                                                                  + ". Copy them into the show's media, so every performance "
                                                                    "made from the template has them - or bring the cues without them.")
                                                    .withButton ("Copy them")
                                                    .withButton ("Without them")
                                                    .withButton ("Cancel")
                                                    .withAssociatedComponent (templateReview.get()),
                                                  [this, picks, afterwards] (int answer)
                                                  {
                                                      if (answer == 1 || answer == 2)
                                                          finishTemplateUpdate (picks, answer == 1, afterwards);
                                                  });
                };

                actions.cancel = [this] { closeTemplateReview(); };

                templateReview = std::make_unique<ui::TemplateReviewWindow> (theme, model::TemplateReview (comparison.changes),
                                                                             showAroundThisDocument(), std::move (actions));
                templateReview->setVisible (true);
                templateReview->toFront (true);
            }

            void finishTemplateUpdate (const std::vector<TemplatePick>& picks, bool copySounds, std::function<void()> afterwards)
            {
                const auto updated = host.updateTemplate ? host.updateTemplate (picks, copySounds)
                                                         : TemplateUpdate { false, "this build cannot update a template" };
                shell->transport.setNotice (juce::String (updated.said));
                closeTemplateReview();

                if (updated.ok && afterwards)
                    afterwards();
            }

            //  From inside one of its own buttons, so later rather than now.
            void closeTemplateReview()
            {
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ui::MainWindow> (window.get()), this]
                                                 {
                                                     if (safe != nullptr)
                                                         templateReview.reset();
                                                 });
            }

            /*  NEW PERFORMANCE (§25, JI): a copy of this window's document - the
                show, or a performance used as a template - into a folder of its
                own inside the show's, opened in its own window. Named first,
                today's date suggested, since a performance is most often a date
                and a place. The copy is one `document.saveAs`, the copy the
                engine already makes, followed from the timer as the empty
                show's save is (followTheNewPerformance). */
            //======================================================================
            /*  AN ABLETON LIVE SET, IMPORTED INTO A NEW SHOW (namespace draft §29):
                the sets picked - several are a tour, a performance each - then
                the scenes ticked and where the show goes (QS), then the import,
                off the message thread, saying where it has got to in the foot;
                and the new show opened in a window of its own, with its report. */
            void chooseLiveSets()
            {
                if (! host.readImportScenes || ! host.importSets || importing)
                    return;

                chooser = std::make_unique<juce::FileChooser> ("Choose an Ableton Live set - several for a tour, "
                                                               "a performance each",
                                                               showsFolder(), "*.als");

                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()), this]
                                      (const juce::FileChooser& answered)
                                      {
                                          const auto files = answered.getResults();

                                          if (safe == nullptr || files.isEmpty())
                                              return;

                                          offerScenes (files);
                                      });
            }

            void offerScenes (const juce::Array<juce::File>& files)
            {
                std::vector<std::string> sets;
                juce::StringArray names;

                for (const auto& file : files)
                {
                    sets.push_back (file.getFullPathName().toStdString());
                    names.add (file.getFileNameWithoutExtension());
                }

                const auto scenes = host.readImportScenes (sets);

                if (! scenes.error.empty())
                {
                    shell->transport.setNotice (juce::String::fromUTF8 (scenes.error.c_str()));
                    return;
                }

                /*  THE SHOW'S NAME TO START: the set's, or what a tour's sets
                    share - "Lazzi régie" for "Lazzi régie Pau" and the rest. */
                auto name = names[0];

                if (names.size() > 1)
                {
                    auto shared = names[0];

                    for (const auto& other : names)
                        while (shared.isNotEmpty() && ! other.startsWith (shared))
                            shared = shared.dropLastCharacters (1);

                    if (shared.trim().isNotEmpty())
                        name = shared.trim();
                }

                ui::ImportWindow::Actions actions;
                actions.import = [this, sets] (const std::vector<int>& ticked, const juce::File& into)
                {
                    startImport (sets, ticked, into, host.importSets);
                };
                actions.cancel = [this] { closeImportWindow(); };

                importWindow = std::make_unique<ui::ImportWindow> (theme, names, scenes, showsFolder(), name,
                                                                   std::move (actions));
                importWindow->setVisible (true);
                importWindow->toFront (true);
            }

            /*  A QLAB WORKSPACE INTO A NEW SHOW (namespace draft §46.3): the
                workspace picked, then its cue lists ticked and where the show goes,
                then the import off the message thread, as a Live set's. */
            void chooseQlabWorkspace()
            {
                if (! host.readQlabLists || ! host.importQlab || importing)
                    return;

                chooser = std::make_unique<juce::FileChooser> ("Choose a QLab 4 or QLab 5 workspace", showsFolder(),
                                                               "*.qlab4;*.qlab5");

                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()), this]
                                      (const juce::FileChooser& answered)
                                      {
                                          const auto file = answered.getResult();

                                          if (safe == nullptr || file == juce::File())
                                              return;

                                          offerQlabLists (file);
                                      });
            }

            void offerQlabLists (const juce::File& file)
            {
                const std::vector<std::string> sets { file.getFullPathName().toStdString() };
                const auto lists = host.readQlabLists (sets.front());

                if (! lists.error.empty())
                {
                    shell->transport.setNotice (juce::String::fromUTF8 (lists.error.c_str()));
                    return;
                }

                ui::ImportWindow::Actions actions;
                actions.import = [this, sets] (const std::vector<int>& ticked, const juce::File& into)
                {
                    startImport (sets, ticked, into, host.importQlab);
                };
                actions.cancel = [this] { closeImportWindow(); };

                importWindow = std::make_unique<ui::ImportWindow> (theme, juce::StringArray { file.getFileNameWithoutExtension() },
                                                                   lists, showsFolder(), file.getFileNameWithoutExtension(),
                                                                   std::move (actions));
                importWindow->setVisible (true);
                importWindow->toFront (true);
            }

            /*  Deferred, because the window asking is the one going. */
            void closeImportWindow()
            {
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ui::MainWindow> (window.get()), this]
                {
                    if (safe != nullptr)
                        importWindow.reset();
                });
            }

            /*  THE IMPORT, whichever importer the window was for - a Live set's or a
                QLab workspace's (§46) - with the same words in the foot. */
            void startImport (const std::vector<std::string>& sets, const std::vector<int>& ticked, const juce::File& into,
                              const std::function<wfg::ImportResult (const wfg::ImportRequest&,
                                                                     const std::function<void (const std::string&)>&)>& importer)
            {
                if (importing || ! importer)
                    return;

                if (into.exists())
                {
                    shell->transport.setNotice (into.getFileName() + " is already there - choose another name");
                    return;
                }

                importing = true;
                closeImportWindow();
                shell->transport.setNotice ("importing into " + into.getFileName() + "...");

                wfg::ImportRequest request;
                request.sets = sets;
                request.scenes = ticked;
                request.into = into.getFullPathName().toStdString();

                /*  OFF THE MESSAGE THREAD: a tour copies a gigabyte of sound. Every
                    word back is posted to it, and nothing is touched once this
                    window has gone. */
                const juce::Component::SafePointer<ui::MainWindow> safe (window.get());

                std::thread ([this, importer, request, safe]
                {
                    const auto result = importer (request, [this, safe] (const std::string& sentence)
                    {
                        juce::MessageManager::callAsync ([this, safe, sentence]
                        {
                            if (safe != nullptr)
                                shell->transport.setNotice ("import: " + juce::String::fromUTF8 (sentence.c_str()));
                        });
                    });

                    juce::MessageManager::callAsync ([this, safe, result]
                    {
                        if (safe == nullptr)
                            return;

                        importing = false;
                        shell->transport.setNotice (juce::String::fromUTF8 (result.said.c_str()));

                        if (! result.ok || ! host.openWindow)
                            return;

                        const auto refused = host.openWindow (result.show, false);

                        if (! refused.empty())
                            shell->transport.setNotice (juce::String::fromUTF8 (refused.c_str()));

                        //  The report, in whatever reads Markdown here.
                        if (const juce::File report { juce::String::fromUTF8 (result.report.c_str()) }; report.existsAsFile())
                            report.startAsProcess();
                    });
                }).detach();
            }

            void askForANewPerformance()
            {
                const auto document = documentFolder();

                if (! document.isDirectory() || ! host.openWindow)
                    return;

                const auto showFolder = showAroundThisDocument().isNotEmpty() ? document.getParentDirectory() : document;

                auto* box = new juce::AlertWindow ("New performance",
                                                   "A name for it - a date and a place, say. It goes in "
                                                     + showFolder.getFileName() + "'s folder.",
                                                   juce::MessageBoxIconType::NoIcon);
                box->addTextEditor ("name", juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
                box->addButton ("Make it", 1, juce::KeyPress (juce::KeyPress::returnKey));
                box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                box->enterModalState (true, juce::ModalCallbackFunction::create (
                    [this, box, document, showFolder, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int answer)
                    {
                        if (answer != 1 || safe == nullptr)
                            return;

                        const auto name = juce::File::createLegalFileName (box->getTextEditorContents ("name").trim());
                        const auto target = showFolder.getChildFile (name);

                        if (name.isEmpty())
                        {
                            shell->transport.setNotice ("a performance needs a name");
                            return;
                        }

                        if (target.exists())
                        {
                            shell->transport.setNotice (name + " is already in " + showFolder.getFileName());
                            return;
                        }

                        //  From a template, its own sounds come too; the show's are found from around.
                        const auto templateMedia = document != showFolder ? document.getChildFile ("media") : juce::File();

                        send (gesture::saveAs (target.getFullPathName().toStdString()));
                        makingAPerformance = NewPerformance { target, juce::Time::getCurrentTime(),
                                                              last.writeError, templateMedia };
                        shell->transport.setNotice ("making the performance " + name);
                    }), true);
            }

            /*  On every pass while a performance is being made. */
            void followTheNewPerformance()
            {
                if (! makingAPerformance.has_value())
                    return;

                auto& making = *makingAPerformance;

                if (! last.writeError.empty() && last.writeError != making.errorBefore)
                {
                    shell->transport.setNotice ("the performance was not made: " + juce::String (last.writeError));
                    makingAPerformance.reset();
                    return;
                }

                if (copyLanded (making.folder, making.since))
                {
                    const auto made = making;
                    makingAPerformance.reset();

                    if (made.templateMedia.isDirectory()
                          && ! made.templateMedia.copyDirectoryTo (made.folder.getChildFile ("media")))
                        shell->transport.setNotice ("the performance is made, but its template's own sounds were not all copied");

                    const auto refused = host.openWindow (made.folder.getFullPathName().toStdString(), false);
                    shell->transport.setNotice (refused.empty() ? "opening the performance " + made.folder.getFileName()
                                                                : juce::String (refused));
                    return;
                }

                if ((juce::Time::getCurrentTime() - making.since).inSeconds() > 20.0)
                {
                    shell->transport.setNotice ("the performance did not arrive in " + making.folder.getFileName());
                    makingAPerformance.reset();
                }
            }

            /*  WHERE A SHOW DIALOG STARTS (author, 2026-10-01: "Can they
                default in the document folder?"). Beside the show this window
                is on - where a person's shows are, once they have one - except
                on the launcher's empty show, which lives in Go.dot's own folder
                (Application Support, %APPDATA%, ~/.local/share), a place nobody
                should be sent to keep their work: then the folder of the show
                this machine last opened (Console.h, `rememberIn`), or the
                Documents folder. */
            juce::File showsFolder() const
            {
                const auto documents = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

                if (host.emptyShowAtStart)
                {
                    if (! host.rememberIn.empty())
                    {
                        const auto said = juce::File (juce::String::fromUTF8 (host.rememberIn.c_str()))
                                            .getChildFile ("last-show.txt").loadFileAsString().trim();

                        if (juce::File::isAbsolutePath (said))
                            if (const auto beside = juce::File (said).getParentDirectory(); beside.isDirectory())
                                return beside;
                    }

                    return documents;
                }

                const auto beside = mediaFolder().getParentDirectory().getParentDirectory();
                return beside.isDirectory() ? beside : documents;
            }

            /*  WHERE A MEDIA DIALOG STARTS (author, 2026-10-08: "It's constantly
                bringing me back to a folder I can't access by default"). The
                show's own media folder, where its sounds are - except on the
                launcher's empty show, whose media folder is inside Go.dot's own
                hidden one, and on a show with no media folder yet: then the
                folder a sound was last picked from on this machine, or Music,
                or Documents. */
            juce::File mediaDialogFolder() const
            {
                if (const auto own = mediaFolder(); ! host.emptyShowAtStart && own.isDirectory())
                    return own;

                if (const auto remembered = lastMediaFolderFile(); remembered.existsAsFile())
                    if (const auto said = remembered.loadFileAsString().trim(); juce::File::isAbsolutePath (said))
                        if (const auto folder = juce::File (said); folder.isDirectory())
                            return folder;

                const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
                return music.isDirectory() ? music
                                           : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
            }

            /*  `<Go.dot's own folder>/last-media-folder.txt`, or nothing when
                the launch asked for nothing to be remembered (Console.h,
                `rememberIn`). */
            juce::File lastMediaFolderFile() const
            {
                return host.rememberIn.empty() ? juce::File()
                                               : juce::File (juce::String::fromUTF8 (host.rememberIn.c_str()))
                                                   .getChildFile ("last-media-folder.txt");
            }

            /*  The folder a sound or a picture was just picked from - unless it
                is inside the empty show itself, the place nobody is to be sent. */
            void rememberMediaFolder (const juce::File& picked) const
            {
                const auto file = lastMediaFolderFile();
                const auto folder = picked.getParentDirectory();
                const auto ownFolder = mediaFolder().getParentDirectory();

                if (file == juce::File()
                      || (host.emptyShowAtStart && (folder == ownFolder || folder.isAChildOf (ownFolder))))
                    return;

                file.getParentDirectory().createDirectory();
                file.replaceWithText (folder.getFullPathName());
            }

            /*  SAVE: in place - except on the launcher's empty show, which is
                saved where its maker chooses (below), not into Go.dot's folder. */
            void save()
            {
                if (host.emptyShowAtStart)
                    chooseWhereTheEmptyShowLives();
                else
                    send (gesture::save());
            }

            /*  THE EMPTY SHOW, SAVED, MOVES TO WHERE IT WAS SAVED (author,
                2026-10-01: "ask where, then continue there"), as an untitled
                document does. A folder is chosen, starting in Documents, and
                the show is written there - one `document.saveAs`, the copy the
                engine already knows how to make. Once the copy is on the disk,
                the empty show is put back to empty with `document.revert`, so
                its autosave does not offer the same work again at the next
                launch; once it is clean, the saved show opens in a window of
                its own and this one goes, as the empty show gives way to any
                show New or Open starts. Followed from the timer
                (followTheEmptyShowsSave), because the copy is written on the
                engine's writer, which says when one fails and not when one lands. */
            void chooseWhereTheEmptyShowLives()
            {
                chooser = std::make_unique<juce::FileChooser> ("Choose where to keep this show", showsFolder());

                chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                        | juce::FileBrowserComponent::canSelectDirectories
                                        | juce::FileBrowserComponent::warnAboutOverwriting,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this] (const juce::FileChooser& answered)
                                      {
                                          const auto folder = answered.getResult();

                                          if (safe == nullptr || folder == juce::File())
                                              return;

                                          /*  ITS SOUNDS GO WITH IT, unasked (namespace draft
                                              §32): the empty show lives in Go.dot's own folder
                                              and is put back to empty once saved, so a sound
                                              left there would belong to no show at all. */
                                          const auto carrying = ! soundsTheCopyWouldNotFind (folder).empty();

                                          send (gesture::saveAs (folder.getFullPathName().toStdString(), true));
                                          savingTheEmptyShow = EmptyShowSave { folder, juce::Time::getCurrentTime(),
                                                                               last.writeError, false, carrying };
                                          shell->transport.setNotice ((carrying ? "saving the show and its sounds to "
                                                                                : "saving the show to ")
                                                                        + folder.getFileName());
                                      });
            }

            /*  On every pass while the empty show is being saved somewhere. */
            void followTheEmptyShowsSave()
            {
                if (! savingTheEmptyShow.has_value())
                    return;

                auto& saving = *savingTheEmptyShow;
                const auto waited = juce::Time::getCurrentTime() - saving.since;

                if (! saving.reverting)
                {
                    /*  A new sentence from the writer is this copy failing - or,
                        with the show on the disk, some of its sounds not
                        following, when the empty show is left as it is: it
                        still has them, and Save may be tried again. */
                    if (! last.writeError.empty() && last.writeError != saving.errorBefore)
                    {
                        shell->transport.setNotice (copyLanded (saving.folder, saving.since)
                                                      ? "the show was saved to " + saving.folder.getFileName()
                                                          + ", but not all its sounds: " + juce::String (last.writeError)
                                                      : "the show was not saved: " + juce::String (last.writeError));
                        savingTheEmptyShow.reset();
                        return;
                    }

                    if (copyLanded (saving.folder, saving.since))
                    {
                        send (gesture::revert());
                        saving.reverting = true;
                        saving.since = juce::Time::getCurrentTime();
                        return;
                    }
                }
                else if (last.dirty == model::Flag::no)
                {
                    const auto folder = saving.folder;
                    const auto refused = host.openWindow (folder.getFullPathName().toStdString(), false);
                    savingTheEmptyShow.reset();

                    if (refused.empty() && host.quit)
                        host.quit();
                    else
                        shell->transport.setNotice (refused.empty() ? "saved to " + folder.getFileName()
                                                                    : juce::String (refused));
                    return;
                }

                //  Sounds being copied take what they take; the revert does not.
                if (waited.inSeconds() > 20.0 && (saving.reverting || ! saving.carrying))
                {
                    shell->transport.setNotice (saving.reverting
                                                  ? "saved to " + saving.folder.getFileName()
                                                      + ", but the empty show did not come back"
                                                  : "the show did not arrive in " + saving.folder.getFileName());
                    savingTheEmptyShow.reset();
                }
            }

            /*  The copy is on the disk: its manifest, show.xml and state.xml,
                each written since the save was asked for. Each is written whole
                or not at all (Bundle's atomic writes), so a file that is there
                is a file that is finished. */
            static bool copyLanded (const juce::File& folder, juce::Time since)
            {
                const auto earliest = since - juce::RelativeTime::seconds (2.0);

                for (const auto& file : { folder.getChildFile (folder.getFileName() + ".wfg"),
                                          folder.getChildFile ("show.xml"),
                                          folder.getChildFile ("state.xml") })
                    if (! file.existsAsFile() || file.getLastModificationTime() < earliest)
                        return false;

                return true;
            }

            /*  SAVE AS: a folder, and one `document.saveAs` on it. The engine
                writes the copy and keeps this session on the show it opened,
                which is what the command was drawn to do (§14.10); the foot
                says where the copy went. A copy leaving the show's folder with
                sounds behind it is asked about first (namespace draft §32). */
            void chooseSaveAsFolder()
            {
                chooser = std::make_unique<juce::FileChooser> (
                            "Choose an empty folder for the copy",
                            showsFolder());

                chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                        | juce::FileBrowserComponent::canSelectDirectories
                                        | juce::FileBrowserComponent::warnAboutOverwriting,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this] (const juce::FileChooser& answered)
                                      {
                                          const auto folder = answered.getResult();

                                          if (safe == nullptr || folder == juce::File())
                                              return;

                                          if (const auto missing = soundsTheCopyWouldNotFind (folder); ! missing.empty())
                                              askAboutTheSounds (folder, missing);
                                          else
                                              copyTheShow (folder, false);
                                      });
            }

            /*  THE SOUNDS A COPY AT `folder` WOULD NOT FIND STRAIGHT AWAY (author,
                2026-10-06: "When saving outside the workfolder (show) warn the
                user and ask if they need to move the bundled media too", then
                "Saving within the same folder, or anywhere it will find its
                media straightaway is fine"). The engine's own rule, which its
                copy then follows (`audio::mediaACopyWouldNotFind`), handed
                the sound each cue names - read in one pass over the tree, once,
                when the folder is chosen. */
            std::vector<audio::MediaToCarry> soundsTheCopyWouldNotFind (const juce::File& folder) const
            {
                const auto document = documentFolder();

                if (latest == nullptr || ! document.isDirectory())
                    return {};

                std::vector<std::string> named;
                const std::string cues = "/godot/cue/", file = "/file";

                for (const auto* node : latest->all())
                {
                    const auto& address = node->address;

                    if (address.size() > cues.size() + file.size() && address.rfind (cues, 0) == 0
                          && address.compare (address.size() - file.size(), file.size(), file) == 0
                          && address.find ('/', cues.size()) == address.size() - file.size())
                        if (auto sound = model::text (*latest, address); ! sound.empty())
                            named.push_back (std::move (sound));
                }

                return audio::mediaACopyWouldNotFind (folder.getFullPathName().toStdString(),
                                                     document.getFullPathName().toStdString(), named);
            }

            //  A few of the names, then how many more.
            static juce::String someOf (const std::vector<audio::MediaToCarry>& sounds)
            {
                constexpr std::size_t shown = 3;
                juce::StringArray names;

                for (std::size_t i = 0; i < sounds.size() && i < shown; ++i)
                    names.add (juce::String::fromUTF8 (sounds[i].relative.c_str()));

                auto text = names.joinIntoString (", ");

                if (sounds.size() > shown)
                    text << " and " << juce::String (sounds.size() - shown) << " more";

                return text;
            }

            void askAboutTheSounds (const juce::File& folder, const std::vector<audio::MediaToCarry>& missing)
            {
                const auto count = missing.size() == 1 ? juce::String ("1 sound")
                                                       : juce::String (missing.size()) + " sounds";

                auto* box = new juce::AlertWindow (
                    "The copy would not find its sounds",
                    "Saved in \"" + folder.getFileName() + "\", the copy would not find " + count
                      + " this show uses: " + someOf (missing) + ".\n\n"
                      "Copy them with it? A large show takes a while; this one goes on playing meanwhile.",
                    juce::MessageBoxIconType::WarningIcon);
                box->addButton ("Copy the sounds too", 1, juce::KeyPress (juce::KeyPress::returnKey));
                box->addButton ("Copy without them", 2);
                box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                box->enterModalState (true, juce::ModalCallbackFunction::create (
                    [this, folder, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int answer)
                    {
                        if (answer != 0 && safe != nullptr)
                            copyTheShow (folder, answer == 1);
                    }), true);
            }

            void copyTheShow (const juce::File& folder, bool withSounds)
            {
                send (gesture::saveAs (folder.getFullPathName().toStdString(), withSounds));

                if (! withSounds)
                {
                    shell->transport.setNotice ("copy of the show written to " + folder.getFileName());
                    return;
                }

                copyingTheShow = ShowCopy { folder, juce::Time::getCurrentTime(), last.writeError };
                shell->transport.setNotice ("copying the show and its sounds to " + folder.getFileName() + "...");
            }

            /*  On every pass while a copy carries its sounds: the writer copies
                them before the show's three files, so those landing is the
                whole copy landing. No time limit - a gigabyte takes what it
                takes, and a copy that fails says so through the writer. */
            void followTheCopy()
            {
                if (! copyingTheShow.has_value())
                    return;

                const auto copy = *copyingTheShow;
                const auto failed = ! last.writeError.empty() && last.writeError != copy.errorBefore;

                if (! failed && ! copyLanded (copy.folder, copy.since))
                    return;

                copyingTheShow.reset();

                if (! failed)
                    shell->transport.setNotice ("the show and its sounds were copied to " + copy.folder.getFileName());
                else if (copyLanded (copy.folder, copy.since))
                    shell->transport.setNotice ("the show was copied to " + copy.folder.getFileName()
                                                  + ", but not all of it: " + juce::String (last.writeError));
                else
                    shell->transport.setNotice ("the copy was not made: " + juce::String (last.writeError));
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
            std::pair<std::string, int> destination (const std::string& kind = "media") const
            {
                const auto& picked = selection.anchor();

                if (latest != nullptr && ! picked.empty())
                {
                    const auto parent = model::text (*latest, "/godot/cue/" + picked + "/parent");

                    if (! parent.empty())
                        return outsideASampler (parent, model::positionAfter (orderOf (parent), picked), kind);
                }

                return { last.listId, -1 };
            }

            /*  A SAMPLER HOLDS SOUNDS AND PICTURES (namespace draft §39, §49): a
                cue of another kind that would land among a sampler's members
                lands just after the sampler group instead, where the engine
                takes it, rather than being refused inside it. A sound or a
                picture goes where it was put. */
            std::pair<std::string, int> outsideASampler (const std::string& parent, int index,
                                                         const std::string& kind) const
            {
                if (latest == nullptr || kind == "media" || kind == "video" || parent.empty()
                    || model::text (*latest, "/godot/cue/" + parent + "/mode") != "sampler")
                    return { parent, index };

                const auto above = model::text (*latest, "/godot/cue/" + parent + "/parent");

                if (above.empty())
                    return { parent, index };

                return { above, model::positionAfter (orderOf (above), parent) };
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
                createCue (kind, {});
            }

            void createFromTemplate (const model::Choice& line)
            {
                const auto [parent, index] = destination (line.kind);

                if (parent.empty())
                {
                    shell->transport.setNotice ("no list to add a cue to");
                    return;
                }

                if (line.kind == "media")
                {
                    chooseMedia (parent, index, line.cueTemplate);
                    return;
                }

                const auto members = static_cast<int> (model::words (orderOf (parent)).size());
                const auto at = index < 0 ? members : juce::jlimit (0, members, index);

                send (gesture::createCueFrom (parent, at, line.cueTemplate, ""));
                creations.push_back ({ parent, at, line.kind, last.revision, 0 });
            }

            /*  AND BORN WITH ITS SETTINGS, when a line of a list chose them:
                one `cue.create`, one record, one Undo. */
            void createCue (const std::string& kind, const model::Settings& bornWith)
            {
                if (refusedWhileLocked())
                    return;

                const auto [parent, index] = destination (kind);

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

                send (gesture::createCue (parent, at, kind, "", bornWith));
                creations.push_back ({ parent, at, kind, last.revision, 0 });
            }

            /*  THE LISTS FOUR BUTTONS OPEN (the author, 2026-09-27): group,
                transport, midi and mic stand for several things, and the list
                under the button is where the one meant is chosen. What each
                line offers and makes is model/NewCueMenus.h's; this reads the
                pick, shows the list, and sends what the line clicked makes.

                THE PICK IS READ WHEN THE LIST OPENS, and what it offered is
                what a click makes: the cues the group list offered to take,
                the cue the transport list said it would aim at. */
            void chooseFromList (const std::string& kind, juce::Component& button)
            {
                if (refusedWhileLocked() || latest == nullptr)
                    return;

                const auto where = destinationSentence().toStdString();

                std::vector<model::Choice> offered;
                std::vector<model::MenuLine> lines;
                model::Wrap around;
                std::string aim;

                if (kind == "group")
                {
                    if (! groupingCue.empty())      // a group still being made around the last pick
                        return;

                    offered = model::groupChoices();
                    around = model::wrapOf (*latest, selection.ids());
                    lines = model::groupMenu (around, where);
                }
                else if (kind == "transport")
                {
                    offered = model::transportChoices();
                    aim = selection.anchor();

                    const auto aimName = aim.empty() ? std::string {}
                                                     : model::text (*latest, "/godot/cue/" + aim + "/name");

                    lines = model::transportMenu (aim.empty() ? std::string {}
                                                              : (aimName.empty() ? std::string ("the picked cue") : aimName),
                                                  where);
                }
                else if (kind == "midi")
                {
                    offered = model::midiChoices();
                    lines = model::midiMenu (where);
                }
                else if (kind == "mic")
                {
                    offered = model::micChoices (*latest);
                    lines = model::micMenu (*latest, offered, where);
                }
                else if (kind == "video")
                {
                    offered = model::videoChoices (*latest);
                    lines = model::videoMenu (*latest, offered, where);
                }
                else if (kind == "media")
                {
                    offered = model::mediaChoices (*latest);
                    lines = model::mediaMenu (offered, where);
                }
                else
                {
                    createCue (kind);
                    return;
                }

                ui::showNewCueMenu (button, lines,
                                    [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                     this, offered, cues = around.cues, aim] (int picked, bool wrap)
                                    {
                                        if (safe == nullptr || picked < 0
                                              || static_cast<std::size_t> (picked) >= offered.size())
                                            return;

                                        createFromChoice (offered[static_cast<std::size_t> (picked)],
                                                          wrap ? cues : std::vector<std::string> {}, aim);
                                    });
            }

            void createFromChoice (const model::Choice& line, const std::vector<std::string>& wrapped,
                                   const std::string& aim)
            {
                if (refusedWhileLocked())
                    return;

                auto bornWith = line.settings;

                if (! wrapped.empty())
                {
                    wrapAround (wrapped, bornWith);
                    return;
                }

                /*  BORN FROM A TEMPLATE (namespace draft §38): a media cue's
                    files chosen first, each cue born from it as it lands; a
                    picture's cue at once, its file chosen in the inspector. */
                if (! line.cueTemplate.empty())
                {
                    createFromTemplate (line);
                    return;
                }

                /*  A STOP WITH NO TARGET STOPS NOTHING: aimed at the cue picked
                    when the list opened, and placed after it. */
                if (line.aimed && ! aim.empty())
                    bornWith.emplace_back ("target", aim);

                createCue (line.kind, bornWith);
            }

            /*  A NEW GROUP AROUND THE PICKED CUES, found next pass by watching
                the outermost of them change parent (finishCreations), then
                picked so the inspector opens on it. */
            void wrapAround (const std::vector<std::string>& ids, const model::Settings& bornWith)
            {
                if (! groupingCue.empty() || latest == nullptr || ids.empty())
                    return;

                const auto picked = [&ids] (const std::string& id)
                {
                    return std::find (ids.begin(), ids.end(), id) != ids.end();
                };

                groupingCue = ids.front();

                for (auto parent = model::text (*latest, "/godot/cue/" + groupingCue + "/parent");
                     ! parent.empty(); parent = model::text (*latest, "/godot/cue/" + parent + "/parent"))
                    if (picked (parent))
                        groupingCue = parent;

                groupingParent = model::text (*latest, "/godot/cue/" + groupingCue + "/parent");
                groupingWait = 0;
                send (gesture::wrapGroup (ids, bornWith));
            }

            /*  `+ media` asks for the files first and imports them where the
                cue would have gone; nothing is made when the dialogue is
                cancelled. Several files make several cues, in the order
                chosen, exactly as a drop of several does. */
            void chooseMedia (const std::string& parent, int index, const std::string& cueTemplate = {})
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();

                chooser = std::make_unique<juce::FileChooser> (
                            "Choose the media for the new cue",
                            mediaDialogFolder(), formats.getWildcardForAllFormats());

                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [safe = juce::Component::SafePointer<ui::MainWindow> (window.get()),
                                       this, parent, index, cueTemplate] (const juce::FileChooser& answered)
                                      {
                                          if (safe == nullptr)
                                              return;

                                          juce::StringArray files;

                                          for (const auto& file : answered.getResults())
                                              if (file.existsAsFile())
                                                  files.add (file.getFullPathName());

                                          if (files.isEmpty())
                                              return;

                                          rememberMediaFolder (juce::File (files[0]));
                                          importMedia (parent, index, files, cueTemplate);
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
            /*  CLOSING A PERFORMANCE THAT DIFFERS FROM ITS SHOW'S TEMPLATE asks
                first whether to bring its changes back (§25, the author: "this
                should be a question when saving or closing" - closing, not
                every save, since a tech saves all day). Then the window's own
                question, as for any document. */
            void closeRequested()
            {
                if (model::isYes (last.locked) || ! host.compareWithTemplate || ! templateAroundThisDocument())
                {
                    askToClose();
                    return;
                }

                const auto dirty = last.dirty == model::Flag::yes;
                const auto comparison = host.compareWithTemplate();

                if (! dirty && (! comparison.ok || comparison.changes.empty()))
                {
                    askToClose();
                    return;
                }

                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                .withTitle ("Bring this performance's changes into the show's template?")
                                                .withMessage (dirty ? "This performance has unsaved changes, and may differ from "
                                                                      + showAroundThisDocument() + "'s template."
                                                                    : "This performance differs from " + showAroundThisDocument()
                                                                      + "'s template in " + juce::String (static_cast<int> (comparison.changes.size()))
                                                                      + (comparison.changes.size() == 1 ? " place." : " places."))
                                                .withButton (dirty ? "Save, then review..." : "Review the changes...")
                                                .withButton ("Close without")
                                                .withButton ("Cancel")
                                                .withAssociatedComponent (window.get()),
                                              [this, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int answer)
                                              {
                                                  if (safe == nullptr)
                                                      return;

                                                  if (answer == 1)
                                                      reviewTemplate ([this] { askToClose(); });
                                                  else if (answer == 2)
                                                      askToClose();
                                              });
            }

            void askToClose()
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

                /*  CHANGES NOT SAVED ASK FIRST (namespace draft §39, WW, the
                    author's pick on 2026-10-07): save them, throw them away, or
                    stay. Until then a close asked nothing about them, kept the
                    autosave, and the next open offered it as lost work - every
                    time, since an offer nobody answers stays. "Close without
                    saving" is `document.revert`, which takes this session's
                    `recovery/` with it, so the answer is the one that offer was
                    waiting for. `isYes`: a show the engine has said nothing
                    about closes as before. */
                if (model::isYes (last.dirty))
                {
                    juce::AlertWindow::showAsync (Options()
                                                    .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                    .withTitle ("Save the changes to " + juce::String (last.show) + " before closing?")
                                                    .withMessage ("Running cues will stop. Closing without saving throws away "
                                                                  "every change since the show was last saved.")
                                                    .withButton ("Save and close")
                                                    .withButton ("Close without saving")
                                                    .withButton ("Cancel")
                                                    .withAssociatedComponent (window.get()),
                                                  [this, safe = juce::Component::SafePointer<ui::MainWindow> (window.get())] (int answer)
                                                  {
                                                      //  Three buttons: 1, 2, and the last answers 0.
                                                      if (safe == nullptr || ! host.quit || (answer != 1 && answer != 2))
                                                          return;

                                                      closeOnceClean = CloseOnceClean { juce::Time::getCurrentTime(), last.writeError,
                                                                                        answer == 1 };
                                                      send (answer == 1 ? gesture::save() : gesture::revert());
                                                  });
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
            std::unique_ptr<ui::ShowSettingsWindow> audioSettings;
            std::unique_ptr<ui::SurfaceWindow> surfaces;    // Show > Surfaces..., made on first open
            std::unique_ptr<ui::NetworkMonitorWindow> networkMonitor;   // Show > Network monitor...
            std::unique_ptr<ui::VideoMonitorWindow> videoMonitor;       // Show > Video monitor...
            ui::Shell* shell = nullptr;                     // owned by the window

            /*  The plugins' own windows, each a helper process. Declared after
                the window, so it goes first: its helpers are told to leave
                while everything they report to still stands. */
            std::unique_ptr<ui::PluginEditors> editors;

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

            /*  EVERY IMPORT UNDER WAY (namespace draft §30, S7): the files
                picked, in order, what each met in `media/`, the cue asked
                for - the decisions, in model/Media.h - and the worker that
                reads and copies, off this thread, one file at a time. */
            model::MediaImports imports;
            ui::MediaCopier copier { [this] (const model::MediaWork& work)
                                     {
                                         imports.worked (work);
                                         followImports();
                                     } };

            bool askingAboutClash = false;  ///< a question about a name already in media/ is up

            /*  Movies imported and not yet asked about, the probe running, the
                question up; and each conversion as last said (37.5, WF-WJ). */
            std::deque<std::string> movieOffers;
            bool probingMovie = false;
            bool askingAboutMovie = false;
            std::map<std::string, model::ConversionRow> conversionsSeen;

            /*  FFmpeg offered once a session; the movies waiting for it; how
                its download was last said (37.5, WN). */
            bool ffmpegOffered = false;
            std::vector<std::string> moviesWaitingForFfmpeg;
            model::FfmpegInstallRow ffmpegInstallSeen;
            std::string importSaid;         ///< the progress last put on the foot, so it is said once
            juce::String importWarning;     ///< what routing an imported cue found missing, said at the end

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
            std::string partClipboardSeen;

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

            /*  The launcher's empty show on its way to where it was saved
                (chooseWhereTheEmptyShowLives): the folder, since when, what the
                writer had already said before, whether the copy has landed
                and the empty show is being put back, and whether it carries
                sounds - which lifts the time limit on the copy. */
            struct EmptyShowSave
            {
                juce::File folder;
                juce::Time since;
                std::string errorBefore;
                bool reverting = false;
                bool carrying = false;
            };

            std::optional<EmptyShowSave> savingTheEmptyShow;

            /*  A performance on its way (askForANewPerformance): its folder,
                since when, what the writer had said before, and the template's
                own media/ to bring along - empty when made from the show. */
            struct NewPerformance
            {
                juce::File folder;
                juce::Time since;
                std::string errorBefore;
                juce::File templateMedia;
            };

            std::optional<NewPerformance> makingAPerformance;

            //  A Save as carrying its sounds (copyTheShow): where, since when, what the writer had said before.
            struct ShowCopy
            {
                juce::File folder;
                juce::Time since;
                std::string errorBefore;
            };

            std::optional<ShowCopy> copyingTheShow;

            //  A save asked for so the template review reads it, and what runs after the review.
            struct ReviewAfterSave
            {
                std::function<void()> afterwards;
                juce::Time since;
                std::string errorBefore;
            };

            std::optional<ReviewAfterSave> reviewAfterSave;

            //  A close answered "save" or "without saving", waiting for the show to be clean (WW).
            struct CloseOnceClean
            {
                juce::Time since;
                std::string errorBefore;
                bool saving = false;
            };

            std::optional<CloseOnceClean> closeOnceClean;
            std::unique_ptr<ui::TemplateReviewWindow> templateReview;

            /*  AN IMPORT UNDER WAY (§29): the scene list while it is open, and
                whether the import itself is running, which greys the menu. */
            std::unique_ptr<ui::ImportWindow> importWindow;
            bool importing = false;
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
