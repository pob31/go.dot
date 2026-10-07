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

#include <wfg/engine/video/PipedChild.h>
#include <wfg/engine/plugin/ProcessUtil.h>     // <windows.h>, lean, on Windows

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <thread>

#if ! defined (_WIN32)
 #include <cerrno>
 #include <fcntl.h>
 #include <spawn.h>
 #include <sys/wait.h>

extern char** environ;
#endif

namespace wfg::video
{
    struct PipedChild::Impl
    {
       #if defined (_WIN32)
        HANDLE process = nullptr;
        HANDLE output = nullptr;
       #else
        pid_t pid = 0;
        int output = -1;
       #endif

        std::atomic<bool> ended { false };
        int exitCode = -1;

        void closeOutput()
        {
           #if defined (_WIN32)
            if (output != nullptr)
            {
                ::CloseHandle (output);
                output = nullptr;
            }
           #else
            if (output >= 0)
            {
                ::close (output);
                output = -1;
            }
           #endif
        }

        void closeProcess()
        {
           #if defined (_WIN32)
            if (process != nullptr)
            {
                ::CloseHandle (process);
                process = nullptr;
            }
           #else
            pid = 0;
           #endif
        }
    };

    PipedChild::PipedChild() : impl (std::make_unique<Impl>()) {}

    PipedChild::~PipedChild()
    {
        if (isRunning())
        {
            kill();
            wait (5000);
        }

        impl->closeOutput();
        impl->closeProcess();
    }

    bool PipedChild::start (const std::vector<std::string>& command, const std::string& errorFile)
    {
        if (command.empty())
            return false;

        impl->closeOutput();
        impl->closeProcess();
        impl->ended = false;
        impl->exitCode = -1;

       #if defined (_WIN32)
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

        std::vector<wchar_t> mutableLine (line.toWideCharPointer(), line.toWideCharPointer() + line.length() + 1);

        SECURITY_ATTRIBUTES inheritable {};
        inheritable.nLength = sizeof (inheritable);
        inheritable.bInheritHandle = TRUE;

        HANDLE readEnd = nullptr, writeEnd = nullptr;

        if (! ::CreatePipe (&readEnd, &writeEnd, &inheritable, 1u << 20))
            return false;

        ::SetHandleInformation (readEnd, HANDLE_FLAG_INHERIT, 0);

        auto* input = ::CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                     OPEN_EXISTING, 0, nullptr);

        const auto errorPath = errorFile.empty() ? juce::String ("NUL") : juce::String::fromUTF8 (errorFile.c_str());
        auto* errors = ::CreateFileW (errorPath.toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &inheritable, errorFile.empty() ? OPEN_EXISTING : CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);

        const auto closeAll = [&]
        {
            for (auto* handle : { readEnd, writeEnd, input, errors })
                if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
                    ::CloseHandle (handle);
        };

        if (input == INVALID_HANDLE_VALUE || errors == INVALID_HANDLE_VALUE)
        {
            closeAll();
            return false;
        }

        /*  THESE THREE AND NO OTHER, whatever else this process holds. */
        SIZE_T listSize = 0;
        ::InitializeProcThreadAttributeList (nullptr, 1, 0, &listSize);
        std::vector<std::uint8_t> listBytes (listSize);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST> (listBytes.data());

        HANDLE handed[3] { input, writeEnd, errors };

        if (! ::InitializeProcThreadAttributeList (list, 1, 0, &listSize)
            || ! ::UpdateProcThreadAttribute (list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handed, sizeof (handed),
                                              nullptr, nullptr))
        {
            closeAll();
            return false;
        }

        STARTUPINFOEXW startup {};
        startup.StartupInfo.cb = sizeof (startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = writeEnd;
        startup.StartupInfo.hStdError = errors;
        startup.lpAttributeList = list;

        PROCESS_INFORMATION info {};
        const auto made = ::CreateProcessW (nullptr, mutableLine.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                            nullptr, nullptr, &startup.StartupInfo, &info);

        ::DeleteProcThreadAttributeList (list);
        ::CloseHandle (writeEnd);
        ::CloseHandle (input);
        ::CloseHandle (errors);

        if (! made)
        {
            ::CloseHandle (readEnd);
            return false;
        }

        ::CloseHandle (info.hThread);
        impl->process = info.hProcess;
        impl->output = readEnd;
        return true;
       #else
        int ends[2] { -1, -1 };

        if (::pipe (ends) != 0)
            return false;

        ::fcntl (ends[0], F_SETFD, FD_CLOEXEC);

        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init (&actions);
        ::posix_spawn_file_actions_addopen (&actions, 0, "/dev/null", O_RDONLY, 0);
        ::posix_spawn_file_actions_adddup2 (&actions, ends[1], 1);
        ::posix_spawn_file_actions_addclose (&actions, ends[1]);
        ::posix_spawn_file_actions_addopen (&actions, 2, errorFile.empty() ? "/dev/null" : errorFile.c_str(),
                                            O_WRONLY | O_CREAT | O_TRUNC, 0644);

        std::vector<char*> words;

        for (const auto& text : command)
            words.push_back (const_cast<char*> (text.c_str()));

        words.push_back (nullptr);

        pid_t child = 0;
        const auto made = ::posix_spawn (&child, command.front().c_str(), &actions, nullptr, words.data(), environ);

        ::posix_spawn_file_actions_destroy (&actions);
        ::close (ends[1]);

        if (made != 0)
        {
            ::close (ends[0]);
            return false;
        }

        impl->pid = child;
        impl->output = ends[0];
        return true;
       #endif
    }

    std::int64_t PipedChild::read (void* into, std::size_t size)
    {
        if (size == 0)
            return 0;

       #if defined (_WIN32)
        if (impl->output == nullptr)
            return -1;

        DWORD got = 0;
        const auto ask = static_cast<DWORD> (std::min<std::size_t> (size, 1u << 30));

        if (! ::ReadFile (impl->output, into, ask, &got, nullptr))
            return ::GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;

        return static_cast<std::int64_t> (got);
       #else
        if (impl->output < 0)
            return -1;

        for (;;)
        {
            const auto got = ::read (impl->output, into, size);

            if (got >= 0)
                return static_cast<std::int64_t> (got);

            if (errno != EINTR)
                return -1;
        }
       #endif
    }

    bool PipedChild::readExactly (void* into, std::size_t size)
    {
        auto* at = static_cast<std::uint8_t*> (into);

        while (size > 0)
        {
            const auto got = read (at, size);

            if (got <= 0)
                return false;

            at += got;
            size -= static_cast<std::size_t> (got);
        }

        return true;
    }

    std::string PipedChild::readAll (std::size_t limit)
    {
        std::string out;
        char buffer[16384];

        while (out.size() < limit)
        {
            const auto got = read (buffer, sizeof (buffer));

            if (got <= 0)
                break;

            out.append (buffer, static_cast<std::size_t> (got));
        }

        return out;
    }

    int PipedChild::wait (int milliseconds)
    {
        if (impl->ended)
            return impl->exitCode;

       #if defined (_WIN32)
        if (impl->process == nullptr)
            return -1;

        if (::WaitForSingleObject (impl->process, static_cast<DWORD> (std::max (milliseconds, 0))) != WAIT_OBJECT_0)
            return -1;

        DWORD code = 0;
        ::GetExitCodeProcess (impl->process, &code);
        impl->exitCode = static_cast<int> (code);
        impl->ended = true;
        return impl->exitCode;
       #else
        if (impl->pid <= 0)
            return -1;

        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (std::max (milliseconds, 0));

        for (;;)
        {
            int status = 0;
            const auto done = ::waitpid (impl->pid, &status, WNOHANG);

            if (done == impl->pid)
            {
                impl->exitCode = WIFEXITED (status) ? WEXITSTATUS (status) : 128 + (WIFSIGNALED (status) ? WTERMSIG (status) : 0);
                impl->ended = true;
                return impl->exitCode;
            }

            if (done < 0 && errno != EINTR)
                return -1;

            if (std::chrono::steady_clock::now() >= until)
                return -1;

            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
       #endif
    }

    bool PipedChild::isRunning() const
    {
       #if defined (_WIN32)
        if (impl->process == nullptr)
            return false;
       #else
        if (impl->pid <= 0)
            return false;
       #endif

        /*  ASKED BY WAITING NO TIME, which also reaps one that has ended - a
            child gone but not waited for is still there to `kill (pid, 0)`. */
        const_cast<PipedChild*> (this)->wait (0);
        return ! impl->ended;
    }

    void PipedChild::kill()
    {
       #if defined (_WIN32)
        if (impl->process != nullptr)
            ::TerminateProcess (impl->process, 1);
       #else
        if (impl->pid > 0 && ! impl->ended)
            ::kill (impl->pid, SIGKILL);
       #endif
    }
}
