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

#include <wfg/engine/json/JsonValue.h>

#include <wfg/engine/osc/OscValue.h>

namespace wfg::json
{
    namespace
    {
        const std::string emptyString;
        const Array emptyArray;
        const Object emptyObject;
    }

    //==============================================================================
    Value::Value() = default;
    Value::~Value() = default;

    Value::Value (bool v) : kind (Type::boolean), booleanValue (v) {}
    Value::Value (double v) : kind (Type::number), numberValue (v) {}
    Value::Value (std::string v) : kind (Type::string), stringValue (std::move (v)) {}

    Value::Value (Array v)
        : kind (Type::array), arrayValue (std::make_unique<Array> (std::move (v))) {}

    Value::Value (Object v)
        : kind (Type::object), objectValue (std::make_unique<Object> (std::move (v))) {}

    Value::Value (const Value& other)
        : kind (other.kind),
          booleanValue (other.booleanValue),
          numberValue (other.numberValue),
          stringValue (other.stringValue),
          arrayValue (other.arrayValue != nullptr ? std::make_unique<Array> (*other.arrayValue)
                                                  : nullptr),
          objectValue (other.objectValue != nullptr ? std::make_unique<Object> (*other.objectValue)
                                                    : nullptr)
    {
    }

    Value::Value (Value&&) noexcept = default;
    Value& Value::operator= (Value&&) noexcept = default;

    Value& Value::operator= (const Value& other)
    {
        if (this != &other)
        {
            Value copy { other };
            *this = std::move (copy);
        }

        return *this;
    }

    //==============================================================================
    bool Value::asBool() const noexcept
    {
        return kind == Type::boolean && booleanValue;
    }

    double Value::asNumber() const noexcept
    {
        return kind == Type::number ? numberValue : 0.0;
    }

    int Value::asInt() const noexcept
    {
        return kind == Type::number ? static_cast<int> (numberValue) : 0;
    }

    const std::string& Value::asString() const noexcept
    {
        return kind == Type::string ? stringValue : emptyString;
    }

    const Array& Value::asArray() const noexcept
    {
        return arrayValue != nullptr ? *arrayValue : emptyArray;
    }

    const Object& Value::asObject() const noexcept
    {
        return objectValue != nullptr ? *objectValue : emptyObject;
    }

    const Value* Value::find (std::string_view key) const noexcept
    {
        if (objectValue == nullptr)
            return nullptr;

        const auto it = objectValue->find (std::string (key));
        return it == objectValue->end() ? nullptr : &it->second;
    }

    const Value* Value::at (std::size_t index) const noexcept
    {
        if (arrayValue == nullptr || index >= arrayValue->size())
            return nullptr;

        return &(*arrayValue)[index];
    }

    std::size_t Value::size() const noexcept
    {
        if (arrayValue != nullptr)  return arrayValue->size();
        if (objectValue != nullptr) return objectValue->size();
        return 0;
    }

    //==============================================================================
    namespace
    {
        /*  A recursive-descent reader over the whole text at once.

            Depth is capped. A JSON document is a tree and this walks it with the
            C++ stack, so a file consisting of one hundred thousand open brackets
            would overflow that stack rather than report a problem - and a
            namespace file arrives from outside, which is exactly the kind of
            input that should not be able to do that. Sixty-four levels is far
            deeper than any OSCQuery description and shallow enough to be safe. */
        constexpr int maxDepth = 64;

        class Reader
        {
        public:
            explicit Reader (std::string_view t) : text (t) {}

            ParseResult run()
            {
                skipWhitespace();

                auto root = readValue (0);

                if (! failure.empty())
                    return { std::nullopt, failure, lineOf (errorAt) };

                skipWhitespace();

                if (position != text.size())
                    return { std::nullopt, "unexpected content after the document",
                             lineOf (position) };

                return { std::move (root), {}, 0 };
            }

        private:
            //==================================================================
            std::size_t lineOf (std::size_t at) const
            {
                std::size_t line = 1;

                for (std::size_t i = 0; i < at && i < text.size(); ++i)
                    if (text[i] == '\n')
                        ++line;

                return line;
            }

            void fail (std::string what)
            {
                if (failure.empty())
                {
                    failure = std::move (what);
                    errorAt = position;
                }
            }

            bool atEnd() const noexcept { return position >= text.size(); }
            char peek() const noexcept { return atEnd() ? '\0' : text[position]; }

            void skipWhitespace()
            {
                while (! atEnd())
                {
                    const auto c = text[position];

                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        ++position;
                    else
                        break;
                }
            }

            bool consume (char expected)
            {
                if (peek() != expected)
                    return false;

                ++position;
                return true;
            }

            bool literal (std::string_view word)
            {
                if (text.compare (position, word.size(), word) != 0)
                    return false;

                position += word.size();
                return true;
            }

            //==================================================================
            /*  A UTF-16 code unit as UTF-8, surrogate pairs included. \uD83C
                followed by \uDF9B is one character, and a reader that emitted
                two would produce text no editor agrees with. */
            void appendUtf8 (std::uint32_t codePoint, std::string& out)
            {
                if (codePoint < 0x80)
                {
                    out += static_cast<char> (codePoint);
                }
                else if (codePoint < 0x800)
                {
                    out += static_cast<char> (0xc0 | (codePoint >> 6));
                    out += static_cast<char> (0x80 | (codePoint & 0x3f));
                }
                else if (codePoint < 0x10000)
                {
                    out += static_cast<char> (0xe0 | (codePoint >> 12));
                    out += static_cast<char> (0x80 | ((codePoint >> 6) & 0x3f));
                    out += static_cast<char> (0x80 | (codePoint & 0x3f));
                }
                else
                {
                    out += static_cast<char> (0xf0 | (codePoint >> 18));
                    out += static_cast<char> (0x80 | ((codePoint >> 12) & 0x3f));
                    out += static_cast<char> (0x80 | ((codePoint >> 6) & 0x3f));
                    out += static_cast<char> (0x80 | (codePoint & 0x3f));
                }
            }

            bool readHex4 (std::uint32_t& out)
            {
                if (position + 4 > text.size())
                    return false;

                out = 0;

                for (int i = 0; i < 4; ++i)
                {
                    const auto c = text[position + static_cast<std::size_t> (i)];
                    std::uint32_t digit = 0;

                    if (c >= '0' && c <= '9')      digit = static_cast<std::uint32_t> (c - '0');
                    else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t> (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t> (c - 'A' + 10);
                    else return false;

                    out = (out << 4) | digit;
                }

                position += 4;
                return true;
            }

            std::string readString()
            {
                std::string out;

                if (! consume ('"'))
                {
                    fail ("expected a string");
                    return out;
                }

                while (! atEnd())
                {
                    const auto c = text[position++];

                    if (c == '"')
                        return out;

                    if (c != '\\')
                    {
                        /*  A raw control character is not legal inside a JSON
                            string, and letting one through would mean this
                            reader accepted something its own writer could never
                            produce. */
                        if (static_cast<unsigned char> (c) < 0x20)
                        {
                            fail ("a control character must be escaped inside a string");
                            return out;
                        }

                        out += c;
                        continue;
                    }

                    if (atEnd())
                        break;

                    switch (text[position++])
                    {
                        case '"':  out += '"';  break;
                        case '\\': out += '\\'; break;
                        case '/':  out += '/';  break;
                        case 'b':  out += '\b'; break;
                        case 'f':  out += '\f'; break;
                        case 'n':  out += '\n'; break;
                        case 'r':  out += '\r'; break;
                        case 't':  out += '\t'; break;

                        case 'u':
                        {
                            std::uint32_t unit = 0;

                            if (! readHex4 (unit))
                            {
                                fail ("\\u needs four hexadecimal digits");
                                return out;
                            }

                            if (unit >= 0xd800 && unit <= 0xdbff)
                            {
                                std::uint32_t low = 0;

                                if (position + 2 <= text.size()
                                    && text[position] == '\\' && text[position + 1] == 'u')
                                {
                                    position += 2;

                                    if (readHex4 (low) && low >= 0xdc00 && low <= 0xdfff)
                                    {
                                        appendUtf8 (0x10000 + ((unit - 0xd800) << 10)
                                                      + (low - 0xdc00), out);
                                        break;
                                    }
                                }

                                fail ("a high surrogate must be followed by a low one");
                                return out;
                            }

                            appendUtf8 (unit, out);
                            break;
                        }

                        default:
                            fail ("unknown escape in a string");
                            return out;
                    }
                }

                fail ("a string was never closed");
                return out;
            }

            //==================================================================
            /*  THE WHOLE REASON THIS FILE EXISTS.

                The token is measured out by JSON's own grammar and then handed
                to osc::parseDouble, which is strtod in an explicit C locale -
                correctly rounded, and the same conversion the document, the log
                and the OSC atom grammar use. There is no accumulator to
                overflow and no second idea of what a number is. */
            Value readNumber()
            {
                const auto start = position;

                if (peek() == '-')
                    ++position;

                if (peek() == '0')
                {
                    ++position;
                }
                else if (peek() >= '1' && peek() <= '9')
                {
                    while (peek() >= '0' && peek() <= '9')
                        ++position;
                }
                else
                {
                    fail ("expected a number");
                    return {};
                }

                if (peek() == '.')
                {
                    ++position;

                    if (! (peek() >= '0' && peek() <= '9'))
                    {
                        fail ("a decimal point must be followed by a digit");
                        return {};
                    }

                    while (peek() >= '0' && peek() <= '9')
                        ++position;
                }

                if (peek() == 'e' || peek() == 'E')
                {
                    ++position;

                    if (peek() == '+' || peek() == '-')
                        ++position;

                    if (! (peek() >= '0' && peek() <= '9'))
                    {
                        fail ("an exponent must have digits");
                        return {};
                    }

                    while (peek() >= '0' && peek() <= '9')
                        ++position;
                }

                const auto parsed = osc::parseDouble (text.substr (start, position - start));

                if (! parsed.has_value())
                {
                    fail ("a number that could not be read");
                    return {};
                }

                return Value { *parsed };
            }

            //==================================================================
            Value readArray (int depth)
            {
                Array elements;

                consume ('[');
                skipWhitespace();

                if (consume (']'))
                    return Value { std::move (elements) };

                for (;;)
                {
                    skipWhitespace();
                    elements.push_back (readValue (depth + 1));

                    if (! failure.empty())
                        return {};

                    skipWhitespace();

                    if (consume (','))
                        continue;

                    if (consume (']'))
                        return Value { std::move (elements) };

                    fail ("expected ',' or ']' in an array");
                    return {};
                }
            }

            Value readObject (int depth)
            {
                Object members;

                consume ('{');
                skipWhitespace();

                if (consume ('}'))
                    return Value { std::move (members) };

                for (;;)
                {
                    skipWhitespace();

                    auto key = readString();

                    if (! failure.empty())
                        return {};

                    skipWhitespace();

                    if (! consume (':'))
                    {
                        fail ("expected ':' after a member name");
                        return {};
                    }

                    skipWhitespace();
                    auto member = readValue (depth + 1);

                    if (! failure.empty())
                        return {};

                    /*  A repeated key is refused rather than resolved. JSON does
                        not say which of two wins, so a file with both is a file
                        whose author disagrees with themselves, and picking one
                        would be inventing an answer. */
                    if (! members.emplace (std::move (key), std::move (member)).second)
                    {
                        fail ("the same member name appears twice in one object");
                        return {};
                    }

                    skipWhitespace();

                    if (consume (','))
                        continue;

                    if (consume ('}'))
                        return Value { std::move (members) };

                    fail ("expected ',' or '}' in an object");
                    return {};
                }
            }

            Value readValue (int depth)
            {
                if (depth > maxDepth)
                {
                    fail ("nested too deeply");
                    return {};
                }

                skipWhitespace();

                switch (peek())
                {
                    case '{': return readObject (depth);
                    case '[': return readArray (depth);
                    case '"': return Value { readString() };

                    case 't':
                        if (literal ("true"))  return Value { true };
                        fail ("expected a value");
                        return {};

                    case 'f':
                        if (literal ("false")) return Value { false };
                        fail ("expected a value");
                        return {};

                    case 'n':
                        if (literal ("null"))  return {};
                        fail ("expected a value");
                        return {};

                    case '\0':
                        fail ("the document is empty");
                        return {};

                    default:
                        return readNumber();
                }
            }

            //==================================================================
            std::string_view text;
            std::size_t position = 0;
            std::string failure;
            std::size_t errorAt = 0;
        };
    }

    //==============================================================================
    ParseResult parse (std::string_view text)
    {
        return Reader { text }.run();
    }
}
