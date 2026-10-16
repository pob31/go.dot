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

/*
    AJA'S CARDS (namespace draft §72, AQB): Card.h's interface over AJA's own
    library, libajantv2 (MIT, compiled in; AQC), through the driver the user
    installed - AJA's installer on Windows and macOS, its kernel module on
    Linux. Written against SDK 18.0 with no card on the bench: what it does on
    one is M79's, by the testers.

    FOUND by AJA's scanner, a card by its serial number (`aja-` and the
    serial), named as AJA names the model; its formats those it offers that
    Go.dot has words for - progressive and interlaced up to 2K, never PsF or
    the ones carried on several links.

    AN OUTPUT is FrameStore 1 (2 on the few that cannot show 1) sent down SDI
    1, and HDMI 1 where there is one, through AutoCirculate: frames handed to
    the card in order, one leaving at each of its vertical interrupts, each
    with its sound. The pacing (APB) is kept by what is handed over: a frame
    plays at the interrupt after those already handed over and after every
    time the card repeated one because none was there - so a frame whose
    moment has passed is never handed over (counted late), and one that is
    ahead waits while the card repeats the last (counted dropped). A thread of
    the output's own waits on each interrupt, stamps the stream's time against
    the steady clock - the clock the renderer fits - and hands over what is
    due; a frame scheduled between interrupts is handed over at once.

    10-bit YUV where nothing is keyed (v210, the card's 10-bit YCbCr);
    otherwise BGRA (the card's ARGB, blue first in memory) through its colour
    converter: fill on SDI 1 and key on SDI 2 (external), or laid over what
    comes in on SDI in its mixer, the fill premultiplied (internal, APG).

    SOUND (APK) rides with its frame: the samples scheduled are kept by stream
    sample, and each frame takes its own span - 960 at 50p, 1601 or 1602 at
    29.97 - silence where none arrived. A frame whose sound is not all there
    waits while the card still holds another to send.

    AN INPUT is SDI input 1 - the last SDI, on a card whose SDIs go both ways -
    into FrameStore 2 where there is one, at the format the card finds, 8-bit
    YUV (UYVY), through AutoCirculate's capture.

    ONE PROGRAM AT A TIME: the card is claimed for Go.dot while an output or
    an input of it is open (AcquireStreamForApplication), let go after the last.
*/

#include <wfg/engine/video/render/Card.h>

#include <wfg/engine/video/VideoModes.h>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
#endif

#include <ajabase/system/process.h>
#include <ntv2card.h>
#include <ntv2devicescanner.h>
#include <ntv2formatdescriptor.h>
#include <ntv2signalrouter.h>
#include <ntv2utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wfg::video::card
{
    namespace
    {
        constexpr const char* ajaPrefix = "aja-";
        constexpr ULWord ajaCards = 16;              ///< the most looked for
        constexpr UWord framesOnCard = 7;            ///< frames AutoCirculate keeps on the card
        constexpr std::int64_t soundRingFrames = 96000;
        constexpr ULWord soundBytes = 256 * 1024;    ///< a frame's sound: 2,003 samples of 16 lanes at 23.98, and more

        std::int64_t steadyNanos() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        ULWord signature() noexcept
        {
            return NTV2_FOURCC ('G', 'D', 'O', 'T');
        }

        std::int32_t processId() noexcept
        {
            return static_cast<std::int32_t> (AJAProcess::GetPid());
        }

        /*  An AJA format as Go.dot's words have it, or nothing for one they
            do not: PsF, a size past 2K, a rate AJA cannot say. */
        std::optional<modes::Format> formatOf (NTV2VideoFormat video)
        {
            if (! NTV2_IS_VALID_VIDEO_FORMAT (video) || NTV2_IS_PSF_VIDEO_FORMAT (video))
                return std::nullopt;

            const NTV2FormatDescriptor described (video, NTV2_FBF_8BIT_YCBCR);

            if (! described.IsValid())
                return std::nullopt;

            ULWord numerator = 0;
            ULWord denominator = 0;

            if (! GetFramesPerSecond (GetNTV2FrameRateFromVideoFormat (video), numerator, denominator) || numerator == 0 || denominator == 0)
                return std::nullopt;

            modes::Format format;
            format.width = static_cast<int> (described.GetRasterWidth());
            format.height = static_cast<int> (described.GetVisibleRasterHeight());
            format.frameDuration = static_cast<std::int64_t> (denominator);
            format.timeScale = static_cast<std::int64_t> (numerator);
            format.interlaced = ! NTV2_VIDEO_FORMAT_HAS_PROGRESSIVE_PICTURE (video);

            if (format.width <= 0 || format.height <= 0 || format.width > 2048 || format.height > 1080)
                return std::nullopt;

            return format;
        }

        /*  A card's formats, each once by its word, AJA's order kept. */
        std::vector<std::pair<NTV2VideoFormat, std::string>> wordsOf (CNTV2Card& device)
        {
            std::vector<std::pair<NTV2VideoFormat, std::string>> out;
            std::set<std::string> seen;

            for (const auto video : device.features().VideoFormats())
                if (const auto format = formatOf (video))
                    if (auto word = modes::wordFor (*format); ! word.empty() && seen.insert (word).second)
                        out.emplace_back (video, std::move (word));

            return out;
        }

        NTV2VideoFormat videoFormatFor (CNTV2Card& device, const std::string& word)
        {
            for (const auto& [video, said] : wordsOf (device))
                if (said == word)
                    return video;

            return NTV2_FORMAT_UNKNOWN;
        }

        std::string idOf (CNTV2Card& device, ULWord index)
        {
            std::string serial;

            if (! device.GetSerialNumberString (serial) || serial.empty())
                serial = "index" + std::to_string (index);

            return ajaPrefix + serial;
        }

        /*  Card `id` opened into `device`. */
        bool openById (const std::string& id, CNTV2Card& device)
        {
            for (ULWord index = 0; index < ajaCards; ++index)
            {
                if (! CNTV2DeviceScanner::GetDeviceAtIndex (index, device))
                    return false;

                if (idOf (device, index) == id)
                    return true;
            }

            return false;
        }

        /*  Where pictures come in: SDI 1, or the last SDI on a card whose SDIs
            go both ways - the first sends. */
        NTV2Channel inputConnector (CNTV2Card& device)
        {
            auto& can = device.features();
            const auto connectors = std::max<int> (1, static_cast<int> (can.GetNumVideoInputs()));

            if (can.HasBiDirectionalSDI() && connectors > 1)
                return static_cast<NTV2Channel> (connectors - 1);

            return NTV2_CHANNEL1;
        }

        /*  THE CLAIM: a card is Go.dot's while anything of it is open. */
        struct Claims
        {
            std::mutex lock;
            std::map<std::string, int> held;

            bool claim (CNTV2Card& device, const std::string& id)
            {
                const std::scoped_lock hold (lock);
                auto& count = held[id];

                if (count == 0 && ! device.AcquireStreamForApplication (signature(), processId()))
                {
                    held.erase (id);
                    return false;
                }

                ++count;
                device.SetTaskMode (NTV2_OEM_TASKS);
                return true;
            }

            void release (CNTV2Card& device, const std::string& id)
            {
                const std::scoped_lock hold (lock);
                const auto found = held.find (id);

                if (found == held.end())
                    return;

                if (--found->second == 0)
                {
                    device.ReleaseStreamForApplication (signature(), processId());
                    held.erase (found);
                }
            }
        };

        //  =====================================================================
        class AjaOutput final : public Output
        {
        public:
            AjaOutput (std::shared_ptr<Claims> claimsToUse, std::string idToUse)
                : claims (std::move (claimsToUse)), id (std::move (idToUse))
            {
            }

            ~AjaOutput() override
            {
                stopping.store (true);

                if (pumping.joinable())
                    pumping.join();

                if (circulating)
                {
                    card.AutoCirculateStop (channel);
                    card.UnsubscribeOutputVerticalEvent (channel);
                }

                if (claimed)
                    claims->release (card, id);
            }

            /*  The card opened at `settings`, or false and why. */
            bool open (const OutputSettings& settings, std::string& why)
            {
                if (! openById (id, card))
                {
                    why = "the card is not connected";
                    return false;
                }

                if (! card.IsDeviceReady (false))
                {
                    why = "the card is not ready: its driver may still be starting";
                    return false;
                }

                auto& can = card.features();

                if (! can.CanDoPlayback())
                {
                    why = "the card does not send pictures";
                    return false;
                }

                const auto video = videoFormatFor (card, settings.mode);
                const auto format = formatOf (video);

                if (video == NTV2_FORMAT_UNKNOWN || ! format)
                {
                    why = "the card does not send " + settings.mode;
                    return false;
                }

                const auto keyed = settings.key != Keying::off;
                const auto converts = can.GetNumCSCs() > 0 && can.CanDoFrameBufferFormat (NTV2_FBF_ARGB);

                if (settings.key == Keying::external && (! converts || can.GetNumVideoOutputs() < 2))
                {
                    why = "the card has no external keyer";
                    return false;
                }

                if (settings.key == Keying::internal && (! converts || can.GetNumMixers() == 0 || ! can.CanDoCapture()))
                {
                    why = "the card has no internal keyer";
                    return false;
                }

                const auto tenBit = settings.tenBit && ! keyed && can.CanDoFrameBufferFormat (NTV2_FBF_10BIT_YCBCR);

                if (! tenBit && ! converts)
                {
                    why = "the card takes neither 10-bit YUV nor colour it can turn to YUV";
                    return false;
                }

                pixelFormat = tenBit ? PixelFormat::yuv10 : PixelFormat::bgra8;
                chosen.word = settings.mode;
                chosen.name = NTV2VideoFormatToString (video, true);
                chosen.format = *format;
                chosen.output = true;

                if (! claims->claim (card, id))
                {
                    why = "another program has the card";
                    return false;
                }

                claimed = true;
                return prepare (settings, video, tenBit ? NTV2_FBF_10BIT_YCBCR : NTV2_FBF_ARGB, why);
            }

            const Mode& mode() const override          { return chosen; }
            PixelFormat format() const override        { return pixelFormat; }

            Frame lend() override
            {
                const std::scoped_lock hold (lock);

                for (std::size_t i = 0; i < slots.size(); ++i)
                {
                    if (slots[i].state != State::free)
                        continue;

                    slots[i].state = State::lent;
                    return { slots[i].buffer.GetHostPointer(), rowBytes, static_cast<int> (i) };
                }

                return {};
            }

            bool schedule (const Frame& frame, std::int64_t index) override
            {
                const std::scoped_lock hold (lock);

                if (frame.slot < 0 || frame.slot >= static_cast<int> (slots.size()))
                    return false;

                auto& slot = slots[static_cast<std::size_t> (frame.slot)];

                if (slot.state != State::lent)
                    return false;

                slot.state = State::scheduled;
                const auto at = std::lower_bound (pending.begin(), pending.end(), index,
                                                  [] (const std::pair<std::int64_t, int>& one, std::int64_t to) { return one.first < to; });
                pending.insert (at, { index, frame.slot });
                pump();
                return true;
            }

            void giveBack (const Frame& frame) override
            {
                const std::scoped_lock hold (lock);

                if (frame.slot >= 0 && frame.slot < static_cast<int> (slots.size())
                      && slots[static_cast<std::size_t> (frame.slot)].state == State::lent)
                    slots[static_cast<std::size_t> (frame.slot)].state = State::free;
            }

            int waiting() override
            {
                const std::scoped_lock hold (lock);
                return static_cast<int> (pending.size()) + static_cast<int> (onCard);
            }

            bool start (std::int64_t firstToUse) override
            {
                const std::scoped_lock hold (lock);

                if (running || begun)
                    return false;

                first = firstToUse;
                begun = true;
                pump();

                if (! card.AutoCirculateStart (channel))
                {
                    lost = "the card would not start sending";
                    return false;
                }

                card.GetOutputVerticalInterruptCount (startedAt, channel);
                running = true;
                return true;
            }

            bool started() const override
            {
                const std::scoped_lock hold (lock);
                return running;
            }

            bool clock (std::int64_t& streamNanos, std::int64_t& nanos) override
            {
                const std::scoped_lock hold (lock);

                if (! running || ! stamped)
                    return false;

                streamNanos = stampedStream;
                nanos = stampedSteady;
                return true;
            }

            Reference reference() override
            {
                const std::scoped_lock hold (lock);
                NTV2ReferenceSource source = NTV2_REFERENCE_FREERUN;

                if (! card.GetReference (source) || source == NTV2_REFERENCE_FREERUN)
                    return Reference::none;

                const auto present = source == NTV2_REFERENCE_EXTERNAL
                                         ? card.GetReferenceVideoFormat() != NTV2_FORMAT_UNKNOWN
                                         : card.GetInputVideoFormat (GetNTV2InputSourceForIndex (static_cast<ULWord> (inputConnector (card))))
                                               != NTV2_FORMAT_UNKNOWN;
                return present ? Reference::locked : Reference::unlocked;
            }

            Completions completions() override
            {
                const std::scoped_lock hold (lock);
                return counted;
            }

            bool scheduleSound (const std::int32_t* samples, int frames, std::int64_t streamSample) override
            {
                const std::scoped_lock hold (lock);

                if (! soundOn || frames <= 0 || streamSample + frames <= soundTo - soundRingFrames)
                    return false;

                const auto lanes = static_cast<std::size_t> (soundChannels);
                const auto put = [this, lanes] (std::int64_t sample, const std::int32_t* from)
                {
                    auto* to = soundRing.data() + static_cast<std::size_t> (sample % soundRingFrames) * lanes;

                    if (from != nullptr)
                        std::memcpy (to, from, lanes * sizeof (std::int32_t));
                    else
                        std::fill (to, to + lanes, 0);
                };

                //  A gap since the last is silence, never what the ring held before.
                for (auto sample = std::max (soundTo, streamSample - soundRingFrames); sample < streamSample; ++sample)
                    put (sample, nullptr);

                for (int f = 0; f < frames; ++f)
                    put (streamSample + f, samples + static_cast<std::size_t> (f) * lanes);

                soundTo = std::max (soundTo, streamSample + frames);
                pump();
                return true;
            }

            int soundWaiting() override
            {
                const std::scoped_lock hold (lock);

                if (! soundOn || ! begun)
                    return 0;

                return static_cast<int> (std::max<std::int64_t> (0, soundTo - sampleOf (due())));
            }

            std::string problem() override
            {
                const std::scoped_lock hold (lock);
                return lost;
            }

        private:
            enum class State { free, lent, scheduled };

            struct Slot
            {
                NTV2Buffer buffer;
                State state = State::free;
            };

            bool prepare (const OutputSettings& settings, NTV2VideoFormat video, NTV2FrameBufferFormat pixels, std::string& why)
            {
                auto& can = card.features();
                channel = can.CanDoFrameStore1Display() ? NTV2_CHANNEL1 : NTV2_CHANNEL2;

                if (can.CanDoMultiFormat())
                    card.SetMultiFormatMode (true);

                card.EnableChannel (channel);
                card.SetVANCMode (NTV2_VANCMODE_OFF, channel);

                if (! card.SetVideoFormat (video, false, false, channel) || ! card.SetFrameBufferFormat (channel, pixels))
                {
                    why = "the card would not send " + chosen.word;
                    return false;
                }

                if (! route (settings.key, video))
                {
                    why = std::string ("the card would not route its picture") + (settings.key == Keying::off ? "" : " and its key");
                    return false;
                }

                //  Locked to the reference when one comes in; to the input it keys over.
                if (settings.key == Keying::internal)
                    card.SetReference (NTV2InputSourceToReferenceSource (GetNTV2InputSourceForIndex (static_cast<ULWord> (inputConnector (card)))));
                else
                    card.SetReference (card.GetReferenceVideoFormat() != NTV2_FORMAT_UNKNOWN ? NTV2_REFERENCE_EXTERNAL : NTV2_REFERENCE_FREERUN);

                if (settings.audioChannels > 0 && ! prepareSound (settings.audioChannels, why))
                    return false;

                const NTV2FormatDescriptor described (video, pixels);
                rowBytes = static_cast<int> (described.GetBytesPerRow());
                frameBytes = described.GetTotalBytes();

                for (auto& slot : slots)
                {
                    if (! slot.buffer.Allocate (frameBytes, true))
                    {
                        why = "there is no memory for the card's frames";
                        return false;
                    }
                }

                card.SubscribeOutputVerticalEvent (channel);
                card.AutoCirculateStop (channel);

                if (! card.AutoCirculateInitForOutput (channel, framesOnCard, soundOn ? soundSystem : NTV2_AUDIOSYSTEM_INVALID, 0))
                {
                    why = "the card would not take frames in turn";
                    return false;
                }

                circulating = true;
                pumping = std::thread ([this] { watch(); });
                return true;
            }

            /*  The picture's way out: the FrameStore, through the colour
                converter for BGRA, to SDI 1 (and HDMI 1); the key to SDI 2,
                or through the mixer over SDI in. */
            bool route (Keying key, NTV2VideoFormat video)
            {
                auto& can = card.features();
                const auto rgb = pixelFormat == PixelFormat::bgra8;
                const auto standard = GetNTV2StandardFromVideoFormat (video);
                const auto store = GetFrameStoreOutputXptFromChannel (channel, rgb, false);
                auto ok = true;

                const auto send = [&] (NTV2Channel connector, NTV2OutputXptID source)
                {
                    if (can.HasBiDirectionalSDI())
                        card.SetSDITransmitEnable (connector, true);

                    ok = card.Connect (GetSDIOutputInputXpt (connector, false), source) && ok;
                    card.SetSDIOutputStandard (static_cast<UWord> (connector), standard);
                };

                auto picture = store;

                if (rgb)
                {
                    ok = card.Connect (GetCSCInputXptFromChannel (channel, false), store) && ok;
                    picture = GetCSCOutputXptFromChannel (channel, false, false);
                }

                const auto keyOut = GetCSCOutputXptFromChannel (channel, true, false);
                auto shown = picture;

                switch (key)
                {
                    case Keying::off:
                        send (channel, picture);
                        break;

                    case Keying::external:
                        send (channel, picture);
                        send (static_cast<NTV2Channel> (static_cast<int> (channel) + 1), keyOut);
                        break;

                    case Keying::internal:
                    {
                        const auto under = inputConnector (card);

                        if (can.HasBiDirectionalSDI())
                            card.SetSDITransmitEnable (under, false);

                        ok = card.Connect (GetMixerFGInputXpt (NTV2_CHANNEL1, false), picture) && ok;
                        ok = card.Connect (GetMixerFGInputXpt (NTV2_CHANNEL1, true), keyOut) && ok;
                        ok = card.Connect (GetMixerBGInputXpt (NTV2_CHANNEL1, false), GetSDIInputOutputXptFromChannel (under, false)) && ok;
                        card.SetMixerMode (0, NTV2MIXERMODE_MIX);
                        card.SetMixerFGInputControl (0, NTV2MIXERINPUTCONTROL_SHAPED);
                        card.SetMixerBGInputControl (0, NTV2MIXERINPUTCONTROL_FULLRASTER);
                        card.SetMixerCoefficient (0, 0x10000);
                        shown = GetMixerOutputXptFromChannel (NTV2_CHANNEL1, false);
                        send (channel, shown);
                        break;
                    }
                }

                if (can.GetNumHDMIVideoOutputs() > 0 && channel == NTV2_CHANNEL1)
                    card.Connect (GetOutputDestInputXpt (NTV2_OUTPUTDESTINATION_HDMI1), shown);

                return ok;
            }

            /*  SOUND IN THE CABLE: an audio system of 8 or 16 lanes, 48 kHz,
                embedded in SDI 1 (and HDMI 1), `wanted` of them Go.dot's. */
            bool prepareSound (int wanted, std::string& why)
            {
                auto& can = card.features();
                const auto most = static_cast<int> (can.GetMaxAudioChannels());

                if (wanted > most)
                {
                    why = "the card embeds no more than " + std::to_string (most) + " channels of sound";
                    return false;
                }

                soundLanes = std::min (most, wanted <= 8 ? 8 : 16);
                soundChannels = wanted;
                soundSystem = can.GetNumAudioSystems() > 1 ? NTV2ChannelToAudioSystem (channel) : NTV2_AUDIOSYSTEM_1;

                card.SetNumberAudioChannels (static_cast<ULWord> (soundLanes), soundSystem);
                card.SetAudioRate (NTV2_AUDIO_48K, soundSystem);
                card.SetAudioBufferSize (NTV2_AUDIO_BUFFER_BIG, soundSystem);
                card.SetSDIOutputAudioSystem (channel, soundSystem);
                card.SetSDIOutputDS2AudioSystem (channel, soundSystem);
                card.SetAudioLoopBack (NTV2_AUDIO_LOOPBACK_OFF, soundSystem);

                if (can.GetNumHDMIVideoOutputs() > 0)
                    card.SetHDMIOutAudioSource8Channel (NTV2_AudioChannel1_8, soundSystem);

                if (! soundBuffer.Allocate (soundBytes, true))
                {
                    why = "there is no memory for the card's sound";
                    return false;
                }

                soundRing.assign (static_cast<std::size_t> (soundRingFrames) * static_cast<std::size_t> (soundChannels), 0);
                soundOn = true;
                return true;
            }

            /*  The stream sample frame `index` begins at. */
            std::int64_t sampleOf (std::int64_t index) const noexcept
            {
                return index * 48000 * chosen.format.frameDuration / chosen.format.timeScale;
            }

            /*  The frame the next one handed over will be shown as: after
                those handed over, and after every frame the card repeated. */
            std::int64_t due() const noexcept
            {
                return first + handed + repeats;
            }

            /*  WHAT IS DUE, HANDED OVER - from schedule, scheduleSound, start
                and each interrupt, under the lock. */
            void pump()
            {
                if (! begun || ! circulating)
                    return;

                AUTOCIRCULATE_STATUS status;

                if (card.AutoCirculateGetStatus (channel, status))
                {
                    onCard = status.GetBufferLevel();
                    repeats = static_cast<std::int64_t> (status.GetDroppedFrameCount());
                    counted.shown = static_cast<std::int64_t> (status.GetProcessedFrameCount());
                    counted.dropped = repeats;
                }

                while (! pending.empty())
                {
                    const auto [index, slot] = pending.front();

                    if (index < due())
                    {
                        //  Its moment has passed: never sent.
                        slots[static_cast<std::size_t> (slot)].state = State::free;
                        pending.erase (pending.begin());
                        ++counted.late;
                        continue;
                    }

                    if (index > due() || onCard + 2 > static_cast<ULWord> (framesOnCard))
                        break;

                    if (soundOn && soundTo < sampleOf (index + 1) && onCard >= 2)
                        break;

                    pending.erase (pending.begin());
                    handOver (slot, index);
                }
            }

            void handOver (int slot, std::int64_t index)
            {
                auto& held = slots[static_cast<std::size_t> (slot)];
                transfer.SetVideoBuffer (static_cast<ULWord*> (held.buffer.GetHostPointer()), frameBytes);

                if (soundOn)
                    transfer.SetAudioBuffer (static_cast<ULWord*> (soundBuffer.GetHostPointer()), fillSound (index));

                const auto sent = card.AutoCirculateTransfer (channel, transfer);
                held.state = State::free;

                if (! sent)
                {
                    if (++refused > 25 && lost.empty())
                        lost = "the card stopped taking frames";

                    return;
                }

                refused = 0;
                ++handed;
                ++onCard;
            }

            /*  Frame `index`'s sound into the transfer's buffer, each sample
                frame `soundLanes` wide: silence where none arrived. */
            ULWord fillSound (std::int64_t index)
            {
                const auto from = sampleOf (index);
                const auto count = std::min<std::int64_t> (sampleOf (index + 1) - from,
                                                           static_cast<std::int64_t> (soundBytes / 4) / soundLanes);
                auto* out = static_cast<std::int32_t*> (soundBuffer.GetHostPointer());
                const auto lanes = static_cast<std::size_t> (soundLanes);
                const auto ours = static_cast<std::size_t> (soundChannels);

                for (std::int64_t f = 0; f < count; ++f)
                {
                    const auto sample = from + f;
                    auto* to = out + static_cast<std::size_t> (f) * lanes;
                    std::fill (to, to + lanes, 0);

                    if (sample < soundTo && sample >= soundTo - soundRingFrames)
                        std::memcpy (to, soundRing.data() + static_cast<std::size_t> (sample % soundRingFrames) * ours, ours * sizeof (std::int32_t));
                }

                return static_cast<ULWord> (count) * static_cast<ULWord> (soundLanes) * 4u;
            }

            /*  THE INTERRUPTS, on a thread of the output's own: each stamped,
                the stream's time against the steady clock, and what is due
                handed over. */
            void watch()
            {
                int failures = 0;

                while (! stopping.load())
                {
                    const auto waited = card.WaitForOutputVerticalInterrupt (channel);
                    const auto at = steadyNanos();

                    if (! waited)
                    {
                        if (++failures > 50)
                        {
                            const std::scoped_lock hold (lock);

                            if (lost.empty())
                                lost = "the card stopped answering";
                        }

                        std::this_thread::sleep_for (std::chrono::milliseconds (10));
                        continue;
                    }

                    failures = 0;
                    const std::scoped_lock hold (lock);
                    ULWord count = 0;

                    if (running && card.GetOutputVerticalInterruptCount (count, channel))
                    {
                        //  Frame `first` leaves at the first interrupt after the start.
                        const auto since = static_cast<std::int64_t> (static_cast<std::uint32_t> (count - startedAt));

                        if (since >= 1)
                        {
                            stampedStream = (first + since - 1) * chosen.format.frameDuration * 1'000'000'000 / chosen.format.timeScale;
                            stampedSteady = at;
                            stamped = true;
                        }
                    }

                    pump();
                }
            }

            const std::shared_ptr<Claims> claims;
            const std::string id;
            CNTV2Card card;
            bool claimed = false;
            bool circulating = false;
            NTV2Channel channel = NTV2_CHANNEL1;
            Mode chosen;
            PixelFormat pixelFormat = PixelFormat::yuv10;
            int rowBytes = 0;
            ULWord frameBytes = 0;
            std::thread pumping;
            std::atomic<bool> stopping { false };

            mutable std::mutex lock;
            std::array<Slot, 8> slots;
            std::vector<std::pair<std::int64_t, int>> pending;    ///< scheduled, not yet handed over, by index
            AUTOCIRCULATE_TRANSFER transfer;
            bool begun = false;
            bool running = false;
            std::int64_t first = 0;
            std::int64_t handed = 0;
            std::int64_t repeats = 0;
            ULWord onCard = 0;
            ULWord startedAt = 0;
            int refused = 0;
            bool stamped = false;
            std::int64_t stampedStream = 0;
            std::int64_t stampedSteady = 0;
            Completions counted;
            std::string lost;

            bool soundOn = false;
            int soundChannels = 0;
            int soundLanes = 0;
            NTV2AudioSystem soundSystem = NTV2_AUDIOSYSTEM_1;
            NTV2Buffer soundBuffer;
            std::vector<std::int32_t> soundRing;
            std::int64_t soundTo = 0;
        };

        //  =====================================================================
        class AjaInput final : public Input
        {
        public:
            AjaInput (std::shared_ptr<Claims> claimsToUse, std::string idToUse, std::string wantedToUse)
                : claims (std::move (claimsToUse)), id (std::move (idToUse)), wanted (std::move (wantedToUse))
            {
            }

            ~AjaInput() override
            {
                stopping.store (true);

                if (reading.joinable())
                    reading.join();

                if (capturing)
                    card.AutoCirculateStop (channel);

                if (subscribed)
                    card.UnsubscribeInputVerticalEvent (channel);

                if (claimed)
                    claims->release (card, id);
            }

            bool open (std::string& why)
            {
                if (! openById (id, card))
                {
                    why = "the card is not connected";
                    return false;
                }

                if (! card.IsDeviceReady (false))
                {
                    why = "the card is not ready: its driver may still be starting";
                    return false;
                }

                auto& can = card.features();

                if (! can.CanDoCapture())
                {
                    why = "the card takes no pictures in";
                    return false;
                }

                if (wanted != modes::automatic && videoFormatFor (card, wanted) == NTV2_FORMAT_UNKNOWN)
                {
                    why = "the card does not take in " + wanted;
                    return false;
                }

                if (! claims->claim (card, id))
                {
                    why = "another program has the card";
                    return false;
                }

                claimed = true;
                channel = can.GetNumFrameStores() >= 2 ? NTV2_CHANNEL2 : NTV2_CHANNEL1;
                connector = inputConnector (card);
                source = GetNTV2InputSourceForIndex (static_cast<ULWord> (connector));

                if (can.CanDoMultiFormat())
                    card.SetMultiFormatMode (true);

                if (can.HasBiDirectionalSDI())
                    card.SetSDITransmitEnable (connector, false);

                card.EnableChannel (channel);
                subscribed = card.SubscribeInputVerticalEvent (channel);
                reading = std::thread ([this] { read(); });
                return true;
            }

            bool newest (std::vector<std::uint8_t>& pixels, int& width, int& height, int& rowBytes, std::uint64_t& serial) override
            {
                const std::scoped_lock hold (lock);

                if (latestSerial == serial || latest.empty())
                    return false;

                pixels = latest;
                width = latestFormat.width;
                height = latestFormat.height;
                rowBytes = latestRowBytes;
                serial = latestSerial;
                return true;
            }

            bool signal() override
            {
                const std::scoped_lock hold (lock);
                return trouble.empty() && arrivedAt != 0 && steadyNanos() - arrivedAt < 1'000'000'000;
            }

            std::string modeWord() override
            {
                const std::scoped_lock hold (lock);
                return arrivingWord;
            }

            std::string problem() override
            {
                const std::scoped_lock hold (lock);
                return trouble;
            }

        private:
            void say (const std::string& text, const std::string& word = {})
            {
                const std::scoped_lock hold (lock);
                trouble = text;
                arrivingWord = word;
            }

            /*  Capture set to `video`, as the card found it. */
            bool begin (NTV2VideoFormat video, const modes::Format& format)
            {
                card.SetVideoFormat (video, false, false, channel);
                card.SetVANCMode (NTV2_VANCMODE_OFF, channel);
                card.SetFrameBufferFormat (channel, NTV2_FBF_8BIT_YCBCR);
                card.Connect (GetFrameStoreInputXptFromChannel (channel, false), GetSDIInputOutputXptFromChannel (connector, false));

                const NTV2FormatDescriptor described (video, NTV2_FBF_8BIT_YCBCR);
                bytes = described.GetTotalBytes();

                if (! buffer.Allocate (bytes, true))
                    return false;

                card.AutoCirculateStop (channel);

                if (! card.AutoCirculateInitForInput (channel, 4) || ! card.AutoCirculateStart (channel))
                    return false;

                const std::scoped_lock hold (lock);
                latestFormat = format;
                latestRowBytes = static_cast<int> (described.GetBytesPerRow());
                return true;
            }

            /*  THE READING THREAD: the format the card finds, capture set to
                it, and each frame taken as it arrives. */
            void read()
            {
                NTV2VideoFormat current = NTV2_FORMAT_UNKNOWN;
                AUTOCIRCULATE_TRANSFER taking;

                while (! stopping.load())
                {
                    const auto found = card.GetInputVideoFormat (source);

                    if (found != current)
                    {
                        current = found;

                        if (capturing)
                        {
                            card.AutoCirculateStop (channel);
                            capturing = false;
                        }

                        const auto format = formatOf (found);

                        if (! format)
                        {
                            say ("no signal");
                            continue;
                        }

                        const auto word = modes::wordFor (*format);

                        if (wanted != modes::automatic && word != wanted)
                        {
                            say ("the input sends " + word + ", not " + wanted);
                            continue;
                        }

                        capturing = begin (found, *format);
                        say (capturing ? std::string() : std::string ("the card would not take pictures in"), capturing ? word : std::string());
                    }

                    if (! capturing)
                    {
                        std::this_thread::sleep_for (std::chrono::milliseconds (50));
                        continue;
                    }

                    AUTOCIRCULATE_STATUS status;

                    if (card.AutoCirculateGetStatus (channel, status) && status.IsRunning() && status.HasAvailableInputFrame())
                    {
                        taking.SetVideoBuffer (static_cast<ULWord*> (buffer.GetHostPointer()), bytes);

                        if (card.AutoCirculateTransfer (channel, taking))
                        {
                            const auto* from = static_cast<const std::uint8_t*> (buffer.GetHostPointer());
                            const std::scoped_lock hold (lock);
                            latest.assign (from, from + bytes);
                            ++latestSerial;
                            arrivedAt = steadyNanos();
                        }

                        continue;
                    }

                    if (! card.WaitForInputVerticalInterrupt (channel))
                        std::this_thread::sleep_for (std::chrono::milliseconds (10));
                }
            }

            const std::shared_ptr<Claims> claims;
            const std::string id;
            const std::string wanted;
            CNTV2Card card;
            bool claimed = false;
            bool subscribed = false;
            bool capturing = false;
            NTV2Channel channel = NTV2_CHANNEL1;
            NTV2Channel connector = NTV2_CHANNEL1;
            NTV2InputSource source = NTV2_INPUTSOURCE_SDI1;
            NTV2Buffer buffer;
            ULWord bytes = 0;
            std::thread reading;
            std::atomic<bool> stopping { false };

            std::mutex lock;
            std::vector<std::uint8_t> latest;
            modes::Format latestFormat;
            int latestRowBytes = 0;
            std::uint64_t latestSerial = 0;
            std::int64_t arrivedAt = 0;
            std::string trouble;
            std::string arrivingWord;
        };

        //  =====================================================================
        class AjaSystem final : public System
        {
        public:
            Found find() override
            {
                Found found;
                std::map<std::string, int> named;

                for (ULWord index = 0; index < ajaCards; ++index)
                {
                    CNTV2Card device;

                    if (! CNTV2DeviceScanner::GetDeviceAtIndex (index, device))
                        break;

                    auto& can = device.features();
                    Card one;
                    one.id = idOf (device, index);
                    one.name = NTV2DeviceIDToString (device.GetDeviceID(), true);
                    one.output = can.CanDoPlayback();
                    one.input = can.CanDoCapture();

                    const auto converts = can.GetNumCSCs() > 0 && can.CanDoFrameBufferFormat (NTV2_FBF_ARGB);
                    one.externalKey = one.output && converts && can.GetNumVideoOutputs() >= 2;
                    one.internalKey = one.output && one.input && converts && can.GetNumMixers() > 0;
                    one.tenBit = can.CanDoFrameBufferFormat (NTV2_FBF_10BIT_YCBCR);
                    one.audioChannels = std::min (16, static_cast<int> (can.GetMaxAudioChannels()));

                    for (const auto& [video, word] : wordsOf (device))
                    {
                        Mode offered;
                        offered.word = word;
                        offered.name = NTV2VideoFormatToString (video, true);
                        offered.format = *formatOf (video);
                        offered.output = one.output;
                        offered.input = one.input;
                        one.modes.push_back (std::move (offered));
                    }

                    ++named[one.name];
                    found.cards.push_back (std::move (one));
                }

                //  Two of a model told apart as Desktop Video does: Kona 5 (1), Kona 5 (2).
                std::map<std::string, int> numbered;

                for (auto& one : found.cards)
                    if (named[one.name] > 1)
                        one.name += " (" + std::to_string (++numbered[one.name]) + ")";

                return found;
            }

            std::unique_ptr<Output> openOutput (const std::string& id, const OutputSettings& settings, std::string& why) override
            {
                auto output = std::make_unique<AjaOutput> (claims, id);
                return output->open (settings, why) ? std::move (output) : nullptr;
            }

            std::unique_ptr<Input> openInput (const std::string& id, const std::string& modeWord, std::string& why) override
            {
                auto input = std::make_unique<AjaInput> (claims, id, modeWord.empty() ? std::string (modes::automatic) : modeWord);
                return input->open (why) ? std::move (input) : nullptr;
            }

        private:
            const std::shared_ptr<Claims> claims = std::make_shared<Claims>();
        };
    }

    std::unique_ptr<System> makeAjaSystem()
    {
        return std::make_unique<AjaSystem>();
    }

    std::vector<std::string> ajaFormatWords()
    {
        std::vector<std::string> out;
        std::set<std::string> seen;

        for (int value = 0; value < static_cast<int> (NTV2_MAX_NUM_VIDEO_FORMATS); ++value)
            if (const auto format = formatOf (static_cast<NTV2VideoFormat> (value)))
                if (auto word = modes::wordFor (*format); ! word.empty() && seen.insert (word).second)
                    out.push_back (std::move (word));

        return out;
    }
}
