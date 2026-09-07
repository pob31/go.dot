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

#include <wfg/engine/midi/MidiMessages.h>

#include <cctype>

namespace wfg::midi
{
    namespace
    {
        /*  The status byte for a channel message. The channel is one-based
            everywhere a musician looks and nought-based on the wire, and this
            is the one place that conversion happens. */
        std::uint8_t status (std::uint8_t high, int channel) noexcept
        {
            return static_cast<std::uint8_t> (high | ((channel - 1) & 0x0f));
        }

        bool inSevenBits (int value) noexcept  { return value >= 0 && value <= 127; }

        int hexDigit (char c) noexcept
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
    }

    bool hexBytes (const std::string& text, Bytes& out)
    {
        out.clear();

        int high = -1;

        for (const auto c : text)
        {
            if (std::isspace (static_cast<unsigned char> (c)) != 0)
                continue;

            const auto digit = hexDigit (c);

            if (digit < 0)
                return false;

            if (high < 0)
            {
                high = digit;
                continue;
            }

            out.push_back (static_cast<std::uint8_t> ((high << 4) | digit));
            high = -1;
        }

        /*  HALF A BYTE IS NOT A BYTE. Somebody who typed an odd number of
            digits has lost one, and guessing which end it went missing from is
            not a thing a cue engine should do at half past seven. */
        return high < 0;
    }

    BuiltMessage messageFor (const MessageSpec& spec)
    {
        BuiltMessage out;

        /*  SYSEX IS THE ONE THAT IS NOT A CHANNEL MESSAGE, so it is answered
            first and on its own terms: no channel, no number, no data, and a
            length nobody here decides. */
        if (spec.type == "sysex")
        {
            Bytes body;

            if (! hexBytes (spec.sysex, body) || body.empty())
            {
                out.problem = sendError::badMessage;
                return out;
            }

            /*  THE FRAMING IS THE AUTHOR'S IF THEY WROTE IT, and added if they
                did not. A manual prints a dump with its F0 and F7; a person
                copying the middle of one out of a table does not. Both are what
                somebody meant, and the difference is not worth a refusal - but
                a dump with only one of the two is a dump that got truncated,
                and that IS. */
            const auto opensIt = body.front() == 0xf0;
            const auto closesIt = body.back() == 0xf7;

            if (opensIt != closesIt)
            {
                out.problem = sendError::badMessage;
                return out;
            }

            if (! opensIt)
            {
                out.bytes.push_back (0xf0);
                out.bytes.insert (out.bytes.end(), body.begin(), body.end());
                out.bytes.push_back (0xf7);
                return out;
            }

            /*  Every byte between the framing has to have its top bit clear:
                anything else is a status byte, which would end the dump early
                and leave the rest of it being read as messages. */
            for (std::size_t i = 1; i + 1 < body.size(); ++i)
                if ((body[i] & 0x80) != 0)
                {
                    out.problem = sendError::badMessage;
                    return out;
                }

            out.bytes = std::move (body);
            return out;
        }

        if (spec.channel < 1 || spec.channel > 16)
        {
            out.problem = sendError::badMessage;
            return out;
        }

        /*  PITCH BEND IS FOURTEEN BITS IN TWO SEVEN-BIT HALVES, least
            significant first, and 8192 is the centre. It is the one type whose
            `data` is not a seven-bit number, which is why the range check is
            per type rather than once at the top. */
        if (spec.type == "pitchBend")
        {
            if (spec.data < 0 || spec.data > 16383)
            {
                out.problem = sendError::badMessage;
                return out;
            }

            out.bytes = { status (0xe0, spec.channel),
                          static_cast<std::uint8_t> (spec.data & 0x7f),
                          static_cast<std::uint8_t> ((spec.data >> 7) & 0x7f) };
            return out;
        }

        /*  THE ONE-BYTE-OF-DATA TYPES, where `number` means nothing and saying
            so is better than sending it somewhere. */
        if (spec.type == "programChange" || spec.type == "channelPressure")
        {
            const auto value = spec.type == "programChange" ? spec.number : spec.data;

            if (! inSevenBits (value))
            {
                out.problem = sendError::badMessage;
                return out;
            }

            out.bytes = { status (spec.type == "programChange" ? 0xc0 : 0xd0, spec.channel),
                          static_cast<std::uint8_t> (value) };
            return out;
        }

        if (! inSevenBits (spec.number) || ! inSevenBits (spec.data))
        {
            out.problem = sendError::badMessage;
            return out;
        }

        std::uint8_t high = 0;

        if (spec.type == "noteOn")              high = 0x90;
        else if (spec.type == "noteOff")        high = 0x80;
        else if (spec.type == "controlChange")  high = 0xb0;
        else if (spec.type == "aftertouch")     high = 0xa0;
        else
        {
            out.problem = sendError::badMessage;
            return out;
        }

        out.bytes = { status (high, spec.channel),
                      static_cast<std::uint8_t> (spec.number),
                      static_cast<std::uint8_t> (spec.data) };
        return out;
    }
}
