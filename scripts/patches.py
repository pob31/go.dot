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
"""Go.dot's patches to a vendored submodule (Tracktion Engine, JUCE), by hand.

Produces: nothing but the change it names. Exits 0 when it did what was asked,
         1 with a sentence saying why not.
Usage:   python3 scripts/patches.py <submodule> status
         python3 scripts/patches.py <submodule> apply [--3way]
         python3 scripts/patches.py <submodule> revert
         python3 scripts/patches.py <submodule> refresh
         python3 scripts/patches.py <submodule> new <name.patch> <file> [<file>...]
         where <submodule> is tracktion_engine or JUCE: the directory name under
         ThirdParty/ and under patches/. scripts/te-patches.py is this with
         tracktion_engine already given.
Build requirements: python3 and `git` on PATH, like scripts/check-pins.py.

WHAT IT IS FOR

The build applies patches/<submodule>/series to the submodule's working tree on
every configure (cmake/WfgSubmodulePatches.cmake), which is all a build ever
needs. This is for the moments the build cannot handle on its own, the ones a
person does deliberately:

  status   which of the three states the tree is in: carrying the series, a clean
           checkout the series fits, or something else.
  apply    put the series on, as the configure would. --3way lets git merge a hunk
           that no longer fits exactly, leaving conflict markers to resolve - the
           step after a pin move.
  revert   take the series off, leaving a clean checkout. BEFORE pulling a commit
           that moves the pin: `git submodule update` refuses to check a new commit
           out over files the patches changed.
  refresh  rewrite each patch from the working tree, over the files that patch
           already names, keeping the header above its first `diff --git`. How an
           edit made in the submodule becomes an edit to the patch.
  new      start a patch over the named files (paths inside the submodule), with a
           header to fill in, and add it to the end of the series.

Every command that changes the tree also writes the stamp the configure reads
(<the submodule's git dir>/wfg-applied/): the series that is on, so an edited
patch can be taken off again by the configure after it.

A patch is handled as bytes, never as text: JUCE stores most of its sources with
CRLF, so a patch to one carries a CR at the end of every hunk line, and reading it
as text would quietly turn them into LF and the patch into one that fits nothing.

Nothing here resets a tree it does not recognise. That is a person's decision:
`git -C ThirdParty/<submodule> checkout -- .` is the command, and it is printed,
never run.
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# The series of patches Go.dot keeps, by their folder under patches/: the tree
# under ThirdParty/ each applies to, and what a message calls it. JUCE has two
# (namespace draft §125, BFD): JUCE's own fixes, and Go.dot's own changes.
PATCHED = {
    "tracktion_engine": ("tracktion_engine", "Tracktion Engine"),
    "JUCE": ("JUCE", "JUCE"),
    "JUCE-local": ("JUCE", "JUCE (Go.dot's own changes)"),
}

# Set by main() from the first argument, before any command runs: the series,
# and the submodule it applies to.
SERIES_NAME = ""
SUBMODULE = ""
NAME = ""
TREE = Path()
PATCH_DIR = Path()
SERIES = Path()


def git(*args, check=False):
    """Run git inside the submodule, never raising unless asked. Text out: for
    messages and short answers only - a diff is read with git_bytes()."""
    result = subprocess.run(["git", *args], cwd=str(TREE), capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        sys.exit(f"patches: git {' '.join(args)} failed:\n{result.stderr}")
    return result


def git_bytes(*args):
    """Run git inside the submodule and return its output exactly as written."""
    result = subprocess.run(["git", *args], cwd=str(TREE), capture_output=True)
    if result.returncode != 0:
        sys.exit(f"patches: git {' '.join(args)} failed:\n"
                 + result.stderr.decode("utf-8", errors="replace"))
    return result.stdout


def read_series(path: Path):
    """The patch file names a series lists, in order."""
    if not path.is_file():
        return []
    names = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            names.append(line)
    return names


def series_patches():
    names = read_series(SERIES)
    missing = [n for n in names if not (PATCH_DIR / n).is_file()]
    if missing:
        sys.exit(f"patches: the series names {', '.join(missing)}, "
                 f"which is not in patches/{SERIES_NAME}/")
    return [PATCH_DIR / n for n in names]


def other_series_files():
    """The files the other series on the same tree change: never this one's, and
    never a sign that the tree carries something foreign."""
    files = set()
    for name, (tree, _) in PATCHED.items():
        if name == SERIES_NAME or tree != SUBMODULE:
            continue
        folder = REPO_ROOT / "patches" / name
        for patch in read_series(folder / "series"):
            if (folder / patch).is_file():
                files.update(split_patch((folder / patch).read_bytes())[1])
    return files


def git_dir() -> Path:
    r = git("rev-parse", "--absolute-git-dir")
    if r.returncode != 0 or not r.stdout.strip():
        sys.exit(f"patches: ThirdParty/{SUBMODULE} is not a git checkout.\n"
                 "    Fix: scripts/bootstrap.sh   (or: git submodule update --init "
                 f"ThirdParty/{SUBMODULE})")
    return Path(r.stdout.strip())


def stamp_dir() -> Path:
    # The tree's own series keeps the stamp it always had; another its own (§125).
    return git_dir() / ("wfg-applied" if SERIES_NAME == SUBMODULE else f"wfg-applied-{SERIES_NAME}")


def signature(patches):
    """The same text cmake/WfgSubmodulePatches.cmake writes: name and SHA-256 a line."""
    return "".join(f"{p.name} {hashlib.sha256(p.read_bytes()).hexdigest()}\n"
                   for p in patches)


def write_stamp(patches):
    """What is on the tree now, where the configure will look for it."""
    target = stamp_dir()
    shutil.rmtree(target, ignore_errors=True)
    target.mkdir(parents=True)
    shutil.copyfile(SERIES, target / "series")
    for p in patches:
        shutil.copyfile(p, target / p.name)
    (target / "signature").write_text(signature(patches), encoding="utf-8", newline="\n")


def clear_stamp():
    shutil.rmtree(stamp_dir(), ignore_errors=True)


def stamped_patches():
    target = stamp_dir()
    return [target / n for n in read_series(target / "series") if (target / n).is_file()]


def fits(patches, reverse=False):
    if not patches:
        return True
    ordered = list(reversed(patches)) if reverse else list(patches)
    args = ["apply", "--check"] + (["--reverse"] if reverse else [])
    return git(*args, *[str(p) for p in ordered]).returncode == 0


def is_clean():
    """No change in the tree but what another series on it makes."""
    others = other_series_files()
    for line in git("status", "--porcelain", "--untracked-files=no").stdout.splitlines():
        path = line[3:].strip()
        if path and path not in others:
            return False
    return True


def head():
    return git("rev-parse", "--short=12", "HEAD").stdout.strip() or "(unknown)"


def cmd_status(_args):
    patches = series_patches()
    names = ", ".join(p.name for p in patches) or "(none)"
    print(f"patches: {NAME} {head()}, series: {names}")
    if fits(patches, reverse=True):
        print("  carries the series")
        return 0
    if fits(patches):
        print("  a clean checkout the series fits - the next configure applies it")
        return 0
    old = stamped_patches()
    if old and fits(old, reverse=True):
        print("  carries an earlier version of the series - the next configure swaps it")
        return 0
    print("  neither: " + ("a clean checkout the series does not fit (a moved pin?)"
                           if is_clean() else "changes that are not the series"))
    print(git("status", "--short").stdout.rstrip())
    return 1


def cmd_apply(args):
    patches = series_patches()
    if fits(patches, reverse=True):
        write_stamp(patches)
        print(f"patches: {NAME} {head()} already carries the series")
        return 0
    extra = ["--3way"] if args.three_way else []
    r = git("apply", *extra, *[str(p) for p in patches])
    if r.returncode != 0:
        print(r.stderr.rstrip())
        if args.three_way:
            print("patches: conflicts left in the tree; resolve them, then "
                  f"`patches.py {SERIES_NAME} refresh`")
        else:
            print("patches: the series does not apply cleanly; try --3way")
        return 1
    write_stamp(patches)
    print(f"patches: {NAME} {head()} now carries {len(patches)} patch(es)")
    return 0


def cmd_revert(_args):
    patches = series_patches()
    for candidate, what in ((patches, "the series"), (stamped_patches(), "the stamped series")):
        if candidate and fits(candidate, reverse=True):
            git("apply", "--reverse", *[str(p) for p in reversed(candidate)], check=True)
            clear_stamp()
            print(f"patches: took {what} off {NAME} {head()}")
            return 0
    if is_clean():
        clear_stamp()
        print(f"patches: {NAME} {head()} is already a clean checkout")
        return 0
    print("patches: the tree carries changes that are not the series; left alone.\n"
          f"    See them:      git -C ThirdParty/{SUBMODULE} status   (and: diff)\n"
          f"    Discard them:  git -C ThirdParty/{SUBMODULE} checkout -- .")
    return 1


def split_patch(data: bytes):
    """The header (everything before the first `diff --git`) and the files named."""
    if data.startswith(b"diff --git "):
        header = b""
    else:
        at = data.find(b"\ndiff --git ")
        header = data[: at + 1] if at >= 0 else data
    files = []
    for line in data.split(b"\n"):
        if line.startswith(b"diff --git a/"):
            files.append(line[len(b"diff --git a/"):].split(b" b/", 1)[0].decode("utf-8"))
    return header, files


def cmd_refresh(_args):
    patches = series_patches()
    for p in patches:
        header, files = split_patch(p.read_bytes())
        if not files:
            print(f"patches: {p.name} names no file; left as it is")
            continue
        body = git_bytes("diff", "--", *files)
        if not body.strip():
            sys.exit(f"patches: {p.name}'s files carry no change in the tree - "
                     f"is the series on? (`patches.py {SERIES_NAME} status`)")
        p.write_bytes(header + body)
        print(f"patches: {p.name} rewritten from {len(files)} file(s)")
    write_stamp(patches)
    return 0


def cmd_new(args):
    name = args.name if args.name.endswith(".patch") else args.name + ".patch"
    target = PATCH_DIR / name
    if target.exists():
        sys.exit(f"patches: {name} exists already; edit it and `refresh`")
    taken = {f for p in series_patches() for f in split_patch(p.read_bytes())[1]} | other_series_files()
    clash = [f for f in args.files if f in taken]
    if clash:
        sys.exit(f"patches: {', '.join(clash)} already belongs to a patch of a series on this tree; "
                 "a file is kept in one patch, so refresh can split the tree's diff back")
    body = git_bytes("diff", "--", *args.files)
    if not body.strip():
        sys.exit("patches: those files carry no change in the tree yet")
    header = (f"Go.dot patch {name.split('-', 1)[0]} for {NAME}: <one line: what it does>.\n\n"
              "<Why Go.dot needs it, what it changes, and that each file it touches\n"
              "carries a notice that Go.dot modified it (GPL-3, section 5(a)).>\n\n"
              "Applied by Go.dot's build, cmake/WfgSubmodulePatches.cmake, in the order\n"
              f"patches/{SERIES_NAME}/series gives; refreshed from the working tree with\n"
              f"scripts/patches.py {SERIES_NAME} refresh, which keeps this header. Against\n"
              f"{NAME} {head()}.\n\n")
    target.write_bytes(header.encode("utf-8") + body)
    with SERIES.open("a", encoding="utf-8", newline="\n") as series:
        series.write(name + "\n")
    write_stamp(series_patches())
    print(f"patches: {name} written and added to the series; fill in its header")
    return 0


def main() -> int:
    global SERIES_NAME, SUBMODULE, NAME, TREE, PATCH_DIR, SERIES

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("submodule", choices=sorted(PATCHED))
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    apply = sub.add_parser("apply")
    apply.add_argument("--3way", dest="three_way", action="store_true",
                       help="let git merge hunks that no longer fit exactly")
    sub.add_parser("revert")
    sub.add_parser("refresh")
    new = sub.add_parser("new")
    new.add_argument("name")
    new.add_argument("files", nargs="+")
    args = parser.parse_args()

    SERIES_NAME = args.submodule
    SUBMODULE, NAME = PATCHED[SERIES_NAME]
    TREE = REPO_ROOT / "ThirdParty" / SUBMODULE
    PATCH_DIR = REPO_ROOT / "patches" / SERIES_NAME
    SERIES = PATCH_DIR / "series"

    if not TREE.is_dir():
        sys.exit(f"patches: ThirdParty/{SUBMODULE} is not there")

    return {"status": cmd_status, "apply": cmd_apply, "revert": cmd_revert,
            "refresh": cmd_refresh, "new": cmd_new}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
