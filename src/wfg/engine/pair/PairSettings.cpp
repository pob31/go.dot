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

#include <wfg/engine/pair/PairSettings.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/osc/OscValue.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace wfg::pair
{
    namespace
    {
        /*  ASCII ONLY, AND NEVER THROUGH THE LOCALE: `std::isalnum` asks the C
            locale, which a French machine has set, and a host name is the same
            word on every machine. */
        bool isDigit (char c) noexcept { return c >= '0' && c <= '9'; }

        bool isHostCharacter (char c) noexcept
        {
            return isDigit (c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || c == '.' || c == '-' || c == '_';
        }

        bool isHexDigit (char c) noexcept
        {
            return isDigit (c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        /*  A port: digits only, every one of them read, from 1 to 65535. No
            sign - `from_chars` takes none for an int, and a `+` is refused. */
        std::optional<int> portOf (std::string_view text)
        {
            if (text.empty() || ! std::all_of (text.begin(), text.end(), isDigit))
                return std::nullopt;

            int port = 0;
            const auto read = std::from_chars (text.data(), text.data() + text.size(), port);

            if (read.ec != std::errc {} || read.ptr != text.data() + text.size() || port < 1 || port > 65535)
                return std::nullopt;

            return port;
        }

        const char* const fileName = "pairing.xml";
    }

    //==========================================================================
    bool isRole (std::string_view word)
    {
        return word == "main" || word == "backup" || word == "none";
    }

    std::optional<std::string> peerAddress (std::string_view text)
    {
        std::string_view host, port;

        if (! text.empty() && text.front() == '[')
        {
            /*  AN IPv6 ADDRESS, BRACKETED as a URL writes one, so that its
                colons are not taken for the port's. */
            const auto close = text.find (']');

            if (close == std::string_view::npos || close < 2)
                return std::nullopt;

            const auto inside = text.substr (1, close - 1);

            if (! std::all_of (inside.begin(), inside.end(),
                               [] (char c) { return isHexDigit (c) || c == ':' || c == '.'; }))
                return std::nullopt;

            host = text.substr (0, close + 1);

            const auto rest = text.substr (close + 1);

            if (! rest.empty())
            {
                if (rest.front() != ':')
                    return std::nullopt;

                port = rest.substr (1);

                if (port.empty())
                    return std::nullopt;
            }
        }
        else
        {
            const auto colon = text.find (':');
            host = text.substr (0, colon);

            if (colon != std::string_view::npos)
            {
                port = text.substr (colon + 1);

                if (port.empty())
                    return std::nullopt;
            }

            /*  A second colon lands in the port and is refused there: an IPv6
                address without its brackets, whose last group would otherwise
                be dialled as a port. */
            if (host.empty() || ! std::all_of (host.begin(), host.end(), isHostCharacter))
                return std::nullopt;
        }

        auto number = defaultPeerPort;

        if (! port.empty())
        {
            const auto read = portOf (port);

            if (! read.has_value())
                return std::nullopt;

            number = *read;
        }

        return std::string (host) + ":" + std::to_string (number);
    }

    std::optional<std::vector<std::string>> peerAddresses (std::string_view text)
    {
        std::vector<std::string> peers;
        std::size_t at = 0;

        const auto isSeparator = [] (char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; };

        while (at < text.size())
        {
            while (at < text.size() && isSeparator (text[at]))
                ++at;

            const auto start = at;

            while (at < text.size() && ! isSeparator (text[at]))
                ++at;

            if (at == start)
                continue;

            const auto address = peerAddress (text.substr (start, at - start));

            if (! address.has_value())
                return std::nullopt;

            /*  Twice is once: two paths to the one address are one path. */
            if (std::find (peers.begin(), peers.end(), *address) == peers.end())
                peers.push_back (*address);
        }

        return peers;
    }

    std::string peersText (const std::vector<std::string>& peers)
    {
        std::string text;

        for (const auto& peer : peers)
        {
            if (! text.empty())
                text += ' ';

            text += peer;
        }

        return text;
    }

    bool isTimeout (double seconds)
    {
        return std::isfinite (seconds) && seconds >= shortestTimeout && seconds <= longestTimeout;
    }

    //==========================================================================
    Loaded load (const std::string& path)
    {
        Loaded loaded;

        const juce::File file { juce::String (path) };

        if (! file.existsAsFile())
            return loaded;

        const auto xml = juce::XmlDocument::parse (file);

        if (xml == nullptr || ! xml->hasTagName ("Pairing"))
        {
            loaded.problem = file.getFullPathName().toStdString()
                           + " is not a pairing file Go.dot can read: this machine is not paired until its role is set again";
            return loaded;
        }

        std::vector<std::string> setAside;

        if (xml->hasAttribute ("role"))
        {
            const auto role = xml->getStringAttribute ("role").toStdString();

            if (isRole (role))
                loaded.settings.role = role;
            else
                setAside.push_back ("its role \"" + role + "\" is none of main, backup and none, so the role is none");
        }

        if (xml->hasAttribute ("peers"))
        {
            const auto text = xml->getStringAttribute ("peers").toStdString();

            if (const auto peers = peerAddresses (text); peers.has_value())
                loaded.settings.peers = *peers;
            else
                setAside.push_back ("its peers \"" + text + "\" are not all host:port addresses, so there is no peer");
        }

        if (xml->hasAttribute ("timeout"))
        {
            const auto text = xml->getStringAttribute ("timeout").toStdString();
            const auto seconds = osc::parseDouble (text);

            if (! seconds.has_value())
            {
                setAside.push_back ("its timeout \"" + text + "\" is not a number of seconds, so it is "
                                    + osc::formatDouble (defaultTimeout) + " s");
            }
            else if (! isTimeout (*seconds))
            {
                loaded.settings.timeout = std::clamp (*seconds, shortestTimeout, longestTimeout);
                setAside.push_back ("its timeout of " + text + " s is outside " + osc::formatDouble (shortestTimeout)
                                    + " to " + osc::formatDouble (longestTimeout) + " s, so it is "
                                    + osc::formatDouble (loaded.settings.timeout) + " s");
            }
            else
            {
                loaded.settings.timeout = *seconds;
            }
        }

        if (! setAside.empty())
        {
            loaded.problem = file.getFullPathName().toStdString() + ": ";

            for (std::size_t i = 0; i < setAside.size(); ++i)
                loaded.problem += (i == 0 ? "" : "; ") + setAside[i];
        }

        return loaded;
    }

    std::string save (const std::string& path, const PairSettings& settings)
    {
        /*  ONLY WHAT THIS MACHINE COULD READ BACK: a file the next launch would
            set aside is not worth the disk it is written to. The commands have
            refused all of these already; this is for any other caller. */
        if (! isRole (settings.role) || ! isTimeout (settings.timeout))
            return "Invalid pairing settings";

        for (const auto& peer : settings.peers)
            if (peerAddress (peer) != std::optional<std::string> (peer))
                return "Invalid pairing settings";

        juce::XmlElement xml ("Pairing");
        xml.setAttribute ("role", juce::String (settings.role));
        xml.setAttribute ("peers", juce::String (peersText (settings.peers)));
        xml.setAttribute ("timeout", juce::String (osc::formatDouble (settings.timeout)));

        const juce::File file { juce::String (path) };

        if (! file.getParentDirectory().createDirectory())
            return "Could not create " + file.getParentDirectory().getFullPathName().toStdString()
                     + " for " + fileName;

        /*  THROUGH A TEMPORARY FILE AND A RENAME, as the audio defaults are:
            a crash mid-write leaves the old file whole, never half of a new
            one that the next launch would read as no pairing at all. */
        juce::TemporaryFile temporary (file);

        if (! xml.writeTo (temporary.getFile()) || ! temporary.overwriteTargetFileWithTemporary())
            return "Could not write " + file.getFullPathName().toStdString();

        return {};
    }

    //==========================================================================
    void registerPairCommands (CommandRegistry& registry, Pairing& pairing)
    {
        /*  A SETTING CHANGED: published from the next snapshot, and the file
            asked for - written elsewhere, never here (SavePump.h). */
        const auto changed = [&pairing]
        {
            ++pairing.revision;

            if (pairing.requestSave)
                pairing.requestSave (pairing.stored);
        };

        /*  ALL FOUR THIS MACHINE'S (RD.3, AGC): never sent down the link, since
            the other machine's role, peers and timeout are its own - a backup
            told `main` by its main would be two mains - and no step of undo.
            NOT REFUSED FOR THE LOCK: the lock fixes the show, and these are
            the computer it runs on; they knock at no door of the document. */
        registry.add (machineLocal ({ "pair.setRole",
            "Sets this machine's role in a pair of machines playing one show - main, backup, or none for a"
            " machine that is not paired - and writes it to pairing.xml in Go.dot's own folder. Never the"
            " show's: a show copied to the other machine keeps that machine's role.",
            { { "role", 's', false } }, true,
            [&pairing, changed] (CommandContext&, const std::vector<osc::Value>& args)
            {
                const auto& role = args[0].getString();

                if (! isRole (role))
                    return Outcome::rejected (reason::badValue);

                pairing.inForce.role = role;
                pairing.stored.role = role;
                changed();

                return Outcome::ok (args);
            } }));

        registry.add (machineLocal ({ "pair.setPeers",
            "Sets the other machine's addresses, host:port separated by spaces - two for two network paths;"
            " a port left off is 5010 - and writes them to pairing.xml. Empty: no peer.",
            { { "addresses", 's', false } }, true,
            [&pairing, changed] (CommandContext&, const std::vector<osc::Value>& args)
            {
                const auto peers = peerAddresses (args[0].getString());

                if (! peers.has_value())
                    return Outcome::rejected (reason::badValue);

                pairing.inForce.peers = *peers;
                pairing.stored.peers = *peers;
                changed();

                /*  AS APPLIED, each made host:port: the record says what this
                    machine dials, and a replay applies exactly that. */
                return Outcome::ok ({ osc::Value::string (peersText (*peers)) });
            } }));

        registry.add (machineLocal ({ "pair.setTimeout",
            "Sets the seconds of silence from the other machine that lose the link, from a tenth to thirty,"
            " and writes them to pairing.xml.",
            { { "seconds", 'f', false } }, true,
            [&pairing, changed] (CommandContext&, const std::vector<osc::Value>& args)
            {
                /*  A FLOAT ON THE WIRE, READ AS THE NUMBER IT SPELLS: 0.1f is
                    0.100000001490116 as a double, which nobody typed and which
                    the file would then hold. Through the float's own shortest
                    text, which reads back as the same float, the double is 0.1. */
                const auto seconds = args[0].isFloat32()
                                       ? osc::parseDouble (osc::formatFloat (args[0].getFloat32()))
                                       : std::optional<double> (args[0].asDouble());

                if (! seconds.has_value() || ! isTimeout (*seconds))
                    return Outcome::rejected (reason::badValue);

                pairing.inForce.timeout = *seconds;
                pairing.stored.timeout = *seconds;
                changed();

                return Outcome::ok (args);
            } }));

        /*  WHAT CAME OF A WRITE, submitted by serve's pump once the file is on
            the disk or is not. Written, it is an ordinary record; not written,
            it is refused `write-failed` with the reason after it, so it is the
            sentence a client reads at `/godot/engine/lastError` - the settings
            are in force for this run all the same, and the next launch reads
            what the disk still holds. A record either way, as `audio.defaultsReady`
            is, so a replay applies what the session heard. */
        registry.add (machineLocal ({ "pair.saved",
            "This machine's pairing.xml was written, or was not: the reason, empty when it was.",
            { { "error", 's', false } }, false,
            [] (CommandContext&, const std::vector<osc::Value>& args)
            {
                if (! args[0].getString().empty())
                {
                    auto refused = Outcome::rejected (reason::writeFailed);
                    refused.detail = args[0].getString();
                    return refused;
                }

                return Outcome::ok (args);
            } }));
    }
}
