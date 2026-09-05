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
    OSC 1.1 on the wire: bytes in, values out, and bytes back again.

    OURS RATHER THAN JUCE'S OR spatcore's, and for once the reasons are about
    correctness rather than about numbers. juce::OSCReceiver throws on the type
    tags Go.dot needs. spatcore's parser was read closely for this PR and is
    not a foundation to build on - the notes below name what it does, because
    each one is a rule this file follows in the opposite direction.

    A DECODER READS SOMEBODY ELSE'S BYTES. That is the whole design constraint:
    a datagram arrives from a network, from a device nobody here wrote, possibly
    from something malicious, and a decoder that guesses is a decoder that hands
    the engine a message the sender never sent. So:

      * EVERY READ IS BOUNDS-CHECKED AND FAILS. spatcore's readInt32 returns 0
        when the buffer runs out, so a packet whose type-tag string promises
        three floats and carries none is delivered as three zeroes. On a control
        surface that is three real parameter changes nobody made. Here a read
        past the end refuses the packet.

      * AN ELEMENT IS PARSED AGAINST ITS OWN EXTENT, not the whole datagram.
        Otherwise a bundle element whose arguments run past its declared size
        quietly eats the next element's bytes, and the framing hides it.

      * THE SIZE BOUND CANNOT OVERFLOW. `pos + declaredSize > total` is signed
        overflow waiting for a declared size of INT_MAX; the check here is
        `declaredSize > total - pos`, which cannot wrap.

      * NESTING IS CAPPED. A bundle may contain bundles. Twenty bytes per level
        against a 64 KB datagram is several thousand stack frames, which is a
        remote crash for the price of one packet.

      * THE TYPE-TAG STRING IS REQUIRED. OSC 1.0 let it be omitted and OSC 1.1
        does not; treating an absent one as "no arguments", as spatcore does,
        means a corrupted first byte turns a real message into a plausible empty
        one, and nothing downstream can tell.

      * A PACKET IS A MULTIPLE OF FOUR BYTES. Everything OSC contains is padded
        to four, so a datagram that is not is a datagram that lost its tail.

    WHAT IT CARRIES is the full OSC 1.1 set - `i h f d s b T F N I t` - because
    Go.dot's own control plane uses `h` for the tick index and `d` for seconds,
    and Phase 4 needs time-tagged bundles for the state solver. spatcore parses
    six of those and serialises four, and discards a bundle's time tag on the
    way through.

    ENCODING REFUSES WHAT IT CANNOT SAY. A string carrying an embedded NUL is
    not encodable - the NUL terminates it, so writing one would silently
    truncate, which is spatcore's writeString. A non-finite float is refused for
    the same reason the log refuses one.
*/

#include <wfg/engine/osc/OscValue.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::osc
{
    /*  One OSC packet: a message, or a bundle of packets.

        Flat rather than a class hierarchy, because a packet is data on a wire
        and the two shapes share a decoder. `elements` is empty for a message
        and `args` is empty for a bundle. */
    struct Packet
    {
        enum class Kind { message, bundle };

        Kind kind = Kind::message;

        //  message
        std::string address;
        std::vector<Value> args;

        //  bundle
        TimeTag time;
        std::vector<Packet> elements;

        bool isBundle() const noexcept { return kind == Kind::bundle; }

        /*  Parameters deliberately NOT named after the members they fill:
            `address`, `args`, `time` and `elements` are all data members, and a
            parameter that shadows one is a -Wshadow error under the strict
            preset. GCC counts a static member function as being inside the
            class for this; MSVC says nothing, so it would only ever appear on
            the Linux job. */
        static Packet message (std::string toAddress, std::vector<Value> withArgs = {});
        static Packet bundle (TimeTag when, std::vector<Packet> withElements = {});

        bool operator== (const Packet& other) const;
        bool operator!= (const Packet& other) const { return ! (*this == other); }
    };

    //==============================================================================
    /*  The refusal atoms, which are the vocabulary of a dropped packet.

        A refusal is recorded in two registers because two different readers
        need it. The ATOM goes in the log's `X` record: one plain kebab-case
        token, in a fixed column, so a post-mortem can grep and count it and so
        a log written last season still means what it meant. The SENTENCE is for
        the operator, and it may be reworded whenever a clearer wording is
        found - which is exactly why it must not be the thing the log keys on.

        The set is small and closed, and each atom names one of the guards the
        header above describes. Three of them shade into each other, so the line
        between them is drawn once, here, rather than per site:

          * `truncated` - a MESSAGE promised something the packet does not
            contain. The packet's own length is the contract that was broken.
          * `bad-blob` - a blob's declared length cannot be valid at any length,
            because it is negative. Nothing was truncated; the sender lied.
          * `bad-bundle` - a BUNDLE's framing is wrong: the marker, the time tag,
            or an element size that does not fit the bundle declaring it. The
            bundle's own size fields are the contract that was broken, which is
            why this is not `truncated` even when the symptom is a short read. */
    namespace refusal
    {
        inline constexpr const char* notOsc         = "not-osc";
        inline constexpr const char* badAddress     = "bad-address";
        inline constexpr const char* addressPattern = "address-is-pattern";
        inline constexpr const char* noTypeTags     = "no-type-tags";
        inline constexpr const char* unknownTypeTag = "unknown-type-tag";
        inline constexpr const char* truncated      = "truncated";
        inline constexpr const char* badBlob        = "bad-blob";
        inline constexpr const char* badBundle      = "bad-bundle";
        inline constexpr const char* tooDeep        = "too-deep";
        inline constexpr const char* trailingBytes  = "trailing-bytes";
    }

    struct DecodeResult
    {
        bool ok = false;

        /*  Why it was refused, when it was: `reason` for the log, `error` for
            the human. A dropped packet with neither is a dropped packet nobody
            can act on. Both are empty on success. */
        std::string reason;
        std::string error;

        Packet packet;

        static DecodeResult failed (std::string atom, std::string why);
    };

    /*  Decodes one datagram. Never throws, never reads outside the buffer, and
        never returns a packet it had to guess at. */
    DecodeResult decode (const std::uint8_t* data, std::size_t size);
    DecodeResult decode (std::string_view bytes);

    /*  Encodes one packet. nullopt with a reason when the packet holds
        something the wire cannot carry. */
    std::optional<std::vector<std::uint8_t>> encode (const Packet& packet, std::string& error);

    /** How deep a bundle may nest before the decoder refuses it. */
    inline constexpr int maxBundleDepth = 32;
}
