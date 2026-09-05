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

#include <wfg/engine/osc/OscCodec.h>

#include <wfg/engine/osc/OscAddress.h>

#include <cmath>
#include <cstring>

namespace wfg::osc
{
    //==============================================================================
    Packet Packet::message (std::string toAddress, std::vector<Value> withArgs)
    {
        Packet p;
        p.kind = Kind::message;
        p.address = std::move (toAddress);
        p.args = std::move (withArgs);
        return p;
    }

    Packet Packet::bundle (TimeTag when, std::vector<Packet> withElements)
    {
        Packet p;
        p.kind = Kind::bundle;
        p.time = when;
        p.elements = std::move (withElements);
        return p;
    }

    bool Packet::operator== (const Packet& other) const
    {
        if (kind != other.kind)
            return false;

        if (kind == Kind::message)
            return address == other.address && args == other.args;

        return time == other.time && elements == other.elements;
    }

    DecodeResult DecodeResult::failed (std::string atom, std::string why)
    {
        DecodeResult r;
        r.reason = std::move (atom);
        r.error = std::move (why);
        return r;
    }

    //==============================================================================
    namespace
    {
        constexpr std::string_view bundleMarker = "#bundle";
        constexpr std::size_t bundleHeaderSize = 8;      // "#bundle\0"

        std::size_t paddedSize (std::size_t n) noexcept
        {
            return n + ((4 - (n % 4)) % 4);
        }

        //======================================================================
        /*  A cursor over somebody else's bytes.

            Every read asks whether the bytes are there and says no when they
            are not. Nothing in here returns a default on failure, which is the
            single most important difference from the parser this replaces: a
            zero that came from a short buffer is indistinguishable, downstream,
            from a zero somebody sent. */
        class Reader
        {
        public:
            Reader (const std::uint8_t* d, std::size_t n) : data (d), size (n) {}

            std::size_t remaining() const noexcept { return size - position; }
            bool atEnd() const noexcept { return position >= size; }

            bool take (std::size_t n) noexcept
            {
                if (n > remaining())
                    return false;

                position += n;
                return true;
            }

            const std::uint8_t* at (std::size_t offset) const noexcept { return data + offset; }
            std::size_t tell() const noexcept { return position; }

            bool readUInt32 (std::uint32_t& out) noexcept
            {
                if (remaining() < 4)
                    return false;

                out = (static_cast<std::uint32_t> (data[position]) << 24)
                    | (static_cast<std::uint32_t> (data[position + 1]) << 16)
                    | (static_cast<std::uint32_t> (data[position + 2]) << 8)
                    |  static_cast<std::uint32_t> (data[position + 3]);

                position += 4;
                return true;
            }

            bool readUInt64 (std::uint64_t& out) noexcept
            {
                std::uint32_t hi = 0, lo = 0;

                if (! readUInt32 (hi) || ! readUInt32 (lo))
                    return false;

                out = (static_cast<std::uint64_t> (hi) << 32) | lo;
                return true;
            }

            /*  An OSC string: NUL-terminated, then padded to four.

                The terminator must be INSIDE the buffer. spatcore's reader
                accepts a string that simply runs to the end of the datagram,
                which turns a truncated address into a valid-looking message
                with no arguments. */
            bool readString (std::string& out) noexcept
            {
                for (std::size_t i = position; i < size; ++i)
                {
                    if (data[i] != 0)
                        continue;

                    const auto length = i - position;
                    out.assign (reinterpret_cast<const char*> (data + position), length);

                    const auto consumed = paddedSize (length + 1);

                    /*  The padding has to be there too. A string whose
                        terminator is the last byte of the datagram is missing
                        up to three bytes of pad, and a packet that lost its
                        tail should be refused rather than half-read. */
                    if (consumed > remaining())
                        return false;

                    position += consumed;
                    return true;
                }

                return false;       // no terminator before the end
            }

        private:
            const std::uint8_t* data;
            std::size_t size;
            std::size_t position = 0;
        };

        //======================================================================
        float floatFromBits (std::uint32_t bits) noexcept
        {
            float out = 0.0f;
            std::memcpy (&out, &bits, sizeof (out));
            return out;
        }

        double doubleFromBits (std::uint64_t bits) noexcept
        {
            double out = 0.0;
            std::memcpy (&out, &bits, sizeof (out));
            return out;
        }

        std::uint32_t bitsOfFloat (float value) noexcept
        {
            std::uint32_t out = 0;
            std::memcpy (&out, &value, sizeof (out));
            return out;
        }

        std::uint64_t bitsOfDouble (double value) noexcept
        {
            std::uint64_t out = 0;
            std::memcpy (&out, &value, sizeof (out));
            return out;
        }

        //======================================================================
        /*  What a refusal carries. Threaded through the decoders by reference
            instead of a bare string so that the atom and the sentence can never
            drift apart: there is no site that can set one without the other. */
        struct Refusal
        {
            std::string atom;
            std::string text;
        };

        /*  Always false, so a guard reads `return no (why, ..., "...")` and the
            refusal and the return sit on one line. */
        bool no (Refusal& why, const char* atom, std::string text)
        {
            why.atom = atom;
            why.text = std::move (text);
            return false;
        }

        //======================================================================
        bool readArgument (Reader& reader, char tag, Value& out, Refusal& why)
        {
            switch (tag)
            {
                case 'i':
                {
                    std::uint32_t bits = 0;

                    if (! reader.readUInt32 (bits))
                        return no (why, refusal::truncated,
                                  "an int32 argument runs past the end");

                    out = Value::int32 (static_cast<std::int32_t> (bits));
                    return true;
                }

                case 'h':
                {
                    std::uint64_t bits = 0;

                    if (! reader.readUInt64 (bits))
                        return no (why, refusal::truncated,
                                  "an int64 argument runs past the end");

                    out = Value::int64 (static_cast<std::int64_t> (bits));
                    return true;
                }

                case 'f':
                {
                    std::uint32_t bits = 0;

                    if (! reader.readUInt32 (bits))
                        return no (why, refusal::truncated,
                                  "a float32 argument runs past the end");

                    out = Value::float32 (floatFromBits (bits));
                    return true;
                }

                case 'd':
                {
                    std::uint64_t bits = 0;

                    if (! reader.readUInt64 (bits))
                        return no (why, refusal::truncated,
                                  "a float64 argument runs past the end");

                    out = Value::float64 (doubleFromBits (bits));
                    return true;
                }

                case 's':
                {
                    std::string text;

                    if (! reader.readString (text))
                        return no (why, refusal::truncated,
                                  "a string argument is unterminated");

                    out = Value::string (std::move (text));
                    return true;
                }

                case 'b':
                {
                    std::uint32_t declared = 0;

                    if (! reader.readUInt32 (declared))
                        return no (why, refusal::truncated,
                                  "a blob's length runs past the end");

                    /*  The length is a signed int32 on the wire, so the top bit
                        set means somebody sent a negative length. And the
                        PADDED size has to be present, not just the content -
                        the bytes after a blob are the next argument, and they
                        are only where they should be if the pad was sent. */
                    if (declared > 0x7fffffffu)
                        return no (why, refusal::badBlob,
                                  "a blob declares a negative length");

                    const auto padded = paddedSize (declared);

                    if (padded > reader.remaining())
                        return no (why, refusal::truncated,
                                  "a blob runs past the end");

                    Blob blob;
                    blob.bytes.assign (reader.at (reader.tell()),
                                       reader.at (reader.tell()) + declared);
                    reader.take (padded);

                    out = Value::blob (std::move (blob));
                    return true;
                }

                case 't':
                {
                    std::uint64_t bits = 0;

                    if (! reader.readUInt64 (bits))
                        return no (why, refusal::truncated,
                                  "a time tag runs past the end");

                    out = Value::timeTag (TimeTag { bits });
                    return true;
                }

                case 'T': out = Value::boolean (true);  return true;
                case 'F': out = Value::boolean (false); return true;
                case 'N': out = Value::nil();           return true;
                case 'I': out = Value::impulse();       return true;

                default:
                    /*  Refused, not skipped, and the message goes with it. An
                        unknown tag cannot be stepped over: the payload size is
                        a property of the tag, so a reader that guessed would
                        desync and every argument after it would be fiction. */
                    return no (why, refusal::unknownTypeTag,
                               std::string ("unknown OSC type tag '") + tag + "'");
            }
        }

        //======================================================================
        bool decodeMessage (Reader& reader, Packet& out, Refusal& why)
        {
            if (! reader.readString (out.address))
                return no (why, refusal::badAddress,
                                  "the address is unterminated");

            if (! isValidAddress (out.address))
            {
                return containsWildcard (out.address)
                         ? no (why, refusal::addressPattern,
                               "the address is a pattern, and Go.dot does not dispatch patterns")
                         : no (why, refusal::badAddress,
                               "the address is not a well-formed OSC address");
            }

            std::string tags;

            if (! reader.readString (tags))
                return no (why, refusal::noTypeTags,
                                  "the type-tag string is missing or unterminated");

            /*  OSC 1.1 requires it. Treating an absent one as "no arguments" is
                OSC 1.0 leniency, and it means a corrupted first byte becomes a
                plausible empty message that nothing downstream can question. */
            if (tags.empty() || tags.front() != ',')
                return no (why, refusal::noTypeTags,
                                  "the type-tag string does not begin with ','");

            out.args.reserve (tags.size() - 1);

            for (std::size_t i = 1; i < tags.size(); ++i)
            {
                Value value;

                if (! readArgument (reader, tags[i], value, why))
                    return false;

                out.args.push_back (std::move (value));
            }

            if (! reader.atEnd())
                return no (why, refusal::trailingBytes,
                                  "there are bytes after the last argument");

            return true;
        }

        bool decodePacket (const std::uint8_t* data, std::size_t size, int depth,
                           Packet& out, Refusal& why);

        bool decodeBundle (const std::uint8_t* data, std::size_t size, int depth,
                           Packet& out, Refusal& why)
        {
            if (depth > maxBundleDepth)
                return no (why, refusal::tooDeep,
                                  "bundles are nested too deeply");

            Reader reader { data, size };

            std::string marker;

            if (! reader.readString (marker) || marker != bundleMarker)
                return no (why, refusal::badBundle,
                                  "a bundle does not begin with \"#bundle\"");

            std::uint64_t time = 0;

            if (! reader.readUInt64 (time))
                return no (why, refusal::badBundle,
                                  "a bundle has no time tag");

            out.kind = Packet::Kind::bundle;
            out.time = TimeTag { time };

            while (! reader.atEnd())
            {
                std::uint32_t declared = 0;

                if (! reader.readUInt32 (declared))
                    return no (why, refusal::badBundle,
                                  "a bundle element's size runs past the end");

                if (declared > 0x7fffffffu)
                    return no (why, refusal::badBundle,
                                  "a bundle element declares a negative size");

                if (declared == 0)
                    return no (why, refusal::badBundle,
                                  "a bundle element declares no bytes");

                if (declared % 4 != 0)
                    return no (why, refusal::badBundle,
                                  "a bundle element's size is not a multiple of four");

                /*  Written as a subtraction rather than `tell() + declared >
                    size`, which overflows for a declared size near the top of
                    the range and then passes - the exact defect this codec was
                    written to avoid repeating. */
                if (declared > reader.remaining())
                    return no (why, refusal::badBundle,
                                  "a bundle element runs past the end of the bundle");

                Packet element;

                /*  The element is decoded against ITS OWN extent. Handing it
                    the whole datagram lets a truncated element read its
                    neighbour's bytes as its own arguments, and the framing then
                    hides that anything was wrong. */
                if (! decodePacket (reader.at (reader.tell()), declared, depth + 1, element, why))
                    return false;

                out.elements.push_back (std::move (element));
                reader.take (declared);
            }

            return true;
        }

        bool decodePacket (const std::uint8_t* data, std::size_t size, int depth,
                           Packet& out, Refusal& why)
        {
            if (size == 0)
                return no (why, refusal::notOsc,
                                  "the packet is empty");

            if (size % 4 != 0)
                return no (why, refusal::notOsc,
                                  "the packet is not a multiple of four bytes");

            /*  A bundle begins with the eight bytes "#bundle\0". Compared in
                full, including the terminator: spatcore compares seven and
                accepts "#bundleX" as a bundle. */
            if (size >= bundleHeaderSize
                && std::memcmp (data, bundleMarker.data(), bundleMarker.size()) == 0
                && data[bundleMarker.size()] == 0)
                return decodeBundle (data, size, depth, out, why);

            Reader reader { data, size };
            out.kind = Packet::Kind::message;
            return decodeMessage (reader, out, why);
        }

        //======================================================================
        void appendUInt32 (std::vector<std::uint8_t>& out, std::uint32_t value)
        {
            out.push_back (static_cast<std::uint8_t> ((value >> 24) & 0xff));
            out.push_back (static_cast<std::uint8_t> ((value >> 16) & 0xff));
            out.push_back (static_cast<std::uint8_t> ((value >> 8) & 0xff));
            out.push_back (static_cast<std::uint8_t> (value & 0xff));
        }

        void appendUInt64 (std::vector<std::uint8_t>& out, std::uint64_t value)
        {
            appendUInt32 (out, static_cast<std::uint32_t> (value >> 32));
            appendUInt32 (out, static_cast<std::uint32_t> (value & 0xffffffffu));
        }

        bool appendString (std::vector<std::uint8_t>& out, const std::string& text,
                           std::string& error)
        {
            /*  A NUL inside the text would terminate the string early, so what
                came back would not be what went in. Refused rather than
                truncated: spatcore's writer measures with strlen and ships the
                short version without telling anybody. */
            if (text.find ('\0') != std::string::npos)
                return (error = "a string argument contains a NUL and cannot be encoded", false);

            out.insert (out.end(), text.begin(), text.end());
            out.push_back (0);

            while (out.size() % 4 != 0)
                out.push_back (0);

            return true;
        }

        bool appendArgument (std::vector<std::uint8_t>& out, const Value& value,
                             std::string& error)
        {
            if (value.isInt32())   { appendUInt32 (out, static_cast<std::uint32_t> (value.getInt32())); return true; }
            if (value.isInt64())   { appendUInt64 (out, static_cast<std::uint64_t> (value.getInt64())); return true; }
            if (value.isBool())    return true;      // T and F carry no payload
            if (value.isNil())     return true;
            if (value.isImpulse()) return true;

            if (value.isFloat32())
            {
                if (! std::isfinite (value.getFloat32()))
                    return (error = "a non-finite float cannot be encoded", false);

                appendUInt32 (out, bitsOfFloat (value.getFloat32()));
                return true;
            }

            if (value.isFloat64())
            {
                if (! std::isfinite (value.getFloat64()))
                    return (error = "a non-finite double cannot be encoded", false);

                appendUInt64 (out, bitsOfDouble (value.getFloat64()));
                return true;
            }

            if (value.isString())
                return appendString (out, value.getString(), error);

            if (value.isTimeTag())
            {
                appendUInt64 (out, value.getTimeTag().raw);
                return true;
            }

            if (value.isBlob())
            {
                const auto& bytes = value.getBlob().bytes;

                if (bytes.size() > 0x7fffffffu)
                    return (error = "a blob is too large for an OSC length", false);

                appendUInt32 (out, static_cast<std::uint32_t> (bytes.size()));
                out.insert (out.end(), bytes.begin(), bytes.end());

                while (out.size() % 4 != 0)
                    out.push_back (0);

                return true;
            }

            error = "a value of a type OSC has no tag for";
            return false;
        }

        char tagOf (const Value& value) noexcept
        {
            return static_cast<char> (value.type());
        }

        bool encodeInto (const Packet& packet, std::vector<std::uint8_t>& out, std::string& error);

        bool encodeMessage (const Packet& packet, std::vector<std::uint8_t>& out,
                            std::string& error)
        {
            if (! isValidAddress (packet.address))
                return (error = "\"" + packet.address + "\" is not a well-formed OSC address",
                        false);

            if (! appendString (out, packet.address, error))
                return false;

            std::string tags = ",";

            for (const auto& value : packet.args)
                tags += tagOf (value);

            if (! appendString (out, tags, error))
                return false;

            for (const auto& value : packet.args)
                if (! appendArgument (out, value, error))
                    return false;

            return true;
        }

        bool encodeBundle (const Packet& packet, std::vector<std::uint8_t>& out,
                           std::string& error)
        {
            if (! appendString (out, std::string (bundleMarker), error))
                return false;

            appendUInt64 (out, packet.time.raw);

            for (const auto& element : packet.elements)
            {
                std::vector<std::uint8_t> encoded;

                if (! encodeInto (element, encoded, error))
                    return false;

                appendUInt32 (out, static_cast<std::uint32_t> (encoded.size()));
                out.insert (out.end(), encoded.begin(), encoded.end());
            }

            return true;
        }

        bool encodeInto (const Packet& packet, std::vector<std::uint8_t>& out, std::string& error)
        {
            return packet.isBundle() ? encodeBundle (packet, out, error)
                                     : encodeMessage (packet, out, error);
        }
    }

    //==============================================================================
    DecodeResult decode (const std::uint8_t* data, std::size_t size)
    {
        if (data == nullptr)
            return DecodeResult::failed (refusal::notOsc, "there is nothing to decode");

        DecodeResult result;
        Refusal why;

        if (! decodePacket (data, size, 0, result.packet, why))
            return DecodeResult::failed (std::move (why.atom), std::move (why.text));

        result.ok = true;
        return result;
    }

    DecodeResult decode (std::string_view bytes)
    {
        return decode (reinterpret_cast<const std::uint8_t*> (bytes.data()), bytes.size());
    }

    std::optional<std::vector<std::uint8_t>> encode (const Packet& packet, std::string& error)
    {
        std::vector<std::uint8_t> out;
        out.reserve (64);

        if (! encodeInto (packet, out, error))
            return std::nullopt;

        return out;
    }
}
