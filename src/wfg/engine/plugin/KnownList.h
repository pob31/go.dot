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

/*
    WHAT THIS MACHINE'S LAST SCAN FOUND, as a running serve holds it
    (2026-09-26): the plugins, each with the description a child makes it
    from, and the files the scan gave up on.

    ONE LIST FOR EVERY MODE. It used to be read off the hosted engine and
    nowhere else, so a show served on a real interface - the author's own
    way of working - offered nothing in its Plugins tab however much had
    been scanned. It is read from known.xml when serve starts, whatever the
    audio is, and read again when a scan finishes.

    The PluginTable's shape and for its reason: the message thread fills it
    (at start, and when a scan child exits), the tick thread reads it (the
    tree, rebuilding its document half), and the REVISION is what the tree
    compares at every publish, so a list that changes on one thread reaches a
    client without anyone marking the tree stale from a thread that must not.
    Nothing here is on the audio thread. Names no JUCE type.
*/

#include <wfg/engine/plugin/PluginScan.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wfg::plugin
{
    class KnownList
    {
    public:
        /** Replaces the list. Answers whether anything a reader could see
            changed - and only then does the revision move. */
        bool set (std::vector<KnownPlugin> pluginsNow, std::vector<std::string> skippedNow = {})
        {
            const std::lock_guard<std::mutex> lock { mutex };

            if (samePlugins (plugins, pluginsNow) && skipped == skippedNow)
                return false;

            plugins = std::move (pluginsNow);
            skipped = std::move (skippedNow);
            ++revisionCount;
            return true;
        }

        std::vector<KnownPlugin> all() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return plugins;
        }

        /** The files a scan gave up on, which the next scan skips. */
        std::vector<std::string> skippedFiles() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return skipped;
        }

        /** One plugin's description as XML, or empty when the list does not
            know the identifier. */
        std::string describe (const std::string& identifier) const
        {
            const std::lock_guard<std::mutex> lock { mutex };

            for (const auto& plugin : plugins)
                if (plugin.identifier == identifier)
                    return plugin.description;

            return {};
        }

        /** Moves with every change a reader could see. */
        std::uint64_t revision() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return revisionCount;
        }

    private:
        static bool samePlugins (const std::vector<KnownPlugin>& a, const std::vector<KnownPlugin>& b)
        {
            if (a.size() != b.size())
                return false;

            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i].identifier != b[i].identifier || a[i].name != b[i].name
                      || a[i].format != b[i].format || a[i].manufacturer != b[i].manufacturer
                      || a[i].path != b[i].path || a[i].description != b[i].description)
                    return false;

            return true;
        }

        mutable std::mutex mutex;
        std::vector<KnownPlugin> plugins;
        std::vector<std::string> skipped;
        std::uint64_t revisionCount = 0;
    };
}
