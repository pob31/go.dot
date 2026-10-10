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

#include <wfg/engine/tree/AuthoringCommands.h>

#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/document/Sequence.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/TreeCommands.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree
{
    namespace
    {
        const juce::Identifier idKey { "id" };
        const juce::Identifier numberKey { "number" };
        const juce::Identifier nameKey { "name" };
        const juce::Identifier standbyKey { "standby" };

        std::string textOf (const juce::ValueTree& node, const juce::Identifier& key)
        {
            return node.getProperty (key).toString().toStdString();
        }

        std::string attributeOr (const doc::ShowDocument& document, const std::string& address)
        {
            return document.getAttribute (address).value_or (std::string {});
        }

        std::vector<std::string> wordsOf (const std::string& text)
        {
            std::vector<std::string> words;
            std::string word;

            for (const auto c : text)
            {
                if (c == ' ')
                {
                    if (! word.empty())
                        words.push_back (word);
                    word.clear();
                    continue;
                }

                word += c;
            }

            if (! word.empty())
                words.push_back (word);

            return words;
        }

        std::string joined (const std::vector<std::string>& words)
        {
            std::string out;

            for (const auto& word : words)
                out += (out.empty() ? "" : " ") + word;

            return out;
        }

        /*  A ROOT A PROCESSOR MAY DECLARE: absolute, one segment run with no
            empty part, no trailing slash, no space (a declare names one root),
            and nowhere under Go.dot's own `/godot`, `/ui` or `/media` - which
            `createMount` does not refuse, and which would otherwise only be
            found when the show next loaded and refused the device. */
        bool isUsableRoot (const std::string& prefix)
        {
            if (prefix.size() < 2 || prefix.front() != '/' || prefix.back() == '/')
                return false;

            if (prefix.find (' ') != std::string::npos || prefix.find ("//") != std::string::npos)
                return false;

            for (const std::string reserved : { "/godot", "/ui", "/media" })
                if (prefix == reserved || prefix.rfind (reserved + "/", 0) == 0)
                    return false;

            return true;
        }

        /*  The declared device a datagram's host IS, by its `host` row - which
            is how an answer reaches the processor that asked rather than the
            device its cue happens to aim at. Empty when none is. */
        std::string mountAtHost (const doc::ShowDocument& document, const std::string& host)
        {
            if (host.empty())
                return {};

            for (const auto& mountId : declaredMountIds (document))
                if (const auto declaration = mountDeclarationFor (document, mountId);
                    declaration && declaration->host == host && declaration->serial.empty())
                    return mountId;

            return {};
        }

        /*  The declared device whose prefix covers `address` the furthest, or
            empty. Read off the document: see the header. */
        std::string mountCovering (const doc::ShowDocument& document, const std::string& address)
        {
            std::string best;
            std::size_t bestLength = 0;

            for (const auto& mountId : declaredMountIds (document))
            {
                const auto declaration = mountDeclarationFor (document, mountId);

                if (! declaration)
                    continue;

                if (const auto length = prefixMatchLength (address, declaration->prefix); length > bestLength)
                {
                    best = mountId;
                    bestLength = length;
                }
            }

            return best;
        }

        /*  The declared device that holds `root` among its prefixes, exactly. */
        std::string mountWithRoot (const doc::ShowDocument& document, const std::string& root)
        {
            for (const auto& mountId : declaredMountIds (document))
                if (const auto declaration = mountDeclarationFor (document, mountId))
                    for (const auto& each : prefixesOf (declaration->prefix))
                        if (each == root)
                            return mountId;

            return {};
        }

        /*  The cue numbered `number`, anywhere in the show's lists, the focused
            list first - the rule `cue.fireNumber` uses too. Exact text: "12" is
            not "12.0". */
        juce::ValueTree cueNumbered (const doc::ShowDocument& document, const std::string& number)
        {
            if (number.empty())
                return {};

            juce::ValueTree found;

            const std::function<void (const juce::ValueTree&)> walk = [&] (const juce::ValueTree& node)
            {
                for (const auto& child : node)
                {
                    if (found.isValid())
                        return;

                    if (doc::ShowDocument::ownerForElement (child.getType().toString().toStdString()) == "cue"
                        && textOf (child, numberKey) == number)
                    {
                        found = child;
                        return;
                    }

                    walk (child);
                }
            };

            const auto focused = cue::Focus {}.list (document);

            if (focused.isValid())
                walk (focused);

            for (const auto& list : document.root().getChildWithName ("Lists"))
                if (! found.isValid() && list != focused)
                    walk (list);

            return found;
        }

        /*  Where a new cue lands, as `createCue` takes it: a parent identifier
            and a member position. */
        struct Landing
        {
            std::string parent;
            int index = doc::endOfSequence;
        };

        /*  After the standby of the focused list - in the standby's own parent,
            one member later, since the standby may stand inside a group - or at
            the end of that list when nothing stands by. Nothing when the show
            has no list. */
        std::optional<Landing> afterStandby (const doc::ShowDocument& document)
        {
            const auto list = cue::Focus {}.list (document);

            if (! list.isValid())
                return std::nullopt;

            const auto listId = textOf (list, idKey);
            const auto standby = document.findById (textOf (list, standbyKey));

            if (! standby.isValid())
                return Landing { listId, doc::endOfSequence };

            const auto parent = standby.getParent();
            int position = 0;

            for (const auto& sibling : parent)
            {
                if (sibling == standby)
                    break;

                if (doc::isSequenceChild (sibling))
                    ++position;
            }

            return Landing { textOf (parent, idKey), position + 1 };
        }

        /*  The end of a list named by identifier or by name; the focused list
            when `target` is empty. */
        std::optional<Landing> endOfList (const doc::ShowDocument& document, const std::string& target)
        {
            if (target.empty())
            {
                const auto list = cue::Focus {}.list (document);
                return list.isValid() ? std::optional<Landing> (Landing { textOf (list, idKey), doc::endOfSequence })
                                      : std::nullopt;
            }

            if (const auto byId = document.findById (target); byId.isValid() && byId.hasType ("List"))
                return Landing { target, doc::endOfSequence };

            for (const auto& list : document.root().getChildWithName ("Lists"))
                if (list.hasType ("List") && textOf (list, nameKey) == target)
                    return Landing { textOf (list, idKey), doc::endOfSequence };

            return std::nullopt;
        }

        osc::Packet answerPacket (const std::string& address, std::vector<std::string> words)
        {
            std::vector<osc::Value> values;

            for (auto& word : words)
                values.push_back (osc::Value::string (std::move (word)));

            return osc::Packet::message (address, std::move (values));
        }
    }

    //==========================================================================
    std::string originHost (const std::string& origin)
    {
        const auto isUdp = origin.rfind ("udp:", 0) == 0;
        const auto isWs = origin.rfind ("ws:", 0) == 0;

        if (! isUdp && ! isWs)
            return {};

        const auto first = origin.find (':');
        const auto last = origin.rfind (':');

        if (last == first)
            return origin.substr (first + 1);

        return origin.substr (first + 1, last - first - 1);
    }

    std::optional<AnswerAddress> answerAddressOf (const doc::ShowDocument& document, const std::string& mountId)
    {
        const auto declaration = mountDeclarationFor (document, mountId);

        if (! declaration || ! declaration->serial.empty() || declaration->port < 1 || declaration->port > 65535
            || declaration->host.empty())
            return std::nullopt;

        return AnswerAddress { declaration->host, declaration->port };
    }

    //==========================================================================
    void registerAuthoringCommands (CommandRegistry& registry, doc::ShowDocument& document, RawSender& answers,
                                    DescribeRequest describe)
    {
        //----------------------------------------------------------------------
        /*  THE PROCESSOR BECOMES A DEVICE (namespace draft §56, AEJ).

            Matched by its ROOT, which is what every cue aimed at it carries and
            what an operator would recognise: a device already holding `/wfs`
            is WFS-DIY whatever it is called. Found, only where it is moves -
            host, port, query port - because those are facts about the rig
            tonight and the processor knows them better than the show does;
            its name, rx and tx are the operator's once the row exists. Not
            found, it is made with rx and tx on: a processor that declares
            itself means to be heard and spoken to.

            THE HOST IS THE DATAGRAM'S, recorded. A processor rarely knows
            which of its addresses Go.dot sees, and the one the packet came
            from is the one that reached; written into the applied record, it
            is what a replay uses, which has no packet to read. */
        registry.add ({ "mount.declare",
                        "A processor declares itself as a device: made at its root the first time, its host and"
                        " ports moved after. Answers /godot/declared <id> <outcome> at host:port.",
                        { { "prefix", 's', false }, { "port", 'i', false }, { "queryPort", 'i', false },
                          { "name", 's', false }, { "id", 's', true }, { "host", 's', true } },
                        true,
                        [&document, &answers, describe] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto prefix = args[0].getString();
                            const auto port = args[1].getInt32();
                            const auto queryPort = args[2].getInt32();
                            const auto deviceName = args[3].getString();
                            const auto wantedId = args.size() > 4 ? args[4].getString() : std::string {};
                            auto host = args.size() > 5 ? args[5].getString() : std::string {};

                            if (host.empty() && context.origin != nullptr)
                                host = originHost (*context.origin);

                            const auto answer = [&answers, &host, port] (const std::string& deviceId,
                                                                         const std::string& outcome)
                            {
                                answers.queue (host, port, answerPacket ("/godot/declared", { deviceId, outcome }));
                            };

                            if (host.empty() || ! isUsableRoot (prefix) || deviceName.empty()
                                || port < 1 || port > 65535 || queryPort < 0 || queryPort > 65535)
                            {
                                answer (wantedId, reason::badValue);
                                return Outcome::rejected (reason::badValue);
                            }

                            auto deviceId = mountWithRoot (document, prefix);
                            const auto creating = deviceId.empty();

                            /*  Each row only when it differs, so a declare that
                                changes nothing - a processor reconnecting, or
                                saying its snapshots changed - is an empty step
                                rather than writes of what was already there. */
                            std::vector<std::pair<std::string, std::string>> writes {
                                { "host", host }, { "port", std::to_string (port) },
                                { "queryPort", std::to_string (queryPort) } };

                            if (creating)
                            {
                                writes.push_back ({ "name", deviceName });
                                writes.push_back ({ "rx", "true" });
                                writes.push_back ({ "tx", "true" });
                            }
                            else
                            {
                                const auto base = "/godot/mount/" + deviceId + "/";
                                std::erase_if (writes, [&document, &base] (const auto& write)
                                                       { return attributeOr (document, base + write.first) == write.second; });
                            }

                            /*  UNDER THE LOCK ONLY WHAT EDITS THE SHOW IS REFUSED:
                                a new device or one that moved. A declare that
                                changes nothing is answered and its description
                                fetched again, so a snapshot stored during a locked
                                show still reaches the menus. */
                            if (document.isLocked() && (creating || ! writes.empty()))
                            {
                                answer (creating ? wantedId : deviceId, reason::locked);
                                return Outcome::rejected (reason::locked);
                            }

                            if (creating)
                            {
                                const auto made = document.createMount (prefix, {}, wantedId);

                                if (! made.ok)
                                {
                                    answer (wantedId, made.reason);
                                    return Outcome::rejected (made.reason);
                                }

                                deviceId = made.id;
                            }

                            const auto base = "/godot/mount/" + deviceId + "/";

                            for (const auto& [row, text] : writes)
                                if (const auto edit = document.setAttribute (base + row, text); ! edit.ok)
                                {
                                    answer (deviceId, edit.reason);
                                    return Outcome::rejected (edit.reason);
                                }

                            answer (deviceId, creating ? "created" : "updated");

                            if (queryPort > 0 && describe)
                                describe (deviceId, host, queryPort, prefix);

                            auto applied = args;
                            applied.resize (6, osc::Value::string ({}));
                            applied[4] = osc::Value::string (deviceId);
                            applied[5] = osc::Value::string (host);
                            return Outcome::ok (std::move (applied));
                        } });

        //----------------------------------------------------------------------
        /*  ONE CUE, MADE OR UPDATED (namespace draft §56, AEK).

            `where` says where a NEW cue lands: after the standby, at the end of
            a list, in place of a cue named by identifier or number, or - for a
            capture too long for one datagram - appended to the cue the first
            chunk made. The IDENTIFIER WINS over it: a cue that already holds
            `id` is updated where it stands, which is how a processor's second
            export of a snapshot replaces the first instead of adding to it.

            The first pair is the cue's own message, the rest its further
            messages in order, all under ONE declared device (the one-device
            rule of §45, YV). Every address and every value is checked before
            anything is written, so the only refusal that can arrive half way
            is a door's - and the lock, the one that could, is asked first. */
        registry.add ({ "cue.capture",
                        "A processor writes one OSC cue: after the standby, at the end of a list, in place of a"
                        " cue, or appended to one; the same cue updated when its id already exists. Answers"
                        " /godot/captured <id> <outcome> <number> <name>.",
                        { { "where", 's', false }, { "target", 's', false }, { "id", 's', false },
                          { "name", 's', false }, { "number", 's', false }, { "notes", 's', false },
                          { "messageIds", 's', false }, { "pair", 's', true, true } },
                        true,
                        [&document, &answers] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto where = args[0].getString();
                            const auto target = args[1].getString();
                            auto cueId = args[2].getString();
                            const auto cueName = args[3].getString();
                            const auto cueNumber = args[4].getString();
                            const auto cueNotes = args[5].getString();
                            const auto recordedIds = wordsOf (args[6].getString());

                            std::vector<std::pair<std::string, std::string>> pairs;

                            for (std::size_t at = 7; at + 1 < args.size(); at += 2)
                                pairs.emplace_back (args[at].getString(), args[at + 1].getString());

                            const auto senderHost = context.origin != nullptr ? originHost (*context.origin)
                                                                               : std::string {};

                            /*  WHERE THE ANSWER GOES: the device the sender IS,
                                else the device the cue aims at - S21_HiJack's
                                console capture aims at the desk, and the answer
                                still belongs to S21_HiJack when it declared
                                itself. Neither: nobody is answered, and the `R`
                                record and lastError are where a refusal is read. */
                            const auto answer = [&] (const std::string& outcome)
                            {
                                auto deviceId = mountAtHost (document, senderHost);

                                if (deviceId.empty() && ! pairs.empty())
                                    deviceId = mountCovering (document, pairs.front().first);

                                const auto to = deviceId.empty() ? std::nullopt : answerAddressOf (document, deviceId);

                                if (! to)
                                    return;

                                const auto landed = outcome == "created" || outcome == "updated" || outcome == "appended";
                                const auto base = "/godot/cue/" + cueId + "/";

                                answers.queue (to->host, to->port,
                                               answerPacket ("/godot/captured",
                                                             { cueId, outcome,
                                                               landed ? attributeOr (document, base + "number") : std::string {},
                                                               landed ? attributeOr (document, base + "name") : std::string {} }));
                            };

                            const auto refuse = [&] (const std::string& why)
                            {
                                answer (why);
                                return Outcome::rejected (why);
                            };

                            if (document.isLocked())
                                return refuse (reason::locked);

                            //--------------------------------------------------
                            // Everything checked before anything is written.

                            if (where != "standby" && where != "list" && where != "cue" && where != "more")
                                return refuse (reason::badValue);

                            if (pairs.empty() || (args.size() - 7) % 2 != 0)
                                return refuse (reason::badValue);

                            for (const auto& [address, value] : pairs)
                                if (address.empty() || address.front() != '/' || ! osc::valuesFromAtoms (value))
                                    return refuse (reason::badValue);

                            const auto device = mountCovering (document, pairs.front().first);

                            if (device.empty())
                                return refuse (reason::unknownId);

                            for (const auto& [address, value] : pairs)
                            {
                                const auto other = mountCovering (document, address);

                                if (other.empty())
                                    return refuse (reason::unknownId);

                                if (other != device)
                                    return refuse (reason::badAddress);
                            }

                            //--------------------------------------------------
                            // Which cue: the one `id` names, else where `where` says.

                            juce::ValueTree existing = cueId.empty() ? juce::ValueTree {} : document.findById (cueId);

                            if (where == "more" && ! existing.isValid())
                            {
                                existing = document.findById (target);
                                cueId = target;
                            }

                            if (where == "cue" && ! existing.isValid())
                            {
                                existing = document.findById (target);

                                if (! existing.isValid())
                                    existing = cueNumbered (document, target);

                                if (existing.isValid())
                                    cueId = textOf (existing, idKey);
                            }

                            if ((where == "more" || where == "cue") && ! existing.isValid())
                                return refuse (reason::unknownId);

                            if (existing.isValid() && ! existing.hasType ("Osc"))
                                return refuse (reason::typeMismatch);

                            std::vector<std::string> madeIds;
                            std::size_t nextRecorded = 0;

                            const auto addMessage = [&] (const std::string& address, const std::string& value)
                            {
                                const auto wanted = nextRecorded < recordedIds.size() ? recordedIds[nextRecorded] : std::string {};
                                ++nextRecorded;

                                const auto made = document.createMessage (cueId, address, value, wanted);

                                if (made.ok)
                                    madeIds.push_back (made.id);

                                return made;
                            };

                            std::string outcome;

                            if (where == "more")
                            {
                                for (const auto& [address, value] : pairs)
                                    if (const auto made = addMessage (address, value); ! made.ok)
                                        return refuse (made.reason);

                                outcome = "appended";
                            }
                            else if (existing.isValid())
                            {
                                /*  IN PLACE: the further messages taken away, the
                                    cue's own message and its head rewritten, the
                                    rest made again - one step, where it stands. */
                                std::vector<std::string> oldMessages;

                                for (const auto& child : existing)
                                    if (child.hasType ("Message"))
                                        oldMessages.push_back (textOf (child, idKey));

                                for (const auto& messageId : oldMessages)
                                    if (const auto removed = document.remove (messageId); ! removed.ok)
                                        return refuse (removed.reason);

                                const auto base = "/godot/cue/" + cueId + "/";

                                std::vector<std::pair<std::string, std::string>> rows {
                                    { "address", pairs.front().first }, { "value", pairs.front().second } };

                                if (! cueName.empty())   rows.push_back ({ "name", cueName });
                                if (! cueNumber.empty()) rows.push_back ({ "number", cueNumber });
                                if (! cueNotes.empty())  rows.push_back ({ "notes", cueNotes });

                                for (const auto& [row, text] : rows)
                                    if (const auto edit = document.setAttribute (base + row, text); ! edit.ok)
                                        return refuse (edit.reason);

                                for (std::size_t at = 1; at < pairs.size(); ++at)
                                    if (const auto made = addMessage (pairs[at].first, pairs[at].second); ! made.ok)
                                        return refuse (made.reason);

                                outcome = "updated";
                            }
                            else
                            {
                                const auto landing = where == "list" ? endOfList (document, target)
                                                                     : afterStandby (document);

                                if (! landing)
                                    return refuse (reason::unknownId);

                                doc::ShowDocument::Attributes born {
                                    { "address", pairs.front().first }, { "value", pairs.front().second } };

                                if (! cueNumber.empty()) born.push_back ({ "number", cueNumber });
                                if (! cueNotes.empty())  born.push_back ({ "notes", cueNotes });

                                const auto made = document.createCue (landing->parent, landing->index, "osc",
                                                                      cueName, cueId, born);

                                if (! made.ok)
                                    return refuse (made.reason);

                                cueId = made.id;

                                for (std::size_t at = 1; at < pairs.size(); ++at)
                                    if (const auto added = addMessage (pairs[at].first, pairs[at].second); ! added.ok)
                                        return refuse (added.reason);

                                outcome = "created";
                            }

                            answer (outcome);

                            auto applied = args;
                            applied[2] = osc::Value::string (cueId);
                            applied[6] = osc::Value::string (joined (madeIds));
                            return Outcome::ok (std::move (applied));
                        } });

        //----------------------------------------------------------------------
        /*  FIRING AND PARKING BY NUMBER (namespace draft §56, AEN, the author's
            pick): what a console's "GO cue 12" asks for, and what QLab's
            `/go "12"` does. The number is found - the focused list first, then
            the others, exact text - and the verb that already exists is asked
            to do the rest, by name, so there is one `cue.fire` and one
            `standby.set` and these two are only the lookup in front of them.
            The record keeps the NUMBER: a replay finds the same cue in the same
            show, and the log says what was asked for. */
        const auto byNumber = [&registry, &document] (const char* verb)
        {
            return [&registry, &document, verb] (CommandContext& context, const std::vector<osc::Value>& args)
            {
                const auto cue = cueNumbered (document, args[0].getString());

                if (! cue.isValid())
                    return Outcome::rejected (reason::unknownId);

                const auto* command = registry.find (verb);

                if (command == nullptr || ! command->handler)
                    return Outcome::rejected (reason::unknownCommand);

                auto byName = args;
                byName[0] = osc::Value::string (textOf (cue, idKey));

                auto outcome = command->handler (context, byName);

                if (outcome.applied && ! outcome.appliedArgs.empty())
                    outcome.appliedArgs[0] = args[0];

                return outcome;
            };
        };

        registry.add ({ "cue.fireNumber",
                        "Fires the cue with this number without touching standby, as cue.fire does by"
                        " identifier: the focused list first, then the others; the number as written.",
                        { { "number", 's', false }, { "run", 's', true } },
                        true,
                        byNumber ("cue.fire") });

        registry.add ({ "standby.setNumber",
                        "Parks the focused list's standby on the cue with this number, as standby.set does"
                        " by identifier. With go after it, in one bundle, it is GO at that cue.",
                        { { "number", 's', false } },
                        true,
                        byNumber ("standby.set") });
    }

    //==========================================================================
    void registerDescriptionCommands (CommandRegistry& registry, doc::ShowDocument& document, MountTable& mounts,
                                      const juce::File& bundleFolder, RawSender& answers)
    {
        /*  A FETCHED DESCRIPTION ADOPTED (namespace draft §56, AEM): submitted
            by serve's MountFetcher once the file is on disk, or with the
            problem that kept it from being. The device's `namespace` row is
            pointed at the file - which makes it a described device - and the
            file is loaded here, on this tick, as `mount.load` loads one: the
            after-tick refresh reloads only when the file's NAME changes, and a
            replay refreshes nothing, so either would leave a re-fetched menu
            stale.

            UNDER THE LOCK only what edits the show is refused: a device that
            already names this file is reloaded, which changes nothing anybody
            decided. */
        registry.add ({ "mount.described",
                        "A device's fetched description, adopted: its namespace row set to the file and the"
                        " file loaded, or the fetch's problem shown on the device. Sent by the engine."
                        " Answers /godot/described <id> <nodeCount> <problem>.",
                        { { "mount", 's', false }, { "file", 's', false }, { "nodeCount", 'i', false },
                          { "problem", 's', false } },
                        true,
                        [&document, &mounts, &bundleFolder, &answers] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto deviceId = args[0].getString();
                            const auto file = args[1].getString();
                            const auto problem = args[3].getString();

                            if (! mountDeclarationFor (document, deviceId))
                                return Outcome::rejected (reason::unknownId);

                            const auto to = answerAddressOf (document, deviceId);

                            const auto answer = [&answers, &to, &deviceId] (int nodeCount, const std::string& why)
                            {
                                if (to)
                                    answers.queue (to->host, to->port,
                                                   osc::Packet::message ("/godot/described",
                                                                         { osc::Value::string (deviceId),
                                                                           osc::Value::int32 (nodeCount),
                                                                           osc::Value::string (why) }));
                            };

                            if (! problem.empty())
                            {
                                /*  WHAT IT HAD IT KEEPS: a description that could
                                    not be read again is not a reason to forget the
                                    one that was. The problem cell says so. */
                                mounts.setProblem (deviceId, "description: " + problem);
                                answer (0, problem);
                                return Outcome::ok (args);
                            }

                            const auto row = "/godot/mount/" + deviceId + "/namespace";

                            if (attributeOr (document, row) != file)
                            {
                                if (document.isLocked())
                                {
                                    answer (0, reason::locked);
                                    return Outcome::rejected (reason::locked);
                                }

                                if (const auto edit = document.setAttribute (row, file); ! edit.ok)
                                {
                                    answer (0, edit.reason);
                                    return Outcome::rejected (edit.reason);
                                }
                            }

                            const auto loaded = loadMountFromBundle (document, mounts, bundleFolder, deviceId);

                            if (loaded.ok)
                                answer (static_cast<int> (mounts.nodeCount (deviceId)), {});
                            else
                                answer (0, loaded.problems.empty() ? std::string ("the description did not load")
                                                                   : loaded.problems.front());

                            return Outcome::ok (args);
                        } });
    }
}
