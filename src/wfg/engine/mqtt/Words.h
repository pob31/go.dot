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
    A TOPIC AS AN ADDRESS, A PAYLOAD AS VALUES (namespace draft §67, ANA,
    ANC, ANF; MQ.1): the two rules every MQTT message crosses on its way
    between a broker and the show, written once for the session that hears
    and the sender that publishes.

    Topic `room1/temp` on a device whose prefix is `/house` is the address
    `/house/room1/temp`; back the other way, the address under the prefix,
    its leading slash gone, is the topic. A topic that cannot be an OSC
    address - a space, or `# * , ? [ ] { }` - is nothing, and the caller
    counts it dropped.

    A payload is read as a serial line is (§51, ACR): split on spaces and
    commas, numbers as numbers; one beginning with `{` or `[` is one string,
    for JSON; one that is not text is one blob. Out, the values joined by
    spaces, numbers in no locale's way, one string as it is.
*/

#include <wfg/engine/osc/OscValue.h>

#include <optional>
#include <string>
#include <string_view>

namespace wfg::mqtt
{
    /*  `prefixRow` is the device's `prefix` row - one root, or several
        space-separated (§15.1a) - and the first is the one topics land
        under. nullopt for a topic that cannot be an address. */
    std::optional<std::string> addressOfTopic (const std::string& prefixRow, std::string_view topic);

    /*  The topic for an address under one of the device's roots; nullopt for
        an address not under them, or one that is the root alone. */
    std::optional<std::string> topicOfAddress (const std::string& prefixRow, const std::string& address);

    osc::Values valuesOfPayload (const std::string& payload);
    std::string payloadOfValues (const osc::Values& values);

    /*  Whether the bytes are well-formed UTF-8 with no NUL and no control
        character but tab, carriage return and new line - text a patch or a
        script can be handed as words. */
    bool isText (std::string_view bytes) noexcept;
}
