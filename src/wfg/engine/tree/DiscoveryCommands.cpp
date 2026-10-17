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

#include <wfg/engine/tree/DiscoveryCommands.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/tree/TreeCommands.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace wfg::tree
{
    namespace
    {
        std::string attributeOf (const doc::ShowDocument& document, const std::string& address)
        {
            return document.getAttribute (address).value_or (std::string {});
        }
    }

    bool isIpv4Address (const std::string& text)
    {
        int parts = 0;
        std::size_t at = 0;

        while (at <= text.size())
        {
            const auto dot = text.find ('.', at);
            const auto part = text.substr (at, dot == std::string::npos ? std::string::npos : dot - at);

            if (part.empty() || part.size() > 3
                || ! std::all_of (part.begin(), part.end(), [] (char c) { return c >= '0' && c <= '9'; })
                || std::stoi (part) > 255)
                return false;

            ++parts;

            if (dot == std::string::npos)
                break;

            at = dot + 1;
        }

        return parts == 4;
    }

    void registerDiscoveryCommands (CommandRegistry& registry, doc::ShowDocument& document, MountTable& mounts,
                                    const juce::File& bundleFolder, DiscoveryHooks hooks)
    {
        /*  THE NETWORK ASKED NOW (namespace draft §76, ARW): what answers is
            `network/found`, filled by the discovery thread; nothing here
            waits for it. */
        registry.add (machineLocal ({ "network.browse",
                        "Asks the network now for the OSCQuery servers announced on it (namespace draft 76):"
                        " what answers is listed in network/found. Taken and ignored where nothing asks.",
                        {},
                        false,
                        [ask = std::move (hooks.browse)] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (ask)
                                ask();

                            return Outcome::ok (args);
                        } }));

        /*  A DEVICE FROM AN ADDRESS (namespace draft §76, ARX): picked in FOUND
            or typed. The identifier is drawn here, on the tick, so the fetch
            can name the file before the device exists; it is on the record,
            and a replay - whose hook is empty - draws nothing and asks nobody:
            its `mount.discovered` record makes the device. Refused under the
            lock, since what it starts ends in a device made. */
        registry.add ({ "mount.describeAt",
                        "Makes a device from the OSCQuery description at an address (namespace draft 76): a server"
                        " found on the network or typed. Its description and HOST_INFO are fetched off the tick;"
                        " mount.discovered makes the device, at the description's own roots.",
                        { { "host", 's', false }, { "queryPort", 'i', false }, { "name", 's', true }, { "id", 's', true } },
                        true,
                        [&document, ask = std::move (hooks.describe)] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto host = args[0].getString();
                            const auto queryPort = args[1].getInt32();
                            const auto named = args.size() > 2 ? args[2].getString() : std::string {};
                            auto id = args.size() > 3 ? args[3].getString() : std::string {};

                            if (! isIpv4Address (host) || queryPort < 1 || queryPort > 65535)
                                return Outcome::rejected (reason::badValue);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            if (id.empty())
                                id = document.ids().generate();

                            if (ask)
                                ask (id, host, queryPort, named);

                            return Outcome::ok ({ osc::Value::string (host), osc::Value::int32 (queryPort),
                                                  osc::Value::string (named), osc::Value::string (id) });
                        } });

        /*  WHAT THE FETCH LEARNED, APPLIED (§76, ARX): the engine's own record.
            Every root free - the device made, rx and tx on, as a processor
            that declares itself is. Every root held by one device - that one
            moved to this address and description: the same box answering
            somewhere else tonight. Anything else - roots split across
            devices, or some held and some not - is refused in a sentence,
            since a device merged by guess is two shows' worth of cues aimed
            somewhere nobody chose. */
        registry.add ({ "mount.discovered",
                        "A device's description fetched from an address, applied: the device made at its roots, or"
                        " the device already holding them moved to it. Sent by the engine.",
                        { { "id", 's', false }, { "file", 's', false }, { "host", 's', false }, { "transport", 's', false },
                          { "oscPort", 'i', false }, { "queryPort", 'i', false }, { "readback", 's', false },
                          { "name", 's', false }, { "roots", 's', false }, { "nodeCount", 'i', false },
                          { "problem", 's', false }, { "device", 's', true } },
                        true,
                        [&document, &mounts, &bundleFolder, told = std::move (hooks.told)]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();
                            const auto file = args[1].getString();
                            const auto host = args[2].getString();
                            const auto transport = args[3].getString();
                            const auto oscPort = args[4].getInt32();
                            const auto queryPort = args[5].getInt32();
                            const auto readback = args[6].getString();
                            const auto named = args[7].getString();
                            const auto roots = args[8].getString();
                            const auto problem = args[10].getString();

                            const auto say = [&told] (const std::string& sentence)
                            {
                                if (told)
                                    told (sentence);
                            };

                            //  A fetch that failed made nothing, and says why.
                            if (! problem.empty())
                            {
                                say (problem);
                                return Outcome::ok (args);
                            }

                            const auto wanted = prefixesOf (roots);
                            std::vector<std::string> holders;

                            for (const auto& root : wanted)
                                holders.push_back (mountWithRoot (document, root));

                            const auto unheld = std::all_of (holders.begin(), holders.end(),
                                                           [] (const std::string& each) { return each.empty(); });
                            const auto oneHolder = ! holders.empty() && ! holders.front().empty()
                                                && std::all_of (holders.begin(), holders.end(),
                                                                [&holders] (const std::string& each) { return each == holders.front(); });

                            if (! unheld && ! oneHolder)
                            {
                                say ("the roots " + roots + " belong to more than one device, or partly to one;"
                                     " a device found at " + host + " is not merged into them");
                                return Outcome::rejected (reason::badValue);
                            }

                            const auto device = unheld ? (args.size() > 11 && ! args[11].getString().empty() ? args[11].getString() : id)
                                                     : holders.front();
                            const auto base = "/godot/mount/" + device + "/";

                            std::vector<std::pair<std::string, std::string>> writes {
                                { "host", host }, { "queryPort", std::to_string (queryPort) }, { "namespace", file } };

                            if (oscPort > 0)
                                writes.push_back ({ "port", std::to_string (oscPort) });

                            writes.push_back ({ "transport", transport });

                            if (transport == "tcp")
                                writes.push_back ({ "framing", "slip" });

                            writes.push_back ({ "readback", readback });

                            if (unheld)
                            {
                                writes.push_back ({ "name", named });
                                writes.push_back ({ "rx", "true" });
                                writes.push_back ({ "tx", "true" });

                                //  A new device keeps its rows' defaults unwritten, as a preset's does.
                                std::erase_if (writes, [] (const auto& write)
                                               {
                                                   return (write.first == "transport" && write.second == "udp")
                                                       || (write.first == "readback" && write.second == "none");
                                               });
                            }
                            else
                            {
                                if (attributeOf (document, base + "prefix") != roots)
                                    writes.push_back ({ "prefix", roots });

                                //  Each row only when it differs: a device found again where it was is no edit.
                                std::erase_if (writes, [&document, &base] (const auto& write)
                                                       { return attributeOf (document, base + write.first) == write.second; });
                            }

                            if (document.isLocked() && (unheld || ! writes.empty()))
                            {
                                say ("the show is locked");
                                return Outcome::rejected (reason::locked);
                            }

                            if (unheld)
                            {
                                const auto made = document.createMount (roots, file, device);

                                if (! made.ok)
                                {
                                    say ("the device could not be made: " + made.reason);
                                    return Outcome::rejected (made.reason);
                                }
                            }

                            for (const auto& [row, text] : writes)
                                if (const auto edit = document.setAttribute (base + row, text); ! edit.ok)
                                {
                                    say ("the device's " + row + " could not be written: " + edit.reason);
                                    return Outcome::rejected (edit.reason);
                                }

                            //  LOADED ON THIS TICK; what does not load is on the device's problem cell.
                            loadMountFromBundle (document, mounts, bundleFolder, device);
                            say ({});

                            auto applied = args;
                            applied.resize (12, osc::Value::string ({}));
                            applied[11] = osc::Value::string (device);
                            return Outcome::ok (std::move (applied));
                        } });
    }
}
