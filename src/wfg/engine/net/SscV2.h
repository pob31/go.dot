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

/*  SENNHEISER'S SOUND CONTROL PROTOCOL, VERSION 2 (namespace draft §124, BET):
    a Spectera base station - and an EW-DX from firmware 4 - over HTTPS, REST
    and JSON, its changes told as Server-Sent Events.

    ONE WORKER THREAD PER DEVICE, in `MountProbe`'s shape: the tick thread
    hands it what the show declares and the requests a cue renders, and takes
    back what came, never waiting on a socket. The worker keeps two TLS
    connections - one long GET on `/api/ssc/state/subscriptions`, whose first
    event names the session and whose later ones are the device's changes,
    and one for requests, kept alive between them - and opens either again,
    with the links' backoff, when it goes.

    TRUST BEFORE ANYTHING IS SENT. Each connection's certificate fingerprint
    is compared with the device's pin before the first byte of a request -
    which carries the password - leaves. No pin: the connection is closed, the
    fingerprint is shown (`certificate`) and the device waits to be trusted.
    A different one: closed, and said, since the box answering is not the one
    trusted. */

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wfg::pair { class Hold; }

namespace wfg::net
{
    struct SscV2Wanted
    {
        std::string id;
        std::string host;
        int port = 443;
        std::string user;
        std::string password;
        std::string pin;
        bool rx = false;
        bool tx = true;

        /*  The resources to subscribe to, `/api/...`, added once the session
            opens; the base station answers each with its values as they are. */
        std::vector<std::string> subscribe;
    };

    /*  What a device said: an `event` (a JSON object keyed by resource path),
        a `reply` to a request (the JSON body, for `path`), or an `error` (a
        sentence, for `path`). */
    struct SscV2Heard
    {
        std::string id;
        std::string kind;
        std::string path;
        std::string text;
    };

    struct SscV2State
    {
        std::string state = "closed";     // closed, opening, open, retrying - the links' words
        std::string problem;
        std::string certificate;          // the fingerprint last presented
    };

    class SscV2Table
    {
    public:
        SscV2Table();
        ~SscV2Table();

        SscV2Table (const SscV2Table&) = delete;
        SscV2Table& operator= (const SscV2Table&) = delete;

        /*  The devices as the show declares them now: one whose address, login,
            pin, rx or subscriptions changed is closed and opened again; one no
            longer declared is closed. Tick thread; never waits. */
        void reconcile (const std::vector<SscV2Wanted>& wanted);

        /*  A REQUEST AS THE WIRE RENDERS IT: `METHOD /api/path`, a new line,
            and the JSON body. False when the device is not declared, does not
            transmit, or has too many waiting. */
        bool request (const std::string& id, const std::string& rendered);

        /*  What came since the last take, oldest first, at most `perDevice`
            from each. */
        std::vector<SscV2Heard> take (std::size_t perDevice);

        SscV2State stateOf (const std::string& id) const;

        /*  The machine's hold (§58, RD.1): a held machine hands a request to
            nothing and answers it sent. */
        void setHold (const pair::Hold* holdToUse) noexcept { hold = holdToUse; }

    private:
        struct Worker;
        const pair::Hold* hold = nullptr;
        std::map<std::string, std::shared_ptr<Worker>> workers;
        std::vector<std::shared_ptr<Worker>> retired;
        mutable std::mutex lock;

        void reap();
    };
}
