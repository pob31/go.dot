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

#include <wfg/engine/surface/SurfaceBridge.h>

#include <wfg/engine/audio/EqColours.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/surface/FaderCurve.h>
#include <wfg/engine/surface/McuCodec.h>
#include <wfg/engine/surface/SurfacePages.h>
#include <wfg/engine/surface/SurfaceProfile.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::surface
{
    namespace
    {
        //======================================================================
        //  The numbers of the protocol this file uses, each named once.

        constexpr std::size_t stripsPerBank = 8;

        /*  THE DEVICE ID ON EVERY SYSEX, whatever the surface says it is: both
            of the D700's banks accept 0x14 (protocol §2.1), and the bank is the
            port a message goes to, never the id inside it. */
        constexpr std::uint8_t deviceId = d700DeviceId;

        constexpr int recNote = 0x00;           // + element: REC, where the strip's fader starts, lit a moment when set
        constexpr int soloNote = 0x08;          // + element: SOLO, the bank locked to the strip
        constexpr int muteNote = 0x10;          // + element: MUTE, the strip's kill, lit a moment when it kills
        constexpr int selectNote = 0x18;        // + element: SELECT, whose LED says the rotaries are aimed at its cue
        constexpr int vpotNote = 0x20;          // + element: the V-Pot - its press, and the D700's colour
        constexpr int sendButtonNote = 0x29;    // Send, lit while its page is up
        constexpr int fxButtonNote = 0x2b;      // FX (Mackie's Plug-In), lit while its page is up
        constexpr int eqButtonNote = 0x2c;      // EQ, lit while its page is up

        constexpr int ringFillMode = 2;         // MCU "wrap" and the D700's channel 3: fill from the left
        constexpr int d700RingSteps = 127;      // a D700 ring's value, 0..127 (control guide §4.3)
        constexpr int mcuRingSteps = 11;        // MCU's ring positions, 0 lighting none

        //  A run's `meter` when nothing has left its track: the tree's word for silence.
        constexpr double meterSilentDb = -120.0;

        /*  WHAT THE CALLBACK THREAD MAY QUEUE BEFORE THE TICK THREAD HAS DRAINED
            IT. Hundreds of messages a tick is a busy surface; this many is a
            tick thread that has stopped, and a callback that went on
            allocating for it would take the machine down with the show. */
        constexpr std::size_t inboxLimit = 8192;

        constexpr std::uint8_t sysexStart = 0xf0;
        constexpr std::uint8_t firstRealTime = 0xf8;   // clock, active sensing: nothing a surface means

        /*  A cue's `velocityFloor` when the tree has none to say - the row's
            own default. Only a media cue has the row, and only a media cue
            has `pressure` either, so this is never what decides a level. */
        constexpr double defaultFloorDb = -40.0;

        /*  Where the tree says which cue the rotaries are aimed at: one for
            every surface, set by `surface.aim` (2026-09-25). */
        const std::string& aimAddress()
        {
            static const std::string address { "/godot/surface/aim" };
            return address;
        }

        /*  And the show's mix channels in output order: a Send page's rotaries. */
        const std::string& mixesAddress()
        {
            static const std::string address { "/godot/audio/mixes" };
            return address;
        }

        /*  A space-separated list of identifiers, as the tree publishes one. */
        std::vector<std::string> wordsOf (std::string_view text)
        {
            std::vector<std::string> out;
            std::size_t at = 0;

            while (at < text.size())
            {
                const auto start = text.find_first_not_of (' ', at);

                if (start == std::string_view::npos)
                    break;

                const auto end = text.find (' ', start);
                out.emplace_back (text.substr (start, end == std::string_view::npos ? std::string_view::npos
                                                                                     : end - start));
                at = end == std::string_view::npos ? text.size() : end;
            }

            return out;
        }

        /*  A send's rotary wears its cue's colour; one with none, a plain light. */
        constexpr Rgb neutralLight { 80, 80, 80 };

        /*  A level to a tenth of a decibel as text, digit by digit - what
            `send.create` is handed, so the log reads "-12.5" and not the last
            bit of a double. */
        std::string tenthsText (double decibels)
        {
            const auto tenths = std::llround (decibels * 10.0);
            const auto magnitude = tenths < 0 ? -tenths : tenths;
            return (tenths < 0 ? "-" : "") + std::to_string (magnitude / 10) + "."
                 + std::to_string (magnitude % 10);
        }

        /*  A theme's 0xAARRGGBB as the D700 takes a colour, each eight-bit
            component halved (control guide §4.4) - the EQ's band colours. */
        Rgb rgbOf (std::uint32_t argb, double share) noexcept
        {
            const auto component = [share] (std::uint32_t eight)
            {
                return static_cast<int> (std::lround (static_cast<double> (eight >> 1) * share));
            };

            return Rgb { component ((argb >> 16) & 0xffu), component ((argb >> 8) & 0xffu),
                         component (argb & 0xffu) };
        }

        //======================================================================
        const std::string& noText()
        {
            static const std::string none;
            return none;
        }

        /*  THE TREE, READ WITHOUT COPYING. `Node::soleValue` hands a value
            back by copy, which for a string is an allocation; fifty times a
            second over every strip that is a lot of them for nothing. So the
            one value a node holds is read where it lies.

            The address is a `const std::string&` rather than a view, and every
            caller hands in a string that outlives the call: a view made on the
            way in would be a temporary argument to a function returning a
            reference, which is what GCC's -Wdangling-reference looks for. */
        const osc::Value* soleAt (const tree::TreeSnapshot* snapshot, const std::string& address)
        {
            if (snapshot == nullptr || address.empty())
                return nullptr;

            const auto* node = snapshot->find (address);

            if (node == nullptr || node->values.size() != 1)
                return nullptr;

            return &node->values.front();
        }

        const std::string& textAt (const tree::TreeSnapshot* snapshot, const std::string& address)
        {
            const auto* value = soleAt (snapshot, address);
            return value != nullptr && value->isString() ? value->getString() : noText();
        }

        std::optional<double> numberAt (const tree::TreeSnapshot* snapshot, const std::string& address)
        {
            const auto* value = soleAt (snapshot, address);

            if (value == nullptr || ! value->isNumber() || value->isNonFinite())
                return std::nullopt;

            return value->asDouble();
        }

        bool flagAt (const tree::TreeSnapshot* snapshot, const std::string& address)
        {
            const auto* value = soleAt (snapshot, address);
            return value != nullptr && value->isBool() && value->getBool();
        }

        //======================================================================
        Event commandFrom (const std::string& origin, const char* name,
                           std::vector<osc::Value> args = {})
        {
            Event event;
            event.origin = origin;
            event.command = name;
            event.args = std::move (args);
            return event;
        }

        /*  A serial as a client can show it: the printable bytes kept, any
            other one a '?'. Seven bytes that are an identifier and not text,
            so nothing is folded or dropped - a lost byte would be a different
            serial. */
        std::string printable (const std::string& raw)
        {
            std::string out;
            out.reserve (raw.size());

            for (const auto c : raw)
            {
                const auto byte = static_cast<unsigned char> (c);
                out += (byte >= 0x20 && byte <= 0x7e) ? c : '?';
            }

            return out;
        }

        /*  A LEVEL AS A DISPLAY SHOWS IT: one decimal and the unit, "-6.2 dB",
            written digit by digit so no locale can move the point - and at the
            bottom the word rather than the number, "-inf dB", for the reason
            the client's `faderText` gives: -120.0 reads as a very quiet sound,
            and what it means is none. */
        void levelText (double decibels, std::string& out)
        {
            out.clear();

            if (decibels <= faderSilenceDb)
            {
                out.append ("-inf dB");
                return;
            }

            const auto tenths = std::lround (std::clamp (decibels, faderSilenceDb, faderLoudestDb) * 10.0);
            const auto magnitude = tenths < 0 ? -tenths : tenths;

            if (tenths < 0)
                out.push_back ('-');

            std::array<char, 24> digits {};
            const auto written = std::to_chars (digits.data(), digits.data() + digits.size(), magnitude / 10);
            out.append (digits.data(), written.ptr);
            out.push_back ('.');
            out.push_back (static_cast<char> ('0' + static_cast<int> (magnitude % 10)));
            out.append (" dB");
        }

        /*  The cue number a track-number field can show: a whole number from
            one to 127. "12.5" and "4a" are cue numbers and not track numbers,
            so they answer nothing and the strip shows its own. */
        std::optional<int> trackNumberOf (std::string_view number)
        {
            if (number.empty() || number.size() > 3)
                return std::nullopt;

            auto value = 0;

            for (const auto c : number)
            {
                if (c < '0' || c > '9')
                    return std::nullopt;

                value = value * 10 + (c - '0');
            }

            if (value < 1 || value > 127)
                return std::nullopt;

            return value;
        }

        int hexDigit (char c) noexcept
        {
            if (c >= '0' && c <= '9')   return c - '0';
            if (c >= 'a' && c <= 'f')   return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')   return c - 'A' + 10;
            return -1;
        }

        int sevenBitsOf (double unit) noexcept
        {
            return static_cast<int> (std::lround (std::clamp (unit, 0.0, 1.0) * 127.0));
        }
    }

    //==========================================================================
    std::optional<Rgb> colourFromHex (std::string_view text)
    {
        if (text.size() != 7 || text.front() != '#')
            return std::nullopt;

        std::array<int, 3> parts {};

        for (std::size_t part = 0; part < parts.size(); ++part)
        {
            const auto high = hexDigit (text[1 + part * 2]);
            const auto low = hexDigit (text[2 + part * 2]);

            if (high < 0 || low < 0)
                return std::nullopt;

            //  EIGHT BITS HALVED: the D700 takes 0..127 (control guide §4.4).
            parts[part] = (high * 16 + low) >> 1;
        }

        return Rgb { parts[0], parts[1], parts[2] };
    }

    std::optional<Rgb> colourFromTimbre (std::string_view text)
    {
        std::array<double, 3> hsl {};
        std::size_t at = 0;

        for (auto& component : hsl)
        {
            while (at < text.size() && text[at] == ' ')
                ++at;

            const auto start = at;

            while (at < text.size() && text[at] != ' ')
                ++at;

            if (at == start)
                return std::nullopt;

            //  Written by `osc::formatDouble`, so read back by its twin.
            const auto parsed = osc::parseDouble (text.substr (start, at - start));

            if (! parsed.has_value())
                return std::nullopt;

            component = *parsed;
        }

        while (at < text.size() && text[at] == ' ')
            ++at;

        if (at != text.size())
            return std::nullopt;

        /*  THE HUE AND THE SATURATION AS ANALYSED, AND THE LIGHT AT FULL
            (author, 2026-09-23: keep the saturation - it shows how broad the
            spectrum is). The saturation is one minus the spectral flatness, a
            tone vivid and noise pale, and it is what an LED has to carry beside
            the hue. The timbre's lightness is its frequency axis again
            (Timbre.h: 0.15 low to 0.85 high); read as HSL it would make a
            bass bed a dark LED and a high effect a white one, washing the
            saturation out at both ends, so it is not read - except that
            nought is silence, and silence is dark. Brightness is left free for
            what the bench decides to put on it - and since 2026-09-25 that is
            the sound's variation, applied after this by `pulsed` - and a
            ceiling for a house that needs a dark booth.

            So HSV at full value: the chroma is the saturation, the hue's sector
            chooses which component carries it, and the rest is white. */
        const auto hue = std::fmod (std::fmod (hsl[0], 360.0) + 360.0, 360.0) / 60.0;
        const auto saturation = std::clamp (hsl[1], 0.0, 1.0);
        const auto lightness = std::clamp (hsl[2], 0.0, 1.0);

        if (! (lightness > 0.0))
            return Rgb {};

        const auto chroma = saturation;
        const auto second = chroma * (1.0 - std::abs (std::fmod (hue, 2.0) - 1.0));
        const auto lift = 1.0 - chroma;

        auto red = 0.0;
        auto green = 0.0;
        auto blue = 0.0;

        switch (std::clamp (static_cast<int> (hue), 0, 5))
        {
            case 0:  red = chroma;  green = second; break;
            case 1:  red = second;  green = chroma; break;
            case 2:  green = chroma; blue = second; break;
            case 3:  green = second; blue = chroma; break;
            case 4:  red = second;  blue = chroma;  break;
            default: red = chroma;  blue = second;  break;
        }

        return Rgb { sevenBitsOf (red + lift), sevenBitsOf (green + lift), sevenBitsOf (blue + lift) };
    }

    Rgb forTheLeds (Rgb colour) noexcept
    {
        auto red = std::clamp (colour.red, 0, 127) / 127.0;
        auto green = std::clamp (colour.green, 0, 127) / 127.0;
        auto blue = std::clamp (colour.blue, 0, 127) / 127.0;

        /*  THE TOTAL LIGHT HELD: white lights all three channels, and at the
            same brightness gave three times the light of a pure red (author,
            2026-09-25: "The white 'looks' louder"). */
        if (const auto total = red + green + blue; total > ledLightBudget)
        {
            const auto share = ledLightBudget / total;
            red *= share;
            green *= share;
            blue *= share;
        }

        //  Each channel trimmed, then the LEDs' response straightened.
        const auto shaped = [] (double value, double trim)
        {
            return static_cast<int> (std::lround (127.0 * std::pow (std::clamp (value * trim, 0.0, 1.0), ledGamma)));
        };

        return Rgb { shaped (red, ledRedTrim), shaped (green, ledGreenTrim), shaped (blue, ledBlueTrim) };
    }

    Rgb colourLevels (Rgb colour) noexcept
    {
        const auto levelOf = [] (int component) { return std::clamp (component, 0, 127) >> 4; };
        return Rgb { levelOf (colour.red), levelOf (colour.green), levelOf (colour.blue) };
    }

    //==========================================================================
    struct SurfaceBridge::State
    {
        /*  WHAT A STRIP'S HAND ASKED FOR THIS TICK, folded into one write: a
            fader's position or a gate's reset replaces it, an encoder's detents
            add to it. */
        struct Write
        {
            enum class Kind { none, absolute, relative };

            Kind kind = Kind::none;
            double db = 0.0;        // absolute
            int steps = 0;          // relative: detents, `encoderStepDb` each
        };

        /*  One display field, by the text it was last written FROM - not the
            fitted text, so a long name is compared rather than folded and cut
            again on every tick. */
        struct Row
        {
            bool known = false;
            std::string source;
        };

        struct Strip
        {
            std::string id;

            //  Where the tree says what the strip is doing: fixed by its id.
            std::string targetAt, wordAt, roleAt, cueAt, dcaAt, holderAt;

            //  Addresses that follow what is on the strip, made again only when
            //  that changes - at a handover, or when the show edits the strip.
            std::string cueId, cueNameAt, cueShortAt, cueNumberAt, cueColourAt, cuePressureAt,
                        cueFloorAt, cueDurationAt, cueStartAt;
            std::string dcaId, dcaNameAt, dcaShortAt;
            std::string holderId, timbreAt, envelopeAt, meterAt, soloAt, positionAt;

            /*  THE PULSE'S OWN MEMORY (2026-09-25): the slow average of the
                holder's envelope and the brightness being let go, for the run
                they were measured on - a new holder starts again. */
            std::string pulseFor;
            double pulseLevel = 0.0;
            double pulseAverage = 0.0;
            double pulseSpread = 0.0;
            double pulseShown = 0.0;

            /*  THE METER'S (2026-09-25): the loudest the holder sent since the
                last message, the step last sent (-1 for nobody knows), when,
                and the run its peak hold was last cleared for. */
            double meterPeak = meterSilentDb;
            int meterStep = -1;
            std::int64_t meterSentAt = std::numeric_limits<std::int64_t>::min() / 2;
            std::string meterFor;

            //  The hand.
            bool handDown = false;          // the fader's touch sense
            std::string touched;            // the address this surface's node.touch holds for it
            bool gateDown = false;          // a V-Pot press held on a sampler strip
            int padOrder = 0;               // midiPads: when this pad went down; 0 while it is up
            Write pending;

            //  What the surface was last told, or put there itself.
            int motor = -1;                 // where the fader is, as far as anybody knows; -1 for nobody
            std::int64_t reassertAt = -1;   // when a let-go fader is next sent its level again
            int reassertsLeft = 0;
            bool held = false;              // held by this surface at the last afterTick
            std::array<Row, 3> rows;
            int led = -1;
            int muteLed = -1;                       // the MUTE light as last sent; -1 for nobody knows
            std::int64_t muteLitUntil = -1;         // the tick the kill's flash goes out
            int soloLed = -1;                       // the SOLO light as last sent
            int recLed = -1;                        // the REC light as last sent
            std::int64_t recLitUntil = -1;          // the tick the starting level's flash goes out
            int ring = -1;
            int ringMode = -1;                      // the fill the ring was last sent in: a page changes it
            bool colourKnown = false;
            Rgb colourLevel;
            std::int64_t colourTick = std::numeric_limits<std::int64_t>::min() / 2;

            /*  ON AN EQ OR SEND PAGE (2026-09-25): what the hand did this tick,
                folded - detents add, a press is a press - and the control under
                this rotary, made when the page, its index or the aim changes. */
            int pageSteps = 0;
            bool pagePress = false;
            int control = -1;                       // which of the page's controls; -1 for none
            std::string controlAt;                  // its row on the aimed cue; on a Send page, empty for no send yet
            std::string switchAt;                   // the switch that takes its band, or its send, out
            std::string busId;                      // a Send page: the mix channel under the rotary
            std::string busNameAt;
            std::string paramAt;                    // an FX page: /godot/plugin/<id>/param/<n>/, its name, steps, default
            std::string fxTextAt;                   // an FX page: /godot/fx/<id>/t<n>, the plugin's own words for it
        };

        /*  One port of a surface: a bank of eight. */
        struct Bank
        {
            std::string port;
            bool numbersKnown = false;
            std::array<int, stripsPerBank> numbers {};

            //  The page buttons' lights on this port, as last sent; -1 for nobody knows.
            int eqLed = -1;
            int sendLed = -1;
            int fxLed = -1;
        };

        /*  WHAT A SURFACE'S ROTARIES SHOW (author, 2026-09-25), and where the
            controls of that page live - carried whole through a show edit, as
            the strips are, since every unlocked turn is one. */
        struct Paging
        {
            Page page = Page::show;
            int index = 0;
            int count = 1;
            std::size_t bank = 0;               // where its button was pressed, and where its light answers
            std::string edited;                 // what the page last wrote

            //  What the controls' addresses were made for.
            std::string madeForAim;
            Page madeForPage = Page::show;
            int madeForIndex = -1;
            std::string madeForSends, madeForMixes, madeForFx;

            /*  AN FX PAGE (2026-09-26): which insert is up, as the first
                rotary's third row says it - "Verb 1/3" - its place in the
                chain for its colour, and whether the cue has none in at all. */
            std::string fxTitle;
            int fxInsert = 0;
            bool fxNone = false;

            //  The aimed cue's rows every rotary of the page reads.
            std::string eqOnAt, aimShortAt, aimNameAt, aimColourAt, aimSendsAt;
        };

        struct Surface
        {
            std::string id;
            std::string origin;             // "surface:" + id, made once
            std::string word;               // the profile as the show spelled it
            std::optional<Profile> profile;
            Topology topology;
            bool enabled = true;
            int channel = 0;
            int firstNote = 36;
            std::vector<Bank> banks;
            std::vector<Strip> strips;

            bool connected = false;
            bool repaint = false;           // ask who it is, and paint everything, at the next afterTick
            std::string problem;
            std::string serial;

            std::optional<std::int64_t> lastStop;
            int padCounter = 0;

            Paging paging;

            bool isMackie() const noexcept
            {
                return profile == Profile::mcu || profile == Profile::d700;
            }

            /*  How many rotaries a page has: every strip a declared port
                carries - a D700 with one of its two ports named has eight. */
            int rotaries() const noexcept
            {
                return static_cast<int> (std::min (strips.size(), banks.size() * stripsPerBank));
            }
        };

        /*  A port a surface has claimed, and whether what arrives on it is
            heard - which it is while the surface is connected. */
        struct Owned
        {
            std::string port;
            std::size_t surface = 0;
            std::size_t bank = 0;
            bool hears = false;
        };

        struct Inbound
        {
            std::string port;
            midi::Bytes bytes;
        };

        using Submit = SurfaceBridge::Submit;

        State (midi::MidiSink& sinkToUse, SurfaceTable& tableToUse)
            : sink (sinkToUse), table (tableToUse)
        {
            levelScratch.reserve (32);
            labelScratch.reserve (32);
            pageScratch.reserve (32);
            colourScratch.reserve (3);
        }

        //======================================================================
        static Strip stripFor (const std::string& stripId)
        {
            Strip strip;
            strip.id = stripId;

            const auto base = "/godot/slot/" + stripId + "/";
            strip.targetAt = base + "target";
            strip.wordAt = base + "word";
            strip.roleAt = base + "role";
            strip.cueAt = base + "cue";
            strip.dcaAt = base + "dca";
            strip.holderAt = base + "holder";
            return strip;
        }

        /*  THE SAME SURFACE, DRAWN THE SAME WAY: whether a new declaration can
            keep what the old one knew about the hardware. */
        static bool sameShape (const Surface& before, const Surface& after)
        {
            if (before.word != after.word || before.enabled != after.enabled
                || before.channel != after.channel || before.firstNote != after.firstNote
                || before.banks.size() != after.banks.size()
                || before.strips.size() != after.strips.size())
                return false;

            for (std::size_t i = 0; i < before.banks.size(); ++i)
                if (before.banks[i].port != after.banks[i].port)
                    return false;

            for (std::size_t i = 0; i < before.strips.size(); ++i)
                if (before.strips[i].id != after.strips[i].id)
                    return false;

            return true;
        }

        /*  A surface that has gone - unplugged, switched off, edited out of
            the show - lets go of what its hands held, because it cannot any
            more: a held pad would sound for ever and a touched node stay gated
            against everybody (PRD §3.16). Queued for the next beforeTick. */
        void owe (Surface& box)
        {
            auto touchedAny = false;

            for (auto& strip : box.strips)
            {
                if (strip.gateDown || strip.padOrder > 0)
                    owed.push_back (commandFrom (box.origin, "strip.release",
                                                 { osc::Value::string (strip.id) }));

                touchedAny = touchedAny || strip.handDown || ! strip.touched.empty();

                strip.gateDown = false;
                strip.padOrder = 0;
                strip.handDown = false;
                strip.touched.clear();
                strip.pending = Write {};
            }

            if (touchedAny)
                owed.push_back (commandFrom (box.origin, "node.releaseAll"));
        }

        /*  Everything the surface was shown is unknown again, so the next
            afterTick sends all of it. The colour keeps the tick it was last
            written at, so a repaint does not break the rate. */
        static void forgetShown (Strip& strip)
        {
            strip.motor = -1;
            strip.held = false;

            for (auto& row : strip.rows)
                row.known = false;

            strip.led = -1;
            strip.muteLed = -1;
            strip.ring = -1;
            strip.ringMode = -1;
            strip.colourKnown = false;
            strip.meterStep = -1;
            strip.meterFor.clear();
            strip.soloLed = -1;
            strip.recLed = -1;
        }

        /*  WHAT A PAGE CHANGES, forgotten when the page changes: the screen,
            the ring and the colour. Never the motor - a page is the rotaries'
            and the faders stay where they are; forgotten, a fader would be
            flown again and twitch. */
        static void forgetPage (Strip& strip)
        {
            for (auto& row : strip.rows)
                row.known = false;

            strip.ring = -1;
            strip.ringMode = -1;
            strip.colourKnown = false;
        }

        //======================================================================
        //  The EQ and Send pages (author, 2026-09-25).

        /*  The cue the rotaries are aimed at, as the tree last said. */
        const std::string& aimNow() const
        {
            return textAt (published.get(), aimAddress());
        }

        /*  WHAT A CLIENT SEES OF THE PAGE: its word, which, how many, and what
            it last wrote - read by the tree's runtime half at every publish. */
        void publishPage (const Surface& box)
        {
            SurfaceTable::Page page;
            page.word = std::string (pageWord (box.paging.page));
            page.index = box.paging.index;
            page.count = box.paging.count;
            page.edited = box.paging.edited;
            table.setPage (box.id, page);
        }

        /*  THE FX PAGES (2026-09-26, the author's decisions): the aimed cue's
            switched-in inserts in chain order - its `fx` row - each as many
            pages of its parameters, in the plugin's own order, as the rotaries
            need; one at least, so an insert whose plugin nobody has catalogued
            still says its name. An insert switched out is not walked: a turn
            must never switch a plugin in. */
        struct FxPage
        {
            std::string fxId, pluginId, name;
            int first = 0;          // the first parameter under the first rotary
            int count = 0;          // how many of the rotaries carry one
            int page = 0;           // which of this insert's pages
            int pages = 1;
            int insert = 0;         // its place among the cue's inserts
        };

        std::vector<FxPage> fxPagesOf (const std::string& aim, int rotaries) const
        {
            std::vector<FxPage> out;

            if (aim.empty() || rotaries <= 0)
                return out;

            const auto* at = published.get();
            auto insert = 0;

            for (const auto& fxId : wordsOf (textAt (at, "/godot/cue/" + aim + "/fx")))
            {
                const auto pluginId = textAt (at, "/godot/fx/" + fxId + "/plugin");
                const auto params = static_cast<int> (numberAt (at, "/godot/plugin/" + pluginId + "/paramCount").value_or (0.0));
                const auto pages = pageCount (params, rotaries);

                for (int page = 0; page < pages; ++page)
                    out.push_back ({ fxId, pluginId, textAt (at, "/godot/fx/" + fxId + "/name"),
                                     page * rotaries, std::clamp (params - page * rotaries, 0, rotaries),
                                     page, pages, insert });

                ++insert;
            }

            return out;
        }

        /*  What an FX page was made from: which inserts, and how many
            parameters each - an insert switched in or out, or a catalogue
            arriving, makes it again. */
        std::string fxKeyOf (const std::vector<FxPage>& pages) const
        {
            std::string key;

            for (const auto& page : pages)
                key += page.fxId + ':' + std::to_string (page.first + page.count) + ' ';

            return key;
        }

        /*  THE CONTROLS UNDER THE ROTARIES, made again when the page, its
            index or the aim changes - and every strip's screen, ring and
            colour forgotten then, so the next paint shows the new ones. */
        void composePage (Surface& box, const std::string& aim) const
        {
            auto& paging = box.paging;
            const auto* at = published.get();
            const auto base = aim.empty() ? std::string {} : "/godot/cue/" + aim + "/";

            /*  A SEND PAGE IS ALSO MADE AGAIN when the show's mix channels or
                the cue's sends change - a send made by the page's own turn, a
                bus added - since which send is under which rotary moved. */
            const auto& mixes = paging.page == Page::send ? textAt (at, mixesAddress()) : noText();
            const auto sends = paging.page == Page::send && ! base.empty()
                                 ? textAt (at, base + "sends") : std::string {};

            /*  AND AN FX PAGE when the cue's inserts or their catalogues move. */
            const auto fxPages = paging.page == Page::fx ? fxPagesOf (aim, box.rotaries()) : std::vector<FxPage> {};
            const auto fxKey = fxKeyOf (fxPages);

            if (paging.madeForAim == aim && paging.madeForPage == paging.page
                  && paging.madeForIndex == paging.index
                  && paging.madeForMixes == mixes && paging.madeForSends == sends
                  && paging.madeForFx == fxKey)
                return;

            const auto sameControls = paging.madeForAim == aim && paging.madeForPage == paging.page
                                        && paging.madeForIndex == paging.index && paging.madeForFx == fxKey;

            paging.madeForAim = aim;
            paging.madeForPage = paging.page;
            paging.madeForIndex = paging.index;
            paging.madeForMixes = mixes;
            paging.madeForSends = sends;
            paging.madeForFx = fxKey;
            paging.fxTitle.clear();
            paging.fxInsert = 0;
            paging.fxNone = false;

            paging.eqOnAt = base.empty() ? std::string {} : base + "eqOn";
            paging.aimShortAt = base.empty() ? std::string {} : base + "shortName";
            paging.aimNameAt = base.empty() ? std::string {} : base + "name";
            paging.aimColourAt = base.empty() ? std::string {} : base + "colour";
            paging.aimSendsAt = base.empty() ? std::string {} : base + "sends";

            for (auto& strip : box.strips)
            {
                strip.control = -1;
                strip.controlAt.clear();
                strip.switchAt.clear();
                strip.busId.clear();
                strip.busNameAt.clear();
                strip.paramAt.clear();
                strip.fxTextAt.clear();

                /*  A hand's detents stay when only the sends moved under it -
                    the send its own turn made - and the screens are drawn
                    again from what changed. */
                if (! sameControls)
                {
                    strip.pageSteps = 0;
                    strip.pagePress = false;
                }

                forgetPage (strip);
            }

            if (base.empty())
                return;

            const auto rotaries = box.rotaries();

            if (paging.page == Page::send)
            {
                //  Which of the cue's sends goes into which mix channel.
                std::map<std::string, std::string> sendInto;

                for (const auto& sendId : wordsOf (sends))
                {
                    const auto busAt = "/godot/send/" + sendId + "/bus";
                    sendInto[textAt (at, busAt)] = sendId;
                }

                const auto channels = wordsOf (mixes);

                for (int position = 0; position < rotaries; ++position)
                {
                    const auto index = paging.index * rotaries + position;

                    if (index >= static_cast<int> (channels.size()))
                        break;

                    auto& strip = box.strips[static_cast<std::size_t> (position)];
                    strip.control = index;
                    strip.busId = channels[static_cast<std::size_t> (index)];
                    strip.busNameAt = "/godot/bus/" + strip.busId + "/name";

                    if (const auto found = sendInto.find (strip.busId); found != sendInto.end())
                    {
                        strip.controlAt = "/godot/send/" + found->second + "/level";
                        strip.switchAt = "/godot/send/" + found->second + "/on";
                    }
                }

                return;
            }

            if (paging.page == Page::fx)
            {
                /*  THE INSERT AND THE PARAMETERS UNDER THIS PAGE: the rotaries
                    left to right are the plugin's parameters in its own order,
                    from where this page of it starts. */
                if (fxPages.empty())
                {
                    paging.fxNone = true;
                    return;
                }

                const auto& page = fxPages[static_cast<std::size_t> (std::clamp (paging.index, 0,
                                                                              static_cast<int> (fxPages.size()) - 1))];
                paging.fxInsert = page.insert;
                paging.fxTitle = page.name.empty() ? page.pluginId : page.name;

                if (page.pages > 1)
                    paging.fxTitle += " " + std::to_string (page.page + 1) + "/" + std::to_string (page.pages);

                for (int position = 0; position < page.count && position < rotaries; ++position)
                {
                    const auto parameter = page.first + position;
                    auto& strip = box.strips[static_cast<std::size_t> (position)];
                    strip.control = parameter;
                    strip.controlAt = "/godot/fx/" + page.fxId + "/p" + std::to_string (parameter);
                    strip.fxTextAt = "/godot/fx/" + page.fxId + "/t" + std::to_string (parameter);
                    strip.paramAt = "/godot/plugin/" + page.pluginId + "/param/" + std::to_string (parameter) + "/";
                }

                return;
            }

            if (paging.page != Page::eq)
                return;

            for (int position = 0; position < rotaries; ++position)
            {
                const auto index = paging.index * rotaries + position;

                if (index >= eqControlCount)
                    break;

                const auto& control = eqControls[static_cast<std::size_t> (index)];
                auto& strip = box.strips[static_cast<std::size_t> (position)];
                strip.control = index;
                strip.controlAt = base + std::string (control.row);
                strip.switchAt = base + std::string (control.switchRow);
            }
        }

        /*  How many controls a kind of page has: the EQ's sixteen, or one a
            mix channel of the show. */
        int controlsOf (Page page) const
        {
            if (page == Page::eq)
                return eqControlCount;

            if (page == Page::send)
                return static_cast<int> (wordsOf (textAt (published.get(), mixesAddress())).size());

            return 0;
        }

        /*  A PAGE SHOWN: which kind, which of them, and the bank whose button
            asked. Anything it had written is forgotten, so a client reading
            `edited` sees only what this page did. */
        void showPage (Surface& box, Page page, int index, std::size_t bank)
        {
            auto& paging = box.paging;
            paging.page = page;
            paging.index = page == Page::show ? 0 : index;
            paging.bank = bank;
            paging.edited.clear();
            paging.count = page == Page::show ? 1
                         : page == Page::fx ? std::max (1, static_cast<int> (fxPagesOf (aimNow(), box.rotaries()).size()))
                                            : pageCount (controlsOf (page), box.rotaries());

            composePage (box, page == Page::show ? std::string {} : aimNow());
            publishPage (box);
        }

        /*  EQ OR SEND PRESSED: that kind's first page, or its next, or - after
            its last - the surface's own page again ("revert to normal mode
            once all sends have been displayed"). With nothing aimed at there
            is nothing to show, and the press does nothing. */
        void turnPage (Surface& box, Page kind, std::size_t bank)
        {
            if (aimNow().empty())
                return;

            /*  AN FX PAGE IS SHOWN EVEN FOR A CUE WITH NO INSERT IN (2026-09-26)
                - "no FX" on its first rotary is an answer, where a press that
                did nothing would be a question. */
            if (kind == Page::fx)
            {
                const auto count = std::max (1, static_cast<int> (fxPagesOf (aimNow(), box.rotaries()).size()));

                if (box.paging.page != kind)
                    showPage (box, kind, 0, bank);
                else if (box.paging.index + 1 < count)
                    showPage (box, kind, box.paging.index + 1, bank);
                else
                    showPage (box, Page::show, 0, bank);

                return;
            }

            //  A show with no mix channel has no Send page to show.
            if (controlsOf (kind) == 0)
                return;

            const auto count = pageCount (controlsOf (kind), box.rotaries());

            if (box.paging.page != kind)
                showPage (box, kind, 0, bank);
            else if (box.paging.index + 1 < count)
                showPage (box, kind, box.paging.index + 1, bank);
            else
                showPage (box, Page::show, 0, bank);
        }

        /*  SELECT AIMS THE ROTARIES at the cue on the strip, and a lit one
            lets go (author, 2026-09-25). A strip with no cue - free, or a
            dca strip - has nothing to aim at. */
        void aim (const Surface& box, Strip& strip, const Submit& submit)
        {
            follow (strip);

            if (strip.cueId.empty())
                return;

            const auto again = strip.cueId == aimNow();
            submit (commandFrom (box.origin, "surface.aim",
                                 { osc::Value::string (again ? std::string {} : strip.cueId) }));
        }

        //======================================================================
        /*  The addresses that depend on what is on the strip, made again when
            that changes. */
        void follow (Strip& strip) const
        {
            const auto* at = published.get();

            const auto under = [] (const std::string& base, const char* leaf)
            {
                return base.empty() ? std::string {} : base + leaf;
            };

            if (const auto& onStrip = textAt (at, strip.cueAt); onStrip != strip.cueId)
            {
                strip.cueId = onStrip;
                const auto base = onStrip.empty() ? std::string {} : "/godot/cue/" + onStrip + "/";
                strip.cueNameAt = under (base, "name");
                strip.cueShortAt = under (base, "shortName");
                strip.cueNumberAt = under (base, "number");
                strip.cueColourAt = under (base, "colour");
                strip.cuePressureAt = under (base, "pressure");
                strip.cueFloorAt = under (base, "velocityFloor");
                strip.cueDurationAt = under (base, "duration");
                strip.cueStartAt = under (base, "startOffset");
            }

            if (const auto& marked = textAt (at, strip.dcaAt); marked != strip.dcaId)
            {
                strip.dcaId = marked;
                const auto base = marked.empty() ? std::string {} : "/godot/dca/" + marked + "/";
                strip.dcaNameAt = under (base, "name");
                strip.dcaShortAt = under (base, "shortName");
            }

            if (const auto& holder = textAt (at, strip.holderAt); holder != strip.holderId)
            {
                strip.holderId = holder;
                strip.timbreAt = holder.empty() ? std::string {} : "/godot/run/" + holder + "/timbre";
                strip.envelopeAt = holder.empty() ? std::string {} : "/godot/run/" + holder + "/envelope";
                strip.meterAt = holder.empty() ? std::string {} : "/godot/run/" + holder + "/meter";
                strip.soloAt = holder.empty() ? std::string {} : "/godot/run/" + holder + "/solo";
                strip.positionAt = holder.empty() ? std::string {} : "/godot/run/" + holder + "/position";
            }
        }

        const std::string& targetOf (const Strip& strip) const
        {
            return textAt (published.get(), strip.targetAt);
        }

        static Strip* stripAt (Surface& box, std::size_t bank, int element)
        {
            if (element < 0 || element >= static_cast<int> (stripsPerBank))
                return nullptr;

            const auto index = bank * stripsPerBank + static_cast<std::size_t> (element);
            return index < box.strips.size() ? &box.strips[index] : nullptr;
        }

        static Strip* padFor (Surface& box, int note)
        {
            const auto index = note - box.firstNote;

            if (index < 0 || static_cast<std::size_t> (index) >= box.strips.size())
                return nullptr;

            return &box.strips[static_cast<std::size_t> (index)];
        }

        /*  The pad pressed most recently and still down, which is where a
            channel's pressure goes: channel pressure names no note. */
        static Strip* latestPad (Surface& box)
        {
            Strip* latest = nullptr;

            for (auto& strip : box.strips)
                if (strip.padOrder > 0 && (latest == nullptr || strip.padOrder > latest->padOrder))
                    latest = &strip;

            return latest;
        }

        //======================================================================
        //  Inbound, on the tick thread.

        /*  A HAND STILL DOWN ON A STRIP WHOSE TARGET MOVED UNDER IT - a
            handover while the fader was held - moves its touch with it: the
            node the fader rides now is the one the touch table says this
            surface holds, the old one is given back rather than held for
            ever, and the release, when it comes, lands on the right node. */
        void followTouches (const Submit& submit)
        {
            for (auto& box : surfaces)
            {
                if (! box.connected)
                    continue;

                for (auto& strip : box.strips)
                {
                    /*  A HAND RESTING THROUGH A HANDOVER LETS GO OF THE NODE IT
                        HELD, and holds nothing until it lands again. A touch
                        starts a sampler clip (author, 2026-09-23), so a hand
                        that did not move must not start the clip that has just
                        arrived under it - a member armed again after its clip
                        ended, a bank taken over. The fader stays still under
                        it all the same, since `handDown` holds the motor, and
                        what it moves goes to the new node: a ride with no
                        touch, which starts nothing. */
                    if (! strip.handDown || strip.touched.empty() || targetOf (strip) == strip.touched)
                        continue;

                    submit (commandFrom (box.origin, "node.release", { osc::Value::string (strip.touched) }));
                    strip.touched.clear();
                }
            }
        }

        void handle (const Inbound& in, const Submit& submit, std::int64_t tick)
        {
            /*  READ WITHOUT THE LOCK, and safely: the tick thread is the only
                one that writes `owners`, and this is the tick thread. */
            const auto owner = std::find_if (owners.begin(), owners.end(),
                                             [&in] (const Owned& claim) { return claim.port == in.port; });

            if (owner == owners.end() || owner->surface >= surfaces.size())
                return;

            auto& box = surfaces[owner->surface];

            /*  Asked again here, because the declaration may have changed
                between the push and the drain. */
            if (! box.connected || ! box.profile.has_value())
                return;

            switch (*box.profile)
            {
                case Profile::mcu:
                case Profile::d700:
                    if (const auto event = decodeMcu (in.bytes))
                        mackie (box, owner->bank, *event, submit, tick);
                    break;

                case Profile::midiPads:
                    if (const auto event = decodePads (in.bytes))
                        pads (box, *event, submit);
                    break;

                case Profile::virtualPanel:
                    break;
            }
        }

        void mackie (Surface& box, std::size_t bank, const McuEvent& event, const Submit& submit,
                     std::int64_t tick)
        {
            switch (event.kind)
            {
                case McuEvent::Kind::fader:
                    if (auto* strip = stripAt (box, bank, event.strip))
                    {
                        /*  ONLY A HAND WRITES A LEVEL (author, 2026-09-25: "The
                            rec is using the wrong fader curve. Each press
                            lowers the level from where it was recorded"). A
                            position sent while nobody touches the fader is the
                            motor saying where it has got to - the D700 says so
                            on its way up to a start level - and taken as a
                            level it pulled each recalled start down to wherever
                            the motor was: a clip armed at -8.75 dB was written
                            -11.08, and REC kept that. The bridge's own motor
                            reckoning stands. A surface with no touch sense has
                            only hands to send positions, so every one counts. */
                        if (box.topology.hasTouch && ! strip->handDown)
                        {
                            /*  BUT STILL WHERE THE FADER IS: a report well away
                                from where it was sent means the surface moved it
                                on its own - a D700 puts a released fader back
                                where the host last put it - and believed, the
                                motor is sent back where the engine says. Close
                                to it is the motor settling, and left alone. */
                            if (strip->motor < 0 || std::abs (event.value - strip->motor) > motorSlack)
                                strip->motor = event.value;

                            break;
                        }

                        /*  WHERE THE HAND PUT IT is where the fader is: the
                            motor is not sent there again when the engine
                            answers with the same value (the echo, §16.6). */
                        strip->motor = event.value;
                        strip->pending.kind = Write::Kind::absolute;
                        strip->pending.db = dbForFourteenBit (event.value, box.topology.faderLaw);
                        strip->pending.steps = 0;
                    }
                    break;

                case McuEvent::Kind::touch:
                    if (auto* strip = stripAt (box, bank, event.strip))
                        touch (box, *strip, event.down, submit);
                    break;

                /*  ON THE SURFACE'S OWN PAGE A TURN MOVES NOTHING (author,
                    2026-09-25: "The rotaries don't have to move with the
                    faders. It's either or. We'll find other uses for the
                    rotaries."). A strip's level is its fader's.

                    ON AN EQ OR SEND PAGE it turns the control under it - the
                    use the author found the same day. The detents are folded
                    and written once a tick (flushPageWrites). */
                case McuEvent::Kind::encoder:
                    if (box.paging.page != Page::show)
                        if (auto* strip = stripAt (box, bank, event.strip))
                            strip->pageSteps += event.value;
                    break;

                case McuEvent::Kind::button:
                    button (box, bank, event, submit, tick);
                    break;

                case McuEvent::Kind::hostConnectionQuery:
                    identify (box, bank, event.serial);
                    break;

                /*  The D700's volume knob, the master touch, the jog wheel:
                    nothing in Go.dot is under them yet. And no reply to the
                    handshake is ever sent (plan decision 6), so its answers do
                    not come. */
                case McuEvent::Kind::masterFader:
                case McuEvent::Kind::masterTouch:
                case McuEvent::Kind::jog:
                case McuEvent::Kind::hostConnectionConfirmation:
                case McuEvent::Kind::hostConnectionError:
                    break;
            }
        }

        void touch (const Surface& box, Strip& strip, bool down, const Submit& submit)
        {
            if (down)
            {
                if (strip.handDown)
                    return;

                strip.handDown = true;

                if (const auto& target = targetOf (strip); ! target.empty())
                {
                    submit (commandFrom (box.origin, "node.touch", { osc::Value::string (target) }));
                    strip.touched = target;
                }

                return;
            }

            if (! strip.handDown)
                return;

            strip.handDown = false;

            /*  What was TOUCHED is what is released, which is not always the
                target now: a handover may have moved it (followTouches). */
            if (! strip.touched.empty())
            {
                submit (commandFrom (box.origin, "node.release", { osc::Value::string (strip.touched) }));
                strip.touched.clear();
            }
        }

        void button (Surface& box, std::size_t bank, const McuEvent& event, const Submit& submit,
                     std::int64_t tick)
        {
            switch (actionFor (*box.profile, event.id))
            {
                case Action::gate:
                    if (auto* strip = stripAt (box, bank, event.id.index))
                    {
                        /*  ON A PAGE THE PRESS IS THE CONTROL'S - a band in or
                            out - and never a clip. A gate held down from the
                            surface's own page is still let go. */
                        if (box.paging.page != Page::show && (event.down || ! strip->gateDown))
                        {
                            strip->pagePress = strip->pagePress || event.down;
                            break;
                        }

                        gate (box, *strip, event.down, submit);
                    }
                    break;

                case Action::aim:
                    if (event.down)
                        if (auto* strip = stripAt (box, bank, event.id.index))
                            aim (box, *strip, submit);
                    break;

                case Action::eqPage:
                    if (event.down)
                        turnPage (box, Page::eq, bank);
                    break;

                case Action::sendPage:
                    if (event.down)
                        turnPage (box, Page::send, bank);
                    break;

                case Action::fxPage:
                    if (event.down)
                        turnPage (box, Page::fx, bank);
                    break;

                case Action::leavePage:
                    if (event.down && box.paging.page != Page::show)
                        showPage (box, Page::show, 0, box.paging.bank);
                    break;

                case Action::go:
                    if (event.down)
                        submit (commandFrom (box.origin, "go"));
                    break;

                case Action::stop:
                    if (event.down)
                    {
                        /*  ESC, AND ESC AGAIN (PRD §4.4): the second STOP inside
                            the window is the immediate level. Each reading is
                            one named command, so the log says which level was
                            reached and a replay reaches the same one. */
                        const auto again = box.lastStop.has_value() && tick >= *box.lastStop
                                             && tick - *box.lastStop <= doubleStopTicks;
                        box.lastStop = tick;
                        submit (commandFrom (box.origin, again ? "run.killAll" : "run.stopAll"));
                    }
                    break;

                case Action::kill:
                    if (event.down)
                        if (auto* strip = stripAt (box, bank, event.id.index))
                            kill (box, *strip, submit, tick);
                    break;

                case Action::solo:
                    if (event.down)
                        if (auto* strip = stripAt (box, bank, event.id.index))
                            solo (box, *strip, submit);
                    break;

                case Action::startLevel:
                    if (event.down)
                        if (auto* strip = stripAt (box, bank, event.id.index))
                            startLevel (box, *strip, submit, tick);
                    break;

                case Action::rewind:
                    if (event.down)
                        submit (commandFrom (box.origin, "standby.previous"));
                    break;

                case Action::forward:
                    if (event.down)
                        submit (commandFrom (box.origin, "standby.next"));
                    break;

                case Action::none:
                    break;
            }
        }

        /*  MUTE KILLS, and does not mute (author, 2026-09-25): `run.kill` on
            the run holding the strip - what the cross in the running pane
            sends - while something sounds there. The member is armed on its
            fader again for the next touch, as after any end. A dca strip, a
            free one and a member armed and waiting have nothing to kill. */
        void kill (const Surface& box, Strip& strip, const Submit& submit, std::int64_t tick) const
        {
            const auto* at = published.get();

            if (strip.holderId.empty() || textAt (at, strip.roleAt) == "dca")
                return;

            const auto& word = textAt (at, strip.wordAt);

            if (word != "playing" && word != "held" && word != "stopping" && word != "closing")
                return;

            submit (commandFrom (box.origin, "run.kill", { osc::Value::string (strip.holderId) }));

            //  And the red light says it happened, for `killFlashTicks`.
            strip.muteLitUntil = tick + killFlashTicks;
        }

        /*  SOLO LOCKS THE BANK TO THE STRIP (author, 2026-09-25): `run.solo`
            on the run holding it, as a toggle - the engine says what it came
            to, and lets go of it when the clip stops. A dca strip and a free
            one have nothing to solo. */
        void solo (const Surface& box, const Strip& strip, const Submit& submit) const
        {
            if (strip.holderId.empty() || textAt (published.get(), strip.roleAt) == "dca")
                return;

            submit (commandFrom (box.origin, "run.solo", { osc::Value::string (strip.holderId) }));
        }

        /*  REC SETS WHERE THE FADER STARTS (author, 2026-09-25: "Pressing Rec
            on a sampler fader sets the starting level. Confirm with a LED
            pulse."): the level the strip's fader is at, written as its
            member's `initialLevel`. An edit to the show, so a locked show
            takes none, and then nothing is confirmed. */
        void startLevel (const Surface& box, Strip& strip, const Submit& submit, std::int64_t tick) const
        {
            const auto* at = published.get();

            if (strip.cueId.empty() || textAt (at, strip.roleAt) == "dca"
                  || flagAt (at, "/godot/document/locked"))
                return;

            const auto& target = textAt (at, strip.targetAt);
            const auto level = target.empty() ? std::optional<double> {} : numberAt (at, target);

            if (! level.has_value())
                return;

            submit (commandFrom (box.origin, "node.set",
                                 { osc::Value::string ("/godot/cue/" + strip.cueId + "/initialLevel"),
                                   osc::Value::float64 (std::clamp (*level, -120.0, 12.0)) }));

            //  And its light says it happened, for `startLevelFlashTicks`.
            strip.recLitUntil = tick + startLevelFlashTicks;
        }

        /*  THE STRIP'S GATE, the V-Pot press: a hand on a sampler strip, or on
            a dca strip the DCA back to nought (plan decision 12). */
        void gate (const Surface& box, Strip& strip, bool down, const Submit& submit) const
        {
            if (! down)
            {
                if (strip.gateDown)
                {
                    strip.gateDown = false;
                    submit (commandFrom (box.origin, "strip.release", { osc::Value::string (strip.id) }));
                }

                return;
            }

            if (textAt (published.get(), strip.roleAt) == "dca")
            {
                /*  A WRITE, and folded with the fader's: a fader moved after
                    the reset in the same tick is the later word. */
                if (! targetOf (strip).empty())
                {
                    strip.pending.kind = Write::Kind::absolute;
                    strip.pending.db = 0.0;
                    strip.pending.steps = 0;
                }

                return;
            }

            if (strip.gateDown)
                return;

            strip.gateDown = true;
            submit (commandFrom (box.origin, "strip.press", { osc::Value::string (strip.id) }));
        }

        /*  THE SERIAL THE HANDSHAKE CARRIES (plan decision 6: read, never
            answered). The first bank's is the surface's; another bank's is
            kept only until the first bank has spoken. */
        void identify (Surface& box, std::size_t bank, const std::string& raw)
        {
            auto said = printable (raw);

            if (said.empty() || said == box.serial || (bank != 0 && ! box.serial.empty()))
                return;

            box.serial = std::move (said);

            SurfaceTable::Status status;
            status.connected = box.connected;
            status.problem = box.problem;
            status.serial = box.serial;
            tableMoved = table.set (box.id, status) || tableMoved;
        }

        void pads (Surface& box, const PadEvent& event, const Submit& submit)
        {
            if (box.channel != 0 && event.channel != box.channel)
                return;

            switch (event.kind)
            {
                case PadEvent::Kind::noteOn:
                case PadEvent::Kind::noteOff:
                {
                    auto* strip = padFor (box, event.note);

                    if (strip == nullptr)
                        break;

                    /*  A NOTE-ON OF VELOCITY NOUGHT IS A RELEASE, as a note-off
                        is: the codec reports what the wire said, and this is
                        where it is read (McuCodec.h). */
                    if (event.kind == PadEvent::Kind::noteOn && event.value > 0)
                    {
                        strip->padOrder = ++box.padCounter;
                        submit (commandFrom (box.origin, "strip.press",
                                             { osc::Value::string (strip->id),
                                               osc::Value::int32 (event.value) }));
                    }
                    else if (strip->padOrder > 0)
                    {
                        strip->padOrder = 0;
                        submit (commandFrom (box.origin, "strip.release", { osc::Value::string (strip->id) }));
                    }

                    break;
                }

                case PadEvent::Kind::polyPressure:
                    if (auto* strip = padFor (box, event.note); strip != nullptr && strip->padOrder > 0)
                        press (*strip, event.value);
                    break;

                case PadEvent::Kind::channelPressure:
                    if (auto* strip = latestPad (box))
                        press (*strip, event.value);
                    break;
            }
        }

        /*  PRESSURE RIDES THE TRIM A FADER WOULD (decision AA): on velocity's
            line, through the Runner's own `levelForByte`, only while the pad is
            down and only when the member asks for it. A pressure of nought is
            ignored (plan decision 15) - most pads fall to it the instant the
            hit is over, and reading that as "to the floor" would make every
            hit a blip. */
        void press (Strip& strip, int amount)
        {
            if (amount <= 0)
                return;

            follow (strip);

            const auto* at = published.get();

            if (strip.cueId.empty() || ! flagAt (at, strip.cuePressureAt) || targetOf (strip).empty())
                return;

            const auto floorDb = numberAt (at, strip.cueFloorAt).value_or (defaultFloorDb);

            strip.pending.kind = Write::Kind::absolute;
            strip.pending.db = cue::Runner::levelForByte (amount, floorDb);
            strip.pending.steps = 0;
        }

        /*  THE TICK'S WRITES, one per strip, after every touch of the tick. */
        void flushWrites (const Submit& submit)
        {
            const auto* at = published.get();

            for (auto& box : surfaces)
                for (auto& strip : box.strips)
                {
                    if (strip.pending.kind == Write::Kind::none)
                        continue;

                    const auto asked = std::exchange (strip.pending, Write {});
                    const auto& target = textAt (at, strip.targetAt);

                    //  A strip riding nothing writes nothing.
                    if (target.empty())
                        continue;

                    auto value = asked.db;

                    if (asked.kind == Write::Kind::relative)
                    {
                        const auto current = numberAt (at, target);

                        if (! current.has_value())
                            continue;

                        value = std::clamp (*current + encoderStepDb * static_cast<double> (asked.steps),
                                            faderSilenceDb, faderLoudestDb);

                        //  Turned against an end, the value is where it was.
                        if (std::abs (value - *current) < 1.0e-9)
                            continue;
                    }

                    submit (commandFrom (box.origin, "node.set",
                                         { osc::Value::string (target), osc::Value::float64 (value) }));
                }
        }

        /*  THE TICK'S PAGE WRITES, one per rotary that moved or was pressed:
            the control under it turned by its law from what the tree last
            said, or switched. A turn against an end writes nothing. Every
            write is a `node.set` on the aimed cue's row - an edit to the show,
            one undo step per control per hand, which a locked show refuses. */
        void flushPageWrites (const Submit& submit)
        {
            const auto& aimed = aimNow();

            for (auto& box : surfaces)
            {
                if (box.paging.page == Page::show || aimed.empty())
                {
                    for (auto& strip : box.strips)
                    {
                        strip.pageSteps = 0;
                        strip.pagePress = false;
                    }

                    continue;
                }

                composePage (box, aimed);
                const auto before = box.paging.edited;

                for (auto& strip : box.strips)
                {
                    const auto steps = std::exchange (strip.pageSteps, 0);
                    const auto pressed = std::exchange (strip.pagePress, false);

                    if ((steps == 0 && ! pressed) || strip.control < 0)
                        continue;

                    if (box.paging.page == Page::eq)
                        eqWrite (box, strip, steps, pressed, submit);
                    else if (box.paging.page == Page::send)
                        sendWrite (box, strip, steps, pressed, submit);
                    else if (box.paging.page == Page::fx)
                        fxWrite (box, strip, steps, pressed, submit);
                }

                if (box.paging.edited != before)
                    publishPage (box);
            }
        }

        void eqWrite (Surface& box, const Strip& strip, int steps, bool pressed, const Submit& submit) const
        {
            const auto* at = published.get();
            const auto& control = eqControls[static_cast<std::size_t> (strip.control)];

            const auto write = [&] (const std::string& address, osc::Value value)
            {
                submit (commandFrom (box.origin, "node.set",
                                     { osc::Value::string (address), std::move (value) }));
                box.paging.edited = address;
            };

            if (pressed)
            {
                if (control.press == Press::toggleSwitch && soleAt (at, strip.switchAt) != nullptr)
                    write (strip.switchAt, osc::Value::boolean (! flagAt (at, strip.switchAt)));
                else if (control.press == Press::toggleShape && soleAt (at, strip.controlAt) != nullptr)
                    write (strip.controlAt,
                           osc::Value::string (std::string (pressedShape (textAt (at, strip.controlAt),
                                                                          control.shelf))));
            }

            if (steps == 0)
                return;

            if (control.law == Law::shape)
            {
                const auto& shape = textAt (at, strip.controlAt);
                const auto next = turnedShape (shape, steps, control.shelf);

                if (soleAt (at, strip.controlAt) != nullptr && next != shape)
                    write (strip.controlAt, osc::Value::string (std::string (next)));

                return;
            }

            const auto current = numberAt (at, strip.controlAt);

            if (! current.has_value())
                return;

            const auto next = turned (control.law, *current, steps, control.minimum, control.maximum,
                                      box.topology.faderLaw);

            //  Turned against an end, the value is where it was.
            if (std::abs (next - *current) < 1.0e-9)
                return;

            write (strip.controlAt, osc::Value::float64 (next));
        }

        /*  A SEND'S ROTARY: a turn is its level along the fader's law, a press
            its switch. A mix channel the cue does not send to yet is made one
            by the hand - `send.create` at the level a turn up from silence
            reaches, or at nought for a press, the row's default - and under
            the lock that send rides live (the author, 2026-09-25). A turn
            down from nothing makes nothing. */
        void sendWrite (Surface& box, const Strip& strip, int steps, bool pressed, const Submit& submit) const
        {
            const auto* at = published.get();

            if (strip.controlAt.empty())
            {
                if (! pressed && steps <= 0)
                    return;

                const auto level = pressed ? 0.0
                                           : turned (Law::level, faderSilenceDb, steps, faderSilenceDb,
                                                     faderLoudestDb, box.topology.faderLaw);

                submit (commandFrom (box.origin, "send.create",
                                     { osc::Value::string (box.paging.madeForAim),
                                       osc::Value::string (strip.busId),
                                       osc::Value::string (std::string {}),
                                       osc::Value::string (tenthsText (level)) }));
                box.paging.edited = box.paging.aimSendsAt;
                return;
            }

            const auto write = [&] (const std::string& address, osc::Value value)
            {
                submit (commandFrom (box.origin, "node.set",
                                     { osc::Value::string (address), std::move (value) }));
                box.paging.edited = address;
            };

            if (pressed && soleAt (at, strip.switchAt) != nullptr)
                write (strip.switchAt, osc::Value::boolean (! flagAt (at, strip.switchAt)));

            if (steps == 0)
                return;

            const auto current = numberAt (at, strip.controlAt);

            if (! current.has_value())
                return;

            const auto next = turned (Law::level, *current, steps, faderSilenceDb, faderLoudestDb,
                                      box.topology.faderLaw);

            if (std::abs (next - *current) < 1.0e-9)
                return;

            write (strip.controlAt, osc::Value::float64 (next));
        }

        /*  A PLUGIN PARAMETER'S ROTARY (2026-09-26, the author's decisions): a
            turn moves it along its travel - a step a detent for a stepped one -
            and a press puts it back where the set entry's preset leaves it,
            the catalogue's `default`. Normalised 0..1, as the insert's p<n>
            takes it, and one write a rotary a tick. */
        void fxWrite (Surface& box, const Strip& strip, int steps, bool pressed, const Submit& submit) const
        {
            const auto* at = published.get();
            const auto current = numberAt (at, strip.controlAt);

            if (! current.has_value())
                return;

            auto next = *current;

            if (pressed)
                next = std::clamp (numberAt (at, strip.paramAt + "default").value_or (next), 0.0, 1.0);

            if (steps != 0)
                next = turnedParameter (next, steps,
                                        static_cast<int> (numberAt (at, strip.paramAt + "steps").value_or (0.0)));

            //  Turned against an end, or pressed at its rest, it is where it was.
            if (std::abs (next - *current) < 1.0e-9)
                return;

            submit (commandFrom (box.origin, "node.set",
                                 { osc::Value::string (strip.controlAt), osc::Value::float64 (next) }));
            box.paging.edited = strip.controlAt;
        }

        //======================================================================
        //  Outbound, on the tick thread.

        /*  EVERY SEND COMES THROUGH HERE, and every SysEx is asked whether it
            is one of the six the D700 is known to survive before it leaves. A
            refusal is a programming error: counted, never sent. */
        void send (const std::string& port, const midi::Bytes& bytes)
        {
            if (bytes.empty())
                return;

            if (bytes.front() == sysexStart && ! isSafeSysEx (bytes))
            {
                refused.fetch_add (1, std::memory_order_relaxed);
                return;
            }

            sink.send (port, bytes);
        }

        /*  THREE MESSAGES, BLUE LAST, each its own send: a sink hands a port
            one message at a time, and `d700Colour` answers all three in one. */
        void sendColour (const std::string& port, int note, Rgb colour)
        {
            const auto three = d700Colour (note, colour.red, colour.green, colour.blue);

            if (three.size() != 9)
                return;

            for (std::size_t start = 0; start < three.size(); start += 3)
            {
                colourScratch.assign (three.begin() + static_cast<std::ptrdiff_t> (start),
                                      three.begin() + static_cast<std::ptrdiff_t> (start + 3));
                send (port, colourScratch);
            }
        }

        /*  A MOTOR NEVER CROSSES ITS WHOLE TRAVEL IN ONE MESSAGE, and is never
            clamped short of either end (control guide §4.1): each tick it moves
            at most `motorStepPerTick` towards where it should be.

            Where nobody knows where the fader is - a surface just connected,
            the firmware having left it anywhere - the first move stops a step
            short of either end, so that whatever the distance, the fader is not
            driven into its stop at full speed; the next tick takes it the rest
            of the way. */
        void moveMotor (const std::string& port, int element, Strip& strip, int desired, bool letGo)
        {
            if (strip.motor < 0)
            {
                const auto first = std::clamp (desired, motorStepPerTick, faderTop - motorStepPerTick);
                send (port, faderPosition (element, first));
                strip.motor = first;
                return;
            }

            /*  THE ECHO: the fader is already there - often because the hand
                put it there and this is the engine agreeing. The one exception
                is the tick the hand lets go, when the value is sent once
                whatever it is, so the fader ends up agreeing with the engine
                (the touch table's own rule, PRD §3.16 amended). */
            if (desired == strip.motor && ! letGo)
                return;

            const auto next = strip.motor
                            + std::clamp (desired - strip.motor, -motorStepPerTick, motorStepPerTick);

            send (port, faderPosition (element, next));
            strip.motor = next;
        }

        /*  HOW FAR THE STRIP'S CLIP HAS GOT, in `steps`: its position through
            the span it plays - from its start offset to the end of its file,
            the running pane's bar - while it sounds, and nought otherwise. A
            dca strip and a free one hold no clip. */
        int progressOf (const Strip& strip, std::string_view word, int steps) const
        {
            const auto sounding = word == "playing" || word == "held" || word == "stopping" || word == "closing";

            if (! sounding || strip.cueId.empty())
                return 0;

            const auto* at = published.get();
            const auto position = numberAt (at, strip.positionAt);
            const auto length = numberAt (at, strip.cueDurationAt);
            const auto from = numberAt (at, strip.cueStartAt).value_or (0.0);

            if (! position.has_value() || ! length.has_value() || ! (*length > from))
                return 0;

            const auto through = std::clamp ((*position - from) / (*length - from), 0.0, 1.0);
            return static_cast<int> (std::lround (through * static_cast<double> (steps)));
        }

        /*  A FLASH AS THE LIGHT IS SENT: blinked here, a quarter of a second
            each way (`blinkHalfTicks`), since the D700 lights a flash
            steadily. Every other state goes as it is. */
        static Led blinked (Led wanted, std::int64_t tick) noexcept
        {
            if (wanted != Led::flash)
                return wanted;

            return (tick / blinkHalfTicks) % 2 == 0 ? Led::on : Led::off;
        }

        static bool changed (Row& row, std::string_view source)
        {
            if (row.known && row.source == source)
                return false;

            row.known = true;
            row.source.assign (source.data(), source.size());
            return true;
        }

        void paint (Surface& box, const tree::TouchTable& touches, std::int64_t tick)
        {
            if (box.repaint)
            {
                box.repaint = false;

                /*  WHO IS THERE: a Mackie unit answers with the handshake's
                    opening, which carries its serial - the one identifier that
                    survives the operating system renumbering the ports. */
                for (auto& bank : box.banks)
                {
                    send (bank.port, deviceQuery (deviceId));
                    bank.numbersKnown = false;
                    bank.eqLed = -1;
                    bank.sendLed = -1;
                    bank.fxLed = -1;
                }

                for (auto& strip : box.strips)
                    forgetShown (strip);
            }

            /*  A PAGE WITH NOTHING TO SHOW - its cue let go of, or gone - is
                the surface's own page again. */
            const auto& aimed = aimNow();

            if (box.paging.page != Page::show && aimed.empty())
                showPage (box, Page::show, 0, box.paging.bank);

            composePage (box, box.paging.page == Page::show ? std::string {} : aimed);

            for (std::size_t bank = 0; bank < box.banks.size(); ++bank)
            {
                std::array<int, stripsPerBank> numbers {};

                for (std::size_t element = 0; element < stripsPerBank; ++element)
                {
                    const auto index = bank * stripsPerBank + element;
                    numbers[element] = static_cast<int> (index) + 1;

                    if (index < box.strips.size())
                        paintStrip (box, box.strips[index], box.banks[bank].port,
                                    static_cast<int> (element), touches, tick, numbers[element]);
                }

                /*  THE D700'S TRACK-NUMBER FIELD: all eight of a bank in one
                    message, sent when any of them changed. */
                if (box.topology.nativeDisplay)
                {
                    auto& shown = box.banks[bank];

                    if (! shown.numbersKnown || shown.numbers != numbers)
                    {
                        send (shown.port, d700TrackNumbers (numbers));
                        shown.numbers = numbers;
                        shown.numbersKnown = true;
                    }
                }

                paintPageButtons (box, bank, tick);
            }
        }

        /*  EQ AND SEND SAY WHICH PAGE IS UP, on the bank whose button asked:
            lit steadily with one page of the kind, and with more, blinked
            once, twice... every second and a half (`pageButtonLit`). Dark on
            the surface's own page. */
        void paintPageButtons (Surface& box, std::size_t bank, std::int64_t tick)
        {
            auto& shown = box.banks[bank];
            const auto here = bank == box.paging.bank;
            const auto lit = pageButtonLit (box.paging.index, box.paging.count, tick);

            const auto eq = here && box.paging.page == Page::eq && lit ? Led::on : Led::off;
            const auto sends = here && box.paging.page == Page::send && lit ? Led::on : Led::off;
            const auto fx = here && box.paging.page == Page::fx && lit ? Led::on : Led::off;

            if (static_cast<int> (fx) != shown.fxLed)
            {
                send (shown.port, led (fxButtonNote, fx));
                shown.fxLed = static_cast<int> (fx);
            }

            if (static_cast<int> (eq) != shown.eqLed)
            {
                send (shown.port, led (eqButtonNote, eq));
                shown.eqLed = static_cast<int> (eq);
            }

            if (static_cast<int> (sends) != shown.sendLed)
            {
                send (shown.port, led (sendButtonNote, sends));
                shown.sendLed = static_cast<int> (sends);
            }
        }

        void paintStrip (const Surface& box, Strip& strip, const std::string& port, int element,
                         const tree::TouchTable& touches, std::int64_t tick, int& number)
        {
            const auto* at = published.get();
            follow (strip);

            const auto& target = textAt (at, strip.targetAt);
            const auto& word = textAt (at, strip.wordAt);
            const auto isDca = textAt (at, strip.roleAt) == "dca";
            const auto level = target.empty() ? std::optional<double> {} : numberAt (at, target);

            //------------------------------------------------------------------
            /*  THE MOTOR, left alone while this surface holds it - the finger
                on it is the one setting it (PRD §3.16) - and a strip riding
                nothing flies to the bottom, where the next clip's fader starts
                (§3.9a: the start value is reasserted at every handover). */
            const auto heldNow = strip.handDown || (! target.empty() && touches.isHeld (box.origin, target));
            const auto letGo = strip.held && ! heldNow;
            strip.held = heldNow;

            /*  AND AGAIN A MOMENT AFTER A LET-GO (`motorReasserts`): the one
                position sent as the hand lifts can reach a D700 too soon to
                count, and the fader goes back where the host put it before. */
            if (letGo)
            {
                strip.reassertAt = tick + motorReassertTicks;
                strip.reassertsLeft = motorReasserts;
            }

            auto again = false;

            if (! heldNow && strip.reassertsLeft > 0 && tick >= strip.reassertAt)
            {
                again = true;
                --strip.reassertsLeft;
                strip.reassertAt = tick + motorReassertTicks;
            }

            if (heldNow)
                strip.reassertsLeft = 0;

            if (! heldNow)
                moveMotor (port, element, strip,
                           level.has_value() ? fourteenBitForDb (*level, box.topology.faderLaw) : 0,
                           letGo || again);

            /*  PICKED: the rotaries are aimed at this strip's cue (author,
                2026-09-25: "while a sample is selected (select button)"). */
            const auto& aimed = aimNow();
            const auto picked = ! strip.cueId.empty() && strip.cueId == aimed;

            if (box.paging.page != Page::show)
                paintControl (box, strip, port, element, tick);
            else
                paintOwn (box, strip, port, element, tick, number, word, isDca, level, picked);

            //------------------------------------------------------------------
            /*  MUTE, LIT FOR HALF A SECOND after a kill it sent - the only
                thing its light says (SurfaceProfile.h, `killFlashTicks`). */
            const auto muteLit = tick < strip.muteLitUntil ? Led::on : Led::off;

            if (static_cast<int> (muteLit) != strip.muteLed)
            {
                send (port, led (muteNote + element, muteLit));
                strip.muteLed = static_cast<int> (muteLit);
            }

            /*  SOLO, flashing while a soloed clip waits for its start, lit while
                it sounds, and dark once the solo has gone with the clip. */
            const auto soloed = ! isDca && flagAt (at, strip.soloAt);
            const auto soloLit = blinked (! soloed ? Led::off
                                                   : (word == "playing" || word == "held") ? Led::on : Led::flash,
                                          tick);

            if (static_cast<int> (soloLit) != strip.soloLed)
            {
                send (port, led (soloNote + element, soloLit));
                strip.soloLed = static_cast<int> (soloLit);
            }

            //  REC, lit a moment after it set the starting level.
            const auto recLit = tick < strip.recLitUntil ? Led::on : Led::off;

            if (static_cast<int> (recLit) != strip.recLed)
            {
                send (port, led (recNote + element, recLit));
                strip.recLed = static_cast<int> (recLit);
            }

            /*  SELECT IS THE PICK (author, 2026-09-25) - on a D700 the thin
                white bar at the top of the screen - and no longer says the
                strip sounds: the ring's progress and the pulsing colour say
                that, and the screen says the word. */
            const auto lit = picked ? Led::on : Led::off;

            if (static_cast<int> (lit) != strip.led)
            {
                send (port, led (selectNote + element, lit));
                strip.led = static_cast<int> (lit);
            }

            //------------------------------------------------------------------
            if (box.topology.hasMeters && ! isDca)
                paintMeter (port, element, strip, word, tick);
        }

        /*  THE SURFACE'S OWN PAGE: the strip's name, level and role on its
            screen, its clip's progress on its ring, and its sound's colour. */
        void paintOwn (const Surface& box, Strip& strip, const std::string& port, int element,
                       std::int64_t tick, int& number, std::string_view word, bool isDca,
                       std::optional<double> level, bool picked)
        {
            const auto* at = published.get();

            /*  WHAT THE STRIP IS CALLED: the authored short name, else the name
                - cut to the field, the truncation §3.16 says a display should
                not have to rely on, and the reason `shortName` exists - else
                the word. */
            std::string_view name = word;

            if (isDca)
            {
                if (const auto& shortName = textAt (at, strip.dcaShortAt); ! shortName.empty())
                    name = shortName;
                else if (const auto& longName = textAt (at, strip.dcaNameAt); ! longName.empty())
                    name = longName;
            }
            else if (! strip.cueId.empty())
            {
                if (const auto& shortName = textAt (at, strip.cueShortAt); ! shortName.empty())
                    name = shortName;
                else if (const auto& longName = textAt (at, strip.cueNameAt); ! longName.empty())
                    name = longName;
            }

            if (box.topology.nativeDisplay)
            {
                /*  THE D700'S OWN THREE ROWS, never MCU's 0x12 (§16.6): the
                    name in twelve; the level while the strip rides something
                    that has one, else the word; and what the strip is for, in
                    eight. The field choice is (proposed) and the author's to
                    reassign. */
                if (changed (strip.rows[0], name))
                    send (port, d700DisplayRow (element, 0, name));

                if (level.has_value())
                    levelText (*level, levelScratch);
                else
                    levelScratch.assign (word);

                if (changed (strip.rows[1], levelScratch))
                    send (port, d700DisplayRow (element, 1, levelScratch));

                /*  "sampler" and not "pads" (2026-09-25): a D700 strip is a
                    fader, and the author read "pads" on it as something the
                    fader could not do. And "picked" on the strip the rotaries
                    are aimed at - the SELECT light says so too, and a light is
                    never the only carrier (§4.8). */
                const std::string_view role = isDca                ? "dca"
                                            : strip.cueId.empty() ? "free"
                                            : picked               ? "picked"
                                                                   : "sampler";

                if (changed (strip.rows[2], role))
                    send (port, d700DisplayRow3 (element, role));

                /*  THE RING IS THE CLIP'S PROGRESS (author, 2026-09-25: "So
                    use the rotary LED ring then"). Asked for as the thin white
                    bar at the top of the screen - which is the D700's own mark
                    of a lit SELECT, on or off and nothing between, so it cannot
                    be one. Filled from the left as far as the clip has got, in
                    the D700's 128 steps, and empty when nothing sounds; turning
                    the knob still does nothing, and the ring no longer repeats
                    the fader ("either or"). Sent when it moves, and again
                    whenever the strip is painted whole. */
                const auto ring = progressOf (strip, word, d700RingSteps);

                if (ring != strip.ring || strip.ringMode != ringFillMode)
                {
                    send (port, d700Ring (element, ring, ringFillMode));
                    strip.ring = ring;
                    strip.ringMode = ringFillMode;
                }

                /*  THE CUE'S NUMBER in the strip's number field when it is one
                    the field can show, else the strip's own. */
                if (! strip.cueId.empty())
                    if (const auto shown = trackNumberOf (textAt (at, strip.cueNumberAt)))
                        number = *shown;
            }
            else
            {
                /*  MCU's scribble strip, two rows of seven, always padded - its
                    buffer is flat, and a short write leaves the last one's tail
                    showing (control guide §4.6). */
                if (changed (strip.rows[0], name))
                    send (port, lcdCell (deviceId, 0, element, name));

                if (changed (strip.rows[1], word))
                    send (port, lcdCell (deviceId, 1, element, word));

                //  The clip's progress, as the D700's, in MCU's eleven steps: it does not repeat the fader.
                const auto ring = progressOf (strip, word, mcuRingSteps);

                if (ring != strip.ring || strip.ringMode != ringFillMode)
                {
                    send (port, ringMcu (element, ring, ringFillMode, false));
                    strip.ring = ring;
                    strip.ringMode = ringFillMode;
                }
            }

            //------------------------------------------------------------------
            if (box.topology.hasRgb)
            {
                /*  WHAT IT SOUNDS LIKE while it sounds, the authored colour
                    otherwise - §3.30's (proposed) policy, built as the default -
                    and dark with neither. */
                std::optional<Rgb> wanted;

                if (word == "playing" || word == "held")
                {
                    wanted = colourFromTimbre (textAt (at, strip.timbreAt));

                    if (wanted.has_value())
                        wanted = pulsed (*wanted, strip, textAt (at, strip.envelopeAt));
                }
                else
                {
                    strip.pulseFor.clear();
                }

                if (! wanted.has_value() && ! strip.cueId.empty())
                    wanted = colourFromHex (textAt (at, strip.cueColourAt));

                paintColour (port, vpotNote + element, strip, wanted.value_or (Rgb {}), tick);
            }
        }

        /*  AN EQ OR SEND PAGE: the control under the rotary - what it is on the
            screen's first row, "off" beside it when its band is out; its value
            on the second; the page on the first rotary's third row and the
            aimed cue's name on the others'. Its ring stands at its value, and
            its surround wears its band's colour, dimmed while the band is out
            (author, 2026-09-25: "Put the colours of the parameters (EQ band)
            on the rotaries. Use the LEDs around the rotaries to show the
            value"). A rotary past the page's last control is dark and blank. */
        void paintControl (const Surface& box, Strip& strip, const std::string& port, int element,
                           std::int64_t tick)
        {
            const auto* at = published.get();
            const auto native = box.topology.nativeDisplay;
            const auto& paging = box.paging;

            labelScratch.clear();
            levelScratch.clear();
            Ring ring { 0, ringFillMode };
            Rgb colour {};

            if (strip.control >= 0 && paging.page == Page::eq)
            {
                const auto& control = eqControls[static_cast<std::size_t> (strip.control)];
                const auto eqIn = soleAt (at, paging.eqOnAt) == nullptr || flagAt (at, paging.eqOnAt);
                const auto out = ! eqIn || ! flagAt (at, strip.switchAt);

                labelScratch.assign (native ? control.label : control.shortLabel);

                if (native && out)
                    labelScratch.append (" off");

                if (control.law == Law::shape)
                {
                    const auto& shape = textAt (at, strip.controlAt);
                    levelScratch.assign (shapeText (shape, ! native));
                    ring = native ? d700ShapeRing (shape) : mcuShapeRing (shape);
                }
                else if (const auto value = numberAt (at, strip.controlAt))
                {
                    valueText (control.law, *value, ! native, levelScratch);
                    ring = native ? d700RingFor (control.law, *value, control.minimum, control.maximum,
                                                 box.topology.faderLaw)
                                  : mcuRingFor (control.law, *value, control.minimum, control.maximum,
                                                box.topology.faderLaw);
                }

                //  Seven characters have no room for both: an MCU says "off" instead of the value.
                if (! native && out)
                    levelScratch.assign ("off");

                colour = rgbOf (audio::eqColours[static_cast<std::size_t> (control.colour)],
                                out ? pageOffLight : 1.0);
            }
            else if (strip.control >= 0 && paging.page == Page::send)
            {
                /*  A SEND: the mix channel's name, "off" beside it when the
                    send is switched out; its level, or "no send" into a
                    channel the cue does not reach yet; its ring where a fader
                    would stand; the cue's own colour, dimmed while out. */
                const auto present = ! strip.controlAt.empty();
                const auto level = present ? numberAt (at, strip.controlAt) : std::optional<double> {};
                const auto out = ! present || ! flagAt (at, strip.switchAt);
                const auto& busName = textAt (at, strip.busNameAt);

                labelScratch.assign (busName.empty() ? std::string_view (strip.busId) : std::string_view (busName));

                if (native && present && out)
                    labelScratch.append (" off");

                if (level.has_value())
                {
                    valueText (Law::level, *level, ! native, levelScratch);
                    ring = native ? d700RingFor (Law::level, *level, faderSilenceDb, faderLoudestDb,
                                                 box.topology.faderLaw)
                                  : mcuRingFor (Law::level, *level, faderSilenceDb, faderLoudestDb,
                                                box.topology.faderLaw);
                }
                else
                {
                    levelScratch.assign (native ? "no send" : "none");
                }

                if (! native && present && out)
                    levelScratch.assign ("off");

                const auto own = colourFromHex (textAt (at, paging.aimColourAt)).value_or (neutralLight);
                const auto share = out ? pageOffLight : 1.0;
                colour = Rgb { static_cast<int> (std::lround (own.red * share)),
                               static_cast<int> (std::lround (own.green * share)),
                               static_cast<int> (std::lround (own.blue * share)) };
            }
            else if (strip.control >= 0 && paging.page == Page::fx)
            {
                /*  A PLUGIN PARAMETER (2026-09-26): its name as the plugin
                    gives it - the seven-character one on an MCU - and its
                    value in the plugin's own words; its ring from the centre
                    when its middle is its rest; the surround in the insert's
                    colour, so two inserts a page apart are told apart. */
                const auto name = textAt (at, strip.paramAt + (native ? "name" : "shortName"));
                const auto fallback = textAt (at, strip.paramAt + "name");
                labelScratch.assign (! name.empty() ? name
                                     : ! fallback.empty() ? fallback
                                                          : "p" + std::to_string (strip.control + 1));
                levelScratch.assign (textAt (at, strip.fxTextAt));

                if (const auto value = numberAt (at, strip.controlAt))
                {
                    const auto bipolar = flagAt (at, strip.paramAt + "bipolar");
                    ring = native ? d700ParameterRing (*value, bipolar) : mcuParameterRing (*value, bipolar);
                }

                colour = rgbOf (audio::eqColours[static_cast<std::size_t> (paging.fxInsert % 6)], 1.0);
            }
            else if (paging.page == Page::fx && paging.fxNone && &strip == &box.strips.front())
            {
                //  A cue with no insert in says so, on the first rotary.
                labelScratch.assign (native ? "no FX in" : "no FX");
            }

            if (native)
            {
                if (changed (strip.rows[0], labelScratch))
                    send (port, d700DisplayRow (element, 0, labelScratch));

                if (changed (strip.rows[1], levelScratch))
                    send (port, d700DisplayRow (element, 1, levelScratch));

                /*  THE THIRD ROW NAMES THE PAGE on the first rotary - "EQ",
                    "EQ 1/2", "EQ out" while the cue's whole EQ is out - and the
                    cue on the others, so a glance says whose EQ this is. */
                pageScratch.clear();

                if (strip.control >= 0 || (paging.page == Page::fx && paging.fxNone && &strip == &box.strips.front()))
                {
                    if (paging.page == Page::fx && &strip == &box.strips.front())
                    {
                        //  Which insert, and which of its pages: "Verb 1/3"; "FX" with none.
                        pageScratch.assign (paging.fxNone ? std::string ("FX") : paging.fxTitle);
                    }
                    else if (&strip == &box.strips.front())
                    {
                        const auto eqOut = paging.page == Page::eq
                                             && soleAt (at, paging.eqOnAt) != nullptr && ! flagAt (at, paging.eqOnAt);
                        pageScratch.assign (paging.page == Page::send ? "Send" : eqOut ? "EQ out" : "EQ");

                        if (! eqOut && paging.count > 1)
                        {
                            pageScratch.push_back (' ');
                            pageScratch.append (std::to_string (paging.index + 1));
                            pageScratch.push_back ('/');
                            pageScratch.append (std::to_string (paging.count));
                        }
                    }
                    else if (const auto& shortName = textAt (at, paging.aimShortAt); ! shortName.empty())
                    {
                        pageScratch.assign (shortName);
                    }
                    else
                    {
                        pageScratch.assign (textAt (at, paging.aimNameAt));
                    }
                }

                if (changed (strip.rows[2], pageScratch))
                    send (port, d700DisplayRow3 (element, pageScratch));

                if (ring.value != strip.ring || ring.mode != strip.ringMode)
                {
                    send (port, d700Ring (element, ring.value, ring.mode));
                    strip.ring = ring.value;
                    strip.ringMode = ring.mode;
                }
            }
            else
            {
                if (changed (strip.rows[0], labelScratch))
                    send (port, lcdCell (deviceId, 0, element, labelScratch));

                if (changed (strip.rows[1], levelScratch))
                    send (port, lcdCell (deviceId, 1, element, levelScratch));

                if (ring.value != strip.ring || ring.mode != strip.ringMode)
                {
                    send (port, ringMcu (element, ring.value, ring.mode, false));
                    strip.ring = ring.value;
                    strip.ringMode = ring.mode;
                }
            }

            if (box.topology.hasRgb)
                paintColour (port, vpotNote + element, strip, colour, tick);
        }

        /*  THE METER, AFTER THE FADER (SurfaceProfile.h, `meterStepsDb`): the
            loudest the strip's run sent since the last message, every
            `meterEveryTicks` while the strip sounds, and dark once when it
            stops. A run starts with the peak hold cleared, so a clip does not
            wear the loudest moment of the one before it. */
        void paintMeter (const std::string& port, int element, Strip& strip, std::string_view word,
                         std::int64_t tick)
        {
            const auto sounding = ! strip.holderId.empty()
                               && (word == "playing" || word == "held" || word == "stopping" || word == "closing");

            if (! sounding)
            {
                strip.meterPeak = meterSilentDb;
                strip.meterFor.clear();

                if (strip.meterStep != 0)
                {
                    send (port, meter (element, 0));
                    send (port, meterClearPeak (element));
                    strip.meterStep = 0;
                }

                return;
            }

            if (strip.meterFor != strip.holderId)
            {
                strip.meterFor = strip.holderId;
                send (port, meterClearPeak (element));
            }

            if (const auto db = numberAt (published.get(), strip.meterAt); db.has_value())
                strip.meterPeak = std::max (strip.meterPeak, *db);

            if (tick - strip.meterSentAt < meterEveryTicks)
                return;

            const auto step = meterStepFor (strip.meterPeak);
            send (port, meter (element, step));

            strip.meterStep = step;
            strip.meterSentAt = tick;
            strip.meterPeak = meterSilentDb;
        }

        /*  THE COLOUR AT THE BRIGHTNESS ITS SOUND'S MOVEMENT GIVES IT - see
            the `pulse` numbers in SurfaceProfile.h. An envelope the tree does
            not have yet leaves the colour as it is. */
        static Rgb pulsed (Rgb colour, Strip& strip, const std::string& envelopeText)
        {
            const auto envelope = osc::parseDouble (envelopeText);

            if (! envelope.has_value())
                return colour;

            if (strip.pulseFor != strip.holderId)
            {
                strip.pulseFor = strip.holderId;
                strip.pulseLevel = *envelope;
                strip.pulseAverage = *envelope;
                strip.pulseSpread = pulseScaleLeastDb / pulseSpreadsForFull;
                strip.pulseShown = 0.0;
            }

            //  One-pole means over their seconds of ticks: the level, a faster one, and how far it strays.
            const auto rate = static_cast<double> (TickClock::rateHz);
            const auto mean = [rate] (double& into, double value, double seconds)
            {
                into += (value - into) / std::max (1.0, seconds * rate);
            };

            mean (strip.pulseLevel, *envelope, pulseLevelSeconds);
            mean (strip.pulseAverage, *envelope, pulseAverageSeconds);

            const auto height = *envelope - strip.pulseAverage;
            mean (strip.pulseSpread, std::abs (height), pulseSpreadSeconds);

            //  THE LEVEL'S SHARE: where the long average sits between the floor and the ceiling.
            const auto level = std::clamp ((strip.pulseLevel - pulseLevelFloorDb)
                                              / (pulseLevelCeilingDb - pulseLevelFloorDb), 0.0, 1.0);

            //  THE MOVEMENT'S: steady in the middle, a hit at the top, a dip at the bottom.
            const auto full = std::clamp (pulseSpreadsForFull * strip.pulseSpread, pulseScaleLeastDb, pulseScaleMostDb);
            const auto movement = std::clamp (0.5 + 0.5 * height / full, 0.0, 1.0);

            const auto now = pulseFloor + (1.0 - pulseFloor)
                                            * (pulseLevelShare * level + (1.0 - pulseLevelShare) * movement);

            //  Up at once, down at the release: a flash outlives the rate limit.
            strip.pulseShown = std::max (now, strip.pulseShown - pulseReleasePerTick);

            const auto scale = [&strip] (int component)
            {
                return static_cast<int> (std::lround (static_cast<double> (component) * strip.pulseShown));
            };

            return Rgb { scale (colour.red), scale (colour.green), scale (colour.blue) };
        }

        /*  COLOUR, QUANTISED, RATE-LIMITED AND RE-ASSERTED (§16.6): written
            when it moved a visible step, never more often than
            `colourIntervalTicks` apart on one element, and written again every
            `idleColourReassertTicks` unchanged - the firmware's idle animation
            takes an undriven LED back. */
        void paintColour (const std::string& port, int note, Strip& strip, Rgb colour, std::int64_t tick)
        {
            /*  A VISIBLE STEP IS JUDGED ON THE COLOUR AS SEEN, before the
                LEDs' shaping squeezes the dim end together. */
            const auto levels = colourLevels (colour);
            const auto since = tick - strip.colourTick;

            const auto due = ! strip.colourKnown || levels != strip.colourLevel
                             || since >= idleColourReassertTicks;

            if (! due || since < colourIntervalTicks)
                return;

            sendColour (port, note, forTheLeds (colour));
            strip.colourKnown = true;
            strip.colourLevel = levels;
            strip.colourTick = tick;
        }

        //======================================================================
        midi::MidiSink& sink;
        SurfaceTable& table;

        std::vector<Surface> surfaces;

        /*  WHICH PORT IS WHOSE. Written by `declare` on the tick thread under
            `inboxLock`; read under it by `arrived`, and without it by the
            tick thread, which is its only writer. */
        std::vector<Owned> owners;

        std::vector<Event> owed;
        std::shared_ptr<const tree::TreeSnapshot> published;
        std::atomic<std::size_t> refused { 0 };
        bool tableMoved = false;

        std::string levelScratch;
        std::string labelScratch;
        std::string pageScratch;
        midi::Bytes colourScratch;

        std::mutex inboxLock;
        std::vector<Inbound> inbox;
        std::vector<Inbound> draining;
    };

    //==========================================================================
    SurfaceBridge::SurfaceBridge (midi::MidiSink& sink, SurfaceTable& table)
        : impl (std::make_unique<State> (sink, table))
    {
    }

    SurfaceBridge::~SurfaceBridge() = default;

    bool SurfaceBridge::declare (std::vector<SurfaceSpec> specs, const PortStates& portState)
    {
        auto& s = *impl;
        auto moved = false;

        std::vector<State::Surface> next;
        next.reserve (specs.size());

        /*  THE PORTS CLAIMED, in declaration order: a port carries one
            surface, and the first to name it has it. Two surfaces driving the
            same motors would fight each other and the hand. */
        std::vector<State::Owned> claimed;

        for (auto& spec : specs)
        {
            State::Surface box;
            box.id = spec.id;
            box.origin = "surface:" + spec.id;
            box.word = spec.profile;
            box.profile = profileFor (spec.profile);
            box.topology = box.profile.has_value() ? topologyOf (*box.profile) : Topology {};
            box.enabled = spec.enabled;
            box.channel = spec.channel;
            box.firstNote = spec.firstNote;

            box.strips.reserve (spec.strips.size());

            for (const auto& stripId : spec.strips)
                box.strips.push_back (State::stripFor (stripId));

            //------------------------------------------------------------------
            //  Whether it can be talked to tonight, and if not, why not.
            if (! box.profile.has_value())
            {
                box.problem = "the profile \"" + spec.profile + "\" is not one Go.dot can drive";
            }
            else if (*box.profile == Profile::virtualPanel)
            {
                box.connected = true;
            }
            else if (! box.enabled)
            {
                box.problem = "it is switched off in the show";
            }
            else if (spec.ports.empty())
            {
                const auto word = std::string (profileWord (*box.profile));
                box.problem = std::string (*box.profile == Profile::mcu ? "an " : "a ") + word
                            + " surface needs its ports: none is declared";
            }
            else
            {
                for (std::size_t bank = 0; bank < spec.ports.size(); ++bank)
                {
                    const auto& port = spec.ports[bank];
                    const auto found = portState ? portState (port) : PortState {};
                    const auto name = "the port \"" + (found.name.empty() ? port : found.name) + "\"";

                    State::Bank carried;
                    carried.port = port;
                    box.banks.push_back (std::move (carried));

                    std::string trouble;

                    const auto owner = std::find_if (claimed.begin(), claimed.end(),
                                                     [&port] (const State::Owned& claim)
                                                     { return claim.port == port; });

                    if (owner != claimed.end())
                    {
                        trouble = owner->surface == next.size() ? name + " is named twice"
                                                                : name + " already carries another surface";
                    }
                    else
                    {
                        State::Owned claim;
                        claim.port = port;
                        claim.surface = next.size();
                        claim.bank = bank;
                        claimed.push_back (std::move (claim));

                        if (! found.bound)
                            trouble = name + " has no device behind it";
                        else if (! found.rx)
                            trouble = name + " has rx turned off";
                        else if (! found.tx)
                            trouble = name + " has tx turned off";
                    }

                    if (box.problem.empty())
                        box.problem = std::move (trouble);
                }

                box.connected = box.problem.empty();
            }

            //------------------------------------------------------------------
            /*  WHAT CARRIES OVER from the declaration before. The same surface,
                connected before and after, drawn the same way, keeps everything
                - its caches, its hands, its serial - so a show edit repaints
                nothing. Anything else starts again: what its hands held is let
                go, and a surface that is connected now is painted whole. */
            const auto previous = std::find_if (s.surfaces.begin(), s.surfaces.end(),
                                                [&box] (const State::Surface& old)
                                                { return ! old.id.empty() && old.id == box.id; });

            if (previous != s.surfaces.end())
            {
                if (previous->connected && box.connected && State::sameShape (*previous, box))
                {
                    box.strips = std::move (previous->strips);
                    box.banks = std::move (previous->banks);
                    box.repaint = previous->repaint;
                    box.serial = previous->serial;
                    box.lastStop = previous->lastStop;
                    box.padCounter = previous->padCounter;

                    /*  AND ITS PAGE: every unlocked turn of a rotary is an
                        edit to the show, which declares again - a page that
                        did not carry over would close at the first detent. */
                    box.paging = std::move (previous->paging);
                }
                else
                {
                    s.owe (*previous);
                    box.repaint = box.connected;

                    if (previous->connected && box.connected)
                        box.serial = previous->serial;
                }

                previous->id.clear();       // matched: not forgotten below
            }
            else
            {
                box.repaint = box.connected;
            }

            if (! box.connected)
                box.serial.clear();

            SurfaceTable::Status status;
            status.connected = box.connected;
            status.problem = box.problem;
            status.serial = box.serial;
            moved = s.table.set (box.id, status) || moved;
            s.publishPage (box);

            next.push_back (std::move (box));
        }

        //  The surfaces the show no longer declares let go, and are forgotten.
        for (auto& old : s.surfaces)
            if (! old.id.empty())
            {
                s.owe (old);
                s.table.forget (old.id);
                moved = true;
            }

        for (auto& claim : claimed)
            claim.hears = next[claim.surface].connected;

        {
            const std::lock_guard<std::mutex> lock { s.inboxLock };
            s.owners = std::move (claimed);
        }

        s.surfaces = std::move (next);
        return moved;
    }

    bool SurfaceBridge::arrived (const std::string& portId, const midi::Bytes& message)
    {
        auto& s = *impl;
        const std::lock_guard<std::mutex> lock { s.inboxLock };

        for (const auto& owner : s.owners)
        {
            if (owner.port != portId)
                continue;

            /*  OWNED EITHER WAY, so no trigger fires on a surface's traffic;
                queued only when the surface is heard, and never a real-time
                byte - a clock or an active sensing means nothing here. */
            if (owner.hears && ! message.empty() && message.front() < firstRealTime
                && s.inbox.size() < inboxLimit)
                s.inbox.push_back ({ portId, message });

            return true;
        }

        return false;
    }

    bool SurfaceBridge::beforeTick (const Submit& submit, std::int64_t tick)
    {
        auto& s = *impl;
        s.tableMoved = false;

        static const Submit nowhere = [] (Event) { return true; };
        const auto& to = submit ? submit : nowhere;

        //  What surfaces that went away still owed, first.
        for (auto& event : s.owed)
            to (std::move (event));

        s.owed.clear();

        s.followTouches (to);

        {
            const std::lock_guard<std::mutex> lock { s.inboxLock };
            std::swap (s.inbox, s.draining);
        }

        for (const auto& in : s.draining)
            s.handle (in, to, tick);

        s.draining.clear();

        s.flushWrites (to);
        s.flushPageWrites (to);
        return s.tableMoved;
    }

    void SurfaceBridge::afterTick (std::shared_ptr<const tree::TreeSnapshot> snapshot,
                                   const tree::TouchTable& touches, std::int64_t tick)
    {
        auto& s = *impl;
        s.published = std::move (snapshot);

        if (s.published == nullptr)
            return;

        /*  ONLY A MACKIE SURFACE IS SHOWN ANYTHING: a pad controller has
            nothing to show here, and the virtual panel draws itself. */
        for (auto& box : s.surfaces)
            if (box.connected && box.enabled && box.isMackie())
                s.paint (box, touches, tick);
    }

    std::size_t SurfaceBridge::refusedSysEx() const noexcept
    {
        return impl->refused.load (std::memory_order_relaxed);
    }
}
