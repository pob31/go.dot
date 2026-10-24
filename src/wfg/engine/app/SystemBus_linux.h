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
    THE SYSTEM BUS, AS MUCH OF IT AS GO.DOT NEEDS (Linux only; namespace draft
    §108, the Linux half). Three services answer there, each with one or two
    methods and a few properties: RealtimeKit grants a thread the real-time
    class the user's limits refused; power-profiles-daemon holds the
    performance profile and says which profile is active; logind keeps the
    computer from sleeping under a show. libdbus-1 does the wire; this header
    does the five calls, blocking with a short timeout, on whatever thread
    asks - never the audio thread, which asks for nothing.

    A Connection is PRIVATE to its holder and closed with it, which is what
    makes a hold or an inhibitor end with the process even when the process
    ends badly: the bus sees the socket close. Included by the two Linux
    halves (RealtimeRights.cpp, MachinePower.cpp) and by nothing else.
*/

#include <dbus/dbus.h>

#include <cstdint>
#include <optional>
#include <string>

namespace wfg::app::bus
{
    /** Who to ask: the well-known name, the object and the interface. */
    struct Target
    {
        const char* service;
        const char* path;
        const char* interface;
    };

    //==============================================================================
    class Connection
    {
    public:
        Connection() noexcept
        {
            /*  libdbus is thread-unsafe until told otherwise, even between
                distinct connections; told once, and it is safe to tell twice. */
            static const auto threaded = dbus_threads_init_default();
            (void) threaded;

            DBusError error;
            dbus_error_init (&error);
            handle = dbus_bus_get_private (DBUS_BUS_SYSTEM, &error);
            dbus_error_free (&error);

            if (handle != nullptr)
                dbus_connection_set_exit_on_disconnect (handle, FALSE);
        }

        ~Connection()
        {
            if (handle != nullptr)
            {
                dbus_connection_close (handle);
                dbus_connection_unref (handle);
            }
        }

        Connection (const Connection&) = delete;
        Connection& operator= (const Connection&) = delete;

        /** False where there is no system bus, or it refused us. */
        bool open() const noexcept { return handle != nullptr; }

        DBusConnection* get() const noexcept { return handle; }

    private:
        DBusConnection* handle = nullptr;
    };

    //==============================================================================
    /** A reply owned until it goes out of scope; null when the call failed. */
    class Reply
    {
    public:
        Reply() = default;
        explicit Reply (DBusMessage* m) noexcept : message (m) {}
        ~Reply() { if (message != nullptr) dbus_message_unref (message); }

        Reply (Reply&& other) noexcept : message (other.message) { other.message = nullptr; }
        Reply& operator= (Reply&& other) noexcept
        {
            if (this != &other)
            {
                if (message != nullptr) dbus_message_unref (message);
                message = other.message;
                other.message = nullptr;
            }
            return *this;
        }

        Reply (const Reply&) = delete;
        Reply& operator= (const Reply&) = delete;

        bool ok() const noexcept { return message != nullptr; }
        DBusMessage* get() const noexcept { return message; }

    private:
        DBusMessage* message = nullptr;
    };

    /** Sends `request` (consumed) and waits up to `timeoutMs` for the reply. */
    inline Reply call (const Connection& connection, DBusMessage* request, int timeoutMs) noexcept
    {
        if (request == nullptr)
            return {};

        if (! connection.open())
        {
            dbus_message_unref (request);
            return {};
        }

        DBusError error;
        dbus_error_init (&error);
        auto* reply = dbus_connection_send_with_reply_and_block (connection.get(), request, timeoutMs, &error);
        dbus_message_unref (request);
        dbus_error_free (&error);
        return Reply { reply };
    }

    inline DBusMessage* methodCall (const Target& target, const char* method) noexcept
    {
        return dbus_message_new_method_call (target.service, target.path, target.interface, method);
    }

    //==============================================================================
    /** One property, read through org.freedesktop.DBus.Properties.Get: a string. */
    inline std::optional<std::string> stringProperty (const Connection& connection, const Target& target,
                                                      const char* property, int timeoutMs = 250) noexcept
    {
        auto* request = methodCall ({ target.service, target.path, "org.freedesktop.DBus.Properties" }, "Get");

        if (request == nullptr)
            return std::nullopt;

        dbus_message_append_args (request, DBUS_TYPE_STRING, &target.interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);
        const auto reply = call (connection, request, timeoutMs);

        if (! reply.ok())
            return std::nullopt;

        DBusMessageIter outer, inner;
        dbus_message_iter_init (reply.get(), &outer);

        if (dbus_message_iter_get_arg_type (&outer) != DBUS_TYPE_VARIANT)
            return std::nullopt;

        dbus_message_iter_recurse (&outer, &inner);

        if (dbus_message_iter_get_arg_type (&inner) != DBUS_TYPE_STRING)
            return std::nullopt;

        const char* value = nullptr;
        dbus_message_iter_get_basic (&inner, &value);
        return value != nullptr ? std::optional<std::string> (value) : std::nullopt;
    }

    /** One property, a whole number of any D-Bus width. */
    inline std::optional<std::int64_t> integerProperty (const Connection& connection, const Target& target,
                                                        const char* property, int timeoutMs = 250) noexcept
    {
        auto* request = methodCall ({ target.service, target.path, "org.freedesktop.DBus.Properties" }, "Get");

        if (request == nullptr)
            return std::nullopt;

        dbus_message_append_args (request, DBUS_TYPE_STRING, &target.interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);
        const auto reply = call (connection, request, timeoutMs);

        if (! reply.ok())
            return std::nullopt;

        DBusMessageIter outer, inner;
        dbus_message_iter_init (reply.get(), &outer);

        if (dbus_message_iter_get_arg_type (&outer) != DBUS_TYPE_VARIANT)
            return std::nullopt;

        dbus_message_iter_recurse (&outer, &inner);

        switch (dbus_message_iter_get_arg_type (&inner))
        {
            case DBUS_TYPE_INT32:  { dbus_int32_t v = 0;  dbus_message_iter_get_basic (&inner, &v); return v; }
            case DBUS_TYPE_UINT32: { dbus_uint32_t v = 0; dbus_message_iter_get_basic (&inner, &v); return static_cast<std::int64_t> (v); }
            case DBUS_TYPE_INT64:  { dbus_int64_t v = 0;  dbus_message_iter_get_basic (&inner, &v); return v; }
            case DBUS_TYPE_UINT64: { dbus_uint64_t v = 0; dbus_message_iter_get_basic (&inner, &v); return static_cast<std::int64_t> (v); }
            default: break;
        }

        return std::nullopt;
    }
}
