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

#include <wfg/engine/video/FfmpegInstall.h>

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/video/Ffmpeg.h>
#include <wfg/engine/video/PipedChild.h>

#include <juce_core/juce_core.h>

#include <vector>

namespace wfg::video::ffmpeg
{
    namespace
    {
        /*  ONE ARCHIVE TO FETCH: where from, and whether it is a zip or a
            tar.xz (which the system's `tar` unpacks). */
        struct Archive
        {
            const char* url;
            bool zip;
        };

        std::vector<Archive> archives()
        {
           #if JUCE_WINDOWS && (defined (_M_X64) || defined (__x86_64__))
            return { { "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip", true } };
           #elif JUCE_MAC && (defined (__arm64__) || defined (__aarch64__))
            return { { "https://ffmpeg.martin-riedl.de/redirect/latest/macos/arm64/release/ffmpeg.zip", true },
                     { "https://ffmpeg.martin-riedl.de/redirect/latest/macos/arm64/release/ffprobe.zip", true } };
           #elif JUCE_MAC
            return { { "https://ffmpeg.martin-riedl.de/redirect/latest/macos/amd64/release/ffmpeg.zip", true },
                     { "https://ffmpeg.martin-riedl.de/redirect/latest/macos/amd64/release/ffprobe.zip", true } };
           #elif JUCE_LINUX && (defined (__aarch64__))
            return { { "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-linuxarm64-gpl-8.1.tar.xz", false } };
           #elif JUCE_LINUX && (defined (__x86_64__))
            return { { "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-linux64-gpl-8.1.tar.xz", false } };
           #else
            return {};
           #endif
        }

       #if JUCE_WINDOWS
        constexpr const char* ffmpegName = "ffmpeg.exe";
        constexpr const char* ffprobeName = "ffprobe.exe";
       #else
        constexpr const char* ffmpegName = "ffmpeg";
        constexpr const char* ffprobeName = "ffprobe";
       #endif

        /*  The folder under `root` that holds both programs, however deep the
            archive put them. */
        juce::File folderHolding (const juce::File& root)
        {
            for (const auto& found : root.findChildFiles (juce::File::findFiles, true, ffmpegName))
                if (found.getSiblingFile (ffprobeName).existsAsFile())
                    return found.getParentDirectory();

            return {};
        }
    }

    std::string installFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Go.dot").getChildFile ("ffmpeg").getFullPathName().toStdString();
    }

    bool canDownload()
    {
        return ! archives().empty();
    }

    std::string downloadSource()
    {
        const auto all = archives();
        return all.empty() ? std::string {} : juce::URL (all.front().url).getDomain().toStdString();
    }

    Installer::~Installer()
    {
        stopping = true;

        if (thread.joinable())
            thread.join();
    }

    bool Installer::start()
    {
        if (! canDownload())
            return false;

        const std::lock_guard<std::mutex> held (lock);

        if (current.state == "downloading" || current.state == "unpacking" || current.state == "checking")
            return true;

        if (thread.joinable())
            thread.join();

        current = { "downloading", 0, {} };
        thread = std::thread ([this] { run(); });
        return true;
    }

    InstallStatus Installer::status() const
    {
        const std::lock_guard<std::mutex> held (lock);
        return current;
    }

    void Installer::set (const char* state, int percent, const std::string& problem)
    {
        const std::lock_guard<std::mutex> held (lock);
        current = { state, percent, problem };
    }

    void Installer::run()
    {
        const juce::File destination (juce::String::fromUTF8 ((into.empty() ? installFolder() : into).c_str()));
        const auto staging = destination.getSiblingFile ("ffmpeg-downloading");
        staging.deleteRecursively();

        if (! staging.createDirectory())
        {
            set ("failed", 0, "its folder could not be made");
            return;
        }

        const auto all = archives();
        auto index = 0;

        for (const auto& archive : all)
        {
            /*  FETCHED A PIECE AT A TIME, so how far it has got can be said and
                a closing Go.dot can stop it. */
            const auto file = staging.getChildFile ("archive-" + juce::String (index) + (archive.zip ? ".zip" : ".tar.xz"));
            int status = 0;
            juce::StringPairArray headers;

            auto in = juce::URL (archive.url).createInputStream (
                juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (20000)
                    .withNumRedirectsToFollow (10)
                    .withStatusCode (&status)
                    .withResponseHeaders (&headers));

            if (in == nullptr || status >= 400)
            {
                set ("failed", 0, "the download could not be reached (" + std::string (archive.url) + ")");
                staging.deleteRecursively();
                return;
            }

            const auto total = in->getTotalLength();
            juce::FileOutputStream out (file);

            if (! out.openedOk())
            {
                set ("failed", 0, "the download could not be saved");
                staging.deleteRecursively();
                return;
            }

            juce::HeapBlock<char> buffer (1 << 16);
            std::int64_t got = 0;

            for (;;)
            {
                if (stopping)
                {
                    staging.deleteRecursively();
                    return;
                }

                const auto read = in->read (buffer, 1 << 16);

                if (read <= 0)
                    break;

                out.write (buffer, static_cast<std::size_t> (read));
                got += read;

                const auto share = total > 0 ? static_cast<double> (got) / static_cast<double> (total) : 0.0;
                set ("downloading", static_cast<int> ((static_cast<double> (index) + share) * 100.0 / static_cast<double> (all.size())));
            }

            out.flush();

            if (got == 0 || (total > 0 && got < total))
            {
                set ("failed", 0, "the download stopped part way");
                staging.deleteRecursively();
                return;
            }

            /*  UNPACKED where it landed: a zip by JUCE, a tar.xz by the system. */
            set ("unpacking", 100);

            if (archive.zip)
            {
                juce::ZipFile zip (file);

                if (zip.getNumEntries() == 0 || zip.uncompressTo (staging, true).failed())
                {
                    set ("failed", 0, "the download would not unpack");
                    staging.deleteRecursively();
                    return;
                }
            }
            else
            {
                PipedChild tar;
                auto unpacked = tar.start ({ "/usr/bin/tar", "-xJf", file.getFullPathName().toStdString(),
                                             "-C", staging.getFullPathName().toStdString() });

                if (unpacked)
                {
                    tar.readAll();
                    unpacked = tar.wait (600000) == 0;
                }

                if (! unpacked)
                {
                    set ("failed", 0, "the download would not unpack (is xz installed?)");
                    staging.deleteRecursively();
                    return;
                }
            }

            file.deleteFile();
            ++index;
        }

        /*  THE TWO PROGRAMS, wherever the archives put them, into place - and
            run once each, which is the check a moving link allows. */
        set ("checking", 100);

        auto holding = folderHolding (staging);

        if (! holding.isDirectory())
        {
            //  The Mac's two zips each hold one program, side by side once unpacked.
            if (staging.getChildFile (ffmpegName).existsAsFile() && staging.getChildFile (ffprobeName).existsAsFile())
                holding = staging;
        }

        if (! holding.isDirectory())
        {
            set ("failed", 0, "the download held no ffmpeg and ffprobe");
            staging.deleteRecursively();
            return;
        }

       #if ! JUCE_WINDOWS
        holding.getChildFile (ffmpegName).setExecutePermission (true);
        holding.getChildFile (ffprobeName).setExecutePermission (true);
       #endif

        for (const auto* program : { ffmpegName, ffprobeName })
        {
            PipedChild check;
            auto runs = check.start ({ holding.getChildFile (program).getFullPathName().toStdString(), "-version" });

            if (runs)
            {
                check.readAll();
                runs = check.wait (30000) == 0;
            }

            if (! runs)
            {
                set ("failed", 0, std::string ("the downloaded ") + program + " would not run");
                staging.deleteRecursively();
                return;
            }
        }

        destination.deleteRecursively();

        if (! holding.moveFileTo (destination))
        {
            set ("failed", 0, "it could not be put in place");
            staging.deleteRecursively();
            return;
        }

        staging.deleteRecursively();
        set ("done", 100);
    }

    void registerInstallCommands (CommandRegistry& registry, Installer* installer)
    {
        registry.add ({ "ffmpeg.install",
                        "Downloads FFmpeg into Go.dot's own folder (namespace draft 37.5, WN): what converts and"
                        " previews a movie that is not HAP. Taken and ignored where nothing downloads.",
                        {},
                        false,
                        [installer] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (installer != nullptr && ! installer->start())
                                return Outcome::rejected (reason::badValue);

                            return Outcome::ok (args);
                        } });
    }
}
