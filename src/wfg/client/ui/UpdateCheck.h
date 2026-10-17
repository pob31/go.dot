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
    ASKING GITHUB WHETHER A NEWER GO.DOT IS OUT (namespace draft §78).

    WFS-DIY's UpdateChecker, ported (its Source/UpdateChecker.h): ONE GET, on a
    thread of its own that is NEVER JOINED. A blocking request on Windows cannot
    be interrupted from another thread - the connection timeout and cancel are
    both ignored by a synchronous WinINet handle - so a window that waited for
    the thread on its way out could hang for the system's TCP timeout. Instead
    the window's token is cleared as it goes: the worker then touches nothing of
    the window's, and if it is still blocked the system reaps it at exit. What
    it found reaches the window on the message thread, by `callAsync`, or not
    at all.

    IT CAN NEVER TOUCH THE SHOW (AST): no lock or wait on any thread the engine
    uses, nothing on the audio thread, nothing in the tree or the log. Only the
    window starts it (ASS) - `wfg serve` without a window, the tests and CI never
    reach the network, the window asking at launch only when the launchers said
    `--remember`.

    WHAT IT SENDS (ASU): `User-Agent: Go.dot/<version>`, which GitHub's API
    requires, and the API's own Accept; nothing about the machine or the show.
*/

#include <wfg/client/model/Updates.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace wfg::client::ui
{
    class UpdateCheck final
    {
    public:
        //  What an ask came back with, read by the model (model/Updates.h, `answerFor`).
        using Answer = model::updates::Answer;

        using Done = std::function<void (const Answer&)>;

        /*  `url` is GitHub's `latest` unless a test names a file (`file:` URLs are
            read as files, which is how the tests ask without a network). */
        UpdateCheck (std::string url, std::string version);
        ~UpdateCheck();

        /*  Asks on a thread of its own; `done` is called on the message thread,
            once, unless this has gone first. Two asks at once are two threads. */
        void ask (Done done);

        /*  The ask itself, blocking: what the thread runs, and what a test calls. */
        static Answer askNow (const std::string& url, const std::string& version);

    private:
        std::string url, version;
        std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };
    };
}
