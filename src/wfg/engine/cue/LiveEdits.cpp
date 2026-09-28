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

#include <wfg/engine/cue/LiveEdits.h>
#include <wfg/engine/cue/FxRows.h>

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>

#include <string_view>
#include <utility>

namespace wfg::cue
{
    namespace
    {
        /*  `/godot/<owner>/<id>/<row>`, the one shape the rows here have. */
        struct Parts
        {
            std::string owner;
            std::string id;
            std::string row;
        };

        std::optional<Parts> partsOf (const std::string& address)
        {
            constexpr std::string_view prefix = "/godot/";

            if (address.rfind (prefix, 0) != 0)
                return std::nullopt;

            const auto ownerEnd = address.find ('/', prefix.size());

            if (ownerEnd == std::string::npos)
                return std::nullopt;

            const auto idEnd = address.find ('/', ownerEnd + 1);

            if (idEnd == std::string::npos || address.find ('/', idEnd + 1) != std::string::npos)
                return std::nullopt;

            Parts parts;
            parts.owner = address.substr (prefix.size(), ownerEnd - prefix.size());
            parts.id = address.substr (ownerEnd + 1, idEnd - ownerEnd - 1);
            parts.row = address.substr (idEnd + 1);

            if (parts.id.empty() || parts.row.empty())
                return std::nullopt;

            return parts;
        }

        bool isEqRow (const Parts& parts)
        {
            return parts.owner == "cue" && parts.row.rfind ("eq", 0) == 0;
        }

        bool isSendRow (const Parts& parts)
        {
            return parts.owner == "send" && (parts.row == "level" || parts.row == "on");
        }

        const doc::AttributeRow* rowFor (std::string_view owner, std::string_view name)
        {
            for (const auto* row : doc::Schema::rowsForOwner (owner))
                if (row->name == name)
                    return row;

            return nullptr;
        }

        /*  A send's value as the document would take it: parsed against the
            `send` row, in the text the writer would put in a file. */
        std::optional<std::string> sendText (const std::string& row, std::string_view text)
        {
            const auto* found = rowFor ("send", row);

            if (found == nullptr)
                return std::nullopt;

            const doc::Attribute attribute { "Send", found };
            doc::Value value;

            if (! doc::Schema::parseValue (attribute, text, value).ok)
                return std::nullopt;

            return doc::Schema::formatValue (attribute, value);
        }

        std::vector<osc::Value> withId (std::vector<osc::Value> args, std::size_t at, const std::string& id)
        {
            if (args.size() > at)
                args[at] = osc::Value::string (id);
            else
                args.push_back (osc::Value::string (id));

            return args;
        }

        /*  The Send a cue already has into a bus, by identifier, or empty. */
        std::string sendOfCueInto (const juce::ValueTree& cue, const std::string& busId)
        {
            for (const auto& child : cue)
                if (child.hasType ("Send") && child.getProperty ("bus").toString().toStdString() == busId)
                    return child.getProperty ("id").toString().toStdString();

            return {};
        }
    }

    //==========================================================================
    const std::string* LiveEdits::rowOf (const std::string& cueId, const std::string& row) const
    {
        const auto cue = rows.find (cueId);

        if (cue == rows.end())
            return nullptr;

        const auto found = cue->second.find (row);
        return found != cue->second.end() ? &found->second : nullptr;
    }

    std::string LiveEdits::rowsOf (const std::string& cueId) const
    {
        std::string out;
        const auto cue = rows.find (cueId);

        if (cue == rows.end())
            return out;

        for (const auto& [row, text] : cue->second)
        {
            if (! out.empty())
                out.push_back (' ');

            out += row;
        }

        return out;
    }

    const LiveEdits::Send* LiveEdits::sendOf (const std::string& sendId) const
    {
        const auto found = sends.find (sendId);
        return found != sends.end() ? &found->second : nullptr;
    }

    std::vector<std::string> LiveEdits::createdSendsOf (const std::string& cueId) const
    {
        std::vector<std::string> out;

        for (const auto& [id, send] : sends)
            if (send.created && send.cue == cueId)
                out.push_back (id);

        return out;
    }

    bool LiveEdits::sendsInto (const std::string& cueId, const std::string& busId) const
    {
        for (const auto& [id, send] : sends)
            if (send.created && send.cue == cueId && send.bus == busId)
                return true;

        return false;
    }

    std::size_t LiveEdits::size() const noexcept
    {
        std::size_t count = 0;

        for (const auto& [cue, riding] : rows)
            count += riding.size();

        for (const auto& [id, send] : sends)
            count += send.created ? 1u : (send.level.has_value() ? 1u : 0u) + (send.on.has_value() ? 1u : 0u);

        for (const auto& [id, values] : fx)
            count += values.size();

        return count;
    }

    const std::map<int, double>* LiveEdits::fxValuesOf (const std::string& fxId) const
    {
        const auto found = fx.find (fxId);
        return found != fx.end() ? &found->second : nullptr;
    }

    void LiveEdits::setFxValue (const std::string& fxId, int index, double value)
    {
        fx[fxId][index] = value;
        ++rev;
    }

    void LiveEdits::dropFxValue (const std::string& fxId, int index)
    {
        const auto insert = fx.find (fxId);

        if (insert == fx.end() || insert->second.erase (index) == 0)
            return;

        if (insert->second.empty())
            fx.erase (insert);

        ++rev;
    }

    void LiveEdits::setRow (const std::string& cueId, const std::string& row, std::string text)
    {
        rows[cueId][row] = std::move (text);
        ++rev;
    }

    void LiveEdits::dropRow (const std::string& cueId, const std::string& row)
    {
        const auto cue = rows.find (cueId);

        if (cue == rows.end() || cue->second.erase (row) == 0)
            return;

        if (cue->second.empty())
            rows.erase (cue);

        ++rev;
    }

    void LiveEdits::setSendValue (const std::string& sendId, const std::string& cueId, const std::string& busId,
                                  const std::string& row, std::string text)
    {
        auto& send = sends[sendId];

        if (! send.created)
        {
            send.cue = cueId;
            send.bus = busId;
        }

        (row == "level" ? send.level : send.on) = std::move (text);
        ++rev;
    }

    void LiveEdits::dropSendValue (const std::string& sendId, const std::string& row)
    {
        const auto found = sends.find (sendId);

        if (found == sends.end() || found->second.created)
            return;

        (row == "level" ? found->second.level : found->second.on).reset();

        if (! found->second.level.has_value() && ! found->second.on.has_value())
            sends.erase (found);

        ++rev;
    }

    void LiveEdits::createSend (const std::string& sendId, const std::string& cueId, const std::string& busId,
                                std::string level)
    {
        Send send;
        send.cue = cueId;
        send.bus = busId;
        send.level = std::move (level);
        send.created = true;
        sends[sendId] = std::move (send);
        ++rev;
    }

    void LiveEdits::clear()
    {
        if (empty())
            return;

        rows.clear();
        sends.clear();
        fx.clear();
        ++rev;
    }

    //==========================================================================
    doc::LiveWrite liveEditFor (LiveEdits& live, doc::ShowDocument& document)
    {
        return [&live, &document] (const std::string& address, const std::string& text,
                                   const std::vector<osc::Value>& args) -> std::optional<Outcome>
        {
            const auto parts = partsOf (address);

            if (! parts.has_value() || (! isEqRow (*parts) && ! isSendRow (*parts)))
                return std::nullopt;

            const auto* riding = isSendRow (*parts) ? live.sendOf (parts->id) : nullptr;

            /*  A SEND MADE LIVE has no <Send> in the show, so its level and its
                switch are the layer's whatever the lock says - until Keep makes
                it real, or Discard lets it go. */
            if (riding != nullptr && riding->created)
            {
                const auto canonical = sendText (parts->row, text);

                if (! canonical.has_value())
                    return Outcome::rejected (reason::typeMismatch);

                live.setSendValue (parts->id, riding->cue, riding->bus, parts->row, *canonical);
                return Outcome::ok (args);
            }

            /*  UNLOCKED, A ROW THAT RIDES LIVE IS EDITED LIKE ANY OTHER: the
                show is written - an undo step, as always - and the live value
                is gone, the new decision replacing it. One that does not ride
                is the document's alone. */
            if (! document.isLocked())
            {
                const auto isRiding = isEqRow (*parts)
                                        ? live.rowOf (parts->id, parts->row) != nullptr
                                        : riding != nullptr
                                            && (parts->row == "level" ? riding->level.has_value()
                                                                      : riding->on.has_value());

                if (! isRiding)
                    return std::nullopt;

                const auto edit = document.setAttribute (address, text);

                if (! edit.ok)
                    return Outcome::rejected (edit.reason);

                if (isEqRow (*parts))
                    live.dropRow (parts->id, parts->row);
                else
                    live.dropSendValue (parts->id, parts->row);

                return Outcome::ok (args);
            }

            /*  LOCKED: the show's own row, resolved and parsed exactly as its
                door would - the same refusals - and held here instead. */
            const auto target = document.resolve (address);

            if (! target.isValid())
                return Outcome::rejected (reason::badAddress);

            if (target.isDerived || target.attribute->access() == doc::Access::read)
                return Outcome::rejected (reason::readOnly);

            doc::Value value;

            if (! doc::Schema::parseValue (*target.attribute, text, value).ok)
                return Outcome::rejected (reason::typeMismatch);

            const auto canonical = doc::Schema::formatValue (*target.attribute, value);

            /*  WHAT THE SHOW ALREADY SAYS IS NO CHANGE: a knob turned back to
                where the cue was saved leaves nothing riding, so the count the
                window shows is of what really differs. */
            const auto saved = document.getAttribute (address).value_or (std::string {});

            if (isEqRow (*parts))
            {
                if (canonical == saved)
                    live.dropRow (parts->id, parts->row);
                else
                    live.setRow (parts->id, parts->row, canonical);
            }
            else if (canonical == saved)
            {
                live.dropSendValue (parts->id, parts->row);
            }
            else
            {
                const auto owner = target.node.getParent();
                live.setSendValue (parts->id, owner.getProperty ("id").toString().toStdString(),
                                   target.node.getProperty ("bus").toString().toStdString(),
                                   parts->row, canonical);
            }

            return Outcome::ok (args);
        };
    }

    doc::LiveCreate liveSendFor (LiveEdits& live, doc::ShowDocument& document)
    {
        return [&live, &document] (const std::vector<osc::Value>& args) -> std::optional<Outcome>
        {
            const auto cueId = args[0].getString();
            const auto busId = args[1].getString();

            /*  UNLOCKED, the show makes its sends - unless a send made live
                already goes there, which Keep would then make twice. */
            if (! document.isLocked())
            {
                if (live.sendsInto (cueId, busId))
                    return Outcome::rejected (reason::badValue);

                return std::nullopt;
            }

            const auto cue = document.findById (cueId);

            if (! cue.isValid())
                return Outcome::rejected (reason::unknownId);

            if (! cue.hasType ("Media"))
                return Outcome::rejected (reason::typeMismatch);

            const auto bus = document.findById (busId);

            if (! bus.isValid() || ! bus.hasType ("Bus"))
                return Outcome::rejected (reason::unknownId);

            //  A MIX CHANNEL, which is what a send is into; a direct out is a cue's own.
            if (document.getAttribute ("/godot/bus/" + busId + "/kind").value_or (std::string {}) != "mix")
                return Outcome::rejected (reason::badValue);

            //  ONE SEND PER BUS PER CUE, the show's rule, across the show and the layer together.
            if (! sendOfCueInto (cue, busId).empty() || live.sendsInto (cueId, busId))
                return Outcome::rejected (reason::badValue);

            std::string level = "0";

            if (args.size() > 3 && ! args[3].getString().empty())
            {
                const auto canonical = sendText ("level", args[3].getString());

                if (! canonical.has_value())
                    return Outcome::rejected (reason::typeMismatch);

                level = *canonical;
            }

            /*  AN IDENTIFIER OF THE SHOW'S OWN, drawn here or handed back by a
                replay, and reserved so nothing else takes it before Keep gives
                it to the real Send. */
            auto id = args.size() > 2 ? args[2].getString() : std::string {};

            if (id.empty())
                id = document.ids().generate();
            else if (! document.ids().reserve (id))
                return Outcome::rejected (reason::badValue);

            live.createSend (id, cueId, busId, level);
            return Outcome::ok (withId (args, 2, id));
        };
    }

    bool isLiveEdit (const std::string& commandName, const std::vector<osc::Value>& args,
                     const doc::ShowDocument& document, const LiveEdits& live)
    {
        if (commandName == "send.create" || commandName == "eq.reset")
            return document.isLocked();

        if (commandName != "node.set" || args.empty() || ! args[0].isString())
            return false;

        //  A plugin's parameter rides live under the lock too (2026-09-26).
        if (isFxParameterAddress (args[0].getString()))
            return document.isLocked();

        const auto parts = partsOf (args[0].getString());

        if (! parts.has_value())
            return false;

        if (isEqRow (*parts))
            return document.isLocked();

        if (isSendRow (*parts))
        {
            const auto* riding = live.sendOf (parts->id);
            return document.isLocked() || (riding != nullptr && riding->created);
        }

        return false;
    }

    //==========================================================================
    void registerLiveCommands (CommandRegistry& registry, doc::ShowDocument& document, LiveEdits& live)
    {
        /*  KEEP: what rode live under the lock, written into the show - one
            command, so one transaction, so one undo step. A send made live
            becomes a real Send with the identifier it rode under, so a client
            that was looking at it is still looking at it; if the show has
            since been given a send into that bus, that one takes the values. A
            cue or a bus deleted since is skipped. */
        registry.add ({ "live.keep",
                        "Writes the EQ, send and plugin changes ridden live while the show was locked into"
                        " the show, as one undo step. Refused while the show is locked.",
                        {},
                        true,
                        [&document, &live] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            for (const auto& [cueId, riding] : live.allRows())
                            {
                                if (! document.findById (cueId).isValid())
                                    continue;

                                for (const auto& [row, text] : riding)
                                    document.setAttribute ("/godot/cue/" + cueId + "/" + row, text);
                            }

                            for (const auto& [sendId, send] : live.allSends())
                            {
                                auto target = sendId;

                                if (send.created)
                                {
                                    const auto cue = document.findById (send.cue);
                                    document.ids().release (sendId);

                                    if (! cue.isValid())
                                        continue;

                                    if (const auto existing = sendOfCueInto (cue, send.bus); ! existing.empty())
                                    {
                                        target = existing;
                                    }
                                    else
                                    {
                                        const auto made = document.createSend (send.cue, send.bus, sendId,
                                                                               send.level.value_or (std::string {}));

                                        if (! made.ok)
                                            continue;

                                        if (send.on.has_value())
                                            document.setAttribute ("/godot/send/" + sendId + "/on", *send.on);

                                        continue;
                                    }
                                }

                                if (! document.findById (target).isValid())
                                    continue;

                                if (send.level.has_value())
                                    document.setAttribute ("/godot/send/" + target + "/level", *send.level);

                                if (send.on.has_value())
                                    document.setAttribute ("/godot/send/" + target + "/on", *send.on);
                            }

                            /*  A PLUGIN'S PARAMETERS (2026-09-26): merged into
                                the insert's values row, as the p<n> door writes
                                them; an insert deleted since is skipped. */
                            for (const auto& [fxId, riding] : live.allFx())
                            {
                                const auto insert = document.findById (fxId);

                                if (! insert.isValid() || ! insert.hasType ("Fx"))
                                    continue;

                                auto values = parseFxValues (insert.getProperty ("values").toString().toStdString());

                                for (const auto& [index, value] : riding)
                                    values[index] = value;

                                document.setAttribute ("/godot/fx/" + fxId + "/values", formatFxValues (values));
                            }

                            live.clear();
                            return Outcome::ok (args);
                        } });

        /*  DISCARD: the cues back to their saved sound. Whatever the lock
            says - letting go of a live change is never an edit. The
            identifiers the sends made live had reserved are given back. */
        registry.add ({ "live.drop",
                        "Lets go of the EQ, send and plugin changes ridden live while the show was locked,"
                        " so every cue sounds as it is saved.",
                        {},
                        true,
                        [&document, &live] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            for (const auto& [sendId, send] : live.allSends())
                                if (send.created)
                                    document.ids().release (sendId);

                            live.clear();
                            return Outcome::ok (args);
                        } });
    }
}
