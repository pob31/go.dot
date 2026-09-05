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

#include <wfg/engine/cue/OscJob.h>

namespace wfg::cue
{
    OscWait oscWaitFrom (const std::string& text) noexcept
    {
        /*  Anything unknown is `none` rather than a refusal, because the
            grammar has already checked this word against a closed set - and a
            cue that refused to fire because somebody hand-edited a document
            would be a cue that does nothing on a show night. `none` is also the
            weaker promise of the two, which is the right way to be wrong: it
            reports done early rather than waiting for something that is not
            coming. */
        if (text == "verified")
            return OscWait::verified;

        return text == "sent" ? OscWait::sent : OscWait::none;
    }
}
