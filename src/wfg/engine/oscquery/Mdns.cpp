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

#include <wfg/engine/oscquery/Mdns.h>

#include <algorithm>

namespace wfg::mdns
{
    namespace
    {
        constexpr std::uint16_t classIn = 1;
        constexpr std::uint16_t topBit = 0x8000;
        constexpr std::size_t headerSize = 12;

        //  A name of more than 255 bytes is not one (RFC 1035 §3.1); pointers followed at most this often.
        constexpr std::size_t longestName = 255;
        constexpr int mostHops = 64;

        std::uint16_t read16 (const std::uint8_t* at) noexcept
        {
            return static_cast<std::uint16_t> ((static_cast<unsigned> (at[0]) << 8) | static_cast<unsigned> (at[1]));
        }

        std::uint32_t read32 (const std::uint8_t* at) noexcept
        {
            return (static_cast<std::uint32_t> (at[0]) << 24) | (static_cast<std::uint32_t> (at[1]) << 16)
                 | (static_cast<std::uint32_t> (at[2]) << 8) | static_cast<std::uint32_t> (at[3]);
        }

        void put16 (std::vector<std::uint8_t>& out, std::uint16_t value)
        {
            out.push_back (static_cast<std::uint8_t> (value >> 8));
            out.push_back (static_cast<std::uint8_t> (value & 0xFFu));
        }

        void put32 (std::vector<std::uint8_t>& out, std::uint32_t value)
        {
            put16 (out, static_cast<std::uint16_t> (value >> 16));
            put16 (out, static_cast<std::uint16_t> (value & 0xFFFFu));
        }

        /*  A NAME, WRITTEN WHOLE: each label at most 63 bytes - what does not
            fit is cut, which only a name nobody could type would need - then
            the empty label that ends it. */
        void putName (std::vector<std::uint8_t>& out, const Name& name)
        {
            for (const auto& label : name)
            {
                if (label.empty())
                    continue;

                const auto length = std::min<std::size_t> (label.size(), 63);
                out.push_back (static_cast<std::uint8_t> (length));
                out.insert (out.end(), label.begin(), label.begin() + static_cast<std::ptrdiff_t> (length));
            }

            out.push_back (0);
        }

        /*  A NAME, READ: its labels, following compression pointers (RFC 1035
            §4.1.4). `at` moves past the name as it sits in place - two bytes
            for a pointer - whatever the pointers led to. A pointer forward, a
            reserved label kind, a label past the end, more hops than any name
            needs or more bytes than a name may have: not a name. */
        bool readName (const std::uint8_t* data, std::size_t size, std::size_t& at, Name& out)
        {
            auto position = at;
            auto jumped = false;
            auto hops = 0;
            std::size_t total = 0;

            for (;;)
            {
                if (position >= size)
                    return false;

                const auto length = static_cast<std::size_t> (data[position]);

                if (length == 0)
                {
                    if (! jumped)
                        at = position + 1;

                    return true;
                }

                if ((length & 0xC0u) == 0xC0u)
                {
                    if (position + 1 >= size)
                        return false;

                    const auto target = ((length & 0x3Fu) << 8) | static_cast<std::size_t> (data[position + 1]);

                    if (target >= position || ++hops > mostHops)
                        return false;

                    if (! jumped)
                        at = position + 2;

                    jumped = true;
                    position = target;
                    continue;
                }

                if ((length & 0xC0u) != 0)
                    return false;

                if (size - position - 1 < length)
                    return false;

                total += length + 1;

                if (total > longestName)
                    return false;

                out.emplace_back (reinterpret_cast<const char*> (data + position + 1), length);
                position += length + 1;
            }
        }

        bool readRecord (const std::uint8_t* data, std::size_t size, std::size_t& at, Record& out)
        {
            if (! readName (data, size, at, out.name) || size - at < 10)
                return false;

            out.type = read16 (data + at);
            const auto klass = read16 (data + at + 2);
            out.cacheFlush = (klass & topBit) != 0;
            out.ttl = read32 (data + at + 4);
            const auto length = static_cast<std::size_t> (read16 (data + at + 8));
            at += 10;

            if (size - at < length)
                return false;

            const auto end = at + length;

            if (out.type == type::ptr)
            {
                auto inside = at;

                if (! readName (data, size, inside, out.target) || inside > end)
                    return false;
            }
            else if (out.type == type::srv)
            {
                if (length < 7)
                    return false;

                out.port = read16 (data + at + 4);
                auto inside = at + 6;

                if (! readName (data, size, inside, out.target) || inside > end)
                    return false;
            }
            else if (out.type == type::txt)
            {
                for (auto inside = at; inside < end;)
                {
                    const auto count = static_cast<std::size_t> (data[inside]);
                    ++inside;

                    if (end - inside < count)
                        return false;

                    if (count > 0)
                        out.txt.emplace_back (reinterpret_cast<const char*> (data + inside), count);

                    inside += count;
                }
            }
            else if (out.type == type::a)
            {
                if (length != 4)
                    return false;

                std::copy (data + at, data + at + 4, out.address.begin());
            }

            at = end;
            return true;
        }

        void putRecord (std::vector<std::uint8_t>& out, const Record& record)
        {
            putName (out, record.name);
            put16 (out, record.type);
            put16 (out, static_cast<std::uint16_t> (classIn | (record.cacheFlush ? topBit : 0u)));
            put32 (out, record.ttl);

            std::vector<std::uint8_t> data;

            if (record.type == type::ptr)
            {
                putName (data, record.target);
            }
            else if (record.type == type::srv)
            {
                put16 (data, 0);    // priority
                put16 (data, 0);    // weight
                put16 (data, record.port);
                putName (data, record.target);
            }
            else if (record.type == type::txt)
            {
                for (const auto& text : record.txt)
                {
                    const auto length = std::min<std::size_t> (text.size(), 255);
                    data.push_back (static_cast<std::uint8_t> (length));
                    data.insert (data.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t> (length));
                }

                //  An empty TXT is one empty string, never no bytes (RFC 6763 §6.1).
                if (data.empty())
                    data.push_back (0);
            }
            else if (record.type == type::a)
            {
                data.assign (record.address.begin(), record.address.end());
            }

            put16 (out, static_cast<std::uint16_t> (data.size()));
            out.insert (out.end(), data.begin(), data.end());
        }

        void putHeader (std::vector<std::uint8_t>& out, std::uint16_t id, std::uint16_t flags, std::size_t questions,
                        std::size_t answers, std::size_t additionals)
        {
            put16 (out, id);
            put16 (out, flags);
            put16 (out, static_cast<std::uint16_t> (questions));
            put16 (out, static_cast<std::uint16_t> (answers));
            put16 (out, 0);
            put16 (out, static_cast<std::uint16_t> (additionals));
        }

        char lower (char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char> (c - 'A' + 'a') : c;
        }
    }

    Name nameOf (std::string_view dottedName)
    {
        Name name;
        std::size_t start = 0;

        while (start <= dottedName.size())
        {
            const auto dot = dottedName.find ('.', start);
            const auto end = dot == std::string_view::npos ? dottedName.size() : dot;

            if (end > start)
                name.emplace_back (dottedName.substr (start, end - start));

            if (dot == std::string_view::npos)
                break;

            start = dot + 1;
        }

        return name;
    }

    std::string dotted (const Name& name)
    {
        std::string out;

        for (const auto& label : name)
            out += (out.empty() ? "" : ".") + label;

        return out;
    }

    bool sameName (const Name& a, const Name& b)
    {
        if (a.size() != b.size())
            return false;

        for (std::size_t at = 0; at < a.size(); ++at)
        {
            if (a[at].size() != b[at].size())
                return false;

            for (std::size_t c = 0; c < a[at].size(); ++c)
                if (lower (a[at][c]) != lower (b[at][c]))
                    return false;
        }

        return true;
    }

    std::string Record::addressText() const
    {
        return std::to_string (address[0]) + "." + std::to_string (address[1]) + "."
             + std::to_string (address[2]) + "." + std::to_string (address[3]);
    }

    std::vector<Record> Message::records() const
    {
        std::vector<Record> all (answers);
        all.insert (all.end(), authorities.begin(), authorities.end());
        all.insert (all.end(), additionals.begin(), additionals.end());
        return all;
    }

    std::vector<std::uint8_t> encodeQuery (const std::vector<Question>& questions)
    {
        std::vector<std::uint8_t> out;
        putHeader (out, 0, 0, questions.size(), 0, 0);

        for (const auto& question : questions)
        {
            putName (out, question.name);
            put16 (out, question.type);
            put16 (out, static_cast<std::uint16_t> (classIn | (question.unicastResponse ? topBit : 0u)));
        }

        return out;
    }

    std::vector<std::uint8_t> encodeResponse (const std::vector<Record>& answers, const std::vector<Record>& additionals,
                                              std::uint16_t id, const std::vector<Question>& questions)
    {
        std::vector<std::uint8_t> out;
        putHeader (out, id, 0x8400, questions.size(), answers.size(), additionals.size());

        for (const auto& question : questions)
        {
            putName (out, question.name);
            put16 (out, question.type);
            put16 (out, classIn);
        }

        for (const auto& record : answers)
            putRecord (out, record);

        for (const auto& record : additionals)
            putRecord (out, record);

        return out;
    }

    std::optional<Message> decode (const std::uint8_t* data, std::size_t size)
    {
        if (data == nullptr || size < headerSize)
            return std::nullopt;

        Message message;
        message.id = read16 (data);
        message.response = (read16 (data + 2) & topBit) != 0;

        const auto questions = read16 (data + 4);
        const auto answers = read16 (data + 6);
        const auto authorities = read16 (data + 8);
        const auto additionals = read16 (data + 10);

        std::size_t at = headerSize;

        for (std::uint16_t count = 0; count < questions; ++count)
        {
            Question question;

            if (! readName (data, size, at, question.name) || size - at < 4)
                return std::nullopt;

            question.type = read16 (data + at);
            question.unicastResponse = (read16 (data + at + 2) & topBit) != 0;
            at += 4;
            message.questions.push_back (std::move (question));
        }

        const auto section = [data, size, &at] (std::uint16_t count, std::vector<Record>& into)
        {
            for (std::uint16_t read = 0; read < count; ++read)
            {
                Record record;

                if (! readRecord (data, size, at, record))
                    return false;

                into.push_back (std::move (record));
            }

            return true;
        };

        if (! section (answers, message.answers) || ! section (authorities, message.authorities)
            || ! section (additionals, message.additionals))
            return std::nullopt;

        return message;
    }
}
