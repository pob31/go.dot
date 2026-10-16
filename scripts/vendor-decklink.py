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
"""Copies, or checks, the DeckLink SDK headers carried in ThirdParty/decklink.

Produces: ThirdParty/decklink/{win,mac,linux}/*, the SDK's three include
          folders as Blackmagic ships them; win/generated/DeckLinkAPI_h.h and
          DeckLinkAPI_i.c, made from win/DeckLinkAPI.idl by MIDL; MANIFEST.txt,
          the SDK's version and every file's SHA-256.
Usage:    python3 scripts/vendor-decklink.py --from "<Blackmagic DeckLink SDK folder>"
          python3 scripts/vendor-decklink.py --check
          --check needs no SDK and exits 1 if a carried file is not the one the
          manifest names; with --from as well, it also compares them with the SDK.
Build requirements: python3 and its standard library; for --from, Windows with
          Visual Studio's C++ tools (MIDL, and the preprocessor it calls).

WHY CARRIED (namespace draft §71, APA): the SDK sits behind a registration form,
so no build can fetch it. Its agreement lifts its restrictive clauses for these
three include folders, and each header grants copying and distribution so long
as its notice stays (PRD §6.3). Only the headers: the library comes with the
user's Desktop Video, and the SDK's examples are never copied in. The Windows
header is generated once, here, so no build needs MIDL; the notice of the .idl it
comes from is put at its head, as the headers' grant asks.

Hashes are taken with line endings made LF, as .gitattributes stores .h and .cpp
files that way whatever the SDK shipped.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
FOLDER = REPO_ROOT / "ThirdParty" / "decklink"
MANIFEST = FOLDER / "MANIFEST.txt"
SYSTEMS = {"win": "Win/include", "mac": "Mac/include", "linux": "Linux/include"}
GENERATED = ("DeckLinkAPI_h.h", "DeckLinkAPI_i.c")


def digest(data):
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def sdk_version(sdk):
    text = (sdk / "Linux" / "include" / "DeckLinkAPIVersion.h").read_text(encoding="utf-8", errors="replace")
    found = re.search(r'BLACKMAGIC_DECKLINK_API_VERSION_STRING\s+"([^"]+)"', text)
    return found.group(1) if found else "unknown"


def find_vcvars():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    where = subprocess.run([str(vswhere), "-latest", "-property", "installationPath"],
                           capture_output=True, text=True).stdout.strip()
    bat = Path(where) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return bat if bat.exists() else None


def notice_of(idl):
    text = idl.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
    found = re.search(r"/\* -LICENSE-START-.*?-LICENSE-END-\n \*/\n", text, re.S)
    if not found:
        raise SystemExit("vendor-decklink: DeckLinkAPI.idl carries no licence block")
    return found.group(0)


def generate(win_folder, version):
    """MIDL's header and identifiers for DeckLinkAPI.idl, each headed by its notice."""
    if os.name != "nt":
        raise SystemExit("vendor-decklink: MIDL runs on Windows only")

    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        command = f'midl /nologo /env x64 /W1 /I "{win_folder}" /out "{work}" /h {GENERATED[0]} /iid {GENERATED[1]} /tlb DeckLinkAPI.tlb "{win_folder / "DeckLinkAPI.idl"}"'

        if shutil.which("midl") is None or shutil.which("cl") is None:
            vcvars = find_vcvars()
            if vcvars is None:
                raise SystemExit("vendor-decklink: MIDL is not on PATH and Visual Studio's vcvars64.bat was not found")
            script = work / "midl.bat"
            script.write_text(f'@call "{vcvars}" >nul\r\n@{command}\r\n', encoding="utf-8")
            result = subprocess.run(["cmd", "/c", str(script)], capture_output=True, text=True)
        else:
            result = subprocess.run(command, shell=True, capture_output=True, text=True)

        if result.returncode != 0:
            sys.stderr.write(result.stdout + result.stderr)
            raise SystemExit("vendor-decklink: MIDL failed")

        head = notice_of(win_folder / "DeckLinkAPI.idl")
        head += f"\n/* Generated from DeckLinkAPI.idl of Desktop Video SDK {version} by MIDL, through\n" \
                "   scripts/vendor-decklink.py. Do not edit: run the script again. */\n\n"
        out = win_folder / "generated"
        out.mkdir(exist_ok=True)

        for name in GENERATED:
            body = (work / name).read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
            (out / name).write_text(head + body, encoding="utf-8", newline="\n")


def carried_files():
    files = []
    for system in SYSTEMS:
        files += sorted(p for p in (FOLDER / system).rglob("*") if p.is_file())
    return files


def write_manifest(version):
    lines = [f"# DeckLink SDK headers carried by Go.dot - scripts/vendor-decklink.py", f"version {version}"]
    for path in carried_files():
        lines.append(f"{digest(path.read_bytes())}  {path.relative_to(FOLDER).as_posix()}")
    MANIFEST.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def read_manifest():
    entries = {}
    version = "unknown"
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        if line.startswith("version "):
            version = line.split(" ", 1)[1]
        elif line and not line.startswith("#"):
            hash_, name = line.split("  ", 1)
            entries[name] = hash_
    return version, entries


def check(sdk):
    if not MANIFEST.exists():
        print("vendor-decklink: no MANIFEST.txt", file=sys.stderr)
        return 1

    version, entries = read_manifest()
    present = {p.relative_to(FOLDER).as_posix(): p for p in carried_files()}
    wrong = [name for name, hash_ in entries.items() if name not in present or digest(present[name].read_bytes()) != hash_]
    wrong += [name for name in present if name not in entries]

    if sdk is not None:
        for system, sub in SYSTEMS.items():
            for source in sorted((sdk / sub).iterdir()):
                name = f"{system}/{source.name}"
                if name not in present or digest(source.read_bytes()) != digest(present[name].read_bytes()):
                    wrong.append(f"{name} (against the SDK)")

    if wrong:
        print("vendor-decklink: not the carried SDK's: " + ", ".join(sorted(set(wrong))), file=sys.stderr)
        return 1

    print(f"vendor-decklink: every file is SDK {version}'s, as carried")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--from", dest="sdk", type=Path, help="the unzipped Blackmagic DeckLink SDK folder")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if args.check:
        return check(args.sdk)

    if args.sdk is None:
        parser.error("--from or --check is needed")

    version = sdk_version(args.sdk)

    for system, sub in SYSTEMS.items():
        target = FOLDER / system
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        for source in sorted((args.sdk / sub).iterdir()):
            shutil.copyfile(source, target / source.name)

    generate(FOLDER / "win", version)
    write_manifest(version)
    print(f"vendor-decklink: carried SDK {version}, {len(carried_files())} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
