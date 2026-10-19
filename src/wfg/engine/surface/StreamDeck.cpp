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

#include <wfg/engine/surface/StreamDeck.h>

#include <wfg/engine/surface/DeckPainter.h>
#include <wfg/engine/surface/HidLibrary.h>

#include <juce_core/juce_core.h>

#include <hidapi/hidapi/hidapi.h>

/*  The process list, for Elgato's app: Windows' own, after JUCE, in this one
    translation unit. */
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <tlhelp32.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace wfg::surface
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto lookEvery = std::chrono::seconds { 1 };
        constexpr auto appLookEvery = std::chrono::seconds { 3 };
        constexpr int quietReadMs = 20;
        constexpr std::size_t readsAPass = 8;       // reports drained from one deck before the next
        constexpr std::size_t picturesAPass = 2;    // pictures sent to one deck before reading again

        std::string narrowed (const wchar_t* text)
        {
            if (text == nullptr)
                return {};

            return juce::String (text).toStdString();
        }

       #if JUCE_WINDOWS
        juce::StringArray elgatoProcesses()
        {
            const char* names[] { "StreamDeck.exe", "Stream Deck.exe" };
            juce::StringArray found;

            const auto snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);

            if (snapshot == INVALID_HANDLE_VALUE)
                return found;

            PROCESSENTRY32W entry {};
            entry.dwSize = sizeof (entry);

            if (Process32FirstW (snapshot, &entry))
            {
                do
                {
                    const juce::String exe (entry.szExeFile);

                    for (const auto* name : names)
                        if (exe.equalsIgnoreCase (name))
                            found.addIfNotAlreadyThere (exe);
                }
                while (Process32NextW (snapshot, &entry));
            }

            CloseHandle (snapshot);
            return found;
        }
       #endif

        /*  WHAT A DECK IS ASKED TO SHOW, not yet sent: the newest a key. */
        struct Owed
        {
            std::map<int, KeyPicture> keys;
            std::map<int, ZonePicture> zones;
            std::optional<ScreenPicture> screen;
            std::optional<int> brightness;

            bool empty() const
            {
                return keys.empty() && zones.empty() && ! screen.has_value() && ! brightness.has_value();
            }
        };

        /*  One thing to send, taken out from under the lock. */
        struct Job
        {
            enum class Kind { none, key, zone, screen, brightness };

            Kind kind = Kind::none;
            int index = 0;
            KeyPicture key;
            ZonePicture zone;
            ScreenPicture screen;
            int brightness = 0;
        };
    }

    //==============================================================================
    struct StreamDecks::State
    {
        struct Open
        {
            DeckSpec spec;
            hid_device* device = nullptr;
            std::vector<std::uint8_t> buffer;
        };

        struct Seen
        {
            DeckSpec spec;
            std::vector<std::string> paths;     // one deck may list more than one interface
        };

        mutable std::mutex guard;
        std::condition_variable wake;

        bool stopping = false;
        std::set<std::string> wanted;
        std::vector<Plugged> listed;
        std::vector<DeckEvent> events;
        std::map<std::string, Owed> owed;
        bool app = false;

        //  The reader's own, never touched by another thread.
        std::map<std::string, Open> open;
        std::map<std::string, Seen> seen;
        std::set<std::string> busy;
        Clock::time_point lookAt {};
        Clock::time_point appAt {};

        /*  LOOK NOW, asked by another thread: the show wants another deck, or
            Elgato's app was just closed. */
        std::atomic<bool> lookNow { false };

        std::atomic<bool> running { false };
        std::atomic<bool> killing { false };
        std::thread reader;
        std::thread killer;

        //======================================================================
        void push (DeckEvent event)
        {
            const std::lock_guard<std::mutex> held { guard };
            events.push_back (std::move (event));
        }

        void publishListing()
        {
            std::vector<Plugged> now;

            for (const auto& [serial, deck] : seen)
            {
                Plugged plugged;
                plugged.serial = serial;
                plugged.spec = deck.spec;
                plugged.open = open.count (serial) != 0;
                plugged.busy = busy.count (serial) != 0;
                now.push_back (std::move (plugged));
            }

            const std::lock_guard<std::mutex> held { guard };
            listed = std::move (now);
        }

        /*  EVERY DECK ON THE MACHINE, by serial: Elgato's vendor, a product this
            table knows. A deck that gives no serial is known by its path. */
        void look()
        {
            std::map<std::string, Seen> found;
            auto* const list = hid_enumerate (deckVendor, 0x0000);

            for (auto* at = list; at != nullptr; at = at->next)
            {
                const auto spec = deckSpecFor (at->product_id);

                if (! spec.has_value() || at->path == nullptr)
                    continue;

                auto serial = narrowed (at->serial_number);

                if (serial.empty())
                    serial = std::string ("path:") + at->path;

                auto& seenDeck = found[serial];
                seenDeck.spec = *spec;
                seenDeck.paths.emplace_back (at->path);
            }

            hid_free_enumeration (list);
            seen = std::move (found);

            //  A deck that has gone is no longer busy; one that is back may open now.
            for (auto at = busy.begin(); at != busy.end();)
                at = seen.count (*at) == 0 ? busy.erase (at) : std::next (at);
        }

        bool sendFeature (Open& deck, const DeckBytes& report)
        {
            return ! report.empty() && hid_send_feature_report (deck.device, report.data(), report.size()) >= 0;
        }

        bool sendReports (Open& deck, const std::vector<DeckBytes>& reports)
        {
            for (const auto& report : reports)
                if (hid_write (deck.device, report.data(), report.size()) < 0)
                    return false;

            return true;
        }

        void openWanted (const std::set<std::string>& want)
        {
            for (const auto& serial : want)
            {
                if (open.count (serial) != 0)
                    continue;

                const auto found = seen.find (serial);

                if (found == seen.end())
                    continue;

                hid_device* device = nullptr;

                for (const auto& path : found->second.paths)
                    if ((device = hid_open_path (path.c_str())) != nullptr)
                        break;

                if (device == nullptr)
                {
                    busy.insert (serial);
                    continue;
                }

                busy.erase (serial);
                hid_set_nonblocking (device, 0);

                Open deck;
                deck.spec = found->second.spec;
                deck.device = device;
                deck.buffer.assign (1024, 0);

                //  Arrives knowing nothing: reset, and the bridge draws it all again.
                sendFeature (deck, deckResetReport (deck.spec));

                {
                    const std::lock_guard<std::mutex> held { guard };
                    owed.erase (serial);
                }

                open.emplace (serial, std::move (deck));

                DeckEvent event;
                event.kind = DeckEvent::Kind::opened;
                event.serial = serial;
                push (std::move (event));
            }
        }

        void close (const std::string& serial, bool reset)
        {
            const auto found = open.find (serial);

            if (found == open.end())
                return;

            //  Leaves knowing nothing: Elgato's own picture again.
            if (reset)
                sendFeature (found->second, deckResetReport (found->second.spec));

            hid_close (found->second.device);
            open.erase (found);

            {
                const std::lock_guard<std::mutex> held { guard };
                owed.erase (serial);
            }

            DeckEvent event;
            event.kind = DeckEvent::Kind::closed;
            event.serial = serial;
            push (std::move (event));
        }

        /*  A deck's reports drained: true while it still answers. */
        bool readFrom (const std::string& serial, Open& deck, int waitMs)
        {
            for (std::size_t pass = 0; pass < readsAPass; ++pass)
            {
                const auto length = std::min (deck.buffer.size(), deckInputLength (deck.spec) + 64);
                const auto read = hid_read_timeout (deck.device, deck.buffer.data(), length, pass == 0 ? waitMs : 0);

                if (read < 0)
                    return false;

                if (read == 0)
                    return true;

                const auto input = readDeckReport (deck.spec, deck.buffer.data(), static_cast<std::size_t> (read));

                if (input.kind == DeckInput::Kind::none)
                    continue;

                DeckEvent event;
                event.kind = DeckEvent::Kind::input;
                event.serial = serial;
                event.input = input;
                push (std::move (event));
            }

            return true;
        }

        Job nextJob (const std::string& serial)
        {
            const std::lock_guard<std::mutex> held { guard };
            const auto found = owed.find (serial);
            Job job;

            if (found == owed.end())
                return job;

            auto& debt = found->second;

            if (debt.brightness.has_value())
            {
                job.kind = Job::Kind::brightness;
                job.brightness = *debt.brightness;
                debt.brightness.reset();
            }
            else if (! debt.keys.empty())
            {
                job.kind = Job::Kind::key;
                job.index = debt.keys.begin()->first;
                job.key = std::move (debt.keys.begin()->second);
                debt.keys.erase (debt.keys.begin());
            }
            else if (! debt.zones.empty())
            {
                job.kind = Job::Kind::zone;
                job.index = debt.zones.begin()->first;
                job.zone = std::move (debt.zones.begin()->second);
                debt.zones.erase (debt.zones.begin());
            }
            else if (debt.screen.has_value())
            {
                job.kind = Job::Kind::screen;
                job.screen = std::move (*debt.screen);
                debt.screen.reset();
            }

            if (debt.empty())
                owed.erase (found);

            return job;
        }

        /*  One picture drawn and sent, outside the lock: true while it answers. */
        bool send (Open& deck, const Job& job)
        {
            const auto& spec = deck.spec;

            switch (job.kind)
            {
                case Job::Kind::none:
                    return true;

                case Job::Kind::brightness:
                    return sendFeature (deck, deckBrightnessReport (spec, job.brightness));

                case Job::Kind::key:
                {
                    if (spec.image == DeckImage::none)
                        return true;

                    const auto bytes = painter::keyBytes (spec, painter::key (job.key, spec.keyPixels));
                    return sendReports (deck, deckKeyPackets (spec, job.index, bytes));
                }

                case Job::Kind::zone:
                {
                    if (spec.model != DeckModel::plus || spec.dials <= 0)
                        return true;

                    const auto width = spec.stripWidth / spec.dials;
                    const auto bytes = painter::stripBytes (spec, painter::zone (job.zone, width, spec.stripHeight));
                    return sendReports (deck, deckStripPackets (spec, job.index * width, 0, width,
                                                                spec.stripHeight, bytes));
                }

                case Job::Kind::screen:
                {
                    if (spec.model != DeckModel::neo)
                        return true;

                    const auto bytes = painter::stripBytes (spec, painter::screen (job.screen, spec.stripWidth,
                                                                                    spec.stripHeight));
                    return sendReports (deck, deckScreenPackets (spec, bytes));
                }
            }

            return true;
        }

        //======================================================================
        void run()
        {
            const HidLibrary hid;

            for (;;)
            {
                std::set<std::string> want;
                bool anyOwed = false;

                {
                    std::unique_lock<std::mutex> held { guard };

                    if (stopping)
                        break;

                    /*  NOTHING OPEN AND NOTHING DUE: wait, woken by `want`. The
                        decks are still looked for, so the settings can list them. */
                    if (open.empty() && Clock::now() < lookAt && ! lookNow.load())
                        wake.wait_for (held, std::chrono::milliseconds { 100 });

                    if (stopping)
                        break;

                    want = wanted;
                    anyOwed = ! owed.empty();
                }

                const auto now = Clock::now();

                if (lookNow.exchange (false) || now >= lookAt)
                {
                    lookAt = now + lookEvery;
                    look();

                    /*  ELGATO'S APP is looked for only while a deck is busy:
                        it is a walk of the process list. */
                    if (! busy.empty() && now >= appAt)
                    {
                        appAt = now + appLookEvery;
                        const auto appFound = StreamDecks::elgatoAppRunning();
                        const std::lock_guard<std::mutex> held { guard };
                        app = appFound;
                    }
                    else if (busy.empty())
                    {
                        const std::lock_guard<std::mutex> held { guard };
                        app = false;
                    }

                    //  A deck unplugged is closed without a word to it.
                    std::vector<std::string> gone;

                    for (const auto& [serial, deck] : open)
                        if (seen.count (serial) == 0)
                            gone.push_back (serial);

                    for (const auto& serial : gone)
                        close (serial, false);

                    openWanted (want);
                    publishListing();
                }

                //  Let go of what the show no longer asks for.
                std::vector<std::string> letGo;

                for (const auto& [serial, deck] : open)
                    if (want.count (serial) == 0)
                        letGo.push_back (serial);

                if (! letGo.empty())
                {
                    for (const auto& serial : letGo)
                        close (serial, true);

                    publishListing();
                }

                if (open.empty())
                    continue;

                //  Read every deck, quickly while pictures are owed; then send a few.
                const auto waitMs = anyOwed ? 0 : std::max (1, quietReadMs / static_cast<int> (open.size()));
                std::vector<std::string> failed;

                for (auto& [serial, deck] : open)
                {
                    if (! readFrom (serial, deck, waitMs))
                    {
                        failed.push_back (serial);
                        continue;
                    }

                    for (std::size_t sent = 0; sent < picturesAPass; ++sent)
                    {
                        const auto job = nextJob (serial);

                        if (job.kind == Job::Kind::none)
                            break;

                        if (! send (deck, job))
                        {
                            failed.push_back (serial);
                            break;
                        }
                    }
                }

                if (! failed.empty())
                {
                    for (const auto& serial : failed)
                        close (serial, false);

                    //  Looked for again on the next pass.
                    lookAt = Clock::now();
                    publishListing();
                }
            }

            std::vector<std::string> all;

            for (const auto& [serial, deck] : open)
                all.push_back (serial);

            for (const auto& serial : all)
                close (serial, true);
        }
    };

    //==============================================================================
    StreamDecks::StreamDecks() : state (std::make_unique<State>()) {}

    StreamDecks::~StreamDecks() { stop(); }

    bool StreamDecks::start()
    {
        if (state->running.exchange (true))
            return false;

        {
            const std::lock_guard<std::mutex> held { state->guard };
            state->stopping = false;
        }

        state->reader = std::thread ([this] { state->run(); });
        return true;
    }

    void StreamDecks::stop()
    {
        if (! state->running.load())
            return;

        {
            const std::lock_guard<std::mutex> held { state->guard };
            state->stopping = true;
        }

        state->wake.notify_all();

        if (state->reader.joinable())
            state->reader.join();

        if (state->killer.joinable())
            state->killer.join();

        state->running.store (false);
    }

    std::vector<DeckHost::Plugged> StreamDecks::plugged() const
    {
        const std::lock_guard<std::mutex> held { state->guard };
        return state->listed;
    }

    void StreamDecks::want (const std::vector<std::string>& serials)
    {
        const std::set<std::string> next (serials.begin(), serials.end());

        {
            const std::lock_guard<std::mutex> held { state->guard };

            if (state->wanted == next)
                return;

            state->wanted = next;
        }

        state->lookNow.store (true);   // opened at once, not at the next look

        state->wake.notify_all();
    }

    std::vector<DeckEvent> StreamDecks::drain()
    {
        const std::lock_guard<std::mutex> held { state->guard };
        return std::exchange (state->events, {});
    }

    void StreamDecks::key (const std::string& serial, int keyIndex, const KeyPicture& picture)
    {
        const std::lock_guard<std::mutex> held { state->guard };
        state->owed[serial].keys[keyIndex] = picture;
    }

    void StreamDecks::zone (const std::string& serial, int zoneIndex, const ZonePicture& picture)
    {
        const std::lock_guard<std::mutex> held { state->guard };
        state->owed[serial].zones[zoneIndex] = picture;
    }

    void StreamDecks::screen (const std::string& serial, const ScreenPicture& picture)
    {
        const std::lock_guard<std::mutex> held { state->guard };
        state->owed[serial].screen = picture;
    }

    void StreamDecks::brightness (const std::string& serial, int percent)
    {
        const std::lock_guard<std::mutex> held { state->guard };
        state->owed[serial].brightness = std::clamp (percent, 0, 100);
    }

    bool StreamDecks::appRunning() const
    {
        const std::lock_guard<std::mutex> held { state->guard };
        return state->app;
    }

    void StreamDecks::closeApp()
    {
        /*  ONE AT A TIME, and never waited for here: the caller is the tick
            thread, and a kill waits seconds for the operating system. */
        if (state->killing.exchange (true))
            return;

        if (state->killer.joinable())
            state->killer.join();

        state->killer = std::thread ([this]
        {
            StreamDecks::killElgatoApp();
            state->killing.store (false);
            state->lookNow.store (true);
            state->wake.notify_all();
        });
    }

    //==============================================================================
    bool StreamDecks::elgatoAppRunning()
    {
       #if JUCE_WINDOWS
        return ! elgatoProcesses().isEmpty();
       #elif JUCE_MAC
        juce::ChildProcess launchctl;

        if (! launchctl.start (juce::StringArray { "launchctl", "list", "com.elgato.StreamDeck" }))
            return false;

        launchctl.waitForProcessToFinish (2000);
        const auto output = launchctl.readAllProcessOutput();

        //  "PID\tStatus\tLabel", the PID "-" while it is not running.
        return output.isNotEmpty() && ! output.startsWith ("-");
       #else
        return false;
       #endif
    }

    bool StreamDecks::killElgatoApp()
    {
       #if JUCE_WINDOWS
        auto allKilled = true;

        for (const auto& name : elgatoProcesses())
        {
            juce::ChildProcess taskkill;

            if (taskkill.start ("taskkill /F /IM \"" + name + "\""))
                taskkill.waitForProcessToFinish (3000);
            else
                allKilled = false;
        }

        return allKilled;
       #elif JUCE_MAC
        auto ok = true;
        const auto plist = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                               .getChildFile ("Library/LaunchAgents/com.elgato.StreamDeck.plist")
                               .getFullPathName();

        //  Unloaded first: its launch agent would start it again.
        juce::ChildProcess launchctl;

        if (launchctl.start (juce::StringArray { "launchctl", "unload", plist }))
            launchctl.waitForProcessToFinish (3000);
        else
            ok = false;

        juce::ChildProcess killall;

        if (killall.start (juce::StringArray { "killall", "Stream Deck" }))
            killall.waitForProcessToFinish (3000);

        return ok;
       #else
        return true;
       #endif
    }
}
