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

#include <wfg/engine/surface/McuCodec.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wfg::surface
{
    namespace
    {
        //======================================================================
        //  The protocol's numbers, each named once.

        constexpr int stripsPerBank = 8;

        /*  The strip number `faderPosition` takes for the master fader, and the
            one a decoded master fader or master touch carries. MCU's master is
            the ninth pitch-bend channel. */
        constexpr int masterStrip = 8;

        constexpr int highestFader = 16383;     // fourteen bits
        constexpr int highestData = 127;        // seven

        //  Status bytes, the channel nibble stripped.
        constexpr int noteOffStatus = 0x80;
        constexpr int noteOnStatus = 0x90;
        constexpr int polyPressureStatus = 0xa0;
        constexpr int controllerStatus = 0xb0;
        constexpr int channelPressureStatus = 0xd0;
        constexpr int pitchBendStatus = 0xe0;

        constexpr int touchNote = 0x68;         // + strip, 0x68..0x6F
        constexpr int masterTouchNote = 0x70;
        constexpr int vpotNote = 0x20;          // + strip: a V-Pot's press, and the D700's colour
        constexpr int masterDialNote = 0x38;    // the D700's master dial: its press and its colour

        constexpr int encoderController = 0x10; // + strip
        constexpr int ringController = 0x30;    // + strip
        constexpr int jogController = 0x3c;

        constexpr int highestRingPosition = 11;
        constexpr int highestRingMode = 3;
        constexpr int highestD700RingMode = 2;

        /*  0x0E is the top of what `meter` sends, because 0x0F is not a level:
            it is the peak-hold reset. */
        constexpr int highestMeterLevel = 0x0e;
        constexpr int clearPeakLevel = 0x0f;

        /*  THE D700'S COLOUR: note-ons on channels 2, 3 and 4 at the element's
            own note, and blue - the last - is what makes the element refresh. */
        constexpr std::uint8_t redStatus = 0x91;
        constexpr std::uint8_t greenStatus = 0x92;
        constexpr std::uint8_t blueStatus = 0x93;

        //  SysEx.
        constexpr std::uint8_t sysexStart = 0xf0;
        constexpr std::uint8_t sysexEnd = 0xf7;

        /*  F0, Mackie's manufacturer id 00 00 66, the device id and the
            command: the six bytes every SysEx of the protocol opens with. */
        constexpr std::size_t sysexHeaderSize = 6;

        constexpr std::uint8_t deviceQueryCommand = 0x00;
        constexpr std::uint8_t connectionQueryCommand = 0x01;
        constexpr std::uint8_t connectionReplyCommand = 0x02;
        constexpr std::uint8_t connectionConfirmationCommand = 0x03;
        constexpr std::uint8_t connectionErrorCommand = 0x04;
        constexpr std::uint8_t lcdCommand = 0x12;
        constexpr std::uint8_t trackNumbersCommand = 0x17;
        constexpr std::uint8_t thirdRowCommand = 0x19;
        constexpr std::uint8_t rowsCommand = 0x1a;

        /*  THE SIX COMMANDS ESTABLISHED AS SAFE, and the whole of what
            `isSafeSysEx` judges by. McuCodec.h says why 0x72 is not among them
            although it is known to be safe. */
        constexpr std::array<std::uint8_t, 6> safeCommands { { deviceQueryCommand,
                                                               connectionReplyCommand,
                                                               lcdCommand,
                                                               trackNumbersCommand,
                                                               thirdRowCommand,
                                                               rowsCommand } };

        constexpr std::size_t serialSize = 7;
        constexpr std::size_t challengeSize = 4;

        constexpr int lcdSize = 112;            // two rows of 56
        constexpr int lcdRowSize = 0x38;
        constexpr int lcdCellStride = static_cast<int> (mcuCellWidth);

        //======================================================================
        /*  MACKIE'S BUTTON MAP, in note order: each run of notes and the button
            it is. A run of eight is one of the five per-strip rows, or F1..F8,
            or the eight view buttons, and the position in the run is the
            ButtonId's `index`; a run of one is a button of its own. */
        struct ButtonRun
        {
            Button button;
            int firstNote;
            int count;
        };

        constexpr ButtonRun buttonRuns[] = {
            { Button::rec,              0x00, 8 },
            { Button::solo,             0x08, 8 },
            { Button::mute,             0x10, 8 },
            { Button::select,           0x18, 8 },
            { Button::vpotPress,        0x20, 8 },
            { Button::assignTrack,      0x28, 1 },
            { Button::assignSend,       0x29, 1 },
            { Button::assignPan,        0x2a, 1 },
            { Button::assignPlugin,     0x2b, 1 },
            { Button::assignEq,         0x2c, 1 },
            { Button::assignInstrument, 0x2d, 1 },
            { Button::bankLeft,         0x2e, 1 },
            { Button::bankRight,        0x2f, 1 },
            { Button::channelLeft,      0x30, 1 },
            { Button::channelRight,     0x31, 1 },
            { Button::flip,             0x32, 1 },
            { Button::globalView,       0x33, 1 },
            { Button::nameValue,        0x34, 1 },
            { Button::smpteBeats,       0x35, 1 },
            { Button::function,         0x36, 8 },
            { Button::view,             0x3e, 8 },
            { Button::shift,            0x46, 1 },
            { Button::option,           0x47, 1 },
            { Button::control,          0x48, 1 },
            { Button::alt,              0x49, 1 },
            { Button::readOff,          0x4a, 1 },
            { Button::write,            0x4b, 1 },
            { Button::trim,             0x4c, 1 },
            { Button::touch,            0x4d, 1 },
            { Button::latch,            0x4e, 1 },
            { Button::group,            0x4f, 1 },
            { Button::save,             0x50, 1 },
            { Button::undo,             0x51, 1 },
            { Button::cancel,           0x52, 1 },
            { Button::enter,            0x53, 1 },
            { Button::marker,           0x54, 1 },
            { Button::nudge,            0x55, 1 },
            { Button::cycle,            0x56, 1 },
            { Button::drop,             0x57, 1 },
            { Button::replace,          0x58, 1 },
            { Button::click,            0x59, 1 },
            { Button::globalSolo,       0x5a, 1 },
            { Button::rewind,           0x5b, 1 },
            { Button::forward,          0x5c, 1 },
            { Button::stop,             0x5d, 1 },
            { Button::play,             0x5e, 1 },
            { Button::record,           0x5f, 1 },
            { Button::up,               0x60, 1 },
            { Button::down,             0x61, 1 },
            { Button::left,             0x62, 1 },
            { Button::right,            0x63, 1 },
            { Button::zoom,             0x64, 1 },
            { Button::scrub,            0x65, 1 },
            { Button::userA,            0x66, 1 },
            { Button::userB,            0x67, 1 },
        };

        /*  THE RUNS COVER 0x00..0x67 EXACTLY ONCE, IN ORDER, stopping where the
            touch notes begin. That is what makes the map its own inverse, and a
            table edited out of order fails to compile rather than naming two
            buttons with one note. */
        constexpr bool runsCoverTheMap() noexcept
        {
            auto next = 0;

            for (const auto& run : buttonRuns)
            {
                if (run.firstNote != next)
                    return false;

                next += run.count;
            }

            return next == touchNote;
        }

        static_assert (runsCoverTheMap(), "the button notes are 0x00..0x67, each exactly once");

        /*  The five buttons a strip has, whose `index` is therefore a strip. */
        bool isStripButton (Button button) noexcept
        {
            return button == Button::rec || button == Button::solo || button == Button::mute
                || button == Button::select || button == Button::vpotPress;
        }

        //======================================================================
        bool isData (int byte) noexcept     { return byte >= 0 && byte <= highestData; }
        bool isStrip (int strip) noexcept   { return strip >= 0 && strip < stripsPerBank; }

        std::uint8_t sevenBits (int value) noexcept
        {
            return static_cast<std::uint8_t> (std::clamp (value, 0, highestData));
        }

        /*  SIGN-MAGNITUDE, NOT TWO'S COMPLEMENT. Bit 6 is the direction and
            bits 0-5 the size, so 1 is a step clockwise and 65 is a step back.
            Read as two's complement, 65 is -63: one click of a V-Pot would
            throw a level sixty-three steps the wrong way. It is the single
            most likely bug in a new MCU integration, because two's complement
            is what everybody assumes (control guide §3.2, and the first of its
            gotchas). 0x40 is "minus nought", which is nought. */
        int signMagnitude (int value) noexcept
        {
            const auto steps = value & 0x3f;
            return (value & 0x40) != 0 ? -steps : steps;
        }

        std::uint8_t velocityOf (Led state) noexcept
        {
            switch (state)
            {
                case Led::off:      return 0x00;
                case Led::flash:    return 0x01;
                case Led::on:       return 0x7f;
            }

            return 0x00;
        }

        /*  The seventeen RGB elements of a D700 are nine notes on each bank's
            port: its eight V-Pots, and the master dial. */
        bool carriesColour (int note) noexcept
        {
            return (note >= vpotNote && note < vpotNote + stripsPerBank) || note == masterDialNote;
        }

        //======================================================================
        /*  A byte a display can show. The text reaching here has already been
            through asciiFold, which writes nothing else; this is the second
            net, because a byte with its top bit set would end the SysEx early
            and the rest of the text would be read as MIDI messages. */
        std::uint8_t displayByte (char c) noexcept
        {
            const auto byte = static_cast<std::uint8_t> (c);
            return (byte >= 0x20 && byte <= 0x7e) ? byte : static_cast<std::uint8_t> ('?');
        }

        void appendText (Bytes& out, std::string_view ascii)
        {
            for (const auto c : ascii)
                out.push_back (displayByte (c));
        }

        Bytes sysexOpening (std::uint8_t deviceId, std::uint8_t command)
        {
            return { sysexStart, 0x00, 0x00, 0x66, deviceId, command };
        }

        /*  Where a strip's field starts on a D700 native row, in characters:
            twelve or eight to a strip. The strip has been checked. */
        std::uint8_t fieldStart (int strip, std::size_t width) noexcept
        {
            return static_cast<std::uint8_t> (static_cast<std::size_t> (strip) * width);
        }

        /*  Whether a message is framed as Mackie's - F0 00 00 66, a device, a
            command, data bytes only, F7 - which the decoder and the safety
            check both ask before anything else. */
        bool isMackieSysEx (const Bytes& message) noexcept
        {
            if (message.size() < sysexHeaderSize + 1
                || message.front() != sysexStart || message.back() != sysexEnd
                || message[1] != 0x00 || message[2] != 0x00 || message[3] != 0x66)
                return false;

            for (std::size_t i = 1; i + 1 < message.size(); ++i)
                if (! isData (message[i]))
                    return false;

            return true;
        }

        /*  THE HANDSHAKE, the only SysEx read here: a surface opens it with its
            serial and a challenge, and later says whether the host's reply was
            accepted (protocol §2.1). Nothing requires it - every control works
            without a connection - but it yields the serial. */
        std::optional<McuEvent> decodeSysEx (const Bytes& message)
        {
            if (! isMackieSysEx (message))
                return std::nullopt;

            const auto command = message[5];
            const auto serialEnd = sysexHeaderSize + serialSize;

            McuEvent event;
            event.deviceId = message[4];

            if (command == connectionQueryCommand)
            {
                if (message.size() != serialEnd + challengeSize + 1)
                    return std::nullopt;

                event.kind = McuEvent::Kind::hostConnectionQuery;

                for (std::size_t i = 0; i < challengeSize; ++i)
                    event.challenge[i] = message[serialEnd + i];
            }
            else if (command == connectionConfirmationCommand || command == connectionErrorCommand)
            {
                if (message.size() != serialEnd + 1)
                    return std::nullopt;

                event.kind = command == connectionConfirmationCommand
                                 ? McuEvent::Kind::hostConnectionConfirmation
                                 : McuEvent::Kind::hostConnectionError;
            }
            else
            {
                return std::nullopt;
            }

            for (std::size_t i = sysexHeaderSize; i < serialEnd; ++i)
                event.serial += static_cast<char> (message[i]);

            return event;
        }

        //======================================================================
        /*  One character read from UTF-8: its code point and how many bytes it
            took, or a length of nought where the bytes there are not UTF-8. */
        struct Character
        {
            std::uint32_t codePoint = 0;
            std::size_t length = 0;
        };

        /*  STRICT, because a name arrives from a document, a network or a file
            somebody else wrote. A lead byte must be followed by exactly the
            continuation bytes it promises; the shortest encoding is the only
            legal one, so C0, C1 and an overlong E0 or F0 sequence are refused;
            and a surrogate, or anything past U+10FFFF, is not a character. */
        Character readCharacter (std::string_view text, std::size_t at) noexcept
        {
            const auto lead = static_cast<std::uint8_t> (text[at]);

            if (lead < 0x80)
                return { lead, 1 };

            std::size_t length = 0;
            std::uint32_t codePoint = 0;
            std::uint32_t shortest = 0;

            if (lead >= 0xc2 && lead <= 0xdf)
            {
                length = 2;
                codePoint = lead & 0x1fu;
                shortest = 0x80;
            }
            else if (lead >= 0xe0 && lead <= 0xef)
            {
                length = 3;
                codePoint = lead & 0x0fu;
                shortest = 0x800;
            }
            else if (lead >= 0xf0 && lead <= 0xf4)
            {
                length = 4;
                codePoint = lead & 0x07u;
                shortest = 0x10000;
            }
            else
            {
                return {};
            }

            if (length > text.size() - at)
                return {};

            for (std::size_t i = 1; i < length; ++i)
            {
                const auto next = static_cast<std::uint8_t> (text[at + i]);

                if ((next & 0xc0) != 0x80)
                    return {};

                codePoint = (codePoint << 6) | (next & 0x3fu);
            }

            if (codePoint < shortest || codePoint > 0x10ffff
                || (codePoint >= 0xd800 && codePoint <= 0xdfff))
                return {};

            return { codePoint, length };
        }

        /*  LATIN-1'S SIGNS AND LETTERS, U+00A0..U+00FF, in code point order and
            sixteen to a line. A letter folds to its base letter; a sign with an
            ASCII twin becomes the twin - a no-break space a space, guillemets a
            double quote, the degree sign of "N°" an o - and a sign with none is
            '?'. The five that fold to two letters (Æ Þ ß æ þ) are answered from
            `expansions` before this is read, and the soft hyphen is dropped
            before it too, so their places hold a '?' that is never used. */
        constexpr std::string_view latin1 {
            " !????|???a\"????"         // U+00A0   nbsp ¡ ¢ £ ¤ ¥ ¦ § ¨ © ª « ¬ shy ® ¯
            "o?23'u?.,1o\"????"         // U+00B0   ° ± ² ³ ´ µ ¶ · ¸ ¹ º » ¼ ½ ¾ ¿
            "AAAAAA?CEEEEIIII"          // U+00C0   À Á Â Ã Ä Å Æ Ç È É Ê Ë Ì Í Î Ï
            "DNOOOOOxOUUUUY??"          // U+00D0   Ð Ñ Ò Ó Ô Õ Ö × Ø Ù Ú Û Ü Ý Þ ß
            "aaaaaa?ceeeeiiii"          // U+00E0   à á â ã ä å æ ç è é ê ë ì í î ï
            "dnooooo/ouuuuy?y"          // U+00F0   ð ñ ò ó ô õ ö ÷ ø ù ú û ü ý þ ÿ
        };

        static_assert (latin1.size() == 0x60, "one character for each of U+00A0..U+00FF");

        /*  LATIN EXTENDED-A, U+0100..U+017F, the same way: every letter to its
            base letter. Ĳ ĳ Œ œ are two letters, answered from `expansions`. */
        constexpr std::string_view latinExtendedA {
            "AaAaAaCcCcCcCcDd"          // U+0100   Ā ā Ă ă Ą ą Ć ć Ĉ ĉ Ċ ċ Č č Ď ď
            "DdEeEeEeEeEeGgGg"          // U+0110   Đ đ Ē ē Ĕ ĕ Ė ė Ę ę Ě ě Ĝ ĝ Ğ ğ
            "GgGgHhHhIiIiIiIi"          // U+0120   Ġ ġ Ģ ģ Ĥ ĥ Ħ ħ Ĩ ĩ Ī ī Ĭ ĭ Į į
            "Ii??JjKkkLlLlLlL"          // U+0130   İ ı Ĳ ĳ Ĵ ĵ Ķ ķ ĸ Ĺ ĺ Ļ ļ Ľ ľ Ŀ
            "lLlNnNnNnnNnOoOo"          // U+0140   ŀ Ł ł Ń ń Ņ ņ Ň ň ŉ Ŋ ŋ Ō ō Ŏ ŏ
            "Oo??RrRrRrSsSsSs"          // U+0150   Ő ő Œ œ Ŕ ŕ Ŗ ŗ Ř ř Ś ś Ŝ ŝ Ş ş
            "SsTtTtTtUuUuUuUu"          // U+0160   Š š Ţ ţ Ť ť Ŧ ŧ Ũ ũ Ū ū Ŭ ŭ Ů ů
            "UuUuWwYyYZzZzZzs"          // U+0170   Ű ű Ų ų Ŵ ŵ Ŷ ŷ Ÿ Ź ź Ż ż Ž ž ſ
        };

        static_assert (latinExtendedA.size() == 0x80, "one character for each of U+0100..U+017F");

        /*  THE FEW THAT ARE MORE THAN ONE LETTER. ß is "ss", the one expansion
            that matters; Œ is "OE", which a French show will spell sooner or
            later ("Œuvre"); and an ellipsis is three dots rather than a '?'. */
        struct Expansion
        {
            std::uint32_t codePoint;
            std::string_view ascii;
        };

        constexpr std::array<Expansion, 10> expansions { {
            { 0x00c6, "AE" },       // Æ
            { 0x00de, "TH" },       // Þ
            { 0x00df, "ss" },       // ß
            { 0x00e6, "ae" },       // æ
            { 0x00fe, "th" },       // þ
            { 0x0132, "IJ" },       // Ĳ
            { 0x0133, "ij" },       // ĳ
            { 0x0152, "OE" },       // Œ
            { 0x0153, "oe" },       // œ
            { 0x2026, "..." },      // …
        } };

        /*  WHAT IS INVISIBLE STAYS INVISIBLE: a combining accent - the second
            half of a letter typed decomposed, whose base letter has already
            been written - a soft hyphen, the zero-width spaces and joiners,
            the direction marks and controls, the variation selectors, and a
            byte-order mark. */
        bool isInvisible (std::uint32_t c) noexcept
        {
            return c == 0x00ad
                || (c >= 0x0300 && c <= 0x036f)
                || (c >= 0x200b && c <= 0x200f)
                || (c >= 0x202a && c <= 0x202e)
                || (c >= 0x2060 && c <= 0x206f)
                || (c >= 0xfe00 && c <= 0xfe0f)
                || c == 0xfeff;
        }

        /*  Every other sign with an ASCII twin - the spaces, the dashes, the
            quotes - and '?' for everything that has none. */
        char twinOf (std::uint32_t c) noexcept
        {
            //  The typographic spaces, the narrow no-break one being what French
            //  puts before : ; ! and ?
            if ((c >= 0x2000 && c <= 0x200a) || c == 0x202f || c == 0x205f || c == 0x3000)
                return ' ';

            //  Hyphens, the figure, en and em dashes, the bar, the minus sign.
            if ((c >= 0x2010 && c <= 0x2015) || c == 0x2212)
                return '-';

            //  Single quotes, the prime, single guillemets.
            if ((c >= 0x2018 && c <= 0x201b) || c == 0x2032 || c == 0x2039 || c == 0x203a)
                return '\'';

            //  Double quotes and the double prime.
            if ((c >= 0x201c && c <= 0x201f) || c == 0x2033)
                return '"';

            return '?';
        }

        void appendFolded (std::string& out, std::uint32_t c)
        {
            /*  CONTROL CHARACTERS BECOME SPACES. A tab or a line break inside a
                name is a gap, and a display that drew a control glyph would
                show something nobody typed. */
            if (c < 0x80)
            {
                out += (c < 0x20 || c == 0x7f) ? ' ' : static_cast<char> (c);
                return;
            }

            if (c < 0xa0)   // the C1 controls
            {
                out += ' ';
                return;
            }

            if (isInvisible (c))
                return;

            for (const auto& expansion : expansions)
                if (expansion.codePoint == c)
                {
                    out += expansion.ascii;
                    return;
                }

            if (c <= 0xff)
                out += latin1[c - 0xa0];
            else if (c <= 0x17f)
                out += latinExtendedA[c - 0x100];
            else
                out += twinOf (c);
        }
    }

    //==========================================================================
    ButtonId buttonForNote (int note) noexcept
    {
        for (const auto& run : buttonRuns)
            if (note >= run.firstNote && note < run.firstNote + run.count)
            {
                ButtonId found;
                found.button = run.button;
                found.index = run.count > 1 ? note - run.firstNote : -1;
                return found;
            }

        return {};
    }

    int noteForButton (ButtonId id) noexcept
    {
        for (const auto& run : buttonRuns)
        {
            if (run.button != id.button)
                continue;

            if (run.count == 1)
                return id.index == -1 ? run.firstNote : -1;

            return (id.index >= 0 && id.index < run.count) ? run.firstNote + id.index : -1;
        }

        return -1;
    }

    //==========================================================================
    std::optional<McuEvent> decodeMcu (const Bytes& message)
    {
        if (message.empty())
            return std::nullopt;

        if (message.front() == sysexStart)
            return decodeSysEx (message);

        /*  EVERY CHANNEL MESSAGE A MACKIE SURFACE SENDS IS THREE BYTES, a
            status and two data. A shorter or a longer one is not a message
            this protocol has, and neither is one whose data has a top bit. */
        if (message.size() != 3 || ! isData (message[1]) || ! isData (message[2]))
            return std::nullopt;

        const int status = message[0] & 0xf0;
        const int channel = message[0] & 0x0f;
        const int first = message[1];
        const int second = message[2];

        McuEvent event;

        /*  A FADER IS PITCH BEND, one channel per strip and the ninth for the
            master, the least significant seven bits first (control guide §3.1,
            §3.4). Channels 10 to 16 carry nothing. */
        if (status == pitchBendStatus)
        {
            if (channel > masterStrip)
                return std::nullopt;

            event.kind = channel == masterStrip ? McuEvent::Kind::masterFader
                                                : McuEvent::Kind::fader;
            event.strip = channel;
            event.value = (second << 7) | first;
            return event;
        }

        /*  EVERYTHING ELSE A SURFACE SENDS IS ON CHANNEL 1. Channels 2 to 4
            are where the D700's colour goes out, and nothing comes back on
            them. */
        if (channel != 0)
            return std::nullopt;

        if (status == noteOnStatus || status == noteOffStatus)
        {
            event.down = status == noteOnStatus && second != 0;

            if (first >= touchNote && first < touchNote + stripsPerBank)
            {
                event.kind = McuEvent::Kind::touch;
                event.strip = first - touchNote;
                return event;
            }

            if (first == masterTouchNote)
            {
                event.kind = McuEvent::Kind::masterTouch;
                event.strip = masterStrip;
                return event;
            }

            event.id = buttonForNote (first);

            if (event.id.button == Button::none)
                return std::nullopt;

            event.kind = McuEvent::Kind::button;
            event.strip = isStripButton (event.id.button) ? event.id.index : -1;
            return event;
        }

        if (status == controllerStatus)
        {
            if (first >= encoderController && first < encoderController + stripsPerBank)
            {
                event.kind = McuEvent::Kind::encoder;
                event.strip = first - encoderController;
                event.value = signMagnitude (second);
                return event;
            }

            if (first == jogController)
            {
                event.kind = McuEvent::Kind::jog;
                event.value = signMagnitude (second);
                return event;
            }
        }

        return std::nullopt;
    }

    std::optional<PadEvent> decodePads (const Bytes& message)
    {
        if (message.empty())
            return std::nullopt;

        const int status = message[0] & 0xf0;

        PadEvent event;
        event.channel = (message[0] & 0x0f) + 1;

        /*  Channel pressure is the one with a single data byte, and no note. */
        if (status == channelPressureStatus)
        {
            if (message.size() != 2 || ! isData (message[1]))
                return std::nullopt;

            event.kind = PadEvent::Kind::channelPressure;
            event.value = message[1];
            return event;
        }

        if (message.size() != 3 || ! isData (message[1]) || ! isData (message[2]))
            return std::nullopt;

        if (status == noteOnStatus)             event.kind = PadEvent::Kind::noteOn;
        else if (status == noteOffStatus)       event.kind = PadEvent::Kind::noteOff;
        else if (status == polyPressureStatus)  event.kind = PadEvent::Kind::polyPressure;
        else                                    return std::nullopt;

        event.note = message[1];
        event.value = message[2];
        return event;
    }

    //==========================================================================
    std::string asciiFold (std::string_view utf8)
    {
        std::string out;
        out.reserve (utf8.size());

        for (std::size_t at = 0; at < utf8.size();)
        {
            const auto character = readCharacter (utf8, at);

            /*  A BAD BYTE IS ONE '?', and reading resumes at the very next
                byte - so a name with one corrupt character keeps every other
                one it had, and a truncated sequence cannot swallow the letter
                after it. */
            if (character.length == 0)
            {
                out += '?';
                ++at;
                continue;
            }

            appendFolded (out, character.codePoint);
            at += character.length;
        }

        return out;
    }

    std::string fitText (std::string_view utf8, std::size_t width)
    {
        auto text = asciiFold (utf8);
        text.resize (width, ' ');
        return text;
    }

    //==========================================================================
    Bytes faderPosition (int strip, int value14)
    {
        if (strip < 0 || strip > masterStrip)
            return {};

        const auto value = std::clamp (value14, 0, highestFader);

        return { static_cast<std::uint8_t> (pitchBendStatus + strip),
                 static_cast<std::uint8_t> (value & 0x7f),
                 static_cast<std::uint8_t> (value >> 7) };
    }

    Bytes led (int note, Led state)
    {
        if (! isData (note))
            return {};

        return { static_cast<std::uint8_t> (noteOnStatus),
                 static_cast<std::uint8_t> (note),
                 velocityOf (state) };
    }

    Bytes ringMcu (int strip, int position, int mode, bool centre)
    {
        if (! isStrip (strip))
            return {};

        const auto value = (centre ? 0x40 : 0x00)
                         | (std::clamp (mode, 0, highestRingMode) << 4)
                         | std::clamp (position, 0, highestRingPosition);

        return { static_cast<std::uint8_t> (controllerStatus),
                 static_cast<std::uint8_t> (ringController + strip),
                 static_cast<std::uint8_t> (value) };
    }

    Bytes meter (int strip, int level)
    {
        if (! isStrip (strip))
            return {};

        const auto shown = std::clamp (level, 0, highestMeterLevel);

        return { static_cast<std::uint8_t> (channelPressureStatus),
                 static_cast<std::uint8_t> ((strip << 4) | shown) };
    }

    Bytes meterClearPeak (int strip)
    {
        if (! isStrip (strip))
            return {};

        return { static_cast<std::uint8_t> (channelPressureStatus),
                 static_cast<std::uint8_t> ((strip << 4) | clearPeakLevel) };
    }

    Bytes lcd (std::uint8_t deviceId, int offset, std::string_view utf8)
    {
        if (! isData (deviceId) || offset < 0 || offset >= lcdSize)
            return {};

        auto out = sysexOpening (deviceId, lcdCommand);
        out.push_back (static_cast<std::uint8_t> (offset));

        /*  CUT WHERE THE BUFFER ENDS. What the display does with a write past
            its last character is not something anybody measured, and a text
            that long was never going to be read there anyway. */
        const auto text = asciiFold (utf8);
        const auto room = static_cast<std::size_t> (lcdSize - offset);
        appendText (out, std::string_view (text).substr (0, room));

        out.push_back (sysexEnd);
        return out;
    }

    Bytes lcdCell (std::uint8_t deviceId, int row, int strip, std::string_view utf8)
    {
        if (row < 0 || row > 1 || ! isStrip (strip))
            return {};

        const auto offset = row * lcdRowSize + strip * lcdCellStride;
        return lcd (deviceId, offset, fitText (utf8, mcuCellWidth));
    }

    Bytes deviceQuery (std::uint8_t deviceId)
    {
        if (! isData (deviceId))
            return {};

        auto out = sysexOpening (deviceId, deviceQueryCommand);
        out.push_back (sysexEnd);
        return out;
    }

    //==========================================================================
    Bytes d700Ring (int strip, int value, int mode)
    {
        if (! isStrip (strip))
            return {};

        //  THE CHANNEL IS THE MODE.
        const auto channel = std::clamp (mode, 0, highestD700RingMode);

        return { static_cast<std::uint8_t> (controllerStatus + channel),
                 static_cast<std::uint8_t> (ringController + strip),
                 sevenBits (value) };
    }

    Bytes d700DisplayRow (int strip, int row, std::string_view utf8)
    {
        if (! isStrip (strip) || row < 0 || row > 1)
            return {};

        auto out = sysexOpening (d700DeviceId, rowsCommand);
        out.push_back (fieldStart (strip, d700RowWidth));

        //  ONE-BASED ON THE WIRE: 01 is the top row.
        out.push_back (static_cast<std::uint8_t> (row + 1));

        appendText (out, fitText (utf8, d700RowWidth));
        out.push_back (sysexEnd);
        return out;
    }

    Bytes d700DisplayRow3 (int strip, std::string_view utf8)
    {
        if (! isStrip (strip))
            return {};

        auto out = sysexOpening (d700DeviceId, thirdRowCommand);
        out.push_back (fieldStart (strip, d700Row3Width));
        appendText (out, fitText (utf8, d700Row3Width));
        out.push_back (sysexEnd);
        return out;
    }

    Bytes d700TrackNumbers (const std::array<int, 8>& numbers)
    {
        auto out = sysexOpening (d700DeviceId, trackNumbersCommand);

        //  Where the eight start, which is always the first: a bank has eight.
        out.push_back (0x00);

        for (const auto number : numbers)
            out.push_back (sevenBits (number));

        out.push_back (sysexEnd);
        return out;
    }

    Bytes d700Colour (int note, int red, int green, int blue)
    {
        if (! carriesColour (note))
            return {};

        const auto element = static_cast<std::uint8_t> (note);

        return { redStatus,     element, sevenBits (red),
                 greenStatus,   element, sevenBits (green),
                 blueStatus,    element, sevenBits (blue) };
    }

    //==========================================================================
    bool isSafeSysEx (const Bytes& message) noexcept
    {
        if (! isMackieSysEx (message))
            return false;

        const auto command = message[5];
        return std::find (safeCommands.begin(), safeCommands.end(), command) != safeCommands.end();
    }
}
