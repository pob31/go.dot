#!/usr/bin/env python3
# This file is part of Go.dot — https://github.com/pob31/go.dot
#
# Copyright (C) 2026 Pierre-Olivier Boulant
#
# Go.dot is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. Go.dot is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
# (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetches, or checks, the Spout sources vendored in ThirdParty/spout.

Produces: ThirdParty/spout/*, the files SpoutDX is built from, at SPOUT_COMMIT.
Usage:    python3 scripts/fetch-spout.py [--check]
          --check downloads nothing into the tree and exits 1 if a vendored
          file is not the pinned commit's, byte for byte.
Build requirements: python3, its standard library, and the network.

WHY VENDORED (namespace draft §44, N.1): Spout2's repository is 655 MB of
binaries and examples, and every CI job would clone it to build eight files.
Those eight are copied here unchanged, BSD 2-Clause, with the licence; this
script is how they are moved to another commit, and how anybody checks that
nobody edited them.
"""

import argparse
import sys
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
FOLDER = REPO_ROOT / "ThirdParty" / "spout"
SPOUT_COMMIT = "c2bcc12147711d12ace7d5f08e869d774d840f8a"

FILES = {
    "LICENSE": "LICENSE",
    "SpoutDX.h": "SPOUTSDK/SpoutDirectX/SpoutDX/SpoutDX.h",
    "SpoutDX.cpp": "SPOUTSDK/SpoutDirectX/SpoutDX/SpoutDX.cpp",
}

for name in ("SpoutCommon.h", "SpoutCopy.h", "SpoutCopy.cpp", "SpoutDirectX.h", "SpoutDirectX.cpp",
             "SpoutFrameCount.h", "SpoutFrameCount.cpp", "SpoutSenderNames.h", "SpoutSenderNames.cpp",
             "SpoutSharedMemory.h", "SpoutSharedMemory.cpp", "SpoutUtils.h", "SpoutUtils.cpp"):
    FILES[name] = "SPOUTSDK/SpoutGL/" + name


def fetch(path):
    url = f"https://raw.githubusercontent.com/leadedge/Spout2/{SPOUT_COMMIT}/{path}"
    return urllib.request.urlopen(url, timeout=120).read()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    different = []

    for name, path in FILES.items():
        data = fetch(path)
        here = FOLDER / name

        if args.check:
            if not here.exists() or here.read_bytes().replace(b"\r\n", b"\n") != data.replace(b"\r\n", b"\n"):
                different.append(name)
        else:
            FOLDER.mkdir(parents=True, exist_ok=True)
            here.write_bytes(data)

    if different:
        print("fetch-spout: not the pinned commit's: " + ", ".join(different), file=sys.stderr)
        return 1

    print("fetch-spout: " + ("every file is the pinned commit's" if args.check else f"wrote {len(FILES)} files"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
