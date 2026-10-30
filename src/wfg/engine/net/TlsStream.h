// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/*  A TLS CONNECTION (namespace draft §124, BEO, BET): Mbed TLS over a JUCE
    socket - connected with a bounded wait, as a TCP link is - and nothing of
    the operating system's TLS, so it is the same code on all three platforms.

    THE CERTIFICATE IS NOT JUDGED HERE. A base station on a show network signs
    its own; no authority vouches for it, and checking a chain would refuse
    every one. The handshake is completed whatever it presents, and its
    SHA-256 fingerprint is handed to the caller, which compares it with the
    one the show trusts (`mount/pin`) BEFORE a password or anything else is
    sent. Blocking, with deadlines: a worker's thread only, never the tick. */

#include <memory>
#include <string>
#include <string_view>

namespace wfg::net
{
    class TlsStream
    {
    public:
        TlsStream();
        ~TlsStream();

        TlsStream (const TlsStream&) = delete;
        TlsStream& operator= (const TlsStream&) = delete;

        /*  Connects to `host:port` and completes the handshake within
            `timeoutMs`, or says why not. */
        bool open (const std::string& host, int port, int timeoutMs, std::string& problem);

        /*  The certificate the far end presented, as its SHA-256 in hex pairs
            joined by colons - "3F:A0:…" - the spelling the pin is kept in. */
        const std::string& fingerprint() const noexcept;

        /*  What arrived, appended to `into`, waiting at most `waitMs`: the
            count of bytes, nought when none came, -1 when the connection has
            ended or failed (`problem` says how). */
        int read (std::string& into, int waitMs);

        bool write (std::string_view bytes);

        const std::string& problem() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    /*  A fingerprint as the pin compares it: hex digits only, upper case - so
        "3f:a0" and "3FA0" are the same pin. */
    std::string normalisedFingerprint (const std::string& text);
}
