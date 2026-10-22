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

//  The Mac's half of WorkgroupShare.h; WorkgroupShare.cpp is everyone else's,
//  where nothing is shared. Objective-C++ because JUCE's own is: an
//  os_workgroup_t there is an object pointer, and the one function borrowed
//  from it below is named for that type.

#include <wfg/engine/plugin/WorkgroupShare.h>

#include <juce_audio_basics/juce_audio_basics.h>

#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <os/workgroup.h>
#include <servers/bootstrap.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

/*  DECLARED HERE because JUCE declares it only in a header of its own module's
    (native/juce_AudioWorkgroup_mac.h): what wraps a handle in an AudioWorkgroup,
    which is how the device's handle is told apart from the others - by JUCE's
    own equality, which compares handles. */
namespace juce { AudioWorkgroup makeRealAudioWorkgroup (os_workgroup_t handle); }

namespace wfg::plugin
{
    namespace
    {
        /*  THE MESSAGES. The child sends the generation it last acted on and a
            send-once right to answer on; the parent answers with its
            generation and, when that differs, a send right to the workgroup's
            port - MACH_PORT_NULL when the generation is the same or nothing is
            shared. One shape each way, checked before a byte is believed. */
        constexpr mach_msg_id_t askId = 0x47447771;      // "GDwq"
        constexpr mach_msg_id_t answerId = 0x47447761;   // "GDwa"

        struct Ask
        {
            mach_msg_header_t header;
            std::uint32_t seen;
        };

        struct Answer
        {
            mach_msg_header_t header;
            mach_msg_body_t body;
            mach_msg_port_descriptor_t port;
            std::uint32_t generation;
        };

        std::string serviceNameFor (std::int64_t pid)
        {
            return "com.github.pob31.godot.workgroup." + std::to_string (pid);
        }

        //==============================================================================
        /*  THE PARENT'S SHARE. Made on the first share and never destroyed: the
            dispatch queue may be answering while the process exits, and launchd
            takes the service back with the process. */
        struct Share
        {
            std::mutex mutex;
            os_workgroup_t handle = nullptr;
            mach_port_t port = MACH_PORT_NULL;
            std::uint32_t generation = 0;

            mach_port_t service = MACH_PORT_NULL;
            bool serviceTried = false;
        };

        Share& theShare()
        {
            static auto* share = new Share();
            return *share;
        }

        /*  Every ask waiting, answered in turn; the queue's, never the audio
            thread's. A message of any other shape is destroyed unread. */
        void answerAsks (void* context)
        {
            auto& share = *static_cast<Share*> (context);

            for (;;)
            {
                union
                {
                    Ask ask;
                    char room[sizeof (Ask) + sizeof (mach_msg_max_trailer_t)];
                } received {};

                const auto kr = mach_msg (&received.ask.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                                          sizeof (received), share.service, 0, MACH_PORT_NULL);

                if (kr == MACH_RCV_TOO_LARGE)
                    continue;

                if (kr != MACH_MSG_SUCCESS)
                    return;

                auto& header = received.ask.header;

                if (header.msgh_id != askId || header.msgh_size != sizeof (Ask)
                    || (header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0
                    || MACH_MSGH_BITS_REMOTE (header.msgh_bits) != MACH_MSG_TYPE_PORT_SEND_ONCE)
                {
                    mach_msg_destroy (&header);
                    continue;
                }

                const std::lock_guard<std::mutex> lock (share.mutex);

                Answer answer {};
                answer.header.msgh_bits = MACH_MSGH_BITS (MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
                answer.header.msgh_size = sizeof (Answer);
                answer.header.msgh_remote_port = header.msgh_remote_port;
                answer.header.msgh_id = answerId;
                answer.body.msgh_descriptor_count = 1;
                answer.port.name = received.ask.seen != share.generation ? share.port : MACH_PORT_NULL;
                answer.port.disposition = MACH_MSG_TYPE_COPY_SEND;
                answer.port.type = MACH_MSG_PORT_DESCRIPTOR;
                answer.generation = share.generation;

                /*  A send-once never waits on a queue. Should the child have
                    gone, the right left behind is let go of here; the copied
                    port was never taken. */
                if (mach_msg (&answer.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof (Answer), 0,
                              MACH_PORT_NULL, 0, MACH_PORT_NULL) != MACH_MSG_SUCCESS)
                    mach_port_deallocate (mach_task_self(), header.msgh_remote_port);
            }
        }

        /*  Once for the life of the process: launchd refuses a second check-in
            of the same name from it. A refusal leaves the children asking a
            name nobody holds, which costs them nothing but the ask. */
        void startService (Share& share)
        {
            if (share.serviceTried)
                return;

            share.serviceTried = true;
            const auto name = serviceNameFor (static_cast<std::int64_t> (::getpid()));

            if (bootstrap_check_in (bootstrap_port, name.c_str(), &share.service) != KERN_SUCCESS)
            {
                share.service = MACH_PORT_NULL;
                return;
            }

            auto* queue = dispatch_queue_create ("com.github.pob31.godot.workgroup", DISPATCH_QUEUE_SERIAL);
            auto* source = dispatch_source_create (DISPATCH_SOURCE_TYPE_MACH_RECV, share.service, 0, queue);
            dispatch_set_context (source, &share);
            dispatch_source_set_event_handler_f (source, answerAsks);
            dispatch_resume (source);
        }

        bool shareHandle (os_workgroup_t handle)
        {
            auto& share = theShare();
            const std::lock_guard<std::mutex> lock (share.mutex);
            startService (share);

            if (handle == share.handle)
                return handle != nullptr;

            mach_port_t port = MACH_PORT_NULL;

            if (handle != nullptr && os_workgroup_copy_port (handle, &port) != 0)
            {
                handle = nullptr;
                port = MACH_PORT_NULL;
            }

            if (share.port != MACH_PORT_NULL)
                mach_port_deallocate (mach_task_self(), share.port);

            if (share.handle != nullptr)
                os_release (share.handle);

            if (handle != nullptr)
                os_retain (handle);

            share.handle = handle;
            share.port = port;
            ++share.generation;
            return handle != nullptr;
        }

        /*  THE DEVICE'S HANDLE, retained, or null: every device CoreAudio
            lists, asked for its IO thread's workgroup, and the one JUCE's
            equals kept. JUCE asks the same property the same way. */
        os_workgroup_t deviceHandleOf (const juce::AudioWorkgroup& wanted)
        {
            if (! wanted)
                return nullptr;

            AudioObjectPropertyAddress devicesAddress { kAudioHardwarePropertyDevices,
                                                        kAudioObjectPropertyScopeGlobal,
                                                        kAudioObjectPropertyElementMain };
            UInt32 bytes = 0;

            if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &bytes) != noErr)
                return nullptr;

            std::vector<AudioDeviceID> devices (bytes / sizeof (AudioDeviceID));

            if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &bytes, devices.data()) != noErr)
                return nullptr;

            devices.resize (bytes / sizeof (AudioDeviceID));

            for (const auto device : devices)
            {
                AudioObjectPropertyAddress address { kAudioDevicePropertyIOThreadOSWorkgroup,
                                                     kAudioObjectPropertyScopeWildcard,
                                                     kAudioObjectPropertyElementMain };
                os_workgroup_t handle = nullptr;
                UInt32 size = sizeof (handle);

                if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &handle) != noErr || handle == nullptr)
                    continue;

                if (juce::makeRealAudioWorkgroup (handle) == wanted)
                    return handle;

                os_release (handle);
            }

            return nullptr;
        }
    }

    bool shareAudioWorkgroup (const juce::AudioWorkgroup& deviceWorkgroup)
    {
        auto* handle = deviceHandleOf (deviceWorkgroup);
        const auto shared = shareHandle (handle);

        if (handle != nullptr)
            os_release (handle);

        return shared;
    }

    bool shareWorkgroupHandle (void* osWorkgroup)
    {
        return shareHandle ((__bridge os_workgroup_t) osWorkgroup);
    }

    //==============================================================================
    struct WorkgroupFollower::Impl
    {
        std::string serviceName;
        mach_port_t server = MACH_PORT_NULL;
        mach_port_t replies = MACH_PORT_NULL;
        std::uint32_t seen = 0;

        /*  THE HANDOFF. The message thread stores `offered`, then bumps
            `offeredSerial`; the worker answers with `takenSerial` once it has
            left what it was in and joined what was offered. `held` is the
            message thread's reference to the last offer, `retiring` the one
            before it, let go of once the worker has caught up. */
        std::atomic<os_workgroup_t> offered { nullptr };
        std::atomic<std::uint32_t> offeredSerial { 0 };
        std::atomic<std::uint32_t> takenSerial { 0 };
        os_workgroup_t held = nullptr;
        os_workgroup_t retiring = nullptr;

        /*  The worker's own. */
        os_workgroup_t joinedTo = nullptr;
        os_workgroup_join_token_s token {};
        std::uint32_t taken = 0;

        void dropReplies()
        {
            if (replies != MACH_PORT_NULL)
                mach_port_mod_refs (mach_task_self(), replies, MACH_PORT_RIGHT_RECEIVE, -1);

            replies = MACH_PORT_NULL;
        }

        void dropServer()
        {
            if (server != MACH_PORT_NULL)
                mach_port_deallocate (mach_task_self(), server);

            server = MACH_PORT_NULL;
        }

        /*  One ask and its answer. False when there is nobody to ask yet, or
            no answer came; the next poll asks again. */
        bool ask (std::uint32_t& generation, mach_port_t& port)
        {
            if (server == MACH_PORT_NULL
                && bootstrap_look_up (bootstrap_port, serviceName.c_str(), &server) != KERN_SUCCESS)
            {
                server = MACH_PORT_NULL;
                return false;
            }

            if (replies == MACH_PORT_NULL
                && mach_port_allocate (mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &replies) != KERN_SUCCESS)
            {
                replies = MACH_PORT_NULL;
                return false;
            }

            union
            {
                Ask ask;
                struct
                {
                    Answer answer;
                    mach_msg_max_trailer_t trailer;
                } received;
            } buffer {};

            buffer.ask.header.msgh_bits = MACH_MSGH_BITS (MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
            buffer.ask.header.msgh_size = sizeof (Ask);
            buffer.ask.header.msgh_remote_port = server;
            buffer.ask.header.msgh_local_port = replies;
            buffer.ask.header.msgh_id = askId;
            buffer.ask.seen = seen;

            const auto kr = mach_msg (&buffer.ask.header, MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT,
                                      sizeof (Ask), sizeof (buffer.received), replies, 50, MACH_PORT_NULL);

            if (kr == MACH_SEND_INVALID_DEST)
            {
                dropServer();
                return false;
            }

            /*  A late answer must not be read as the next one's: the port it
                would arrive on goes, and a fresh one is made for the next ask. */
            if (kr == MACH_RCV_TIMED_OUT || kr == MACH_RCV_TOO_LARGE)
            {
                dropReplies();
                return false;
            }

            if (kr != MACH_MSG_SUCCESS)
                return false;

            auto& answer = buffer.received.answer;

            if (answer.header.msgh_id != answerId || answer.header.msgh_size != sizeof (Answer)
                || (answer.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0
                || answer.body.msgh_descriptor_count != 1
                || answer.port.type != MACH_MSG_PORT_DESCRIPTOR)
            {
                mach_msg_destroy (&answer.header);
                return false;
            }

            generation = answer.generation;
            port = answer.port.name;
            return true;
        }
    };

    WorkgroupFollower::WorkgroupFollower (std::int64_t parentPid)
        : impl (std::make_unique<Impl>())
    {
        if (parentPid > 0)
            impl->serviceName = serviceNameFor (parentPid);
    }

    WorkgroupFollower::~WorkgroupFollower()
    {
        if (impl->held != nullptr)
            os_release (impl->held);

        if (impl->retiring != nullptr)
            os_release (impl->retiring);

        impl->dropReplies();
        impl->dropServer();
    }

    void WorkgroupFollower::poll()
    {
        auto& s = *impl;

        if (s.serviceName.empty())
            return;

        /*  Nothing new until the worker has taken the last offer; once it has,
            the one it left is let go of. */
        if (s.takenSerial.load (std::memory_order_acquire) != s.offeredSerial.load (std::memory_order_relaxed))
            return;

        if (s.retiring != nullptr)
        {
            os_release (s.retiring);
            s.retiring = nullptr;
        }

        std::uint32_t generation = 0;
        mach_port_t port = MACH_PORT_NULL;

        if (! s.ask (generation, port))
            return;

        if (generation == s.seen)
        {
            if (port != MACH_PORT_NULL)
                mach_port_deallocate (mach_task_self(), port);

            return;
        }

        /*  No reference to the right is taken by the workgroup made from it. */
        os_workgroup_t next = nullptr;

        if (port != MACH_PORT_NULL)
        {
            next = os_workgroup_create_with_port ("Go.dot plugin child", port);
            mach_port_deallocate (mach_task_self(), port);
        }

        s.seen = generation;
        s.retiring = s.held;
        s.held = next;
        s.offered.store (next, std::memory_order_relaxed);
        s.offeredSerial.fetch_add (1, std::memory_order_release);
    }

    bool WorkgroupFollower::keepUp() noexcept
    {
        auto& s = *impl;
        const auto serial = s.offeredSerial.load (std::memory_order_acquire);

        if (serial != s.taken)
        {
            leave();

            if (auto* next = s.offered.load (std::memory_order_relaxed); next != nullptr
                && os_workgroup_join (next, &s.token) == 0)
                s.joinedTo = next;

            s.taken = serial;
            s.takenSerial.store (serial, std::memory_order_release);
        }

        return s.joinedTo != nullptr;
    }

    void WorkgroupFollower::leave() noexcept
    {
        auto& s = *impl;

        if (s.joinedTo != nullptr)
            os_workgroup_leave (s.joinedTo, &s.token);

        s.joinedTo = nullptr;
    }
}
