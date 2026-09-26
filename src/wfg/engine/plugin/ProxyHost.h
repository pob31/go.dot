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

#pragma once

#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/plugin/ProxyLane.h>
#include <wfg/engine/plugin/SharedRegion.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/*
    ONE PLUGIN OF THE SET, ALIVE: the child process that hosts it on every
    voice, the region they share, and the judgement of whether it is answering
    (Phase 9a, §17.6). The message thread's object; nothing here is on the
    audio thread, which sees only the lanes.

    ITS LIFE. start() makes the region file, lays it out, binds every voice's
    lane to its part and launches the child; the entry is `loading`. poll(),
    every ten milliseconds, reads what the child has said - ready, and the
    entry is `loaded` with its latency and parameter count in the table; failed
    with a sentence, and it is `failed` - and watches the lanes: EIGHT
    CONSECUTIVE MISSES ON ANY LANE, OR A DEAD CHILD, MARKS THE ENTRY FAILED
    (plan decision 12) - every lane's call switched off so the proxy stops
    calling, the table told, and one `plugin.failed` handed to whoever asked
    to hear of it. So the cost of a failure is bounded: eight deadlines once,
    and nothing after. Then ONE RESTART ON ITS OWN, two seconds later, with a
    fresh child on the same region and the lanes' shadowed values re-sent; a
    second failure inside a minute stays failed until restart() is asked for.

    THE REGION OUTLIVES THE CHILD. A relaunch maps nothing anew: the lanes stay
    bound, the audio thread keeps its pointers, and the new child finds the
    same file. stop() is what takes the region away, and it is called before
    the plugins that hold the lanes are destroyed and with the audio stopped.

    NAMES NO JUCE TYPE IN THIS HEADER. The mapping, the child process and the
    clock are JUCE's, behind the Impl.
*/
namespace wfg::plugin
{
    /** How to start the child: which executable, and the words before the
        child's own options. Empty executable means this process's own. */
    struct ProxyLaunch
    {
        std::string executable;
        std::vector<std::string> leadingArgs { "plugin-host" };
    };

    struct ProxySpec
    {
        /** The document's Plugin id: what the table is keyed by. */
        std::string pluginId;

        /** JUCE's identifier, or `godot:test-gain`. */
        std::string identifier;
        std::string name;

        /** A preset file, or empty for the plugin's own defaults. */
        std::string presetPath;

        /*  The plugin's description as the scan recorded it, as XML, for the
            child to instantiate from (PR 9a.7). Empty for the test child;
            empty for anything else means this machine's scan does not know
            the identifier, and the entry reads `missing`. */
        std::string descriptionXml;

        /*  Asked for the description again at each start when the one above
            is empty (2026-09-26): so an entry that read `missing` comes up
            after a scan finds its plugin, without the show being reopened.
            May be empty; then `descriptionXml` is all there is. */
        std::function<std::string (const std::string& identifier)> describe;

        /** Where a child's catalogue report goes once it arrives; may be null. */
        CatalogueStore* catalogues = nullptr;

        /** One lane per voice. */
        int lanes = 0;
        int channels = 2;

        /** The widest block the graph will send: the block size. */
        int maxSamples = 0;
        int sampleRate = 48000;

        /** The spin's limit; nought means the rule - the smaller of 250 µs
            and a quarter of the block period. */
        std::int64_t deadlineMicroseconds = 0;

        /** Where the region file goes: the engine's own folder. */
        std::string regionFolder;

        ProxyLaunch launch;
    };

    class ProxyHost
    {
    public:
        using FailureHandler = std::function<void (const std::string& pluginId, const std::string& problem)>;
        using ChangeHandler = std::function<void()>;

        /** Milliseconds a child has to say it is ready. */
        static constexpr int readyTimeoutMs = 5000;

        /** Milliseconds before the one automatic restart. */
        static constexpr int restartDelayMs = 2000;

        /** A second failure inside this many milliseconds stays failed. */
        static constexpr int failureWindowMs = 60000;

        /*  `lanes` are the voices' lanes for this plugin, track order; the
            table is where the entry's state is written. Neither may go away
            before stop(). */
        ProxyHost (ProxySpec spec, std::vector<ProxyLane*> lanes, PluginTable* table);
        ~ProxyHost();

        ProxyHost (const ProxyHost&) = delete;
        ProxyHost& operator= (const ProxyHost&) = delete;

        /** Called once per failure, after the table has been written. */
        void onFailed (FailureHandler handler);

        /** Called whenever the table entry changed, so the tree can look again. */
        void onChanged (ChangeHandler handler);

        //======================================================================
        /** Makes the region, binds the lanes, launches the child. False, with
            a sentence, when the region could not be made or the child not
            started - and the entry reads `failed` with that sentence. */
        bool start (std::string& problem);

        /** Message thread, every ten milliseconds. */
        void poll();

        /** `plugin.restart`: a fresh child, from any state. */
        bool restart (std::string& problem);

        /** Tells the child to leave, waits a moment, ends it, unbinds the
            lanes and removes the region. Idempotent. */
        void stop();

        //======================================================================
        const std::string& pluginId() const noexcept;

        /** What the table holds for this entry. */
        PluginTable::Status status() const;

        std::string regionPath() const;
        bool childIsRunning() const;

        /** The child's pid, for the console's line; nought where the launcher
            does not say (not Windows) or before start(). */
        std::int64_t childPid() const noexcept;

        /** Ends the child as a task manager would: what a driver does to see
            the failed path. */
        void killChild();

        std::int64_t deadlineMicroseconds() const noexcept;

        /** The region's header, for a test to read the child's report. Null
            before start() and after stop(). */
        region::Header* header() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    /** The deadline rule (plan decision 12). */
    std::int64_t proxyDeadlineFor (int sampleRate, int blockSize, std::int64_t requestedMicroseconds) noexcept;
}
