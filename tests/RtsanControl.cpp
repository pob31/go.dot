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

/*
    THE REAL-TIME SAFETY JOB'S CONTROL: one planted violation, so that a green
    job means the sanitizer looked rather than that nobody could hear it.

    WHY IT EXISTS (2026-09-30). The job ran for weeks unable to go red: with
    halt_on_error=0 a process that reports carries on and exits 0, and ctest
    showed the output of failed tests only. The first repair routed the reports
    through the sanitizers' log_path flag, and that was wrong too - RTSan in
    Clang 20 parses log_path and never applies it, because __rtsan_init does not
    call __sanitizer_set_report_path the way ASan and UBSan do. A gate that is
    green because it is deaf looks exactly like a gate that is green because the
    code is clean. This program is the difference: it walks into the rule on
    purpose, through Go.dot's own WFG_AUDIO_THREAD mark, and
    tests/rtsan_control.py fails unless the report comes out.

    It is built only by the rtsan preset (tests/CMakeLists.txt), where the mark
    is clang::nonblocking and -fsanitize=realtime makes every call inside it a
    real-time context. Everywhere else the mark is empty and there is nothing
    to prove, so nothing is built.

    The allocation goes through a volatile pointer so that no optimiser can
    elide the pair; the job builds Debug, but the control should not depend on
    that. The compiler's own effect analysis would object to the call at build
    time, which is the other half of the net and is silenced here only because
    this one call is the point.
*/

#include <wfg/engine/rt/RtCheck.h>

#include <cstdio>
#include <cstdlib>

#if defined (__clang__)
 #pragma clang diagnostic ignored "-Wfunction-effects"
#endif

namespace
{
    void* volatile planted = nullptr;

    void plantedViolation() noexcept WFG_AUDIO_THREAD
    {
        planted = std::malloc (64);
        std::free (planted);
        planted = nullptr;
    }
}

int main()
{
    plantedViolation();
    std::puts ("control: the planted call has been made");
    return 0;
}
