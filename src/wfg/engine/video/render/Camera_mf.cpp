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
    CAMERAS ON WINDOWS, THROUGH MEDIA FOUNDATION (namespace draft §72, APS).

    Listed by MFEnumDeviceSources - a name and the symbolic link, which is the
    camera's identifier after `cam:` - and never activated to be listed (no
    light comes on). Opened, its native formats are read and kept, the one asked
    for (or `auto`'s) set, and a source reader with Media Foundation's own video
    processing turns whatever it sends - NV12, YUY2, MJPEG - into RGB32.

    READ ASYNCHRONOUSLY: each frame asked for in turn, and handed over on Media
    Foundation's own threads, so nothing of Go.dot's ever waits on a camera. A
    camera that opens and sends nothing - a virtual one given no picture - is
    quiet, says no signal (Camera.h), and is let go at once when its input goes:
    a synchronous read would have waited on it for ever, shut down or not.

    Windows's camera privacy switch refuses the activation: said in words.
*/

#include <wfg/engine/video/render/Camera.h>
#include <wfg/engine/video/render/Card.h>

#include <wfg/engine/video/VideoModes.h>

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#pragma comment (lib, "mf.lib")
#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "mfreadwrite.lib")
#pragma comment (lib, "mfuuid.lib")
#pragma comment (lib, "ole32.lib")

namespace wfg::video::camera
{
    namespace
    {
        template <typename Interface>
        class Ref
        {
        public:
            Ref() = default;
            Ref (const Ref&) = delete;
            Ref& operator= (const Ref&) = delete;
            ~Ref()                                    { reset(); }
            void reset() noexcept                     { if (held != nullptr) held->Release(); held = nullptr; }
            Interface* get() const noexcept           { return held; }
            Interface* operator->() const noexcept    { return held; }
            explicit operator bool() const noexcept   { return held != nullptr; }
            Interface** put() noexcept                { reset(); return &held; }

        private:
            Interface* held = nullptr;
        };

        std::string utf8 (const wchar_t* wide)
        {
            if (wide == nullptr)
                return {};

            const auto bytes = WideCharToMultiByte (CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);

            if (bytes <= 1)
                return {};

            std::string out (static_cast<std::size_t> (bytes - 1), '\0');
            WideCharToMultiByte (CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
            return out;
        }

        std::string attributeText (IMFActivate* device, const GUID& key)
        {
            WCHAR* text = nullptr;
            UINT32 length = 0;

            if (FAILED (device->GetAllocatedString (key, &text, &length)))
                return {};

            auto out = utf8 (text);
            CoTaskMemFree (text);
            return out;
        }

        /*  Media Foundation started once a process, the first time a camera
            is asked for, and kept. */
        bool started()
        {
            static const bool ok = SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_LITE));
            return ok;
        }

        /*  Every camera the system lists, as activation objects, each
            released by the caller. */
        struct Devices
        {
            IMFActivate** list = nullptr;
            UINT32 count = 0;

            Devices()
            {
                Ref<IMFAttributes> attributes;

                if (FAILED (MFCreateAttributes (attributes.put(), 1))
                      || FAILED (attributes->SetGUID (MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))
                      || FAILED (MFEnumDeviceSources (attributes.get(), &list, &count)))
                {
                    list = nullptr;
                    count = 0;
                }
            }

            ~Devices()
            {
                for (UINT32 n = 0; n < count; ++n)
                    list[n]->Release();

                CoTaskMemFree (list);
            }

            Devices (const Devices&) = delete;
            Devices& operator= (const Devices&) = delete;
        };

        std::string formatOf (IMFMediaType* type)
        {
            UINT32 width = 0, height = 0, numerator = 0, denominator = 0;

            if (FAILED (MFGetAttributeSize (type, MF_MT_FRAME_SIZE, &width, &height))
                  || FAILED (MFGetAttributeRatio (type, MF_MT_FRAME_RATE, &numerator, &denominator)) || numerator == 0 || denominator == 0)
                return {};

            modes::Format format;
            format.width = static_cast<int> (width);
            format.height = static_cast<int> (height);
            format.frameDuration = static_cast<std::int64_t> (denominator);
            format.timeScale = static_cast<std::int64_t> (numerator);
            return modes::wordFor (format);
        }

        //======================================================================
        constexpr auto firstVideo = static_cast<DWORD> (MF_SOURCE_READER_FIRST_VIDEO_STREAM);

        /*  What an opened camera and its reader's callback share. The callback
            is COM's, held by the reader, and may outlive the camera by a
            moment; it asks for the next frame only while `reader` is set. */
        struct Shared
        {
            //  Set before the first frame is asked for, then only read.
            UINT32 width = 0;
            UINT32 height = 0;
            LONG defaultStride = 0;

            std::mutex lock;
            std::condition_variable changed;
            IMFSourceReader* reader = nullptr;    ///< not held: the reader holds the callback
            std::vector<std::uint8_t> latest;
            std::uint64_t frameSerial = 0;
            std::int64_t arrivedAt = 0;
            std::string trouble;
            bool flushed = false;
        };

        /*  THE READER'S CALLBACK: a frame turned BGRA, top row first, and the
            next asked for. One frame is asked at a time, so one call at a time. */
        class ReadCallback final : public IMFSourceReaderCallback
        {
        public:
            explicit ReadCallback (std::shared_ptr<Shared> sharedToUse) : shared (std::move (sharedToUse)) {}

            ReadCallback (const ReadCallback&) = delete;
            ReadCallback& operator= (const ReadCallback&) = delete;

            STDMETHODIMP QueryInterface (REFIID iid, void** out) override
            {
                if (out == nullptr)
                    return E_POINTER;

                if (iid == __uuidof (IUnknown) || iid == __uuidof (IMFSourceReaderCallback))
                {
                    *out = static_cast<IMFSourceReaderCallback*> (this);
                    AddRef();
                    return S_OK;
                }

                *out = nullptr;
                return E_NOINTERFACE;
            }

            STDMETHODIMP_ (ULONG) AddRef() override    { return static_cast<ULONG> (++count); }

            STDMETHODIMP_ (ULONG) Release() override
            {
                const auto left = --count;

                if (left == 0)
                    delete this;

                return static_cast<ULONG> (left);
            }

            STDMETHODIMP OnReadSample (HRESULT status, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override
            {
                if (FAILED (status) || (flags & MF_SOURCE_READERF_ERROR) != 0 || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                {
                    const std::scoped_lock hold (shared->lock);

                    if (shared->reader != nullptr)
                        shared->trouble = "the camera stopped: unplugged, or taken by another program";

                    return S_OK;
                }

                const auto taken = sample != nullptr && take (sample);
                const std::scoped_lock hold (shared->lock);

                if (taken)
                {
                    shared->latest.swap (frame);
                    ++shared->frameSerial;
                    shared->arrivedAt = nowNanos();
                    shared->trouble.clear();
                }

                if (shared->reader != nullptr && FAILED (shared->reader->ReadSample (firstVideo, 0, nullptr, nullptr, nullptr, nullptr)))
                    shared->trouble = "the camera stopped: unplugged, or taken by another program";

                return S_OK;
            }

            STDMETHODIMP OnFlush (DWORD) override
            {
                const std::scoped_lock hold (shared->lock);
                shared->flushed = true;
                shared->changed.notify_all();
                return S_OK;
            }

            STDMETHODIMP OnEvent (DWORD, IMFMediaEvent*) override    { return S_OK; }

        private:
            ~ReadCallback() = default;

            /*  Row by row into `frame`, the top first whichever way the rows
                run; the fourth byte of RGB32 means nothing, so it is made solid. */
            bool take (IMFSample* sample)
            {
                Ref<IMFMediaBuffer> buffer;

                if (FAILED (sample->ConvertToContiguousBuffer (buffer.put())))
                    return false;

                const auto width = shared->width;
                const auto height = shared->height;
                BYTE* first = nullptr;
                LONG pitch = 0;
                Ref<IMF2DBuffer> twoD;
                const auto hasTwoD = SUCCEEDED (buffer->QueryInterface (IID_PPV_ARGS (twoD.put())));

                if (hasTwoD)
                {
                    if (FAILED (twoD->Lock2D (&first, &pitch)))
                        return false;
                }
                else
                {
                    BYTE* data = nullptr;
                    DWORD length = 0;

                    if (FAILED (buffer->Lock (&data, nullptr, &length)))
                        return false;

                    pitch = shared->defaultStride;
                    first = pitch < 0 ? data + static_cast<std::ptrdiff_t> (-pitch) * static_cast<std::ptrdiff_t> (height - 1) : data;
                }

                const auto row = static_cast<std::size_t> (width) * 4;
                frame.resize (row * static_cast<std::size_t> (height));

                for (UINT32 y = 0; y < height; ++y)
                {
                    auto* to = frame.data() + static_cast<std::size_t> (y) * row;
                    std::memcpy (to, first + static_cast<std::ptrdiff_t> (y) * pitch, row);

                    for (std::size_t x = 3; x < row; x += 4)
                        to[x] = 255;
                }

                if (hasTwoD)
                    twoD->Unlock2D();
                else
                    buffer->Unlock();

                return true;
            }

            const std::shared_ptr<Shared> shared;
            std::atomic<long> count { 1 };
            std::vector<std::uint8_t> frame;
        };

        //======================================================================
        class MfSource final : public Source
        {
        public:
            MfSource (Ref<IMFMediaSource>&& sourceToUse, Ref<IMFSourceReader>&& readerToUse, std::shared_ptr<Shared> sharedToUse,
                      std::string wordToUse, std::vector<std::string> formatsToUse)
                : shared (std::move (sharedToUse)), word (std::move (wordToUse)), offered (std::move (formatsToUse))
            {
                *source.put() = sourceToUse.get();
                source->AddRef();
                *reader.put() = readerToUse.get();
                reader->AddRef();

                const std::scoped_lock hold (shared->lock);
                shared->reader = reader.get();

                if (FAILED (reader->ReadSample (firstVideo, 0, nullptr, nullptr, nullptr, nullptr)))
                    shared->trouble = "the camera would not be read";
            }

            ~MfSource() override
            {
                {
                    const std::scoped_lock hold (shared->lock);
                    shared->reader = nullptr;
                }

                //  What was asked cancelled, waited for no more than a second,
                //  then the camera let go.
                if (SUCCEEDED (reader->Flush (firstVideo)))
                {
                    std::unique_lock hold (shared->lock);
                    shared->changed.wait_for (hold, std::chrono::seconds (1), [this] { return shared->flushed; });
                }

                source->Shutdown();
            }

            bool newest (std::vector<std::uint8_t>& bgra, int& width, int& height, std::uint64_t& serial) override
            {
                const std::scoped_lock hold (shared->lock);

                if (shared->frameSerial == serial || shared->latest.empty())
                    return false;

                bgra = shared->latest;
                width = static_cast<int> (shared->width);
                height = static_cast<int> (shared->height);
                serial = shared->frameSerial;
                return true;
            }

            bool signal() override
            {
                const std::scoped_lock hold (shared->lock);
                return shared->trouble.empty() && arrivedLately (shared->arrivedAt);
            }

            std::string formatWord() override             { return word; }

            std::string problem() override
            {
                const std::scoped_lock hold (shared->lock);
                return shared->trouble;
            }

            std::vector<std::string> formats() override   { return offered; }

        private:
            Ref<IMFMediaSource> source;
            Ref<IMFSourceReader> reader;
            const std::shared_ptr<Shared> shared;
            const std::string word;
            const std::vector<std::string> offered;
        };

        //======================================================================
        class MfCameras final : public System
        {
        public:
            Found find() override
            {
                const card::ThreadScope apartment;
                Found found;

                if (! started())
                {
                    found.problem = "Media Foundation would not start on this machine";
                    return found;
                }

                const Devices devices;
                const std::scoped_lock hold (lock);

                for (UINT32 n = 0; n < devices.count; ++n)
                {
                    Camera camera;
                    camera.id = idPrefix + attributeText (devices.list[n], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
                    camera.name = attributeText (devices.list[n], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);

                    if (const auto known = formatsKnown.find (camera.id); known != formatsKnown.end())
                        camera.formats = known->second;

                    found.cameras.push_back (std::move (camera));
                }

                return found;
            }

            std::unique_ptr<Source> open (const std::string& id, const std::string& format, std::string& why) override
            {
                const card::ThreadScope apartment;

                if (! started())
                {
                    why = "Media Foundation would not start on this machine";
                    return nullptr;
                }

                const Devices devices;
                IMFActivate* chosen = nullptr;

                for (UINT32 n = 0; n < devices.count && chosen == nullptr; ++n)
                    if (idPrefix + attributeText (devices.list[n], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) == id)
                        chosen = devices.list[n];

                if (chosen == nullptr)
                {
                    why = "the camera is not connected";
                    return nullptr;
                }

                Ref<IMFMediaSource> source;
                const auto activated = chosen->ActivateObject (IID_PPV_ARGS (source.put()));

                if (activated == E_ACCESSDENIED)
                {
                    why = "Windows does not let Go.dot use the camera: allow it in Settings, Privacy & security, Camera";
                    return nullptr;
                }

                if (FAILED (activated) || ! source)
                {
                    why = "the camera would not open: another program may have it";
                    return nullptr;
                }

                Ref<IMFAttributes> settings;
                Ref<IMFSourceReader> reader;
                const auto shared = std::make_shared<Shared>();
                Ref<IMFSourceReaderCallback> callback;
                *callback.put() = new ReadCallback (shared);

                if (FAILED (MFCreateAttributes (settings.put(), 2))
                      || FAILED (settings->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))
                      || FAILED (settings->SetUnknown (MF_SOURCE_READER_ASYNC_CALLBACK, callback.get()))
                      || FAILED (MFCreateSourceReaderFromMediaSource (source.get(), settings.get(), reader.put())))
                {
                    source->Shutdown();
                    why = "the camera would not be read";
                    return nullptr;
                }

                //  ITS FORMATS, each by its word, an uncompressed one before
                //  MJPEG where both are offered at a size.
                std::vector<std::string> offered;
                std::map<std::string, DWORD> typeOf;
                std::map<std::string, bool> compressed;
                const auto stream = firstVideo;

                for (DWORD n = 0; ; ++n)
                {
                    Ref<IMFMediaType> type;

                    if (FAILED (reader->GetNativeMediaType (stream, n, type.put())))
                        break;

                    const auto word = formatOf (type.get());
                    GUID subtype {};
                    type->GetGUID (MF_MT_SUBTYPE, &subtype);
                    const auto jpeg = subtype == MFVideoFormat_MJPG;

                    if (word.empty())
                        continue;

                    if (typeOf.count (word) == 0)
                        offered.push_back (word);

                    if (typeOf.count (word) == 0 || (compressed[word] && ! jpeg))
                    {
                        typeOf[word] = n;
                        compressed[word] = jpeg;
                    }
                }

                {
                    const std::scoped_lock hold (lock);
                    formatsKnown[id] = offered;
                }

                const auto word = format == modes::automatic || format.empty() ? chooseAutomatic (offered) : format;
                const auto found = typeOf.find (word);

                if (found == typeOf.end())
                {
                    source->Shutdown();
                    why = word.empty() ? std::string ("the camera offers no format Go.dot reads") : "the camera does not offer " + word;
                    return nullptr;
                }

                Ref<IMFMediaType> native;
                Ref<IMFMediaType> wanted;

                if (FAILED (reader->GetNativeMediaType (stream, found->second, native.put()))
                      || FAILED (reader->SetCurrentMediaType (stream, nullptr, native.get()))
                      || FAILED (MFCreateMediaType (wanted.put()))
                      || FAILED (wanted->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video))
                      || FAILED (wanted->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_RGB32))
                      || FAILED (reader->SetCurrentMediaType (stream, nullptr, wanted.get())))
                {
                    source->Shutdown();
                    why = "the camera would not send " + word + " as colour";
                    return nullptr;
                }

                //  The size and the way its rows run, as the reader now sends it.
                Ref<IMFMediaType> current;

                if (FAILED (reader->GetCurrentMediaType (stream, current.put()))
                      || FAILED (MFGetAttributeSize (current.get(), MF_MT_FRAME_SIZE, &shared->width, &shared->height)))
                {
                    source->Shutdown();
                    why = "the camera would not say its size";
                    return nullptr;
                }

                UINT32 stride = 0;
                shared->defaultStride = SUCCEEDED (current->GetUINT32 (MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG> (stride)
                                                                                                       : static_cast<LONG> (shared->width) * 4;

                return std::make_unique<MfSource> (std::move (source), std::move (reader), shared, word, offered);
            }

        private:
            std::mutex lock;
            std::map<std::string, std::vector<std::string>> formatsKnown;
        };
    }

    std::unique_ptr<System> makeSystemCameras()
    {
        return std::make_unique<MfCameras>();
    }
}
