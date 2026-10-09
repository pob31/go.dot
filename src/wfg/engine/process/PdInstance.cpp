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
#include <wfg/engine/osc/OscValue.h>

#include <z_libpd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <string_view>
#include <system_error>
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
        std::vector<int> midiBytes;
    };

    namespace
    {
        thread_local PdInstance::Shared* current = nullptr;

        /*  PD'S NUMBERS ARE 32-BIT: 0.1 is 0.100000001490116 as a double, and a
            device would be sent that. Each is taken as the shortest decimal that
            reads back as the same float - 0.1 - through Go.dot's own number
            reader, so no locale is asked. */
        double numberOf (float value)
        {
            std::array<char, 48> text {};
            const auto [end, error] = std::to_chars (text.data(), text.data() + text.size(), value);
            if (error != std::errc())
                return static_cast<double> (value);
            const auto parsed = osc::parseDouble (std::string_view (text.data(), static_cast<std::size_t> (end - text.data())));
            return parsed.has_value() ? *parsed : static_cast<double> (value);
        }

        Atoms atomsOf (int argc, t_atom* argv)
        {
            Atoms atoms;
            atoms.reserve (static_cast<std::size_t> (std::max (argc, 0)));
            for (int i = 0; i < argc; ++i)
            {
                t_atom* a = argv + i;
                if (libpd_is_float (a))
                    atoms.push_back (Atom::of (numberOf (libpd_get_float (a))));
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
        void onFloat (const char* to, float x) { keep ({ to, "float", { Atom::of (numberOf (x)) } }); }
        void onSymbol (const char* to, const char* s) { keep ({ to, "symbol", { Atom::of (std::string (s)) } }); }
        void onList (const char* to, int argc, t_atom* argv) { keep ({ to, "list", atomsOf (argc, argv) }); }
        void onMessage (const char* to, const char* selector, int argc, t_atom* argv)
        {
            keep ({ to, selector, atomsOf (argc, argv) });
        }

        /*  MIDI OUT (PC.3): each message Pd's MIDI objects send, as its bytes.
            libpd numbers a channel from nought, sixteen to a port; Go.dot sends
            on the one port the cue names, so the port part is dropped. */
        void midiOut (std::initializer_list<int> bytes)
        {
            Atoms atoms;
            for (const int byte : bytes)
                atoms.push_back (Atom::of (static_cast<double> (std::clamp (byte, 0, 255))));
            keep ({ midiName, "list", std::move (atoms) });
        }

        void onNoteOn (int channel, int pitch, int velocity)       { midiOut ({ 0x90 | (channel & 15), pitch & 127, velocity & 127 }); }
        void onControl (int channel, int controller, int value)    { midiOut ({ 0xB0 | (channel & 15), controller & 127, value & 127 }); }
        void onProgram (int channel, int value)                    { midiOut ({ 0xC0 | (channel & 15), value & 127 }); }
        void onBend (int channel, int value)
        {
            const int bend = std::clamp (value + 8192, 0, 16383);
            midiOut ({ 0xE0 | (channel & 15), bend & 127, bend >> 7 });
        }
        void onTouch (int channel, int value)                      { midiOut ({ 0xD0 | (channel & 15), value & 127 }); }
        void onPolyTouch (int channel, int pitch, int value)       { midiOut ({ 0xA0 | (channel & 15), pitch & 127, value & 127 }); }
        /*  [midiout] sends a byte at a time; a message leaves once it is
            whole - its status byte's length, or a system exclusive to its end. */
        void onMidiByte (int, int byte)
        {
            if (current == nullptr)
                return;
            auto& pending = current->midiBytes;
            byte &= 255;

            if (byte >= 0xF8)
            {
                midiOut ({ byte });
                return;
            }
            if (byte >= 0x80 && byte != 0xF7)
                pending.clear();
            pending.push_back (byte);

            const int status = pending.front();
            std::size_t whole = 0;
            if (status == 0xF0)
                whole = byte == 0xF7 ? pending.size() : 0;
            else if (status == 0xC0 || status == 0xD0 || (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
                whole = 2;
            else if (status >= 0x80 && status < 0xF0)
                whole = 3;

            if (whole > 0 && pending.size() >= whole)
            {
                Atoms atoms;
                for (const int b : pending)
                    atoms.push_back (Atom::of (static_cast<double> (b)));
                keep ({ midiName, "list", std::move (atoms) });
                pending.clear();
            }
        }

        /*  MIDI IN (PC.3): one message's bytes to the matching call. */
        void midiIn (const Atoms& atoms)
        {
            std::vector<int> b;
            for (const auto& atom : atoms)
                if (atom.isNumber)
                    b.push_back (std::clamp (static_cast<int> (atom.number), 0, 255));
            if (b.empty())
                return;

            const int status = b[0] & 0xF0;
            const int channel = b[0] & 0x0F;
            const auto at = [&b] (std::size_t i) { return i < b.size() ? b[i] : 0; };

            switch (status)
            {
                case 0x90: libpd_noteon (channel, at (1), at (2)); break;
                case 0x80: libpd_noteon (channel, at (1), 0); break;
                case 0xB0: libpd_controlchange (channel, at (1), at (2)); break;
                case 0xC0: libpd_programchange (channel, at (1)); break;
                case 0xE0: libpd_pitchbend (channel, ((at (2) << 7) | at (1)) - 8192); break;
                case 0xD0: libpd_aftertouch (channel, at (1)); break;
                case 0xA0: libpd_polyaftertouch (channel, at (1), at (2)); break;
                default:
                    if (b[0] == 0xF0)
                        for (const int byte : b)
                            libpd_sysex (0, byte);
                    else
                        for (const int byte : b)
                            libpd_sysrealtime (0, byte);
                    break;
            }

            // And every byte to [midiin], as Pd's own MIDI input does.
            for (const int byte : b)
                libpd_midibyte (0, byte);
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
            libpd_set_noteonhook (onNoteOn);
            libpd_set_controlchangehook (onControl);
            libpd_set_programchangehook (onProgram);
            libpd_set_pitchbendhook (onBend);
            libpd_set_aftertouchhook (onTouch);
            libpd_set_polyaftertouchhook (onPolyTouch);
            libpd_set_midibytehook (onMidiByte);
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

            //  An empty patch is a patch with nothing in it: nothing to open.
            if (std::all_of (text.begin(), text.end(), [] (char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }))
            {
                s.work.sends.clear();
                s.work.receives.clear();
                return;
            }

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
                if (input.to == midiName)
                {
                    midiIn (input.atoms);
                    continue;
                }

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
