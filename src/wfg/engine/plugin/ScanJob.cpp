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

#include <wfg/engine/plugin/ScanJob.h>

#include <wfg/engine/plugin/ChildLaunch.h>
#include <wfg/engine/plugin/PluginScan.h>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <chrono>
#include <thread>

namespace wfg::plugin
{
    struct ScanJob::Impl final : juce::Timer
    {
        Impl (std::string storageFolder, Launch launchToUse, ScanTable& tableToWrite, Finished finishedToCall)
            : storage (juce::String (storageFolder)),
              launch (std::move (launchToUse)),
              table (tableToWrite),
              finished (std::move (finishedToCall))
        {
        }

        ~Impl() override
        {
            stopTimer();

            /*  A SCAN STILL RUNNING WHEN THE SESSION ENDS is asked to stop
                between two plugins - which lets it write the list it has - and
                then put down: a plugin hung in the scan's own worker is the
                worker's to lose, and the scan kills that by pid on its way out.
                The wait is short; a session closing is not held for a scan. */
            if (child != nullptr && child->isRunning())
            {
                stopFile().replaceWithText ("stop\n");

                for (int waited = 0; waited < 40 && child->isRunning(); ++waited)
                    std::this_thread::sleep_for (std::chrono::milliseconds (25));

                if (child->isRunning())
                    child->kill();
            }

            stopFile().deleteFile();
        }

        juce::File pluginsFolder() const { return storage.getChildFile ("plugins"); }
        juce::File progressFile() const  { return pluginsFolder().getChildFile ("scan-progress.json"); }
        juce::File stopFile() const      { return pluginsFolder().getChildFile ("scan.stop"); }

        void start (const std::string& formatWord, const std::string& retryFile, const std::string& folder)
        {
            if (child != nullptr && child->isRunning())
                return;

            pluginsFolder().createDirectory();
            progressFile().deleteFile();
            stopFile().deleteFile();
            lastModified = {};

            std::vector<std::string> command { launch.executable };
            command.insert (command.end(), launch.leadingArgs.begin(), launch.leadingArgs.end());
            command.push_back ("plugins");
            command.push_back (formatWord.empty() ? std::string ("--scan") : "--scan=" + formatWord);
            command.push_back ("--engine-folder=" + storage.getFullPathName().toStdString());
            command.push_back ("--progress=" + progressFile().getFullPathName().toStdString());
            command.push_back ("--stop-file=" + stopFile().getFullPathName().toStdString());

            if (! retryFile.empty())
                command.push_back ("--retry=" + retryFile);

            if (! folder.empty())
                command.push_back ("--path=" + folder);

            child = std::make_unique<ChildLaunch>();

            if (! child->start (command))
            {
                child.reset();

                if (finished)
                    finished (0, 0, "the plugin scan could not be started");

                return;
            }

            startTimer (pollMs);
        }

        bool running() const
        {
            return child != nullptr && child->isRunning();
        }

        /*  THE PROGRESS FILE, each time it changes; and when the child has
            gone, the last of it said as a result. A file that reads
            `scanning` after the child has gone is a scan that died on the
            plugin it names - which the next scan's dead man's pedal will
            blacklist, as it does for one that took the command line down. */
        void timerCallback() override
        {
            const auto alive = running();
            const auto file = progressFile();

            if (file.getLastModificationTime() != lastModified || ! alive)
            {
                lastModified = file.getLastModificationTime();
                ScanProgress progress;

                if (readScanProgress (file.getFullPathName().toStdString(), progress))
                {
                    last = progress;
                    table.progress (progress.file, progress.done, progress.total, progress.found,
                                    static_cast<int> (progress.skipped.size()));
                }
            }

            if (alive)
                return;

            stopTimer();
            child.reset();

            std::string problem;

            if (last.state == "stopped")
                problem = "the scan was stopped before it was over";
            else if (last.state != "finished")
                problem = last.file.empty() ? std::string ("the scan ended before it said anything")
                                            : "the scan ended on " + last.file;

            if (finished)
                finished (last.found, static_cast<int> (last.skipped.size()), problem);

            last = {};
        }

        juce::File storage;
        Launch launch;
        ScanTable& table;
        Finished finished;

        std::unique_ptr<ChildLaunch> child;
        juce::Time lastModified;
        ScanProgress last;
    };

    ScanJob::ScanJob (std::string storageFolder, Launch launch, ScanTable& table, Finished finished)
        : impl (std::make_unique<Impl> (std::move (storageFolder), std::move (launch), table, std::move (finished)))
    {
    }

    ScanJob::~ScanJob() = default;

    void ScanJob::start (const std::string& formatWord, const std::string& retryFile, const std::string& folder)
    {
        impl->start (formatWord, retryFile, folder);
    }

    bool ScanJob::running() const
    {
        return impl->running();
    }
}
