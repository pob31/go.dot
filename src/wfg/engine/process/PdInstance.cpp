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

#include <wfg/engine/process/PdInstance.h>
#include <wfg/engine/process/PatchText.h>

#include <z_libpd.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

#if defined (_WIN32)
 #include <locale.h>
 #include <clocale>
#else
 #include <locale.h>
 #if __has_include (<xlocale.h>)
  #include <xlocale.h>
 #endif
#endif

namespace wfg::process
{
    namespace
    {
        enum Job : int { jobNone, jobMake, jobOpen, jobTick, jobClose };

        /*  This thread's numbers in C, the process's left as they are (ACQ). */
        void numbersInCOnThisThread()
        {
           #if defined (_WIN32)
            _configthreadlocale (_ENABLE_PER_THREAD_LOCALE);
            std::setlocale (LC_NUMERIC, "C");
           #else
            const auto base = ::duplocale (LC_GLOBAL_LOCALE);
            const auto c = ::newlocale (LC_NUMERIC_MASK, "C", base);
            if (c != static_cast<locale_t> (nullptr))
                ::uselocale (c);
           #endif
        }

        void quiet (const char*) {}

        /*  libpd's own state, once for the process: its main instance, which no
            patch runs in, made silent. */
        void initialiseLibpdOnce()
        {
            static std::once_flag once;
            std::call_once (once, []
            {
                libpd_set_printhook (quiet);
                libpd_init();
            });
        }
    }

    struct PdInstance::Shared
    {
        Settings settings;

        std::mutex lock;
        std::condition_variable wake;
        std::condition_variable done;
        int job = jobNone;
        std::vector<Input> inputs;
        std::string text;
        std::uint64_t posted = 0;
        std::uint64_t finished = 0;
        bool running = false;
        bool quit = false;
        bool abandoned = false;
        bool leaveUnfreed = false;

        // Written on the thread while a job runs; the owner takes it only when
        // no job is running.
        Outbox outbox;

        // What the jobs found: written on the thread as `work`, and copied to
        // `facts` under the lock as each job ends - the copy is what the owner reads.
        struct Facts
        {
            bool made = false;
            bool opened = false;
            std::string problem;
            std::vector<std::string> sends;
            std::vector<std::string> receives;
        };
        Facts work;
        Facts facts;

        // The thread's own.
        t_pdinstance* instance = nullptr;
        void* file = nullptr;
        std::vector<void*> bindings;
        std::string printing;
    };

    namespace
    {
        thread_local PdInstance::Shared* current = nullptr;

        Atoms atomsOf (int argc, t_atom* argv)
        {
            Atoms atoms;
            atoms.reserve (static_cast<std::size_t> (std::max (argc, 0)));
            for (int i = 0; i < argc; ++i)
            {
                t_atom* a = argv + i;
                if (libpd_is_float (a))
                    atoms.push_back (Atom::of (static_cast<double> (libpd_get_float (a))));
                else if (libpd_is_symbol (a))
                    atoms.push_back (Atom::of (std::string (libpd_get_symbol (a))));
            }
            return atoms;
        }

        void keep (Sent sent)
        {
            if (current == nullptr)
                return;
            if (current->outbox.sent.size() >= current->settings.maxSent)
            {
                ++current->outbox.dropped;
                return;
            }
            current->outbox.sent.push_back (std::move (sent));
        }

        void onBang (const char* to) { keep ({ to, "bang", {} }); }
        void onFloat (const char* to, float x) { keep ({ to, "float", { Atom::of (static_cast<double> (x)) } }); }
        void onSymbol (const char* to, const char* s) { keep ({ to, "symbol", { Atom::of (std::string (s)) } }); }
        void onList (const char* to, int argc, t_atom* argv) { keep ({ to, "list", atomsOf (argc, argv) }); }
        void onMessage (const char* to, const char* selector, int argc, t_atom* argv)
        {
            keep ({ to, selector, atomsOf (argc, argv) });
        }

        /*  Pd prints a line in pieces; a line is kept when its new line comes. */
        void onPrint (const char* s)
        {
            if (current == nullptr)
                return;
            current->printing += s;
            std::size_t at;
            while ((at = current->printing.find ('\n')) != std::string::npos)
            {
                if (current->outbox.printed.size() < current->settings.maxPrinted)
                    current->outbox.printed.push_back (current->printing.substr (0, at));
                current->printing.erase (0, at + 1);
            }
        }

        void sendTo (const std::string& to, const Atoms& atoms)
        {
            if (atoms.empty())
            {
                libpd_bang (to.c_str());
            }
            else if (atoms.size() == 1 && atoms.front().isNumber)
            {
                libpd_float (to.c_str(), static_cast<float> (atoms.front().number));
            }
            else if (atoms.size() == 1)
            {
                libpd_symbol (to.c_str(), atoms.front().word.c_str());
            }
            else
            {
                libpd_start_message (static_cast<int> (atoms.size()));
                for (const auto& atom : atoms)
                {
                    if (atom.isNumber)
                        libpd_add_float (static_cast<float> (atom.number));
                    else
                        libpd_add_symbol (atom.word.c_str());
                }
                libpd_finish_list (to.c_str());
            }
        }

        void unbindAll (PdInstance::Shared& s)
        {
            for (auto* binding : s.bindings)
                libpd_unbind (binding);
            s.bindings.clear();
        }

        void runMake (PdInstance::Shared& s)
        {
            initialiseLibpdOnce();
            s.instance = libpd_new_instance();
            libpd_set_instance (s.instance);
            libpd_set_printhook (onPrint);
            libpd_set_banghook (onBang);
            libpd_set_floathook (onFloat);
            libpd_set_symbolhook (onSymbol);
            libpd_set_listhook (onList);
            libpd_set_messagehook (onMessage);
            libpd_init_audio (0, 0, pdSampleRate);
            for (const auto& path : s.settings.searchPaths)
                libpd_add_to_search_path (path.c_str());
            libpd_start_message (1);
            libpd_add_float (1.0f);
            libpd_finish_message ("pd", "dsp");
            s.work.made = true;
        }

        void runClosePatch (PdInstance::Shared& s)
        {
            if (s.file != nullptr)
                libpd_closefile (s.file);
            s.file = nullptr;
            unbindAll (s);
            s.work.opened = false;
        }

        void runOpen (PdInstance::Shared& s, const std::string& text)
        {
            runClosePatch (s);
            s.work.problem.clear();

            const auto patch = parsePatch (text);
            const auto names = namesIn (patch);
            s.work.sends = names.sends;
            s.work.receives = names.receives;

            const std::filesystem::path file (s.settings.patchFile);
            std::error_code ignored;
            std::filesystem::create_directories (file.parent_path(), ignored);
            {
                std::ofstream out (file, std::ios::binary | std::ios::trunc);
                out << text;
                if (! out)
                {
                    s.work.problem = "the patch could not be written to " + file.string();
                    return;
                }
            }

            // Bound before the patch opens, so what its [loadbang] sends is heard.
            for (const auto& name : s.work.sends)
                if (auto* binding = libpd_bind (name.c_str()))
                    s.bindings.push_back (binding);

            s.file = libpd_openfile (file.filename().string().c_str(), file.parent_path().string().c_str());
            if (s.file == nullptr)
            {
                s.work.problem = patch.problem.empty() ? std::string ("Pure Data could not open the patch") : patch.problem;
                unbindAll (s);
                return;
            }
            s.work.opened = true;
            if (! patch.problem.empty())
                s.work.problem = patch.problem;
        }

        void runTick (const std::vector<Input>& inputs)
        {
            const bool hasIn = libpd_exists ("in") != 0;
            for (const auto& input : inputs)
            {
                if (libpd_exists (input.to.c_str()) != 0)
                    sendTo (input.to, input.atoms);
                if (hasIn)
                {
                    libpd_start_message (static_cast<int> (input.atoms.size()));
                    for (const auto& atom : input.atoms)
                    {
                        if (atom.isNumber)
                            libpd_add_float (static_cast<float> (atom.number));
                        else
                            libpd_add_symbol (atom.word.c_str());
                    }
                    libpd_finish_message ("in", input.to.c_str());
                }
            }
            std::array<float, 1> in {}, out {};
            libpd_process_float (pdBlocksPerTick, in.data(), out.data());
        }

        void runClose (PdInstance::Shared& s)
        {
            runClosePatch (s);
            if (s.instance != nullptr)
                libpd_free_instance (s.instance);
            s.instance = nullptr;
            s.work.made = false;
        }

        void threadMain (std::shared_ptr<PdInstance::Shared> s)
        {
            numbersInCOnThisThread();
            current = s.get();

            std::unique_lock<std::mutex> l (s->lock);
            for (;;)
            {
                s->wake.wait (l, [&] { return s->job != jobNone || s->quit; });
                if (s->job == jobNone)
                    break;

                const int job = s->job;
                auto inputs = std::move (s->inputs);
                auto text = std::move (s->text);
                s->job = jobNone;
                s->running = true;
                l.unlock();

                if (job == jobMake)       runMake (*s);
                else if (job == jobOpen)  runOpen (*s, text);
                else if (job == jobTick)  runTick (inputs);
                else if (job == jobClose) runClose (*s);

                l.lock();
                s->facts = s->work;
                s->running = false;
                ++s->finished;
                s->done.notify_all();
                if (s->abandoned)
                    break;
            }
        }
    }

    PdInstance::PdInstance (Settings settings)
        : shared (std::make_shared<Shared>())
    {
        shared->settings = std::move (settings);
        std::thread (threadMain, shared).detach();
    }

    PdInstance::~PdInstance()
    {
        std::unique_lock<std::mutex> l (shared->lock);
        const bool idle = ! shared->running && shared->job == jobNone;
        if (idle && shared->facts.made && ! shared->leaveUnfreed)
        {
            shared->job = jobClose;
            ++shared->posted;
            shared->wake.notify_all();
            shared->done.wait (l, [&] { return shared->finished == shared->posted; });
        }
        if (! idle)
            shared->abandoned = true;
        shared->quit = true;
        shared->wake.notify_all();
    }

    bool PdInstance::post (int kind, std::vector<Input> inputs, std::string text)
    {
        std::lock_guard<std::mutex> l (shared->lock);
        if (shared->running || shared->job != jobNone)
            return false;
        if (kind == jobMake && shared->facts.made)
            return false;
        if (kind != jobMake && ! shared->facts.made)
            return false;
        if (kind == jobTick && ! shared->facts.opened)
            return false;
        shared->job = kind;
        shared->inputs = std::move (inputs);
        shared->text = std::move (text);
        ++shared->posted;
        shared->wake.notify_all();
        return true;
    }

    bool PdInstance::make() { return post (jobMake, {}, {}); }
    bool PdInstance::open (std::string patchText) { return post (jobOpen, {}, std::move (patchText)); }
    bool PdInstance::tick (std::vector<Input> inputs) { return post (jobTick, std::move (inputs), {}); }
    bool PdInstance::close() { return post (jobClose, {}, {}); }

    void PdInstance::leaveUnfreed()
    {
        std::lock_guard<std::mutex> l (shared->lock);
        shared->leaveUnfreed = true;
    }

    bool PdInstance::finished (std::chrono::microseconds limit)
    {
        std::unique_lock<std::mutex> l (shared->lock);
        return shared->done.wait_for (l, limit, [&] { return shared->finished == shared->posted; });
    }

    bool PdInstance::busy() const
    {
        std::lock_guard<std::mutex> l (shared->lock);
        return shared->running || shared->job != jobNone;
    }

    Outbox PdInstance::takeOutbox()
    {
        std::lock_guard<std::mutex> l (shared->lock);
        if (shared->running || shared->job != jobNone)
            return {};
        return std::exchange (shared->outbox, Outbox {});
    }

    bool PdInstance::made() const
    {
        std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.made;
    }

    bool PdInstance::opened() const
    {
        std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.opened;
    }

    std::string PdInstance::problem() const
    {
        std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.problem;
    }

    std::vector<std::string> PdInstance::sends() const
    {
        std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.sends;
    }

    std::vector<std::string> PdInstance::receives() const
    {
        std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.receives;
    }
}
