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

#include <wfg/engine/plugin/ChildLaunch.h>
#include <wfg/engine/plugin/ProcessUtil.h>     // <windows.h>, lean, on Windows

#include <juce_core/juce_core.h>

namespace wfg::plugin
{
    struct ChildLaunch::Impl
    {
        ~Impl() { close(); }

        void close()
        {
           #if JUCE_WINDOWS
            if (handle != nullptr)
            {
                ::CloseHandle (handle);
                handle = nullptr;
            }
           #else
            process.reset();
           #endif
        }

        std::int64_t pid = 0;

       #if JUCE_WINDOWS
        HANDLE handle = nullptr;
       #else
        std::unique_ptr<juce::ChildProcess> process;
       #endif
    };

    ChildLaunch::ChildLaunch() : impl (std::make_unique<Impl>()) {}
    ChildLaunch::~ChildLaunch() = default;

    bool ChildLaunch::start (const std::vector<std::string>& command)
    {
        impl->close();
        impl->pid = 0;

       #if JUCE_WINDOWS
        juce::String line;

        for (const auto& text : command)
        {
            const auto word = juce::String::fromUTF8 (text.c_str());

            if (line.isNotEmpty())
                line += " ";

            if (word.isEmpty() || word.containsAnyOf (" \t\""))
                line += "\"" + word.replace ("\"", "\\\"") + "\"";
            else
                line += word;
        }

        std::vector<wchar_t> mutableLine (line.toWideCharPointer(),
                                          line.toWideCharPointer() + line.length() + 1);

        STARTUPINFOW startup {};
        startup.cb = sizeof (startup);
        PROCESS_INFORMATION info {};

        if (! ::CreateProcessW (nullptr, mutableLine.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                nullptr, nullptr, &startup, &info))
            return false;

        ::CloseHandle (info.hThread);
        impl->handle = info.hProcess;
        impl->pid = static_cast<std::int64_t> (info.dwProcessId);
        return true;
       #else
        juce::StringArray words;

        for (const auto& text : command)
            words.add (juce::String::fromUTF8 (text.c_str()));

        impl->process = std::make_unique<juce::ChildProcess>();

        if (! impl->process->start (words, 0))
        {
            impl->process.reset();
            return false;
        }

        return true;
       #endif
    }

    bool ChildLaunch::isRunning() const
    {
       #if JUCE_WINDOWS
        return impl->handle != nullptr && ::WaitForSingleObject (impl->handle, 0) == WAIT_TIMEOUT;
       #else
        return impl->process != nullptr && impl->process->isRunning();
       #endif
    }

    void ChildLaunch::kill()
    {
       #if JUCE_WINDOWS
        if (impl->handle != nullptr)
            ::TerminateProcess (impl->handle, 1);
       #else
        if (impl->process != nullptr)
            impl->process->kill();
       #endif
    }

    std::int64_t ChildLaunch::pid() const noexcept
    {
        return impl->pid;
    }

    void ChildLaunch::allowForeground() const
    {
       #if JUCE_WINDOWS
        if (impl->pid > 0)
            ::AllowSetForegroundWindow (static_cast<DWORD> (impl->pid));
       #endif
    }
}
