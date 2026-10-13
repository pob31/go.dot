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

#include <wfg/engine/midi/MtcInbox.h>

#include <wfg/engine/document/ShowDocument.h>

#include <algorithm>

namespace wfg::midi
{
    bool isMtcMessage (const std::uint8_t* bytes, std::size_t size) noexcept
    {
        if (bytes == nullptr || size == 0)
            return false;

        if (bytes[0] == 0xF1)
            return size == static_cast<std::size_t> (timecode::mtcQuarterFrameSize);

        return size == static_cast<std::size_t> (timecode::mtcFullFrameSize)
                 && bytes[0] == 0xF0 && bytes[1] == 0x7F && bytes[3] == 0x01 && bytes[4] == 0x01
                 && bytes[9] == 0xF7;
    }

    //==============================================================================
    void MtcInbox::push (const std::string& portId, const std::uint8_t* bytes, std::size_t size, std::int64_t arrivalNanos)
    {
        if (bytes == nullptr || size == 0 || size > static_cast<std::size_t> (timecode::mtcFullFrameSize))
            return;

        const std::lock_guard<std::mutex> lock { guard };

        /*  FULL, THE OLDEST GOES: the reader behind it wants where the code is
            now, and a quarter frame from two seconds ago is a cycle it can no
            longer finish anyway. */
        if (held == capacity)
        {
            head = (head + 1) % capacity;
            --held;
            dropped.fetch_add (1, std::memory_order_relaxed);
        }

        auto& slot = ring[(head + held) % capacity];
        slot.portId = portId;
        std::copy (bytes, bytes + size, slot.bytes.begin());
        slot.size = size;
        slot.arrivalNanos = arrivalNanos;
        ++held;
    }

    void MtcInbox::takeAll (std::vector<MtcEntry>& into)
    {
        into.clear();

        const std::lock_guard<std::mutex> lock { guard };

        for (std::size_t n = 0; n < held; ++n)
            into.push_back (ring[(head + n) % capacity]);

        head = 0;
        held = 0;
    }

    //==============================================================================
    MtcInSetting mtcInOf (const doc::ShowDocument& document)
    {
        /*  A PORT THE SHOW DECLARES, or nothing - as `Runner::mtcPortOf` reads
            the port MTC leaves on: an empty row is no MTC input, and one naming
            something that is not a `<Port>` (a file edited by hand, a port
            deleted since) is none either. Whether a device is behind it is not
            asked: `--midi-play` feeds a port with none. */
        MtcInSetting setting;
        const auto port = document.getAttribute ("/godot/sync/mtcIn").value_or (std::string {});

        if (port.empty())
            return setting;

        if (const auto node = document.findById (port); ! node.isValid() || ! node.hasType ("Port"))
            return setting;

        setting.port = port;

        /*  ITS `rx`, which is on unless the show turned it off (AIM). A port
            that does not listen hears no code - which is what `rx` says it
            means; that it has never gated a MIDI port's TRIGGERS is an older
            gap, left as it is (see MidiInputs::route). */
        setting.listening = document.getAttribute ("/godot/port/" + port + "/rx")
                              .value_or (std::string ("true")) != "false";
        return setting;
    }

    //==============================================================================
    MtcInput::MtcInput()
    {
        taken.reserve (MtcInbox::capacity);
        heard.reserve (MtcInbox::capacity);
    }

    void MtcInput::configure (const MtcInSetting& settingNow)
    {
        if (settingNow == current)
            return;

        current = settingNow;
        reading.forget();
    }

    void MtcInput::feedClock (std::int64_t sample, std::int64_t nanos, int sampleRate) noexcept
    {
        reading.feedClock (sample, nanos, sampleRate);
    }

    void MtcInput::drain (MtcInbox& inbox)
    {
        inbox.takeAll (taken);
        heard.clear();

        for (const auto& entry : taken)
        {
            if (! current.listening || current.port.empty() || entry.portId != current.port)
            {
                ++ignoredSoFar;
                continue;
            }

            ++readSoFar;
            reading.read (entry.bytes.data(), entry.size, entry.arrivalNanos, heard);
        }
    }
}
