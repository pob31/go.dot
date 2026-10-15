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

/*  A DECLARED MIDI PORT PUT ON ITS DEVICES - WHEN THE SHOW OPENS, AND AGAIN
    WHEN SOMEBODY CHANGES IT WHILE THE SHOW RUNS.

    Found by the author on 2026-09-25, with the D700 on the desk: two ports
    added in the MIDI tab, both halves of each set to the D700, and both read
    "unbound" for as long as the session lasted - with no sentence, because
    nothing had failed. Nothing had been tried. A port was put on its device
    once, as the show opened, and never again (Phase 6's M-B debt).

    THREE THREADS, EACH DOING ONLY WHAT IT MAY.
    - The TICK thread reads the document, so it says what the show now wants
      (`want`), and later takes what came of it (`take`) into the port table
      the tree and the surfaces read. It never opens a device: enumerating
      MIDI devices blocks for milliseconds on Windows, and GO never blocks.
    - The MESSAGE thread opens and closes devices (`rebind`), as it did at
      start, where the same work has always run.
    - The mailbox between them is two vectors under one short lock.

    ONLY WHAT CHANGED IS TOUCHED. A port whose devices are what they were
    keeps its open device, so renaming a port or editing another one never
    drops a byte on the wire. What changed is closed first, all of it, and
    opened second - two ports swapping devices would otherwise each find the
    other's device still held, and Windows MIDI is one program, one owner.

    THE IDENTIFIER MATCHED IS NOT WRITTEN BACK HERE. At start the engine
    writes it into `inputDeviceId` / `outputDeviceId` before the tick thread
    runs; mid-session that would be a document write outside a command. The
    name binds it now, and the next start writes the identifier down.
*/

#pragma once

#include <wfg/engine/midi/PortTable.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wfg::midi
{
    class MidiInputs;
    class MidiSender;

    /** What the show says one port should be put on. */
    struct PortWish
    {
        std::string id;
        std::string label;            ///< the port's name, for the sentences
        std::string inputDevice;
        std::string inputDeviceId;
        std::string outputDevice;
        std::string outputDeviceId;

        /** The same devices: a port whose only change is its name is not reopened. */
        bool sameDevices (const PortWish& other) const noexcept
        {
            return inputDevice == other.inputDevice && inputDeviceId == other.inputDeviceId
                && outputDevice == other.outputDevice && outputDeviceId == other.outputDeviceId;
        }

        bool operator== (const PortWish&) const = default;
    };

    /** What came of one port. */
    struct PortBound
    {
        std::string id;
        PortTable::Binding binding;
        std::string inputMatched;     ///< the identifiers found, for the caller to write down
        std::string outputMatched;
        bool gone = false;            ///< the show no longer declares it: forget it
    };

    class PortBinder
    {
    public:
        PortBinder (MidiInputs& inputs, MidiSender& outputs) noexcept;

        /*  At start, on the thread that opens devices, before the tick thread
            runs: every port put on its devices now, and remembered as done -
            with the identifiers matched, which the caller must write into the
            document as `inputDeviceId` / `outputDeviceId`. */
        std::vector<PortBound> bindAll (const std::vector<PortWish>& wishes);

        /*  TICK THREAD: what the show declares now. True when it differs from
            the last thing handed over - which is when the caller asks the
            message thread to `rebind`. */
        bool want (std::vector<PortWish> wishes);

        /** MESSAGE THREAD: the ports whose devices changed, closed and opened again. */
        void rebind();

        /*  TICK THREAD: what `rebind` finished since the last time, for the
            port table. Empty, with no lock taken, on every tick nothing did. */
        std::vector<PortBound> take();

        /*  MESSAGE THREAD, on `midi.rescan` (namespace draft §64): this
            machine's devices listed again, and the ports `portsToRetry` names
            closed and opened again - one with no device finds the device
            plugged in since, one whose device has gone says so. What came of
            it reaches the tick thread through `take`, and the lists through
            `takeDevices`. */
        void rescan();

        /** What one look found. */
        struct DeviceLists
        {
            std::vector<Device> inputs, outputs;
        };

        /*  TICK THREAD: the lists the last `rescan` found, once; nothing, with
            no lock taken, while none has finished. */
        std::optional<DeviceLists> takeDevices();

        /*  WHICH PORTS A RESCAN TRIES AGAIN - the whole rule, with no device
            opened, so a test can hand it lists. A port that is not bound and
            names a device; and a bound one none of whose devices is there any
            more, by identifier or by name. A bound port whose device is there
            is never touched, so a rescan in the middle of a show drops no byte
            on a working cable - not even when a second device of the same name
            has been plugged in beside it. */
        static std::vector<std::string> portsToRetry (const std::vector<PortWish>& applied,
                                                      const std::map<std::string, bool>& bound,
                                                      const std::vector<Device>& inputs,
                                                      const std::vector<Device>& outputs);

    private:
        PortBound bindOne (const PortWish& wish);
        void release (const std::string& portId);

        MidiInputs& inputs;
        MidiSender& outputs;

        std::mutex mailbox;
        std::vector<PortWish> wanted;         // under `mailbox`
        std::uint64_t wantedSeq = 0;          // under `mailbox`
        std::vector<PortBound> finished;      // under `mailbox`
        std::atomic<bool> anyFinished { false };
        std::optional<DeviceLists> scanned;   // under `mailbox`
        std::atomic<bool> anyScanned { false };

        std::vector<PortWish> handed;         // the tick thread's own
        std::vector<PortWish> applied;        // the message thread's own
        std::uint64_t appliedSeq = 0;         // the message thread's own
        std::map<std::string, bool> lastBound; // the message thread's own: what each port came to

        //  The message thread's own: what came of each port, remembered for the next rescan.
        void remember (const std::vector<PortBound>& results);
    };
}
