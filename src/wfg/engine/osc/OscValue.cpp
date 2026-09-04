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

#include <juce_core/juce_core.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

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
    /*  Why JUCE and not std::to_chars: Apple's libc++ gates std::to_chars
        (double) to macOS 13.3 and ships no from_chars (double) at all, while
        CMakeLists.txt targets macOS 11.0.

        And why juce::var rather than juce::String (double): they are not the
        same formatter, and only one of them is fit for a document.
        String (double) hands the value to a bare std::ostream, whose default
        precision is six SIGNIFICANT digits, so 1234567.0 comes back as
        "1.23457e+06" - lossy, and lossy in a file someone typed a number into.
        var (double).toString() routes through juce_String.cpp's serialiseDouble
        instead (juce_Variant.cpp:215), which is JUCE's "as many decimal places
        as necessary" writer: 0.5 stays "0.5", 1.0 becomes "1.0", and a value
        that needs fifteen digits gets fifteen. It is the same function
        XmlElement::setAttribute (double) uses (juce_XmlElement.cpp:660), so the
        event log and the show document will agree by construction.

        Both go through a stream imbued with std::locale::classic()
        (juce_String.cpp:476), so this is locale-independent whatever setlocale()
        was last told - which tests/LocaleTests.cpp pins under fr_FR on all three
        platforms.

        Not shortest-round-trip to the last ulp: a double needing 17 significant
        digits comes back one ulp off. Every float32 survives exactly (float32
        needs 9 digits, this writes up to 15), which is what the log and the
        parameter tree carry; the document's own precision policy is a separate
        question, open as Q1 of the namespace draft.
    */
    std::string formatDouble (double value)
    {
        return juce::var (value).toString().toStdString();
    }

    std::optional<double> parseDouble (std::string_view text)
    {
        if (text.empty())
            return std::nullopt;

        /*  Validate the shape ourselves before handing over to JUCE:
            String::getDoubleValue() is lenient ("12abc" parses as 12) and a log
            or document parser must not be. Accepted: an optional sign, digits
            with at most one dot, an optional exponent. Nothing else. */
        std::size_t i = 0;

        if (text[i] == '-' || text[i] == '+')
            ++i;

        bool sawDigit = false;
        bool sawDot = false;

        for (; i < text.size(); ++i)
        {
            const char c = text[i];

            if (c >= '0' && c <= '9')          { sawDigit = true; continue; }
            if (c == '.' && ! sawDot)          { sawDot = true; continue; }
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

        const auto parsed = juce::String (std::string (text)).getDoubleValue();

        if (! std::isfinite (parsed))
            return std::nullopt;

        return parsed;
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
            quote, an unknown escape, or anything after the closing quote. */
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
                        return std::nullopt;   // an unescaped quote before the end

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
            case 2:  return "f:" + formatDouble (static_cast<double> (std::get<float> (storage)));
            case 3:  return "d:" + formatDouble (std::get<double> (storage));
            case 4:  return "s:" + escapeString (std::get<std::string> (storage));
            case 5:
            {
                const auto& bytes = std::get<Blob> (storage).bytes;
                return "b:" + juce::Base64::toBase64 (bytes.data(), bytes.size()).toStdString();
            }
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

        if (atom.size() < 3 || atom[1] != ':')
            return std::nullopt;

        const auto payload = atom.substr (2);

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
            {
                juce::MemoryOutputStream decoded;

                if (! juce::Base64::convertFromBase64 (decoded, juce::String (std::string (payload))))
                    return std::nullopt;

                Blob blob;
                const auto* data = static_cast<const std::uint8_t*> (decoded.getData());
                blob.bytes.assign (data, data + decoded.getDataSize());
                return Value::blob (std::move (blob));
            }

            default:
                return std::nullopt;
        }
    }
}
