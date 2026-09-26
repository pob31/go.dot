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

#pragma once

/*
    A CUE'S EQ AND SENDS, RIDDEN WHILE THE SHOW IS LOCKED (author, 2026-09-25) -
    AND ITS PLUGINS' PARAMETERS (2026-09-26, the FX page: "ride live, like EQ").

    The lock refuses every edit to the show, and a cue's EQ and its send levels
    are saved in the cue. The author decided that under the lock they are
    ridden anyway - by a D700's rotaries, by the window's EQ panel and send
    mixer, by the page - the way a fader is: heard at once, written to nothing,
    no step of the show's history. This is where those values live.

    A LAYER OVER THE SHOW, NOT A COPY OF IT. It holds only what differs: a
    row's value written under the lock, a send's level or switch, and a send to
    a mix channel the cue did not send to (the author, again: new sends ride
    live too). Everything that reads a cue's EQ or its routing asks here first
    and the show second - the Runner, so it is heard; the parameter tree, so a
    client sees it at the same address it would see the saved value.

    IT LASTS UNTIL SOMEBODY DECIDES. Unlocking does not drop it: the window
    shows a bar while the show is unlocked and the layer is not empty, and
    `live.keep` writes it into the show as one undo step - a send made live
    becomes a real Send with the same identifier - while `live.drop` lets the
    cues go back to their saved sound. An edit to a row that rides live, made
    once the show is unlocked, is an ordinary edit: it writes the show and the
    live value for that row is gone. Saving writes only the show (PRD §4.10).
    The layer dies with the process, as a fader's trim does.

    THE DOOR IS `node.set`, as it is for a trim: the same addresses the saved
    values are published at, answered in front of the document while the show
    is locked. A send made live is `send.create` itself, answered the same way,
    its identifier drawn and carried on the applied record so a replay makes
    the same one. So every write here is a logged record a replay reproduces.

    THE TICK THREAD ONLY, like the document it lies over.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/DocumentCommands.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::cue
{
    class LiveEdits
    {
    public:
        /*  A send riding live: its level or its switch, as canonical text,
            where either was written under the lock - or a whole send the show
            does not have, made under the lock (`created`). */
        struct Send
        {
            std::string cue;
            std::string bus;
            std::optional<std::string> level;
            std::optional<std::string> on;
            bool created = false;
        };

        /** A cue's row riding live, as canonical text, or null. */
        const std::string* rowOf (const std::string& cueId, const std::string& row) const;

        /** The names of a cue's rows riding live, space-separated: media/live. */
        std::string rowsOf (const std::string& cueId) const;

        /** A send riding live, or null. */
        const Send* sendOf (const std::string& sendId) const;

        /** The sends made live on a cue, by identifier, in identifier order. */
        std::vector<std::string> createdSendsOf (const std::string& cueId) const;

        /** Whether a send made live already goes from this cue to this bus. */
        bool sendsInto (const std::string& cueId, const std::string& busId) const;

        /*  A CUE'S INSERT'S PARAMETERS RIDING LIVE (2026-09-26): index to value,
            normalised as the insert's p<n> takes it; null when none rides. */
        const std::map<int, double>* fxValuesOf (const std::string& fxId) const;

        /*  HOW MANY CHANGES ARE RIDING LIVE, as the window's bar counts them:
            a row, a send's level, a send's switch, a send made live, a
            plugin's parameter. */
        std::size_t size() const noexcept;
        bool empty() const noexcept { return rows.empty() && sends.empty() && fx.empty(); }

        /** Moves on every change, so a reader can tell it has something new. */
        std::uint64_t revision() const noexcept { return rev; }

        void setRow (const std::string& cueId, const std::string& row, std::string text);
        void dropRow (const std::string& cueId, const std::string& row);
        void setSendValue (const std::string& sendId, const std::string& cueId, const std::string& busId,
                           const std::string& row, std::string text);
        void dropSendValue (const std::string& sendId, const std::string& row);
        void createSend (const std::string& sendId, const std::string& cueId, const std::string& busId,
                         std::string level);
        void setFxValue (const std::string& fxId, int index, double value);
        void dropFxValue (const std::string& fxId, int index);
        void clear();

        const std::map<std::string, std::map<std::string, std::string>>& allRows() const noexcept { return rows; }
        const std::map<std::string, Send>& allSends() const noexcept { return sends; }
        const std::map<std::string, std::map<int, double>>& allFx() const noexcept { return fx; }

    private:
        std::map<std::string, std::map<std::string, std::string>> rows;
        std::map<std::string, Send> sends;
        std::map<std::string, std::map<int, double>> fx;
        std::uint64_t rev = 1;
    };

    /*  THE DOOR `node.set` GOES THROUGH for a cue's EQ rows and a send's
        `level` and `on`. It answers:
          - while the show is locked, by parsing the value as the document
            would - the same refusals, `type-mismatch` for a bad value - and
            holding it in the layer; a value the show already has is no change
            and drops whatever rode live there;
          - a send made live, in any state, since the show has nowhere for it;
          - once the show is unlocked, a row that rides live: the show is
            written, as any edit, and the live value is gone.
        Anything else steps aside, so the document answers as it always did. */
    doc::LiveWrite liveEditFor (LiveEdits& live, doc::ShowDocument& document);

    /*  `send.create`'s door. While the show is locked it makes a send live -
        the cue must be media, the bus a mix channel, and neither the show nor
        the layer may already send there - drawing its identifier, or taking
        the one a replay hands back. Once unlocked it refuses a second send to
        a bus a live one already goes to, and otherwise steps aside. */
    doc::LiveCreate liveSendFor (LiveEdits& live, doc::ShowDocument& document);

    /*  Whether an applied command rides the layer rather than edits the show
        - which the transaction hook asks, beside `isLiveWrite`, so that a
        ride opens no transaction. Asked before the command applies. */
    bool isLiveEdit (const std::string& commandName, const std::vector<osc::Value>& args,
                     const doc::ShowDocument& document, const LiveEdits& live);

    /*  `live.keep` and `live.drop`: what the window's bar sends once the show
        is unlocked. Keep is refused under the lock, and is one transaction; a
        cue or a bus deleted since is skipped. */
    void registerLiveCommands (CommandRegistry& registry, doc::ShowDocument& document, LiveEdits& live);
}
