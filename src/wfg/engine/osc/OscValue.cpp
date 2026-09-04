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

#include <wfg/engine/osc/OscValue.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <locale>
#include <sstream>

namespace wfg::osc
{
    //==============================================================================
    Value::Type Value::type() const noexcept
    {
        switch (storage.index())
        {
            case 0:  return Type::int32;
            case 1:  return Type::int64;
            case 2:  return Type::float32;
            case 3:  return Type::float64;
            case 4:  return Type::string;
            case 5:  return Type::blob;
            case 6:  return std::get<bool> (storage) ? Type::boolTrue : Type::boolFalse;
            case 7:  return Type::nil;
            case 8:  return Type::impulse;
            default: return Type::timeTag;
        }
    }

    bool Value::isNumber() const noexcept
    {
        return isInt32() || isInt64() || isFloat32() || isFloat64();
    }

    std::int32_t Value::getInt32() const noexcept   { auto* p = std::get_if<std::int32_t> (&storage); return p != nullptr ? *p : 0; }
    std::int64_t Value::getInt64() const noexcept   { auto* p = std::get_if<std::int64_t> (&storage); return p != nullptr ? *p : 0; }
    float Value::getFloat32() const noexcept        { auto* p = std::get_if<float> (&storage);        return p != nullptr ? *p : 0.0f; }
    double Value::getFloat64() const noexcept       { auto* p = std::get_if<double> (&storage);       return p != nullptr ? *p : 0.0; }
    bool Value::getBool() const noexcept            { auto* p = std::get_if<bool> (&storage);         return p != nullptr && *p; }
    TimeTag Value::getTimeTag() const noexcept      { auto* p = std::get_if<TimeTag> (&storage);      return p != nullptr ? *p : TimeTag {}; }

    const std::string& Value::getString() const noexcept
    {
        static const std::string empty;
        auto* p = std::get_if<std::string> (&storage);
        return p != nullptr ? *p : empty;
    }

    const Blob& Value::getBlob() const noexcept
    {
        static const Blob empty;
        auto* p = std::get_if<Blob> (&storage);
        return p != nullptr ? *p : empty;
    }

    double Value::asDouble() const noexcept
    {
        switch (storage.index())
        {
            case 0:  return static_cast<double> (std::get<std::int32_t> (storage));
            case 1:  return static_cast<double> (std::get<std::int64_t> (storage));
            case 2:  return static_cast<double> (std::get<float> (storage));
            case 3:  return std::get<double> (storage);
            default: return 0.0;
        }
    }

    bool Value::isNonFinite() const noexcept
    {
        if (auto* f = std::get_if<float> (&storage))
            return ! std::isfinite (*f);

        if (auto* d = std::get_if<double> (&storage))
            return ! std::isfinite (*d);

        return false;
    }

    //==============================================================================
    namespace
    {
        template <typename Float>
        bool sameBits (Float a, Float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (Float)) == 0;
        }
    }

    bool Value::operator== (const Value& other) const noexcept
    {
        if (storage.index() != other.storage.index())
            return false;

        switch (storage.index())
        {
            case 2:  return sameBits (std::get<float> (storage),  std::get<float> (other.storage));
            case 3:  return sameBits (std::get<double> (storage), std::get<double> (other.storage));
            default: return storage == other.storage;
        }
    }

    //==============================================================================
    std::string typeTagString (const std::vector<Value>& values)
    {
        std::string tags;
        tags.reserve (values.size());

        for (const auto& v : values)
            tags.push_back (v.typeTag());

        return tags;
    }

    //==============================================================================
    /*  SHORTEST ROUND-TRIP, and it had to be measured rather than assumed.

        Over 19 993 random finite doubles, four writer/reader pairs were tried:

            JUCE write, JUCE read ............... 9 214 failures  (46%)
            to_chars write, JUCE read ................ 51 failures
            to_chars write, classic-stream read ....... 0 failures
            to_chars write, from_chars read ........... 0 failures

        JUCE's writer stops at fifteen significant digits, so nearly half of all
        doubles come back as a different number - which would make "the event log
        replays the session bit-for-bit" false on its own terms. JUCE's READER is
        very nearly right and not quite: it is not correctly rounded, and loses
        about one value in four hundred.

        So: std::to_chars to write, and a classic-locale stream to read.

        Why not from_chars to read, when it also scores zero: libc++ only gained
        it for floating point in LLVM 20, so it is missing on the macOS toolchains
        this project builds on, while to_chars has been available since
        macOS 13.3 - which is why CMakeLists.txt targets 13.3. A stream imbued
        with std::locale::classic() has no such gate anywhere, and num_get is
        specified to convert as if by strtod, which every implementation we build
        on rounds correctly.

        Locale independence is by construction on both sides: to_chars is defined
        never to consult the locale at all, and the stream is imbued explicitly,
        which overrides whatever setlocale() was last told. LocaleTests.cpp and
        OscValueTests.cpp pin both under fr_FR on all three platforms.
    */
    namespace
    {
        template <typename Float>
        std::string shortestRoundTrip (Float value)
        {
            /*  32 characters covers every double to_chars can produce:
                17 significant digits, a sign, a dot and a short exponent. The
                fallback is there anyway, because a buffer that is "obviously big
                enough" is how a number becomes a truncated number. */
            char buffer[32];
            auto result = std::to_chars (buffer, buffer + sizeof (buffer), value);

            if (result.ec == std::errc {})
                return std::string (buffer, result.ptr);

            std::string large (128, ' ');
            result = std::to_chars (large.data(), large.data() + large.size(), value);

            return result.ec == std::errc {} ? std::string (large.data(), result.ptr)
                                             : std::string {};
        }
    }

    std::string formatDouble (double value)
    {
        return shortestRoundTrip (value);
    }

    std::string formatFloat (float value)
    {
        return shortestRoundTrip (value);
    }

    std::optional<double> parseDouble (std::string_view text)
    {
        if (text.empty())
            return std::nullopt;

        /*  Validate the shape before converting. A stream extraction stops at
            the first character it cannot use and reports success for what it
            read, so "12abc" would parse as 12 - and a log or a document that
            quietly reads a typo as a number is how a wrong cue gets fired.
            Accepted: an optional sign, digits with at most one dot, an optional
            exponent. Nothing else, and no leading or trailing space. */
        std::size_t i = 0;

        if (text[i] == '-' || text[i] == '+')
            ++i;

        bool sawDigit = false;
        bool sawDot = false;

        for (; i < text.size(); ++i)
        {
            const char c = text[i];

            if (c >= '0' && c <= '9')   { sawDigit = true; continue; }
            if (c == '.' && ! sawDot)   { sawDot = true; continue; }

            if ((c == 'e' || c == 'E') && sawDigit)
            {
                ++i;

                if (i < text.size() && (text[i] == '-' || text[i] == '+'))
                    ++i;

                if (i >= text.size())
                    return std::nullopt;

                for (; i < text.size(); ++i)
                    if (text[i] < '0' || text[i] > '9')
                        return std::nullopt;

                break;
            }

            return std::nullopt;
        }

        if (! sawDigit)
            return std::nullopt;

        /*  One stream per thread rather than one per call: imbuing a locale is
            the expensive part, and a log parser does this once per numeric atom.
            The stream is reset, not rebuilt. */
        static thread_local std::istringstream stream = []
        {
            std::istringstream s;
            s.imbue (std::locale::classic());
            return s;
        }();

        stream.clear();
        stream.str (std::string (text));

        double value = 0.0;
        stream >> value;

        if (stream.fail() || ! std::isfinite (value))
            return std::nullopt;

        return value;
    }

    //==============================================================================
    namespace
    {
        constexpr char base64Alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        /*  Ours rather than juce::Base64, so that this whole translation unit
            names no vendor type. That is worth forty lines: the value type, the
            number format and the atom grammar are the primitives the document,
            the log and the OSCQuery surface all sit on, and they should not move
            because a vendor changed its mind.

            Standard base64 with padding, which is what the OSCQuery proposal and
            any log reader will expect. */
        std::string toBase64 (const std::vector<std::uint8_t>& bytes)
        {
            std::string out;
            out.reserve (((bytes.size() + 2) / 3) * 4);

            std::size_t i = 0;

            for (; i + 2 < bytes.size(); i += 3)
            {
                const std::uint32_t triple = (static_cast<std::uint32_t> (bytes[i]) << 16)
                                           | (static_cast<std::uint32_t> (bytes[i + 1]) << 8)
                                           |  static_cast<std::uint32_t> (bytes[i + 2]);

                out.push_back (base64Alphabet[(triple >> 18) & 0x3f]);
                out.push_back (base64Alphabet[(triple >> 12) & 0x3f]);
                out.push_back (base64Alphabet[(triple >> 6)  & 0x3f]);
                out.push_back (base64Alphabet[ triple        & 0x3f]);
            }

            if (i < bytes.size())
            {
                const bool haveTwo = (i + 1 < bytes.size());

                std::uint32_t triple = static_cast<std::uint32_t> (bytes[i]) << 16;

                if (haveTwo)
                    triple |= static_cast<std::uint32_t> (bytes[i + 1]) << 8;

                out.push_back (base64Alphabet[(triple >> 18) & 0x3f]);
                out.push_back (base64Alphabet[(triple >> 12) & 0x3f]);
                out.push_back (haveTwo ? base64Alphabet[(triple >> 6) & 0x3f] : '=');
                out.push_back ('=');
            }

            return out;
        }

        int base64Index (char c) noexcept
        {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+')             return 62;
            if (c == '/')             return 63;
            return -1;
        }

        /*  Strict on purpose: a wrong length, a stray character or padding in the
            wrong place is a refusal, not a best effort. The only thing a blob in
            a log carries is a packet that was already rejected once, and guessing
            at it a second time helps nobody. */
        std::optional<std::vector<std::uint8_t>> fromBase64 (std::string_view text)
        {
            if (text.size() % 4 != 0)
                return std::nullopt;

            std::vector<std::uint8_t> out;
            out.reserve ((text.size() / 4) * 3);

            for (std::size_t i = 0; i < text.size(); i += 4)
            {
                const bool padThird  = (text[i + 2] == '=');
                const bool padFourth = (text[i + 3] == '=');

                if (padThird && ! padFourth)
                    return std::nullopt;

                if ((padThird || padFourth) && i + 4 != text.size())
                    return std::nullopt;      // padding may appear only at the end

                int digits[4];

                for (int k = 0; k < 4; ++k)
                {
                    const char c = text[i + static_cast<std::size_t> (k)];

                    if (c == '=')
                    {
                        digits[k] = 0;
                        continue;
                    }

                    digits[k] = base64Index (c);

                    if (digits[k] < 0)
                        return std::nullopt;
                }

                const std::uint32_t triple = (static_cast<std::uint32_t> (digits[0]) << 18)
                                           | (static_cast<std::uint32_t> (digits[1]) << 12)
                                           | (static_cast<std::uint32_t> (digits[2]) << 6)
                                           |  static_cast<std::uint32_t> (digits[3]);

                out.push_back (static_cast<std::uint8_t> ((triple >> 16) & 0xff));

                if (! padThird)
                    out.push_back (static_cast<std::uint8_t> ((triple >> 8) & 0xff));

                if (! padFourth)
                    out.push_back (static_cast<std::uint8_t> (triple & 0xff));
            }

            return out;
        }
    }

    //==============================================================================
    namespace
    {
        std::string escapeString (const std::string& s)
        {
            std::string out;
            out.reserve (s.size() + 2);
            out.push_back ('"');

            for (const unsigned char c : s)
            {
                switch (c)
                {
                    case '\\': out += "\\\\"; break;
                    case '"':  out += "\\\""; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20)
                        {
                            static constexpr char hex[] = "0123456789abcdef";
                            out += "\\u00";
                            out.push_back (hex[(c >> 4) & 0xf]);
                            out.push_back (hex[c & 0xf]);
                        }
                        else
                        {
                            out.push_back (static_cast<char> (c));
                        }
                        break;
                }
            }

            out.push_back ('"');
            return out;
        }

        /*  Parses the quoted form produced above. Returns nullopt on a missing
            quote, an unknown escape, or an unescaped quote before the end. */
        std::optional<std::string> unescapeString (std::string_view quoted)
        {
            if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"')
                return std::nullopt;

            std::string out;
            out.reserve (quoted.size());

            for (std::size_t i = 1; i + 1 < quoted.size(); ++i)
            {
                const char c = quoted[i];

                if (c != '\\')
                {
                    if (c == '"')
                        return std::nullopt;

                    out.push_back (c);
                    continue;
                }

                if (i + 2 >= quoted.size())
                    return std::nullopt;

                const char e = quoted[++i];

                switch (e)
                {
                    case '\\': out.push_back ('\\'); break;
                    case '"':  out.push_back ('"');  break;
                    case 'n':  out.push_back ('\n'); break;
                    case 'r':  out.push_back ('\r'); break;
                    case 't':  out.push_back ('\t'); break;
                    case 'u':
                    {
                        if (i + 5 >= quoted.size())
                            return std::nullopt;

                        unsigned code = 0;

                        for (int k = 1; k <= 4; ++k)
                        {
                            const char h = quoted[i + static_cast<std::size_t> (k)];
                            code <<= 4;

                            if (h >= '0' && h <= '9')       code |= static_cast<unsigned> (h - '0');
                            else if (h >= 'a' && h <= 'f')  code |= static_cast<unsigned> (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')  code |= static_cast<unsigned> (h - 'A' + 10);
                            else return std::nullopt;
                        }

                        if (code > 0x7f)
                            return std::nullopt;   // only control characters are escaped this way

                        out.push_back (static_cast<char> (code));
                        i += 4;
                        break;
                    }
                    default:
                        return std::nullopt;
                }
            }

            return out;
        }

        template <typename Int>
        std::optional<Int> parseInteger (std::string_view text)
        {
            Int value {};
            const auto* first = text.data();
            const auto* last = text.data() + text.size();
            const auto result = std::from_chars (first, last, value);

            if (result.ec != std::errc {} || result.ptr != last)
                return std::nullopt;

            return value;
        }
    }

    std::string Value::toAtom() const
    {
        switch (storage.index())
        {
            case 0:  return "i:" + std::to_string (std::get<std::int32_t> (storage));
            case 1:  return "h:" + std::to_string (std::get<std::int64_t> (storage));
            case 2:  return "f:" + formatFloat (std::get<float> (storage));
            case 3:  return "d:" + formatDouble (std::get<double> (storage));
            case 4:  return "s:" + escapeString (std::get<std::string> (storage));
            case 5:  return "b:" + toBase64 (std::get<Blob> (storage).bytes);
            case 6:  return std::get<bool> (storage) ? "T" : "F";
            case 7:  return "N";
            case 8:  return "I";
            default: return "t:" + std::to_string (std::get<TimeTag> (storage).raw);
        }
    }

    std::optional<Value> Value::fromAtom (std::string_view atom)
    {
        if (atom == "T") return Value::boolean (true);
        if (atom == "F") return Value::boolean (false);
        if (atom == "N") return Value::nil();
        if (atom == "I") return Value::impulse();

        /*  Every other atom is a tag, a colon and a payload. "b:" with an empty
            payload is the one legitimate zero-length case - an empty blob - so
            the emptiness check below excludes it. */
        if (atom.size() < 2 || atom[1] != ':')
            return std::nullopt;

        const auto payload = atom.substr (2);

        if (payload.empty() && atom[0] != 'b')
            return std::nullopt;

        switch (atom[0])
        {
            case 'i':
                if (auto v = parseInteger<std::int32_t> (payload))
                    return Value::int32 (*v);
                return std::nullopt;

            case 'h':
                if (auto v = parseInteger<std::int64_t> (payload))
                    return Value::int64 (*v);
                return std::nullopt;

            case 't':
                if (auto v = parseInteger<std::uint64_t> (payload))
                    return Value::timeTag (TimeTag { *v });
                return std::nullopt;

            case 'f':
                /*  Read at double precision and narrow. That is exact: the
                    shortest text identifying a float, read as a double and
                    rounded back to float, is always the float it came from. */
                if (auto v = parseDouble (payload))
                {
                    const auto narrowed = static_cast<float> (*v);

                    if (! std::isfinite (narrowed))
                        return std::nullopt;

                    return Value::float32 (narrowed);
                }
                return std::nullopt;

            case 'd':
                if (auto v = parseDouble (payload))
                    return Value::float64 (*v);
                return std::nullopt;

            case 's':
                if (auto s = unescapeString (payload))
                    return Value::string (std::move (*s));
                return std::nullopt;

            case 'b':
                if (auto bytes = fromBase64 (payload))
                {
                    Blob blob;
                    blob.bytes = std::move (*bytes);
                    return Value::blob (std::move (blob));
                }
                return std::nullopt;

            default:
                return std::nullopt;
        }
    }
}
