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
    Go.dot.app's main executable, and nothing else: it runs launch.sh from the
    bundle's Resources, which starts wfg.

    WHY A PROGRAM AND NOT THE SCRIPT ITSELF. A script can be a bundle's main
    executable, but it cannot carry the hardened runtime notarization requires,
    and its signature lives in extended attributes a copy can drop. A compiled
    stub signs like any other binary. And why not wfg itself: `wfg` with no
    arguments is an error by design (Console.cpp, "No default command"), and
    Finder gives it none.

    The script, sealed in the bundle's signature like every other resource, is
    where the decisions are - where the empty show lives, where the log goes -
    so they can be read and changed without a compiler.
*/

#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main (void)
{
    char executable[PATH_MAX];
    uint32_t size = sizeof (executable);

    if (_NSGetExecutablePath (executable, &size) != 0)
    {
        fprintf (stderr, "Go.dot: the path to this program is too long\n");
        return 1;
    }

    char resolved[PATH_MAX];

    if (realpath (executable, resolved) == NULL)
    {
        perror ("Go.dot: could not resolve this program's path");
        return 1;
    }

    /* .../Go.dot.app/Contents/MacOS/Go.dot -> .../Go.dot.app/Contents */
    char* slash = strrchr (resolved, '/');
    if (slash != NULL) *slash = '\0';
    slash = strrchr (resolved, '/');
    if (slash != NULL) *slash = '\0';

    char script[PATH_MAX];

    if (snprintf (script, sizeof (script), "%s/Resources/launch.sh", resolved) >= (int) sizeof (script))
    {
        fprintf (stderr, "Go.dot: the path to launch.sh is too long\n");
        return 1;
    }

    execl ("/bin/sh", "sh", script, (char*) NULL);
    perror ("Go.dot: could not start /bin/sh");
    return 1;
}
