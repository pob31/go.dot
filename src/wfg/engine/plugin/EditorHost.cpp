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

#include <wfg/engine/plugin/EditorHost.h>
#include <wfg/engine/plugin/ChildLaunch.h>
#include <wfg/engine/plugin/ProcessUtil.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <new>
#include <optional>
#include <thread>
#include <utility>

namespace wfg::plugin
{
    namespace
    {
        /** How long a helper may take to bring its plugin up before it is given up on. */
        constexpr std::uint32_t readyWithinMs = 30000;

        /** How many subjects' inserts are remembered for routing late events. */
        constexpr std::size_t remembered = 64;

        void writeText (char* into, std::size_t size, const std::string& text)
        {
            std::memset (into, 0, size);
            std::snprintf (into, size, "%s", text.c_str());
        }
    }

    struct EditorHost::Impl
    {
        explicit Impl (EditorSpec specToUse) : spec (std::move (specToUse)) {}

        ~Impl()
        {
            if (region != nullptr)
                region->shouldExit.store (1, std::memory_order_release);

            /*  A MOMENT TO LEAVE, then ended: the helper checks every ten
                milliseconds, and one hung inside its plugin never will. */
            for (int waited = 0; waited < 15 && child != nullptr && child->isRunning(); ++waited)
                std::this_thread::sleep_for (std::chrono::milliseconds (20));

            if (child != nullptr && child->isRunning())
                child->kill();

            child.reset();
            region = nullptr;
            mapping.reset();
            regionFile.deleteFile();
            descriptionFile.deleteFile();
        }

        bool makeRegion (std::string& why)
        {
            static std::atomic<int> opened { 0 };

            const juce::File folder { juce::String::fromUTF8 (spec.workFolder.c_str()) };
            folder.createDirectory();

            /*  Named by the entry, this process and a count, so a window
                reopened while the last helper is still leaving never meets
                its file. */
            regionFile = folder.getChildFile ("editor-" + juce::String (spec.pluginId) + "-"
                                              + juce::String (process::currentId()) + "-"
                                              + juce::String (++opened) + ".shm");

            const auto bytes = editor::regionBytes();

            {
                juce::FileOutputStream out (regionFile);

                if (! out.openedOk())
                {
                    why = "could not create " + regionFile.getFullPathName().toStdString();
                    return false;
                }

                out.setPosition (0);
                out.truncate();
                const std::vector<char> zeros (bytes, 0);
                out.write (zeros.data(), zeros.size());
            }

            mapping = std::make_unique<juce::MemoryMappedFile> (regionFile, juce::MemoryMappedFile::readWrite, false);

            if (mapping->getData() == nullptr || mapping->getSize() < bytes)
            {
                why = "could not map " + regionFile.getFullPathName().toStdString();
                mapping.reset();
                regionFile.deleteFile();
                return false;
            }

            region = new (mapping->getData()) editor::Region {};
            region->magic.store (editor::magic, std::memory_order_relaxed);
            region->version.store (editor::version, std::memory_order_relaxed);
            region->visible.store (1, std::memory_order_relaxed);
            region->layoutHash.store (editor::layoutHash(), std::memory_order_release);
            return true;
        }

        bool start (std::string& why)
        {
            if (! makeRegion (why))
                return false;

            std::vector<std::string> command;
            command.push_back (spec.launch.executable.empty()
                                   ? juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                         .getFullPathName().toStdString()
                                   : spec.launch.executable);

            for (const auto& word : spec.launch.leadingArgs)
                command.push_back (word);

            command.push_back ("--region=" + regionFile.getFullPathName().toStdString());
            command.push_back ("--plugin=" + spec.identifier);
            command.push_back ("--parent-pid=" + std::to_string (process::currentId()));
            command.push_back ("--sample-rate=" + std::to_string (spec.sampleRate));
            command.push_back ("--block-size=" + std::to_string (spec.blockSize));
            command.push_back ("--channels=" + std::to_string (spec.channels));

            if (! spec.name.empty())
                command.push_back ("--name=" + spec.name);

            if (! spec.presetPath.empty())
                command.push_back ("--preset=" + spec.presetPath);

            if (! spec.stateFolder.empty())
                command.push_back ("--state-folder=" + spec.stateFolder);

            command.push_back ("--plugin-id=" + spec.pluginId);

            if (! spec.descriptionXml.empty())
            {
                descriptionFile = regionFile.withFileExtension ("plugin.xml");
                descriptionFile.replaceWithText (juce::String::fromUTF8 (spec.descriptionXml.c_str()), false, false, "\n");
                command.push_back ("--description=" + descriptionFile.getFullPathName().toStdString());
            }

            if (spec.headless)
                command.push_back ("--no-window");

            child = std::make_unique<ChildLaunch>();

            if (! child->start (command))
            {
                child.reset();
                why = "could not start the editing helper " + command.front();
                return false;
            }

            /*  THE HELPER'S FIRST WINDOW MAY COME FORWARD: the window in front
                is Go.dot's, and it says so. */
            child->allowForeground();

            launchedAt = juce::Time::getMillisecondCounter();
            status = Status::starting;
            return true;
        }

        void poll()
        {
            if (region == nullptr || status == Status::failed || status == Status::ended)
                return;

            const auto running = child != nullptr && child->isRunning();

            if (status == Status::starting)
            {
                if (region->failed.load (std::memory_order_acquire) != 0)
                {
                    problem = std::string (region->problem, strnlenOf (region->problem));
                    fail();
                    return;
                }

                if (region->ready.load (std::memory_order_acquire) != 0)
                {
                    status = Status::open;
                }
                else if (! running)
                {
                    problem = "the editing helper left before its plugin was up";
                    fail();
                    return;
                }
                else if (juce::Time::getMillisecondCounter() - launchedAt > readyWithinMs)
                {
                    problem = "the plugin did not come up within thirty seconds";
                    fail();
                    return;
                }
            }

            if (status == Status::open && ! running)
            {
                status = Status::ended;
                return;
            }

            handOver();
        }

        void fail()
        {
            status = Status::failed;

            if (child != nullptr && child->isRunning())
                child->kill();
        }

        static std::size_t strnlenOf (const char* text, std::size_t size = editor::textChars)
        {
            return static_cast<std::size_t> (std::find (text, text + size, '\0') - text);
        }

        /*  THE WAITING SUBJECT, handed over once the helper has taken the
            last: live values first, tagged with the sequence this subject
            will carry, then the subject, then the sequence - so the helper
            never reconciles against values meant for a subject it has not
            taken yet. */
        void handOver()
        {
            if (region == nullptr || ! waiting.has_value())
                return;

            if (region->subjectTaken.load (std::memory_order_acquire) != published)
                return;

            const auto& s = *waiting;
            const auto seq = published + 1;
            const auto count = static_cast<int> (std::min<std::size_t> (s.values.size(), editor::maxParams));

            for (int i = 0; i < editor::maxParams; ++i)
            {
                const auto value = i < count ? s.values[static_cast<std::size_t> (i)] : editor::restsAtPreset;
                region->subject.values[i].store (value, std::memory_order_relaxed);
                region->live[i].store (value, std::memory_order_relaxed);
            }

            region->liveSubject.store (seq, std::memory_order_relaxed);
            region->liveSeq.fetch_add (1, std::memory_order_release);

            writeText (region->subject.cueId, sizeof (region->subject.cueId), s.cueId);
            writeText (region->subject.fxId, sizeof (region->subject.fxId), s.fxId);
            writeText (region->subject.title, sizeof (region->subject.title), s.title);
            writeText (region->subject.reason, sizeof (region->subject.reason), s.reason);
            writeText (region->subject.statePath, sizeof (region->subject.statePath), s.statePath);
            region->subject.greyed.store (s.greyed ? 1u : 0u, std::memory_order_relaxed);
            region->subject.valueCount.store (static_cast<std::uint32_t> (count), std::memory_order_relaxed);
            region->subjectSeq.store (seq, std::memory_order_release);

            published = seq;
            routes.emplace_back (seq, s.greyed ? std::string() : s.fxId);

            while (routes.size() > remembered)
                routes.pop_front();

            waiting.reset();
        }

        EditorSpec spec;
        std::unique_ptr<ChildLaunch> child;
        std::unique_ptr<juce::MemoryMappedFile> mapping;
        juce::File regionFile, descriptionFile;
        editor::Region* region = nullptr;

        Status status = Status::starting;
        std::string problem;
        std::uint32_t launchedAt = 0;

        std::optional<Subject> waiting;
        std::uint32_t published = 0;
        std::deque<std::pair<std::uint32_t, std::string>> routes;
    };

    //==============================================================================
    EditorHost::EditorHost (EditorSpec spec) : impl (std::make_unique<Impl> (std::move (spec))) {}
    EditorHost::~EditorHost() = default;

    bool EditorHost::start (std::string& why)                   { return impl->start (why); }
    EditorHost::Status EditorHost::status() const noexcept      { return impl->status; }
    const std::string& EditorHost::problem() const noexcept     { return impl->problem; }
    void EditorHost::poll()                                     { impl->poll(); }

    void EditorHost::setSubject (const Subject& subject)
    {
        impl->waiting = subject;
        impl->handOver();
    }

    void EditorHost::setLive (const std::vector<float>& values)
    {
        /*  A SUBJECT STILL WAITING TAKES THEM WITH IT; otherwise they are the
            published subject's, and the helper reconciles against them. */
        if (impl->waiting.has_value())
        {
            impl->waiting->values = values;
            return;
        }

        if (impl->region == nullptr)
            return;

        const auto count = static_cast<int> (std::min<std::size_t> (values.size(), editor::maxParams));

        for (int i = 0; i < editor::maxParams; ++i)
            impl->region->live[i].store (i < count ? values[static_cast<std::size_t> (i)] : editor::restsAtPreset,
                                         std::memory_order_relaxed);

        impl->region->liveSubject.store (impl->published, std::memory_order_relaxed);
        impl->region->liveSeq.fetch_add (1, std::memory_order_release);
    }

    void EditorHost::setVisible (bool shown)
    {
        if (impl->region != nullptr)
            impl->region->visible.store (shown ? 1u : 0u, std::memory_order_release);
    }

    void EditorHost::raise()
    {
        if (impl->child != nullptr)
            impl->child->allowForeground();

        if (impl->region != nullptr)
        {
            impl->region->visible.store (1, std::memory_order_relaxed);
            impl->region->raiseSeq.fetch_add (1, std::memory_order_release);
        }
    }

    std::vector<EditorHost::Event> EditorHost::drain()
    {
        std::vector<Event> out;

        if (impl->region == nullptr)
            return out;

        editor::Popped popped;

        while (editor::pop (*impl->region, popped))
            out.push_back ({ popped.kind, static_cast<int> (popped.index), popped.value, popped.subjectSeq });

        return out;
    }

    std::string EditorHost::fxIdFor (std::uint32_t subjectSeq) const
    {
        for (const auto& [seq, fxId] : impl->routes)
            if (seq == subjectSeq)
                return fxId;

        return {};
    }

    void EditorHost::leave (bool keepState)
    {
        if (impl->region != nullptr)
            impl->region->shouldExit.store (keepState ? 1u : 2u, std::memory_order_release);
    }

    std::optional<EditorHost::Capture> EditorHost::takeCapture()
    {
        auto* region = impl->region;

        if (region == nullptr)
            return std::nullopt;

        const auto seq = region->captureSeq.load (std::memory_order_acquire);

        if (seq == region->captureAck.load (std::memory_order_relaxed))
            return std::nullopt;

        Capture capture;
        capture.fxId.assign (region->captureFxId, Impl::strnlenOf (region->captureFxId, sizeof (region->captureFxId)));
        capture.stateFile.assign (region->captureFile, Impl::strnlenOf (region->captureFile, sizeof (region->captureFile)));

        const auto count = std::min<std::uint32_t> (region->captureCount.load (std::memory_order_relaxed),
                                                    static_cast<std::uint32_t> (editor::maxParams));

        for (std::uint32_t i = 0; i < count; ++i)
            capture.values.push_back (region->captureValues[i].load (std::memory_order_relaxed));

        region->captureAck.store (seq, std::memory_order_release);
        return capture;
    }

    std::int64_t EditorHost::pid() const noexcept
    {
        return impl->child != nullptr ? impl->child->pid() : 0;
    }

    void EditorHost::poke (int index, float value)
    {
        if (impl->region == nullptr)
            return;

        impl->region->pokeIndex.store (index, std::memory_order_relaxed);
        impl->region->pokeValue.store (value, std::memory_order_relaxed);
        impl->region->pokeSeq.fetch_add (1, std::memory_order_release);
    }

    float EditorHost::currentValue (int index) const
    {
        if (impl->region == nullptr || index < 0 || index >= editor::maxParams)
            return 0.0f;

        return impl->region->current[index].load (std::memory_order_relaxed);
    }

    int EditorHost::paramCount() const noexcept
    {
        return impl->region != nullptr ? static_cast<int> (impl->region->paramCount.load (std::memory_order_relaxed)) : 0;
    }

    std::uint32_t EditorHost::subjectTaken() const noexcept
    {
        return impl->region != nullptr ? impl->region->subjectTaken.load (std::memory_order_acquire) : 0;
    }

    void EditorHost::kill()
    {
        if (impl->child != nullptr)
            impl->child->kill();
    }

    std::uint32_t EditorHost::loadMicroseconds() const noexcept
    {
        return impl->region != nullptr ? impl->region->loadMicros.load (std::memory_order_relaxed) : 0;
    }
}
