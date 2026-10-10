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

#include <wfg/engine/tree/RawSender.h>

#include <wfg/engine/osc/UdpEndpoint.h>

#include <utility>

namespace wfg::tree
{
    void RawSender::queue (const std::string& host, int port, osc::Packet packet)
    {
        if (host.empty() || port < 1 || port > 65535)
        {
            ++dropped;
            return;
        }

        queued.push_back ({ host, port, std::move (packet) });
    }

    void RawSender::flush()
    {
        if (queued.empty())
            return;

        auto batch = std::move (queued);
        queued.clear();

        for (const auto& raw : batch)
        {
            std::string error;
            const auto bytes = osc::encode (raw.packet, error);

            if (udp != nullptr && bytes && udp->send (raw.host, raw.port, *bytes))
                ++sent;
            else
                ++dropped;
        }
    }
}
