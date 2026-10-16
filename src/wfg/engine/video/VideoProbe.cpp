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

#include <wfg/engine/video/VideoProbe.h>

#include <wfg/engine/video/DeviceNames.h>
#include <wfg/engine/video/render/Camera.h>

#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <locale>
#include <sstream>
#include <thread>

namespace wfg::video
{
    namespace
    {
        const char* yesNo (bool value)  { return value ? "yes" : "no"; }

        std::string keyers (const card::Card& card)
        {
            if (card.internalKey && card.externalKey)  return "internal and external";
            if (card.internalKey)                      return "internal";
            if (card.externalKey)                      return "external";
            return "none";
        }

        void listFormats (std::ostream& out, const char* heading, const card::Card& card, bool output)
        {
            std::string line;

            for (const auto& mode : card.modes)
                if (output ? mode.output : mode.input)
                    line += (line.empty() ? "" : " ") + mode.word;

            out << "  " << heading << ": " << (line.empty() ? "none" : line) << "\n";
        }

        /*  `--name=value`'s value, or `fallback`. */
        std::string option (const std::vector<std::string>& args, const std::string& name, const std::string& fallback)
        {
            const auto prefix = "--" + name + "=";

            for (const auto& arg : args)
                if (arg.rfind (prefix, 0) == 0)
                    return arg.substr (prefix.size());

            return fallback;
        }

        bool flag (const std::vector<std::string>& args, const std::string& name)
        {
            return std::find (args.begin(), args.end(), "--" + name) != args.end();
        }

        std::int64_t steadyNanos()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        /*  THE PATTERN, BGRA: seven bars, the 75% colours a tester's monitor
            names; with a key, the bars' alpha falling left to right. */
        void drawBars (std::uint8_t* pixels, int rowBytes, int width, int height, bool keyed)
        {
            static constexpr std::uint8_t bars[7][3] { { 191, 191, 191 }, { 0, 191, 191 }, { 191, 191, 0 }, { 0, 191, 0 },
                                                       { 191, 0, 191 }, { 0, 0, 191 }, { 191, 0, 0 } };   // B G R

            for (int y = 0; y < height; ++y)
            {
                auto* row = pixels + static_cast<std::ptrdiff_t> (y) * rowBytes;

                for (int x = 0; x < width; ++x)
                {
                    const auto bar = std::min (6, x * 7 / std::max (1, width));
                    const auto alpha = keyed ? static_cast<std::uint8_t> (255 - x * 255 / std::max (1, width - 1)) : std::uint8_t { 255 };
                    auto* px = row + static_cast<std::ptrdiff_t> (x) * 4;

                    //  Premultiplied, as Go.dot sends a keyed fill (APG).
                    for (int c = 0; c < 3; ++c)
                        px[c] = static_cast<std::uint8_t> (bars[bar][c] * alpha / 255);

                    px[3] = alpha;
                }
            }
        }

        void fillWhite (std::uint8_t* pixels, int rowBytes, int width, int height)
        {
            for (int y = 0; y < height; ++y)
                std::memset (pixels + static_cast<std::ptrdiff_t> (y) * rowBytes, 255, static_cast<std::size_t> (width) * 4);
        }

        /*  A block across the bottom, a frame's step each frame: a dropped or
            repeated frame shows as a jump. */
        void drawBlock (std::uint8_t* pixels, int rowBytes, int width, int height, std::int64_t frame)
        {
            const auto size = std::max (8, height / 12);
            const auto x0 = static_cast<int> ((frame * 8) % std::max<std::int64_t> (1, width - size));
            const auto y0 = height - size - size / 2;

            for (int y = y0; y < y0 + size && y < height; ++y)
                std::memset (pixels + static_cast<std::ptrdiff_t> (y) * rowBytes + static_cast<std::ptrdiff_t> (x0) * 4, 255,
                             static_cast<std::size_t> (size) * 4);
        }

        std::vector<float> asFloats (const std::vector<std::uint8_t>& bgra, int width, int height, int rowBytes)
        {
            std::vector<float> rgba (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4);

            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const auto* px = bgra.data() + static_cast<std::ptrdiff_t> (y) * rowBytes + static_cast<std::ptrdiff_t> (x) * 4;
                    auto* to = rgba.data() + (static_cast<std::size_t> (y) * static_cast<std::size_t> (width) + static_cast<std::size_t> (x)) * 4;
                    to[0] = static_cast<float> (px[2]) / 255.0f;
                    to[1] = static_cast<float> (px[1]) / 255.0f;
                    to[2] = static_cast<float> (px[0]) / 255.0f;
                    to[3] = 1.0f;
                }

            return rgba;
        }

        //======================================================================
        /*  --run: the card's output opened and fed for `--seconds`, a white
            frame and - with --sound - a beep on its first two channels once a
            second, both on the same moment of the card's stream; then what
            happened, said. */
        int run (card::System& cards, const card::Found& found, const std::vector<std::string>& args, std::ostream& out)
        {
            const card::Card* chosen = nullptr;
            const auto wanted = option (args, "card", {});

            for (const auto& card : found.cards)
                if (card.output && (wanted.empty() || card.id == wanted || card.name == wanted || nameWithMaker (card.id, card.name) == wanted)
                      && chosen == nullptr)
                    chosen = &card;

            if (chosen == nullptr)
            {
                out << "\nNo card to run: " << (wanted.empty() ? std::string ("none sends pictures") : "no card " + wanted) << "\n";
                return 2;
            }

            card::OutputSettings settings;
            settings.mode = option (args, "mode", modes::defaultWord);
            settings.key = flag (args, "key") ? card::Keying::external : card::Keying::off;
            settings.audioChannels = flag (args, "sound") ? 2 : 0;
            settings.tenBit = flag (args, "ten-bit");
            //  Read in no locale: "1.2" is 1.2 under fr_FR too (PRD §3.20).
            const auto seconds = std::max (0.5, osc::parseDouble (option (args, "seconds", "60")).value_or (60.0));

            std::string why;
            auto output = cards.openOutput (chosen->id, settings, why);

            out << "\nRunning " << nameWithMaker (chosen->id, chosen->name) << " at " << settings.mode << (settings.key != card::Keying::off ? ", keyed" : "")
                << (settings.audioChannels > 0 ? ", with sound" : "") << ", for " << seconds << " s\n";

            if (output == nullptr)
            {
                out << "  the card would not open: " << why << "\n";
                return 3;
            }

            const auto& format = output->mode().format;
            const auto tenBit = output->format() == card::PixelFormat::yuv10;
            const auto rowBytes = card::rowBytesFor (output->format(), format.width);
            const auto perSecond = static_cast<std::int64_t> (std::llround (format.frameRate()));

            out << "  pixels: " << (tenBit ? "10-bit YUV" : "8-bit BGRA") << ", " << format.width << " x " << format.height
                << ", " << format.frameRate() << " frames a second\n";

            //  The two pictures, made once: the bars and the flash.
            const auto bgraRow = card::rowBytesFor (card::PixelFormat::bgra8, format.width);
            std::vector<std::uint8_t> bars (static_cast<std::size_t> (bgraRow) * static_cast<std::size_t> (format.height));
            std::vector<std::uint8_t> white (bars.size());
            drawBars (bars.data(), bgraRow, format.width, format.height, settings.key != card::Keying::off);
            fillWhite (white.data(), bgraRow, format.width, format.height);

            std::vector<std::uint8_t> barsPacked, whitePacked;

            if (tenBit)
            {
                barsPacked = card::packV210 (asFloats (bars, format.width, format.height, bgraRow).data(), format.width, format.height, rowBytes);
                whitePacked = card::packV210 (asFloats (white, format.width, format.height, bgraRow).data(), format.width, format.height, rowBytes);
            }

            //  The beep: 1 kHz for 50 ms, at half level.
            std::vector<std::int32_t> beep (2400 * 2);

            for (int n = 0; n < 2400; ++n)
            {
                const auto value = static_cast<std::int32_t> (0.5 * 2147483647.0 * std::sin (2.0 * 3.14159265358979323846 * 1000.0 * n / 48000.0));
                beep[static_cast<std::size_t> (n) * 2] = value;
                beep[static_cast<std::size_t> (n) * 2 + 1] = value;
            }

            std::int64_t next = 0;
            std::int64_t firstStream = -1, firstNanos = 0, lastStream = 0, lastNanos = 0;
            std::uint64_t beepsScheduled = 0, beepsRefused = 0;
            std::int64_t beepFrame = 0;
            const auto began = steadyNanos();
            const auto until = began + static_cast<std::int64_t> (seconds * 1.0e9);

            while (steadyNanos() < until)
            {
                while (output->waiting() < 3)
                {
                    const auto frame = output->lend();

                    if (frame.pixels == nullptr)
                        break;

                    const auto flash = perSecond > 0 && next % perSecond == 0;
                    auto* to = static_cast<std::uint8_t*> (frame.pixels);

                    for (int y = 0; y < format.height; ++y)
                    {
                        const auto& from = tenBit ? (flash ? whitePacked : barsPacked) : (flash ? white : bars);
                        const auto length = static_cast<std::size_t> (tenBit ? rowBytes : bgraRow);
                        std::memcpy (to + static_cast<std::ptrdiff_t> (y) * frame.rowBytes,
                                     from.data() + static_cast<std::size_t> (y) * length, std::min (length, static_cast<std::size_t> (frame.rowBytes)));
                    }

                    if (! tenBit && ! flash)
                        drawBlock (to, frame.rowBytes, format.width, format.height, next);

                    if (! output->schedule (frame, next))
                        break;

                    ++next;

                    if (! output->started() && output->waiting() >= 3)
                        output->start (0);
                }

                //  The beeps, each placed on its flash's moment, a second ahead.
                while (settings.audioChannels > 0 && beepFrame < next + perSecond && perSecond > 0)
                {
                    const auto sample = beepFrame * format.frameDuration * 48000 / format.timeScale;

                    if (output->scheduleSound (beep.data(), 2400, sample))
                        ++beepsScheduled;
                    else
                        ++beepsRefused;

                    beepFrame += perSecond;
                }

                std::int64_t streamNanos = 0, nanos = 0;

                if (output->clock (streamNanos, nanos))
                {
                    if (firstStream < 0)
                    {
                        firstStream = streamNanos;
                        firstNanos = nanos;
                    }

                    lastStream = streamNanos;
                    lastNanos = nanos;
                }

                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }

            const auto counted = output->completions();
            const auto latencyMs = static_cast<double> (output->waiting()) * static_cast<double> (format.frameDuration) * 1000.0
                                 / static_cast<double> (format.timeScale);
            const auto skew = lastNanos > firstNanos
                                ? (static_cast<double> (lastStream - firstStream) / static_cast<double> (lastNanos - firstNanos) - 1.0) * 1.0e6
                                : 0.0;

            out << "  frames shown: " << counted.shown << ", late: " << counted.late << ", dropped: " << counted.dropped << "\n";
            out << "  frames held by the card at the end: " << output->waiting() << " (" << latencyMs << " ms)\n";
            out << "  the card's clock against this computer's: " << std::lround (skew) << " ppm\n";
            out << "  reference: " << card::referenceWord (output->reference()) << "\n";

            if (settings.audioChannels > 0)
                out << "  beeps placed: " << beepsScheduled << ", refused: " << beepsRefused << "\n";

            if (const auto lost = output->problem(); ! lost.empty())
                out << "  the card said: " << lost << "\n";

            return 0;
        }
    }

    int runVideoProbe (card::System& cards, const std::vector<std::string>& args, std::ostream& said)
    {
        const card::ThreadScope apartment;
        std::ostringstream out;

        //  No locale writes the report's numbers (PRD §3.20): 1.2 s, never 1,2 s.
        out.imbue (std::locale::classic());
        const auto found = cards.find();
        auto code = 0;

        out << "Go.dot video probe\n";
        out << "Desktop Video: " << (found.version.empty() ? "not found" : found.version) << "\n";
        out << "AJA driver: " << (found.ajaDriver.empty() ? "no AJA card answered" : found.ajaDriver) << "\n";

        if (! found.problem.empty())
        {
            out << found.problem << "\n";
            code = 2;
        }
        else
        {
            out << found.cards.size() << (found.cards.size() == 1 ? " card\n" : " cards\n");

            for (const auto& card : found.cards)
            {
                out << "\n" << nameWithMaker (card.id, card.name) << "  (" << card.id << ")\n";
                out << "  sends pictures: " << yesNo (card.output) << "   takes pictures in: " << yesNo (card.input) << "\n";
                out << "  keyers: " << keyers (card) << "   10-bit: " << yesNo (card.tenBit)
                    << "   sound channels: " << card.audioChannels << "\n";
                listFormats (out, "formats it sends", card, true);
                listFormats (out, "formats it takes in", card, false);
            }

            if (flag (args, "run"))
                code = run (cards, found, args, out);
        }

        /*  AND THE CAMERAS (§72): names only, as nothing is opened to be
            listed - a camera lights up when an input opens it. */
        if (! flag (args, "run"))
        {
            const auto cameras = camera::system().find();
            out << "\n" << cameras.cameras.size() << (cameras.cameras.size() == 1 ? " camera\n" : " cameras\n");

            if (! cameras.problem.empty())
                out << cameras.problem << "\n";

            for (const auto& one : cameras.cameras)
                out << "  " << one.name << "  (" << one.id << ")\n";
        }

        said << out.str();

        //  THE REPORT, for the tester to send back: what was said, in a file.
        if (const auto report = option (args, "report", {}); ! report.empty())
        {
            std::ofstream file (report, std::ios::binary);
            file << out.str();
            said << "\nWritten to " << report << "\n";
        }

        return code;
    }
}
