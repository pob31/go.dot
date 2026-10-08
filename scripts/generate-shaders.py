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
"""Translates the video renderer's shaders for Direct3D 11, Metal and OpenGL.

Produces: src/wfg/engine/video/render/shaders/video.glsl.h, from video.glsl
          beside it (namespace draft §44.4).
Usage:    python3 scripts/generate-shaders.py [--check]
          --check writes nothing and exits 1 if the committed header was not
          made from the shader source as it stands - by the hash the header
          carries, so it needs no tool and runs anywhere.
Build requirements: python3 and its standard library; the first run without
          --check downloads sokol-shdc, pinned below, into build/tools.

WHY THE OUTPUT IS COMMITTED

So that a clone builds without the translator: sokol-shdc is a binary per
system, and only somebody changing a shader needs it. The header's first line
is the SHA-256 of the source it was made from; `--check`, and the test
`video: the shader header was made from the shader source`, hold the two
together.

THE TOOL

sokol-shdc from floooh/sokol-tools-bin (MIT), at the commit below, fetched from
GitHub as a single file and held to the git blob hash GitHub lists for it - so
a file that is not that commit's is refused, not run.
"""

import argparse
import hashlib
import io
import os
import platform
import stat
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SHADERS = REPO_ROOT / "src" / "wfg" / "engine" / "video" / "render" / "shaders"
SOURCE = SHADERS / "video.glsl"
HEADER = SHADERS / "video.glsl.h"

TOOLS_COMMIT = "11d0cf678105d614d675e6d9bd2aaf3eeff12f8c"

# (folder in sokol-tools-bin, file name, git blob SHA-1 at TOOLS_COMMIT)
TOOLS = {
    ("Windows", "AMD64"): ("win32", "sokol-shdc.exe", "de6e3133364c6b3207ef8aa19a93010ef558c08c"),
    ("Linux", "x86_64"): ("linux", "sokol-shdc", "eb8120a9846b54f8c6f8d271681bc8ad6b0db207"),
    ("Linux", "aarch64"): ("linux_arm64", "sokol-shdc", "6397df4b955ef81746ecdb487e94913018fa0b91"),
    ("Darwin", "x86_64"): ("osx", "sokol-shdc", "d8a133d402bfeb7d52ef5853c4dea5820c114462"),
    ("Darwin", "arm64"): ("osx_arm64", "sokol-shdc", "b0b03af19c4f86a8f9920f0b63b9250ee40d2bbe"),
}

LANGUAGES = "hlsl5:metal_macos:glsl410"
STAMP = "// source-sha256: "


def source_hash():
    """The SHA-256 of the shader source, line endings as git stores them."""
    text = SOURCE.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(text).hexdigest()


def stamped_hash():
    with io.open(HEADER, encoding="utf-8") as f:
        first = f.readline().strip()
    return first[len(STAMP):] if first.startswith(STAMP) else ""


def blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def tool():
    key = (platform.system(), platform.machine())
    if key not in TOOLS:
        sys.exit(f"generate-shaders: no sokol-shdc for {key[0]} {key[1]}")
    folder, name, blob = TOOLS[key]
    path = REPO_ROOT / "build" / "tools" / "sokol-shdc" / TOOLS_COMMIT[:12] / name

    if path.exists() and blob_sha1(path.read_bytes()) == blob:
        return path

    url = f"https://raw.githubusercontent.com/floooh/sokol-tools-bin/{TOOLS_COMMIT}/bin/{folder}/{name}"
    print(f"generate-shaders: fetching {url}")
    data = urllib.request.urlopen(url, timeout=120).read()
    if blob_sha1(data) != blob:
        sys.exit("generate-shaders: the downloaded sokol-shdc is not the pinned commit's - refused")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def generate():
    out = HEADER.with_suffix(".tmp")
    subprocess.run([str(tool()), "-i", str(SOURCE), "-o", str(out), "-l", LANGUAGES,
                    "--ifdef", "--no-log-cmdline", "--tmpdir", str(out.parent)], check=True)
    body = out.read_bytes().replace(b"\r\n", b"\n")
    out.unlink()
    # Generated code is read as a system header's, so the strict warnings meant
    # for Go.dot's own code do not fall on code nobody here wrote by hand.
    preamble = STAMP + source_hash() + "\n#if defined(__GNUC__) || defined(__clang__)\n#pragma GCC system_header\n#endif\n"
    HEADER.write_bytes(preamble.encode("utf-8") + body)
    print(f"generate-shaders: wrote {HEADER.relative_to(REPO_ROOT)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if args.check:
        if stamped_hash() != source_hash():
            print("generate-shaders: video.glsl.h was not made from video.glsl as it stands; "
                  "run python3 scripts/generate-shaders.py", file=sys.stderr)
            return 1
        print("generate-shaders: the header matches its source")
        return 0

    generate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
