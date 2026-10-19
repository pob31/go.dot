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

#include <wfg/engine/surface/StreamDeckReport.h>

#include <algorithm>

namespace wfg::surface
{
    namespace
    {
        DeckSpec model (DeckModel which, unsigned short product, std::string_view word, std::string_view name,
                        int columns, int rows, DeckImage image, int pixels, int turns, bool mirrorX, bool mirrorY)
        {
            DeckSpec spec;
            spec.model = which;
            spec.product = product;
            spec.word = word;
            spec.name = name;
            spec.columns = columns;
            spec.rows = rows;
            spec.keys = columns * rows;
            spec.image = image;
            spec.keyPixels = pixels;
            spec.quarterTurns = turns;
            spec.mirrorX = mirrorX;
            spec.mirrorY = mirrorY;
            return spec;
        }

        std::vector<DeckSpec> makeModels()
        {
            std::vector<DeckSpec> all;

            //  The first decks: BMP pictures, the Original's upside down, the
            //  Mini's on their side.
            auto original = model (DeckModel::original, 0x0060, "original", "Stream Deck (Original)",
                                   5, 3, DeckImage::bmp, 72, 0, true, true);
            original.legacy = true;
            all.push_back (original);

            auto mini = model (DeckModel::mini, 0x0063, "mini", "Stream Deck Mini",
                               3, 2, DeckImage::bmp, 80, 1, true, false);
            mini.legacy = true;
            all.push_back (mini);

            auto miniMk2 = model (DeckModel::miniMk2, 0x0090, "miniMk2", "Stream Deck Mini MK.2",
                                  3, 2, DeckImage::bmp, 80, 1, true, false);
            miniMk2.legacy = true;
            all.push_back (miniMk2);

            //  Every deck since: JPEG pictures, upside down but on the Stream Deck+.
            all.push_back (model (DeckModel::originalV2, 0x006d, "originalV2", "Stream Deck (Original V2)",
                                  5, 3, DeckImage::jpeg, 72, 0, true, true));
            all.push_back (model (DeckModel::mk2, 0x0080, "mk2", "Stream Deck MK.2",
                                  5, 3, DeckImage::jpeg, 72, 0, true, true));
            all.push_back (model (DeckModel::xl, 0x006c, "xl", "Stream Deck XL",
                                  8, 4, DeckImage::jpeg, 96, 0, true, true));
            all.push_back (model (DeckModel::xlV2, 0x008f, "xlV2", "Stream Deck XL V2",
                                  8, 4, DeckImage::jpeg, 96, 0, true, true));

            auto neo = model (DeckModel::neo, 0x009a, "neo", "Stream Deck Neo",
                              4, 2, DeckImage::jpeg, 96, 0, true, true);
            neo.touchKeys = 2;
            neo.stripWidth = 248;
            neo.stripHeight = 58;
            all.push_back (neo);

            all.push_back (model (DeckModel::pedal, 0x0086, "pedal", "Stream Deck Pedal",
                                  3, 1, DeckImage::none, 0, 0, false, false));

            auto plus = model (DeckModel::plus, 0x0084, "plus", "Stream Deck+",
                               4, 2, DeckImage::jpeg, 120, 0, false, false);
            plus.dials = 4;
            plus.stripWidth = 800;
            plus.stripHeight = 100;
            all.push_back (plus);

            return all;
        }

        /*  The Original v1 numbers a row's keys from the right. */
        int handToOriginal (const DeckSpec& spec, int key) noexcept
        {
            const auto column = key % spec.columns;
            return key - column + (spec.columns - 1 - column);
        }

        int deviceKeyFor (const DeckSpec& spec, int key) noexcept
        {
            return spec.model == DeckModel::original ? handToOriginal (spec, key) : key;
        }

        std::uint8_t low (std::size_t value) noexcept  { return static_cast<std::uint8_t> (value & 0xff); }
        std::uint8_t high (std::size_t value) noexcept { return static_cast<std::uint8_t> ((value >> 8) & 0xff); }

        int littleEndian (const std::uint8_t* at) noexcept
        {
            return static_cast<int> (static_cast<unsigned> (at[0]) | (static_cast<unsigned> (at[1]) << 8));
        }

        /*  Cuts `encoded` into reports of `reportLength`, each a header from
            `header` (page, bytes in it, last) then that page's bytes, padded
            with noughts. */
        template <typename Header>
        std::vector<DeckBytes> cut (const DeckBytes& encoded, std::size_t reportLength, std::size_t payload,
                                    Header&& header)
        {
            std::vector<DeckBytes> reports;

            if (payload == 0)
                return reports;

            std::size_t sent = 0;
            std::size_t page = 0;

            while (sent < encoded.size())
            {
                const auto length = std::min (payload, encoded.size() - sent);
                const auto last = sent + length == encoded.size();

                auto report = header (page, length, last);
                report.insert (report.end(), encoded.begin() + static_cast<std::ptrdiff_t> (sent),
                               encoded.begin() + static_cast<std::ptrdiff_t> (sent + length));
                report.resize (reportLength, 0);
                reports.push_back (std::move (report));

                sent += length;
                ++page;
            }

            return reports;
        }

        DeckBytes padded (DeckBytes bytes, std::size_t length)
        {
            bytes.resize (std::max (bytes.size(), length), 0);
            return bytes;
        }
    }

    //==========================================================================
    const std::vector<DeckSpec>& deckModels()
    {
        static const std::vector<DeckSpec> all = makeModels();
        return all;
    }

    std::optional<DeckSpec> deckSpecFor (unsigned short product)
    {
        for (const auto& spec : deckModels())
            if (spec.product == product)
                return spec;

        return std::nullopt;
    }

    std::optional<DeckSpec> deckSpecFor (std::string_view word)
    {
        for (const auto& spec : deckModels())
            if (spec.word == word)
                return spec;

        return std::nullopt;
    }

    //==========================================================================
    std::size_t deckInputLength (const DeckSpec& spec) noexcept
    {
        if (spec.model == DeckModel::plus)
            return 14;

        if (spec.legacy)
            return 1 + static_cast<std::size_t> (spec.keys);

        return 4 + static_cast<std::size_t> (spec.keys + spec.touchKeys);
    }

    DeckInput readDeckReport (const DeckSpec& spec, const std::uint8_t* data, std::size_t length) noexcept
    {
        DeckInput input;

        if (data == nullptr || length < 1 || data[0] == 0)
            return input;

        if (spec.model == DeckModel::plus)
        {
            if (length < 2)
                return input;

            switch (data[1])
            {
                case 0x00:      // the keys
                    if (length < 4 + static_cast<std::size_t> (spec.keys))
                        return input;

                    input.kind = DeckInput::Kind::keys;

                    for (int key = 0; key < spec.keys; ++key)
                        input.keys[static_cast<std::size_t> (key)] = data[4 + key] != 0;

                    return input;

                case 0x02:      // the touch strip
                {
                    if (length < 10)
                        return input;

                    input.x = littleEndian (data + 6);
                    input.y = littleEndian (data + 8);

                    switch (data[4])
                    {
                        case 0x01: input.kind = DeckInput::Kind::tap; break;
                        case 0x02: input.kind = DeckInput::Kind::longTap; break;

                        case 0x03:
                            if (length < 14)
                                return DeckInput {};

                            input.kind = DeckInput::Kind::swipe;
                            input.toX = littleEndian (data + 10);
                            input.toY = littleEndian (data + 12);
                            break;

                        default:
                            return DeckInput {};
                    }

                    return input;
                }

                case 0x03:      // the dials
                {
                    if (length < 5 + static_cast<std::size_t> (spec.dials))
                        return input;

                    if (data[4] == 0x00)
                    {
                        input.kind = DeckInput::Kind::dialPresses;

                        for (int dial = 0; dial < spec.dials; ++dial)
                            input.dialsDown[static_cast<std::size_t> (dial)] = data[5 + dial] != 0;
                    }
                    else if (data[4] == 0x01)
                    {
                        input.kind = DeckInput::Kind::dialTurns;

                        for (int dial = 0; dial < spec.dials; ++dial)
                            input.detents[static_cast<std::size_t> (dial)] = static_cast<std::int8_t> (data[5 + dial]);
                    }

                    return input;
                }

                default:
                    return input;
            }
        }

        const auto first = spec.legacy ? std::size_t { 1 } : std::size_t { 4 };
        const auto count = spec.keys + spec.touchKeys;

        if (length < first + static_cast<std::size_t> (count))
            return input;

        input.kind = DeckInput::Kind::keys;

        for (int key = 0; key < count; ++key)
        {
            const auto from = key < spec.keys ? deviceKeyFor (spec, key) : key;
            input.keys[static_cast<std::size_t> (key)] = data[first + static_cast<std::size_t> (from)] != 0;
        }

        return input;
    }

    //==========================================================================
    std::vector<DeckBytes> deckKeyPackets (const DeckSpec& spec, int key, const DeckBytes& encoded)
    {
        if (spec.image == DeckImage::none || key < 0 || key >= spec.keys || encoded.empty())
            return {};

        const auto device = static_cast<std::uint8_t> (deviceKeyFor (spec, key));

        if (spec.model == DeckModel::original)
        {
            //  Two reports of 8191, the picture halved between them, pages from one.
            return cut (encoded, 8191, (encoded.size() + 1) / 2, [device] (std::size_t page, std::size_t, bool last)
            {
                return DeckBytes { 0x02, 0x01, static_cast<std::uint8_t> (page + 1), 0x00,
                                   static_cast<std::uint8_t> (last ? 1 : 0), static_cast<std::uint8_t> (device + 1),
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            });
        }

        if (spec.legacy)
        {
            return cut (encoded, 1024, 1024 - 16, [device] (std::size_t page, std::size_t, bool last)
            {
                return DeckBytes { 0x02, 0x01, static_cast<std::uint8_t> (page), 0x00,
                                   static_cast<std::uint8_t> (last ? 1 : 0), static_cast<std::uint8_t> (device + 1),
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            });
        }

        return cut (encoded, 1024, 1024 - 8, [device] (std::size_t page, std::size_t length, bool last)
        {
            return DeckBytes { 0x02, 0x07, device, static_cast<std::uint8_t> (last ? 1 : 0),
                               low (length), high (length), low (page), high (page) };
        });
    }

    std::vector<DeckBytes> deckStripPackets (const DeckSpec& spec, int x, int y, int width, int height,
                                             const DeckBytes& encoded)
    {
        if (spec.model != DeckModel::plus || encoded.empty() || width <= 0 || height <= 0)
            return {};

        const auto ux = static_cast<std::size_t> (std::max (0, x));
        const auto uy = static_cast<std::size_t> (std::max (0, y));
        const auto uw = static_cast<std::size_t> (width);
        const auto uh = static_cast<std::size_t> (height);

        return cut (encoded, 1024, 1024 - 16, [=] (std::size_t page, std::size_t length, bool last)
        {
            return DeckBytes { 0x02, 0x0c, low (ux), high (ux), low (uy), high (uy), low (uw), high (uw),
                               low (uh), high (uh), static_cast<std::uint8_t> (last ? 1 : 0),
                               low (page), high (page), low (length), high (length), 0x00 };
        });
    }

    std::vector<DeckBytes> deckScreenPackets (const DeckSpec& spec, const DeckBytes& encoded)
    {
        if (spec.model != DeckModel::neo || encoded.empty())
            return {};

        return cut (encoded, 1024, 1024 - 8, [] (std::size_t page, std::size_t length, bool last)
        {
            return DeckBytes { 0x02, 0x0b, 0x00, static_cast<std::uint8_t> (last ? 1 : 0),
                               low (length), high (length), low (page), high (page) };
        });
    }

    DeckBytes deckBrightnessReport (const DeckSpec& spec, int percent)
    {
        const auto level = static_cast<std::uint8_t> (std::clamp (percent, 0, 100));

        if (spec.legacy)
            return padded ({ 0x05, 0x55, 0xaa, 0xd1, 0x01, level }, 17);

        return padded ({ 0x03, 0x08, level }, 32);
    }

    DeckBytes deckResetReport (const DeckSpec& spec)
    {
        if (spec.legacy)
            return padded ({ 0x0b, 0x63 }, 17);

        return padded ({ 0x03, 0x02 }, 32);
    }

    DeckBytes deckTouchKeyColourReport (const DeckSpec& spec, int touchKey, int red, int green, int blue)
    {
        if (touchKey < 0 || touchKey >= spec.touchKeys)
            return {};

        const auto byte = [] (int value) { return static_cast<std::uint8_t> (std::clamp (value, 0, 255)); };

        return padded ({ 0x03, 0x06, static_cast<std::uint8_t> (spec.keys + touchKey),
                         byte (red), byte (green), byte (blue) }, 32);
    }

    DeckAsk deckFirmwareAsk (const DeckSpec& spec) noexcept
    {
        if (spec.legacy)
            return { 0x04, 17, 5 };

        return { 0x05, 32, 6 };
    }

    std::string_view deckAnswerText (const DeckAsk& ask, const std::uint8_t* answer, std::size_t length) noexcept
    {
        if (answer == nullptr || length <= ask.textFrom)
            return {};

        const auto* const text = reinterpret_cast<const char*> (answer + ask.textFrom);
        auto size = length - ask.textFrom;

        //  To the first nought: what follows it is padding.
        for (std::size_t at = 0; at < size; ++at)
        {
            if (text[at] == '\0')
            {
                size = at;
                break;
            }
        }

        return { text, size };
    }

    //==========================================================================
    DeckPixels deckTransformed (const DeckPixels& picture, int quarterTurns, bool mirrorX, bool mirrorY)
    {
        const auto turns = ((quarterTurns % 4) + 4) % 4;
        const auto w = picture.width;
        const auto h = picture.height;

        if (w <= 0 || h <= 0 || picture.rgb.size() < static_cast<std::size_t> (w * h * 3))
            return {};

        DeckPixels out;
        out.width = (turns % 2 == 0) ? w : h;
        out.height = (turns % 2 == 0) ? h : w;
        out.rgb.assign (static_cast<std::size_t> (w * h * 3), 0);

        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                //  Where the source pixel lands, turned clockwise ...
                int tx = x;
                int ty = y;

                switch (turns)
                {
                    case 1: tx = h - 1 - y; ty = x; break;
                    case 2: tx = w - 1 - x; ty = h - 1 - y; break;
                    case 3: tx = y; ty = w - 1 - x; break;
                    default: break;
                }

                //  ... then mirrored.
                if (mirrorX)
                    tx = out.width - 1 - tx;

                if (mirrorY)
                    ty = out.height - 1 - ty;

                const auto from = static_cast<std::size_t> ((y * w + x) * 3);
                const auto to = static_cast<std::size_t> ((ty * out.width + tx) * 3);

                out.rgb[to] = picture.rgb[from];
                out.rgb[to + 1] = picture.rgb[from + 1];
                out.rgb[to + 2] = picture.rgb[from + 2];
            }
        }

        return out;
    }

    DeckPixels deckOriented (const DeckSpec& spec, const DeckPixels& picture)
    {
        return deckTransformed (picture, spec.quarterTurns, spec.mirrorX, spec.mirrorY);
    }

    DeckBytes deckBmp (const DeckPixels& picture)
    {
        const auto w = picture.width;
        const auto h = picture.height;

        if (w <= 0 || h <= 0 || picture.rgb.size() < static_cast<std::size_t> (w * h * 3))
            return {};

        const auto rowBytes = static_cast<std::size_t> ((w * 3 + 3) & ~3);
        const auto pixelBytes = rowBytes * static_cast<std::size_t> (h);
        const auto fileBytes = 54 + pixelBytes;

        const auto u32 = [] (DeckBytes& into, std::size_t value)
        {
            into.push_back (static_cast<std::uint8_t> (value & 0xff));
            into.push_back (static_cast<std::uint8_t> ((value >> 8) & 0xff));
            into.push_back (static_cast<std::uint8_t> ((value >> 16) & 0xff));
            into.push_back (static_cast<std::uint8_t> ((value >> 24) & 0xff));
        };

        DeckBytes out;
        out.reserve (fileBytes);

        //  The file header, then the forty-byte information header: 24 bits,
        //  no compression, 3780 pixels a metre (96 a inch) either way.
        out.push_back ('B');
        out.push_back ('M');
        u32 (out, fileBytes);
        u32 (out, 0);
        u32 (out, 54);
        u32 (out, 40);
        u32 (out, static_cast<std::size_t> (w));
        u32 (out, static_cast<std::size_t> (h));
        out.push_back (1);
        out.push_back (0);
        out.push_back (24);
        out.push_back (0);
        u32 (out, 0);
        u32 (out, pixelBytes);
        u32 (out, 3780);
        u32 (out, 3780);
        u32 (out, 0);
        u32 (out, 0);

        for (int y = h - 1; y >= 0; --y)
        {
            const auto rowStart = out.size();

            for (int x = 0; x < w; ++x)
            {
                const auto at = static_cast<std::size_t> ((y * w + x) * 3);
                out.push_back (picture.rgb[at + 2]);
                out.push_back (picture.rgb[at + 1]);
                out.push_back (picture.rgb[at]);
            }

            out.resize (rowStart + rowBytes, 0);
        }

        return out;
    }
}
