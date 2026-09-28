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
"""Go.dot's patches to Tracktion Engine, by hand.

Produces: nothing but the change it names. Exits 0 when it did what was asked,
         1 with a sentence saying why not.
Usage:   python3 scripts/te-patches.py status
         python3 scripts/te-patches.py apply [--3way]
         python3 scripts/te-patches.py revert
         python3 scripts/te-patches.py refresh
         python3 scripts/te-patches.py new <name.patch> <file> [<file>...]
Build requirements: python3 and `git` on PATH, like scripts/check-pins.py.

WHAT IT IS FOR

The build applies patches/tracktion_engine/series to the Tracktion submodule's
working tree on every configure (cmake/WfgTracktionPatches.cmake), which is all a
build ever needs. This is for the moments the build cannot handle on its own, the
ones a person does deliberately:

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
(<Tracktion's git dir>/wfg-applied/): the series that is on, so an edited patch
can be taken off again by the configure after it.

Nothing here resets a tree it does not recognise. That is a person's decision:
`git -C ThirdParty/tracktion_engine checkout -- .` is the command, and it is
printed, never run.
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
TE = REPO_ROOT / "ThirdParty" / "tracktion_engine"
PATCH_DIR = REPO_ROOT / "patches" / "tracktion_engine"
SERIES = PATCH_DIR / "series"


def git(*args, check=False):
    """Run git inside the Tracktion submodule, never raising unless asked."""
    result = subprocess.run(["git", *args], cwd=str(TE), capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        sys.exit(f"te-patches: git {' '.join(args)} failed:\n{result.stderr}")
    return result


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
        sys.exit(f"te-patches: the series names {', '.join(missing)}, "
                 "which is not in patches/tracktion_engine/")
    return [PATCH_DIR / n for n in names]


def git_dir() -> Path:
    r = git("rev-parse", "--absolute-git-dir")
    if r.returncode != 0 or not r.stdout.strip():
        sys.exit("te-patches: ThirdParty/tracktion_engine is not a git checkout.\n"
                 "    Fix: scripts/bootstrap.sh   (or: git submodule update --init "
                 "ThirdParty/tracktion_engine)")
    return Path(r.stdout.strip())


def stamp_dir() -> Path:
    return git_dir() / "wfg-applied"


def signature(patches):
    """The same text cmake/WfgTracktionPatches.cmake writes: name and SHA-256 a line."""
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
    return git("status", "--porcelain", "--untracked-files=no").stdout.strip() == ""


def head():
    return git("rev-parse", "--short=12", "HEAD").stdout.strip() or "(unknown)"


def cmd_status(_args):
    patches = series_patches()
    names = ", ".join(p.name for p in patches) or "(none)"
    print(f"te-patches: Tracktion Engine {head()}, series: {names}")
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
        print(f"te-patches: Tracktion Engine {head()} already carries the series")
        return 0
    extra = ["--3way"] if args.three_way else []
    r = git("apply", *extra, *[str(p) for p in patches])
    if r.returncode != 0:
        print(r.stderr.rstrip())
        if args.three_way:
            print("te-patches: conflicts left in the tree; resolve them, then "
                  "`te-patches.py refresh`")
        else:
            print("te-patches: the series does not apply cleanly; try --3way")
        return 1
    write_stamp(patches)
    print(f"te-patches: Tracktion Engine {head()} now carries {len(patches)} patch(es)")
    return 0


def cmd_revert(_args):
    patches = series_patches()
    for candidate, what in ((patches, "the series"), (stamped_patches(), "the stamped series")):
        if candidate and fits(candidate, reverse=True):
            git("apply", "--reverse", *[str(p) for p in reversed(candidate)], check=True)
            clear_stamp()
            print(f"te-patches: took {what} off Tracktion Engine {head()}")
            return 0
    if is_clean():
        clear_stamp()
        print(f"te-patches: Tracktion Engine {head()} is already a clean checkout")
        return 0
    print("te-patches: the tree carries changes that are not the series; left alone.\n"
          "    See them:      git -C ThirdParty/tracktion_engine status   (and: diff)\n"
          "    Discard them:  git -C ThirdParty/tracktion_engine checkout -- .")
    return 1


def split_patch(text):
    """The header (everything before the first `diff --git`) and the files named."""
    if text.startswith("diff --git "):
        header = ""
    else:
        at = text.find("\ndiff --git ")
        header = text[: at + 1] if at >= 0 else text
    files = []
    for line in text.splitlines():
        if line.startswith("diff --git a/"):
            files.append(line[len("diff --git a/"):].split(" b/", 1)[0])
    return header, files


def cmd_refresh(_args):
    patches = series_patches()
    for p in patches:
        header, files = split_patch(p.read_text(encoding="utf-8"))
        if not files:
            print(f"te-patches: {p.name} names no file; left as it is")
            continue
        body = git("diff", "--", *files, check=True).stdout
        if not body.strip():
            sys.exit(f"te-patches: {p.name}'s files carry no change in the tree - "
                     "is the series on? (`te-patches.py status`)")
        p.write_text(header + body, encoding="utf-8", newline="\n")
        print(f"te-patches: {p.name} rewritten from {len(files)} file(s)")
    write_stamp(patches)
    return 0


def cmd_new(args):
    name = args.name if args.name.endswith(".patch") else args.name + ".patch"
    target = PATCH_DIR / name
    if target.exists():
        sys.exit(f"te-patches: {name} exists already; edit it and `refresh`")
    taken = {f for p in series_patches() for f in split_patch(p.read_text(encoding='utf-8'))[1]}
    clash = [f for f in args.files if f in taken]
    if clash:
        sys.exit(f"te-patches: {', '.join(clash)} already belongs to a patch in the series; "
                 "a file is kept in one patch, so refresh can split the tree's diff back")
    body = git("diff", "--", *args.files, check=True).stdout
    if not body.strip():
        sys.exit("te-patches: those files carry no change in the tree yet")
    header = (f"Go.dot patch {name.split('-', 1)[0]} for Tracktion Engine: <one line: what it does>.\n\n"
              "<Why Go.dot needs it, what it changes, and that each file it touches\n"
              "carries a notice that Go.dot modified it (GPL-3, section 5(a)).>\n\n"
              "Applied by Go.dot's build, cmake/WfgTracktionPatches.cmake, in the order\n"
              "patches/tracktion_engine/series gives; refreshed from the working tree with\n"
              f"scripts/te-patches.py refresh, which keeps this header. Against Tracktion\n"
              f"Engine {head()}.\n\n")
    target.write_text(header + body, encoding="utf-8", newline="\n")
    with SERIES.open("a", encoding="utf-8", newline="\n") as series:
        series.write(name + "\n")
    write_stamp(series_patches())
    print(f"te-patches: {name} written and added to the series; fill in its header")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
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

    if not TE.is_dir():
        sys.exit("te-patches: ThirdParty/tracktion_engine is not there")

    return {"status": cmd_status, "apply": cmd_apply, "revert": cmd_revert,
            "refresh": cmd_refresh, "new": cmd_new}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
