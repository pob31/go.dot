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
    A DECKLINK CARD, THROUGH THE USER'S DESKTOP VIDEO (namespace draft §71).

    Written from the SDK's headers and its manual alone; none of its example
    code is in Go.dot (its agreement keeps the examples to the reader). No card
    has been on the bench that built this: the testers' probe (DL.10) is what
    first runs it against one.

    COM: every object here is reference-counted, released by Ref. The three
    systems differ only in how a string is handed back (BSTR, CFStringRef, a
    malloc'd char*), how a flag is (BOOL, bool) and how an interface id is
    compared; the adapters below hold those differences.
*/

#include <wfg/engine/video/render/Card.h>
#include <wfg/engine/video/render/DeckLinkApi.h>

#if defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

namespace wfg::video::card
{
    namespace
    {
       #if defined (_WIN32)
        using DLString = BSTR;
        using DLBool = BOOL;
        using DLInt = LONGLONG;

        std::string text (BSTR value)
        {
            if (value == nullptr)
                return {};

            const int length = WideCharToMultiByte (CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
            std::string out (static_cast<std::size_t> (length > 0 ? length - 1 : 0), '\0');

            if (length > 1)
                WideCharToMultiByte (CP_UTF8, 0, value, -1, out.data(), length, nullptr, nullptr);

            SysFreeString (value);
            return out;
        }
       #elif defined (__APPLE__)
        using DLString = CFStringRef;
        using DLBool = bool;
        using DLInt = int64_t;

        std::string text (CFStringRef value)
        {
            if (value == nullptr)
                return {};

            std::array<char, 512> buffer {};
            std::string out;

            if (CFStringGetCString (value, buffer.data(), static_cast<CFIndex> (buffer.size()), kCFStringEncodingUTF8))
                out = buffer.data();

            CFRelease (value);
            return out;
        }
       #else
        using DLString = const char*;
        using DLBool = bool;
        using DLInt = int64_t;

        std::string text (const char* value)
        {
            if (value == nullptr)
                return {};

            std::string out (value);
            std::free (const_cast<char*> (value));
            return out;
        }
       #endif

        /*  A COM reference, released when it goes. */
        template <typename Interface>
        class Ref
        {
        public:
            Ref() = default;
            explicit Ref (Interface* adopted) noexcept : held (adopted) {}
            Ref (const Ref&) = delete;
            Ref& operator= (const Ref&) = delete;
            Ref (Ref&& other) noexcept : held (other.held)            { other.held = nullptr; }
            Ref& operator= (Ref&& other) noexcept                      { if (this != &other) { reset(); held = other.held; other.held = nullptr; } return *this; }
            ~Ref()                                                      { reset(); }

            void reset() noexcept                                      { if (held != nullptr) held->Release(); held = nullptr; }
            Interface* get() const noexcept                            { return held; }
            Interface* operator->() const noexcept                     { return held; }
            explicit operator bool() const noexcept                    { return held != nullptr; }

            /*  Where an SDK call writes a new reference. */
            Interface** put() noexcept                                 { reset(); return &held; }

        private:
            Interface* held = nullptr;
        };

        /*  `from`, asked for another of its interfaces. */
        template <typename Interface, typename From>
        Ref<Interface> query (From* from, REFIID iid)
        {
            Ref<Interface> got;

            if (from != nullptr && from->QueryInterface (iid, reinterpret_cast<void**> (got.put())) != S_OK)
                got.reset();

            return got;
        }

        bool sameIid (const void* asked, const void* known)
        {
            return std::memcmp (asked, known, 16) == 0;
        }

        //  IUnknown's identifier, spelled out: Linux's header has it only as a
        //  C compound literal.
        constexpr std::array<unsigned char, 16> unknownIid { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

        constexpr const char* notInstalled = "Desktop Video is not installed: Blackmagic's drivers, from www.blackmagicdesign.com/support";

        Ref<IDeckLinkIterator> makeIterator()
        {
            Ref<IDeckLinkIterator> iterator;
           #if defined (_WIN32)
            if (CoCreateInstance (CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator,
                                  reinterpret_cast<void**> (iterator.put())) != S_OK)
                iterator.reset();
           #else
            iterator = Ref<IDeckLinkIterator> (CreateDeckLinkIteratorInstance());
           #endif
            return iterator;
        }

        /*  Desktop Video's version as it says it, and as a number to compare
            with the SDK's; nothing when it is not installed. */
        std::optional<DLInt> installedVersion (std::string& said)
        {
            Ref<IDeckLinkAPIInformation> information;
           #if defined (_WIN32)
            if (CoCreateInstance (CLSID_CDeckLinkAPIInformation, nullptr, CLSCTX_ALL, IID_IDeckLinkAPIInformation,
                                  reinterpret_cast<void**> (information.put())) != S_OK)
                return std::nullopt;
           #else
            information = Ref<IDeckLinkAPIInformation> (CreateDeckLinkAPIInformationInstance());
           #endif

            if (! information)
                return std::nullopt;

            DLInt version = 0;

            if (information->GetInt (BMDDeckLinkAPIVersion, &version) != S_OK)
                return std::nullopt;

            //  0xMMmmpp00: major, minor, point.
            std::array<char, 32> words {};
            std::snprintf (words.data(), words.size(), "%d.%d.%d", static_cast<int> ((version >> 24) & 0xff),
                           static_cast<int> ((version >> 16) & 0xff), static_cast<int> ((version >> 8) & 0xff));
            said = words.data();
            return version;
        }

        std::string idOf (IDeckLink* deckLink)
        {
            const auto attributes = query<IDeckLinkProfileAttributes> (deckLink, IID_IDeckLinkProfileAttributes);
            DLInt value = 0;
            std::array<char, 40> words {};

            if (attributes && attributes->GetInt (BMDDeckLinkPersistentID, &value) == S_OK)
                std::snprintf (words.data(), words.size(), "dl-%llx", static_cast<unsigned long long> (value));
            else if (attributes && attributes->GetInt (BMDDeckLinkTopologicalID, &value) == S_OK)
                std::snprintf (words.data(), words.size(), "dlt-%llx", static_cast<unsigned long long> (value));
            else
            {
                DLString name {};
                return deckLink->GetDisplayName (&name) == S_OK ? text (name) : std::string();
            }

            return words.data();
        }

        modes::Format formatOf (IDeckLinkDisplayMode* mode)
        {
            modes::Format format;
            BMDTimeValue duration = 0;
            BMDTimeScale scale = 0;

            format.width = static_cast<int> (mode->GetWidth());
            format.height = static_cast<int> (mode->GetHeight());

            if (mode->GetFrameRate (&duration, &scale) == S_OK)
            {
                format.frameDuration = static_cast<std::int64_t> (duration);
                format.timeScale = static_cast<std::int64_t> (scale);
            }

            const auto dominance = mode->GetFieldDominance();
            format.interlaced = dominance == bmdLowerFieldFirst || dominance == bmdUpperFieldFirst;
            return format;
        }

        /*  Every display mode an output or input offers, by its word, with the
            card's code for it. */
        template <typename Direction>
        std::vector<std::pair<Mode, BMDDisplayMode>> modesOf (Direction* direction, bool output)
        {
            std::vector<std::pair<Mode, BMDDisplayMode>> found;
            Ref<IDeckLinkDisplayModeIterator> iterator;

            if (direction == nullptr || direction->GetDisplayModeIterator (iterator.put()) != S_OK || ! iterator)
                return found;

            Ref<IDeckLinkDisplayMode> mode;

            while (iterator->Next (mode.put()) == S_OK && mode)
            {
                Mode offered;
                offered.format = formatOf (mode.get());
                offered.word = modes::wordFor (offered.format);
                DLString name {};
                offered.name = mode->GetName (&name) == S_OK ? text (name) : offered.word;
                offered.output = output;
                offered.input = ! output;

                if (! offered.word.empty())
                    found.emplace_back (std::move (offered), mode->GetDisplayMode());
            }

            return found;
        }

        Ref<IDeckLink> cardById (const std::string& id)
        {
            auto iterator = makeIterator();
            Ref<IDeckLink> deckLink;

            while (iterator && iterator->Next (deckLink.put()) == S_OK && deckLink)
                if (idOf (deckLink.get()) == id)
                    return deckLink;

            return {};
        }

        BMDPixelFormat bmdFormatOf (PixelFormat format)
        {
            switch (format)
            {
                case PixelFormat::yuv10: return bmdFormat10BitYUV;
                case PixelFormat::yuv8:  return bmdFormat8BitYUV;
                case PixelFormat::bgra8: break;
            }

            return bmdFormat8BitBGRA;
        }

        bool outputTakes (IDeckLinkOutput* output, BMDDisplayMode code, BMDPixelFormat format)
        {
            BMDDisplayMode actual {};
            DLBool supported {};
            return output->DoesSupportVideoMode (bmdVideoConnectionUnspecified, code, format, bmdNoVideoOutputConversion,
                                                 bmdSupportedVideoModeDefault, &actual, &supported) == S_OK
                && supported;
        }

        std::int64_t steadyNanos()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        //  =====================================================================

        class DeckLinkOutput;

        /*  The card's word that a frame was sent, on the driver's thread. */
        class OutputCallback final : public IDeckLinkVideoOutputCallback
        {
        public:
            explicit OutputCallback (DeckLinkOutput& ownerToUse) : owner (&ownerToUse) {}

            void forget()
            {
                const std::scoped_lock hold (lock);
                owner = nullptr;
            }

            HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted (IDeckLinkVideoFrame* frame, BMDOutputFrameCompletionResult result) override;

            HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override     { return S_OK; }

            HRESULT STDMETHODCALLTYPE QueryInterface (REFIID iid, LPVOID* to) override
            {
                if (sameIid (&iid, unknownIid.data()) || sameIid (&iid, &IID_IDeckLinkVideoOutputCallback))
                {
                    *to = static_cast<IDeckLinkVideoOutputCallback*> (this);
                    AddRef();
                    return S_OK;
                }

                *to = nullptr;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override      { return static_cast<ULONG> (++count); }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const auto left = --count;

                if (left == 0)
                    delete this;

                return static_cast<ULONG> (left);
            }

        private:
            std::atomic<int> count { 1 };
            std::mutex lock;
            DeckLinkOutput* owner;
        };

        class DeckLinkOutput final : public Output
        {
        public:
            DeckLinkOutput (Ref<IDeckLinkOutput> outputToUse, Mode modeToUse, PixelFormat formatToUse)
                : output (std::move (outputToUse)), chosen (std::move (modeToUse)), pixelFormat (formatToUse)
            {
            }

            /*  The frames, the callback and the sound: false and why when the
                card refuses. */
            bool prepare (const OutputSettings& settings, IDeckLink* deckLink, std::string& why)
            {
                int rowBytesNow = 0;

                if (output->RowBytesForPixelFormat (bmdFormatOf (pixelFormat), chosen.format.width, &rowBytesNow) != S_OK)
                    rowBytesNow = rowBytesFor (pixelFormat, chosen.format.width);

                rowBytes = rowBytesNow;

                for (auto& slot : slots)
                {
                    if (output->CreateVideoFrame (chosen.format.width, chosen.format.height, rowBytes, bmdFormatOf (pixelFormat),
                                                  bmdFrameFlagDefault, slot.frame.put()) != S_OK || ! slot.frame)
                    {
                        why = "the card would not make a frame of " + chosen.word;
                        return false;
                    }

                    slot.buffer = query<IDeckLinkVideoBuffer> (slot.frame.get(), IID_IDeckLinkVideoBuffer);

                    if (! slot.buffer)
                    {
                        why = "the card's frames cannot be written";
                        return false;
                    }
                }

                if (settings.key != Keying::off)
                {
                    keyer = query<IDeckLinkKeyer> (deckLink, IID_IDeckLinkKeyer);

                    if (! keyer || keyer->Enable (settings.key == Keying::external) != S_OK || keyer->SetLevel (255) != S_OK)
                    {
                        why = std::string ("the card has no ") + keyingWord (settings.key) + " keyer";
                        return false;
                    }
                }

                if (settings.audioChannels > 0)
                {
                    if (output->EnableAudioOutput (bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger,
                                                   static_cast<uint32_t> (settings.audioChannels),
                                                   bmdAudioOutputStreamTimestamped) != S_OK)
                    {
                        why = "the card would not embed " + std::to_string (settings.audioChannels) + " channels of sound";
                        return false;
                    }

                    soundOn = true;
                }

                callback = new OutputCallback (*this);

                if (output->SetScheduledFrameCompletionCallback (callback) != S_OK)
                {
                    why = "the card would not say when it sent a frame";
                    return false;
                }

                return true;
            }

            ~DeckLinkOutput() override
            {
                if (running)
                {
                    BMDTimeValue stoppedAt = 0;
                    output->StopScheduledPlayback (0, &stoppedAt, chosen.format.timeScale);
                }

                output->SetScheduledFrameCompletionCallback (nullptr);

                if (callback != nullptr)
                {
                    callback->forget();
                    callback->Release();
                }

                if (keyer)
                    keyer->Disable();

                if (soundOn)
                    output->DisableAudioOutput();

                output->DisableVideoOutput();
            }

            const Mode& mode() const override          { return chosen; }
            PixelFormat format() const override        { return pixelFormat; }

            Frame lend() override
            {
                const std::scoped_lock hold (lock);

                for (int i = 0; i < static_cast<int> (slots.size()); ++i)
                {
                    auto& slot = slots[static_cast<std::size_t> (i)];

                    if (slot.state != State::free)
                        continue;

                    void* bytes = nullptr;

                    if (slot.buffer->StartAccess (bmdBufferAccessWrite) != S_OK)
                        continue;

                    if (slot.buffer->GetBytes (&bytes) != S_OK || bytes == nullptr)
                    {
                        slot.buffer->EndAccess (bmdBufferAccessWrite);
                        continue;
                    }

                    slot.state = State::lent;
                    return { bytes, rowBytes, i };
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

                slot.buffer->EndAccess (bmdBufferAccessWrite);
                const auto duration = chosen.format.frameDuration;

                if (output->ScheduleVideoFrame (slot.frame.get(), index * duration, duration, chosen.format.timeScale) != S_OK)
                {
                    slot.state = State::free;
                    return false;
                }

                slot.state = State::scheduled;
                return true;
            }

            void giveBack (const Frame& frame) override
            {
                const std::scoped_lock hold (lock);

                if (frame.slot < 0 || frame.slot >= static_cast<int> (slots.size()))
                    return;

                auto& slot = slots[static_cast<std::size_t> (frame.slot)];

                if (slot.state == State::lent)
                {
                    slot.buffer->EndAccess (bmdBufferAccessWrite);
                    slot.state = State::free;
                }
            }

            int waiting() override
            {
                uint32_t buffered = 0;
                return output->GetBufferedVideoFrameCount (&buffered) == S_OK ? static_cast<int> (buffered) : 0;
            }

            bool start (std::int64_t first) override
            {
                if (running)
                    return false;

                running = output->StartScheduledPlayback (first * chosen.format.frameDuration, chosen.format.timeScale, 1.0) == S_OK;
                return running;
            }

            bool started() const override      { return running; }

            bool clock (std::int64_t& streamNanos, std::int64_t& nanos) override
            {
                if (! running)
                    return false;

                BMDTimeValue time = 0;
                double speed = 0.0;
                const auto before = steadyNanos();

                if (output->GetScheduledStreamTime (static_cast<BMDTimeScale> (1'000'000'000), &time, &speed) != S_OK || speed <= 0.0)
                    return false;

                nanos = before + (steadyNanos() - before) / 2;
                streamNanos = static_cast<std::int64_t> (time);
                return true;
            }

            Reference reference() override
            {
                BMDReferenceStatus status {};

                if (output->GetReferenceStatus (&status) != S_OK || (status & bmdReferenceNotSupportedByHardware) != 0)
                    return Reference::none;

                return (status & bmdReferenceLocked) != 0 ? Reference::locked : Reference::unlocked;
            }

            Completions completions() override
            {
                const std::scoped_lock hold (lock);
                return counted;
            }

            bool scheduleSound (const std::int32_t* samples, int frames, std::int64_t streamSample) override
            {
                if (! soundOn || frames <= 0)
                    return false;

                uint32_t written = 0;
                return output->ScheduleAudioSamples (const_cast<std::int32_t*> (samples), static_cast<uint32_t> (frames),
                                                     static_cast<BMDTimeValue> (streamSample), static_cast<BMDTimeScale> (48000),
                                                     &written) == S_OK
                    && static_cast<int> (written) == frames;
            }

            int soundWaiting() override
            {
                uint32_t buffered = 0;
                return soundOn && output->GetBufferedAudioSampleFrameCount (&buffered) == S_OK ? static_cast<int> (buffered) : 0;
            }

            std::string problem() override
            {
                const std::scoped_lock hold (lock);
                return lost;
            }

            /*  On the driver's thread: the frame is the card's no longer. */
            void completed (IDeckLinkVideoFrame* frame, BMDOutputFrameCompletionResult result)
            {
                const std::scoped_lock hold (lock);

                for (auto& slot : slots)
                    if (static_cast<IDeckLinkVideoFrame*> (slot.frame.get()) == frame)
                        slot.state = State::free;

                switch (result)
                {
                    case bmdOutputFrameCompleted:     ++counted.shown; break;
                    case bmdOutputFrameDisplayedLate: ++counted.late; break;
                    case bmdOutputFrameDropped:       ++counted.dropped; break;
                    case bmdOutputFrameFlushed:       break;
                    default:                          break;
                }
            }

        private:
            enum class State { free, lent, scheduled };

            struct Slot
            {
                Ref<IDeckLinkMutableVideoFrame> frame;
                Ref<IDeckLinkVideoBuffer> buffer;
                State state = State::free;
            };

            Ref<IDeckLinkOutput> output;
            Ref<IDeckLinkKeyer> keyer;
            const Mode chosen;
            const PixelFormat pixelFormat;
            int rowBytes = 0;
            bool soundOn = false;
            bool running = false;
            OutputCallback* callback = nullptr;

            std::mutex lock;
            std::array<Slot, 8> slots;
            Completions counted;
            std::string lost;
        };

        HRESULT STDMETHODCALLTYPE OutputCallback::ScheduledFrameCompleted (IDeckLinkVideoFrame* frame, BMDOutputFrameCompletionResult result)
        {
            const std::scoped_lock hold (lock);

            if (owner != nullptr)
                owner->completed (frame, result);

            return S_OK;
        }

        //  =====================================================================

        class DeckLinkInput;

        class InputCallback final : public IDeckLinkInputCallback
        {
        public:
            explicit InputCallback (DeckLinkInput& ownerToUse) : owner (&ownerToUse) {}

            void forget()
            {
                const std::scoped_lock hold (lock);
                owner = nullptr;
            }

            HRESULT STDMETHODCALLTYPE VideoInputFormatChanged (BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode* mode,
                                                               BMDDetectedVideoInputFormatFlags flags) override;
            HRESULT STDMETHODCALLTYPE VideoInputFrameArrived (IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket*) override;

            HRESULT STDMETHODCALLTYPE QueryInterface (REFIID iid, LPVOID* to) override
            {
                if (sameIid (&iid, unknownIid.data()) || sameIid (&iid, &IID_IDeckLinkInputCallback))
                {
                    *to = static_cast<IDeckLinkInputCallback*> (this);
                    AddRef();
                    return S_OK;
                }

                *to = nullptr;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override      { return static_cast<ULONG> (++count); }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const auto left = --count;

                if (left == 0)
                    delete this;

                return static_cast<ULONG> (left);
            }

        private:
            std::atomic<int> count { 1 };
            std::mutex lock;
            DeckLinkInput* owner;
        };

        class DeckLinkInput final : public Input
        {
        public:
            DeckLinkInput (Ref<IDeckLinkInput> inputToUse, bool detectsToUse)
                : input (std::move (inputToUse)), detects (detectsToUse)
            {
            }

            bool begin (BMDDisplayMode code, const std::string& word, std::string& why)
            {
                callback = new InputCallback (*this);

                if (input->SetCallback (callback) != S_OK)
                {
                    why = "the card would not hand over its frames";
                    return false;
                }

                const auto enabled = input->EnableVideoInput (code, bmdFormat8BitYUV,
                                                              detects ? bmdVideoInputEnableFormatDetection : bmdVideoInputFlagDefault);

                if (enabled == E_ACCESSDENIED)
                {
                    why = "another program has the card's input";
                    return false;
                }

                if (enabled != S_OK || input->StartStreams() != S_OK)
                {
                    why = "the card would not take in " + word;
                    return false;
                }

                const std::scoped_lock hold (lock);
                currentWord = word;
                return true;
            }

            ~DeckLinkInput() override
            {
                input->StopStreams();
                input->SetCallback (nullptr);

                if (callback != nullptr)
                {
                    callback->forget();
                    callback->Release();
                }

                input->DisableVideoInput();
            }

            bool newest (std::vector<std::uint8_t>& pixels, int& width, int& height, int& rowBytes, std::uint64_t& serial) override
            {
                const std::scoped_lock hold (lock);

                if (! arriving || frameSerial == serial)
                    return false;

                pixels = latest;
                width = latestWidth;
                height = latestHeight;
                rowBytes = latestRowBytes;
                serial = frameSerial;
                return true;
            }

            bool signal() override
            {
                const std::scoped_lock hold (lock);
                return arriving;
            }

            std::string modeWord() override
            {
                const std::scoped_lock hold (lock);
                return currentWord;
            }

            std::string problem() override
            {
                const std::scoped_lock hold (lock);
                return arriving ? std::string() : std::string ("no signal");
            }

            /*  On the driver's thread. */
            void formatChanged (IDeckLinkDisplayMode* mode)
            {
                if (mode == nullptr)
                    return;

                input->PauseStreams();
                input->EnableVideoInput (mode->GetDisplayMode(), bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
                input->FlushStreams();
                input->StartStreams();

                const std::scoped_lock hold (lock);
                currentWord = modes::wordFor (formatOf (mode));
            }

            void arrived (IDeckLinkVideoInputFrame* frame)
            {
                if (frame == nullptr)
                    return;

                if ((static_cast<std::uint32_t> (frame->GetFlags()) & static_cast<std::uint32_t> (bmdFrameHasNoInputSource)) != 0u)
                {
                    const std::scoped_lock hold (lock);
                    arriving = false;
                    return;
                }

                const auto buffer = query<IDeckLinkVideoBuffer> (frame, IID_IDeckLinkVideoBuffer);
                void* bytes = nullptr;

                if (! buffer || buffer->StartAccess (bmdBufferAccessRead) != S_OK)
                    return;

                const auto rows = static_cast<int> (frame->GetHeight());
                const auto rowBytes = static_cast<int> (frame->GetRowBytes());

                if (buffer->GetBytes (&bytes) == S_OK && bytes != nullptr && rows > 0 && rowBytes > 0)
                {
                    const std::scoped_lock hold (lock);
                    const auto* from = static_cast<const std::uint8_t*> (bytes);
                    latest.assign (from, from + static_cast<std::size_t> (rows) * static_cast<std::size_t> (rowBytes));
                    latestWidth = static_cast<int> (frame->GetWidth());
                    latestHeight = rows;
                    latestRowBytes = rowBytes;
                    arriving = true;
                    ++frameSerial;
                }

                buffer->EndAccess (bmdBufferAccessRead);
            }

        private:
            Ref<IDeckLinkInput> input;
            const bool detects;
            InputCallback* callback = nullptr;

            std::mutex lock;
            std::vector<std::uint8_t> latest;
            int latestWidth = 0;
            int latestHeight = 0;
            int latestRowBytes = 0;
            std::uint64_t frameSerial = 0;
            bool arriving = false;
            std::string currentWord;
        };

        HRESULT STDMETHODCALLTYPE InputCallback::VideoInputFormatChanged (BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode* mode,
                                                                          BMDDetectedVideoInputFormatFlags)
        {
            const std::scoped_lock hold (lock);

            if (owner != nullptr)
                owner->formatChanged (mode);

            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE InputCallback::VideoInputFrameArrived (IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket*)
        {
            const std::scoped_lock hold (lock);

            if (owner != nullptr)
                owner->arrived (frame);

            return S_OK;
        }

        //  =====================================================================

        class DeckLinkSystem final : public System
        {
        public:
            Found find() override
            {
                const ThreadScope apartment;
                Found found;
                const auto version = installedVersion (found.version);

                if (! version)
                {
                    found.problem = notInstalled;
                    return found;
                }

                if (*version < static_cast<DLInt> (BLACKMAGIC_DECKLINK_API_VERSION))
                {
                    found.problem = "Desktop Video " + found.version + " is older than "
                                  + BLACKMAGIC_DECKLINK_API_VERSION_STRING + ", which Go.dot needs";
                    return found;
                }

                auto iterator = makeIterator();
                Ref<IDeckLink> deckLink;

                while (iterator && iterator->Next (deckLink.put()) == S_OK && deckLink)
                    found.cards.push_back (describe (deckLink.get()));

                return found;
            }

            std::unique_ptr<Output> openOutput (const std::string& id, const OutputSettings& settings, std::string& why) override
            {
                const ThreadScope apartment;
                const auto deckLink = cardById (id);

                if (! deckLink)
                {
                    why = "the card is not connected";
                    return nullptr;
                }

                auto output = query<IDeckLinkOutput> (deckLink.get(), IID_IDeckLinkOutput);

                if (! output)
                {
                    why = "the card cannot send pictures";
                    return nullptr;
                }

                std::optional<std::pair<Mode, BMDDisplayMode>> chosen;

                for (auto& offered : modesOf (output.get(), true))
                    if (offered.first.word == settings.mode)
                        chosen = std::move (offered);

                if (! chosen)
                {
                    why = "the card cannot send " + settings.mode;
                    return nullptr;
                }

                //  10-bit YUV unless keyed (APD); BGRA where the card will not
                //  take 10-bit, and always for a key, which carries alpha.
                auto format = PixelFormat::yuv10;

                if (settings.key != Keying::off || ! settings.tenBit || ! outputTakes (output.get(), chosen->second, bmdFormat10BitYUV))
                    format = PixelFormat::bgra8;

                if (! outputTakes (output.get(), chosen->second, bmdFormatOf (format)))
                {
                    why = "the card cannot send " + settings.mode;
                    return nullptr;
                }

                const auto enabled = output->EnableVideoOutput (chosen->second, bmdVideoOutputFlagDefault);

                if (enabled == E_ACCESSDENIED)
                {
                    why = "another program has the card";
                    return nullptr;
                }

                if (enabled != S_OK)
                {
                    why = "the card would not start sending " + settings.mode;
                    return nullptr;
                }

                auto opened = std::make_unique<DeckLinkOutput> (std::move (output), chosen->first, format);

                if (! opened->prepare (settings, deckLink.get(), why))
                    return nullptr;

                return opened;
            }

            std::unique_ptr<Input> openInput (const std::string& id, const std::string& modeWord, std::string& why) override
            {
                const ThreadScope apartment;
                const auto deckLink = cardById (id);

                if (! deckLink)
                {
                    why = "the card is not connected";
                    return nullptr;
                }

                auto input = query<IDeckLinkInput> (deckLink.get(), IID_IDeckLinkInput);

                if (! input)
                {
                    why = "the card cannot take pictures in";
                    return nullptr;
                }

                const auto offered = modesOf (input.get(), false);

                if (offered.empty())
                {
                    why = "the card offers no format to take in";
                    return nullptr;
                }

                //  Found by the card (APM): any format to start, the card then
                //  says which it sees. Or the one asked for.
                auto chosen = offered.front();
                const bool automatic = modeWord == modes::automatic;

                if (! automatic)
                {
                    bool known = false;

                    for (const auto& mode : offered)
                        if (mode.first.word == modeWord)
                        {
                            chosen = mode;
                            known = true;
                        }

                    if (! known)
                    {
                        why = "the card cannot take in " + modeWord;
                        return nullptr;
                    }
                }

                const auto attributes = query<IDeckLinkProfileAttributes> (deckLink.get(), IID_IDeckLinkProfileAttributes);
                DLBool detects {};

                if (automatic && (! attributes || attributes->GetFlag (BMDDeckLinkSupportsInputFormatDetection, &detects) != S_OK || ! detects))
                {
                    why = "the card cannot find its input's format: choose one";
                    return nullptr;
                }

                auto opened = std::make_unique<DeckLinkInput> (std::move (input), automatic);

                if (! opened->begin (chosen.second, chosen.first.word, why))
                    return nullptr;

                return opened;
            }

        private:
            Card describe (IDeckLink* deckLink)
            {
                Card card;
                card.id = idOf (deckLink);
                DLString name {};
                card.name = deckLink->GetDisplayName (&name) == S_OK ? text (name) : card.id;

                if (const auto attributes = query<IDeckLinkProfileAttributes> (deckLink, IID_IDeckLinkProfileAttributes))
                {
                    DLInt io = 0;
                    DLBool flag {};

                    if (attributes->GetInt (BMDDeckLinkVideoIOSupport, &io) == S_OK)
                    {
                        card.output = (io & bmdDeviceSupportsPlayback) != 0;
                        card.input = (io & bmdDeviceSupportsCapture) != 0;
                    }

                    card.internalKey = attributes->GetFlag (BMDDeckLinkSupportsInternalKeying, &flag) == S_OK && flag;
                    flag = {};
                    card.externalKey = attributes->GetFlag (BMDDeckLinkSupportsExternalKeying, &flag) == S_OK && flag;

                    DLInt channels = 0;

                    if (attributes->GetInt (BMDDeckLinkMaximumAudioChannels, &channels) == S_OK)
                        card.audioChannels = static_cast<int> (channels);
                }

                if (const auto output = query<IDeckLinkOutput> (deckLink, IID_IDeckLinkOutput))
                    for (auto& [mode, code] : modesOf (output.get(), true))
                    {
                        card.tenBit = card.tenBit || outputTakes (output.get(), code, bmdFormat10BitYUV);
                        card.modes.push_back (std::move (mode));
                    }

                if (const auto input = query<IDeckLinkInput> (deckLink, IID_IDeckLinkInput))
                    for (auto& [mode, code] : modesOf (input.get(), false))
                    {
                        bool merged = false;

                        for (auto& known : card.modes)
                            if (known.word == mode.word)
                            {
                                known.input = true;
                                merged = true;
                            }

                        if (! merged)
                            card.modes.push_back (std::move (mode));
                    }

                return card;
            }
        };
    }

    std::unique_ptr<System> makeDeckLinkSystem()
    {
        return std::make_unique<DeckLinkSystem>();
    }
}
