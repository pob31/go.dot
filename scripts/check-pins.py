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
"""Submodule pin gate for Go.dot.

Produces: nothing. Exits 0 when the checkout is coherent, 1 with a message
         naming the offending SHAs or paths when it is not.
Usage:   python3 scripts/check-pins.py [--allow-skew]
         (also honours WFG_ALLOW_PIN_SKEW=1 in the environment)
Build requirements: python3 and `git` on PATH. Nothing else — no pip packages,
         because this is the first thing CI runs and the first thing
         scripts/bootstrap.sh runs, both before any toolchain exists.

WHY THIS EXISTS

Go.dot pins JUCE itself, at ThirdParty/JUCE, instead of taking the copy
Tracktion Engine vendors at its own modules/juce. That buys us an HTTPS URL
that works on a keyless CI runner (TE's .gitmodules uses
git@github.com:juce-framework/JUCE.git) and a build that never enters TE's root
CMakeLists. It costs us one obligation: OUR JUCE pin has to stay the SHA TE was
tested against, or we are running a combination nobody has ever built.

Nothing in git enforces that. This script does — as the first CI job, so a
skewed pair costs fifteen seconds instead of three platforms' worth of compile
minutes on a repo that is private, and therefore billed, until alpha.

THE SIX CHECKS

 (a) every submodule is checked out at the SHA its gitlink names
 (b) TE's own modules/juce gitlink == our ThirdParty/JUCE gitlink
 (c) ThirdParty/tracktion_engine/modules/juce/ is EMPTY on disk
 (d) no CMake file of ours does add_subdirectory() of TE's ROOT
 (e) no workflow uses `submodules: recursive`
 (f) ThirdParty/juce_simpleweb/asio/ is POPULATED on disk

(c) and (f) are the same question with opposite answers, and the pair is the
whole submodule policy in two lines. There are two nested submodules in this
tree. TE's vendored JUCE must stay EMPTY, because its URL is SSH and recursing
into it breaks every keyless clone. juce_simpleweb's asio must be POPULATED,
because the module does not compile without it and its URL is HTTPS. So the
rule is not "never recurse" but "recurse into exactly one path, by name" — which
is why (e) still forbids the blanket `submodules: recursive` while every job
runs a scoped `git submodule update --init --recursive ThirdParty/juce_simpleweb`.

(b) is the one with an escape hatch: --allow-skew (or WFG_ALLOW_PIN_SKEW=1)
downgrades it to a warning, for the deliberate case of moving JUCE ahead of TE.
The flag exists so that the person doing it has to say so out loud, in the
commit or in the CI invocation, rather than silently.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

JUCE = "ThirdParty/JUCE"
TE = "ThirdParty/tracktion_engine"
SIMPLEWEB = "ThirdParty/juce_simpleweb"
SPATCORE = "ThirdParty/spatcore"
TE_VENDORED_JUCE = "ThirdParty/tracktion_engine/modules/juce"
SIMPLEWEB_ASIO = "ThirdParty/juce_simpleweb/asio"

# How to repair each submodule, which is NOT the same command for all of them:
# juce_simpleweb has a nested asio and needs a scoped --recursive, and the other
# three must never be given one. The wrong advice in a failure message is worse
# than none, because it is the advice that gets pasted.
FIX_HINT = {
    JUCE: "git submodule update --init " + JUCE,
    TE: "git submodule update --init " + TE,
    SPATCORE: "git submodule update --init " + SPATCORE,
    SIMPLEWEB: "git submodule update --init --recursive " + SIMPLEWEB
               + "   (the --recursive is SCOPED to this path, and required: asio)",
}

# Directories we never scan for checks (d) and (e): vendor trees and build
# output. A match inside ThirdParty/ is TE's or JUCE's own CMake, which is
# exactly the code these checks exist to keep us OUT of.
PRUNE_DIRS = {".git", "build", "out", "ThirdParty", "__pycache__", ".ccache"}


def git(*args, cwd=REPO_ROOT):
    """Run git, never raising. Callers decide what a non-zero status means."""
    return subprocess.run(
        ["git", *args], cwd=str(cwd), capture_output=True, text=True
    )


def recorded_gitlink(path: str):
    """The SHA the superproject records for `path`, or None.

    HEAD first, then the index. The index fallback is not paranoia: on the very
    first commit of a repo the gitlinks are staged but not yet in HEAD, and this
    script has to be runnable at that moment — bootstrap.sh calls it.
    """
    # Mind the two output shapes, they differ by one field:
    #   git ls-tree  -> "160000 commit <sha>\t<path>"   (sha is field 2)
    #   git ls-files -> "160000 <sha> 0\t<path>"        (sha is field 1)
    for args, sha_field, where in (
        (("ls-tree", "HEAD", "--", path), 2, "HEAD"),
        (("ls-files", "-s", "--", path), 1, "the index"),
    ):
        r = git(*args)
        if r.returncode != 0 or not r.stdout.strip():
            continue
        fields = r.stdout.split()
        if fields[0] != "160000":
            return None, f"{path} is recorded in {where} as mode {fields[0]}, not a submodule"
        return fields[sha_field], None
    return None, f"{path} has no gitlink in HEAD or in the index"


def checked_out_head(path: str):
    """The SHA the submodule working tree is actually sitting on, or None."""
    r = git("rev-parse", "HEAD", cwd=REPO_ROOT / path)
    if r.returncode != 0:
        return None
    return r.stdout.strip()


def check_a(failures):
    """Every submodule is checked out at the SHA its gitlink names."""
    for path in (JUCE, TE, SIMPLEWEB, SPATCORE):
        recorded, err = recorded_gitlink(path)
        if err:
            failures.append(err)
            continue
        actual = checked_out_head(path)
        if actual is None:
            failures.append(
                f"{path} is not checked out (gitlink says {recorded}).\n"
                f"    Fix: scripts/bootstrap.sh   (or: {FIX_HINT[path]})\n"
                f"    Do NOT add a BLANKET --recursive and do NOT add --depth 1 —\n"
                f"    see the README."
            )
            continue
        if actual != recorded:
            failures.append(
                f"{path} is at the wrong commit.\n"
                f"    gitlink says : {recorded}\n"
                f"    working tree : {actual}\n"
                f"    Fix: {FIX_HINT[path]}   (or commit the move deliberately)"
            )
        else:
            print(f"  ok  (a) {path} @ {recorded[:12]}")


def check_b(failures, allow_skew: bool):
    """TE's own modules/juce gitlink == our ThirdParty/JUCE gitlink.

    This is the whole reason we can take TE's "tested against" guarantee while
    pinning JUCE ourselves.
    """
    ours, err = recorded_gitlink(JUCE)
    if err:
        failures.append(err)
        return

    r = git("ls-tree", "HEAD", "modules/juce", cwd=REPO_ROOT / TE)
    if r.returncode != 0 or not r.stdout.strip():
        failures.append(
            f"could not read tracktion_engine's own modules/juce gitlink "
            f"({TE} not checked out?)"
        )
        return
    theirs = r.stdout.split()[2]     # "160000 commit <sha>\tmodules/juce"

    if ours == theirs:
        print(f"  ok  (b) TE's modules/juce gitlink matches our JUCE pin @ {ours[:12]}")
        return

    message = (
        "JUCE pin skew: our ThirdParty/JUCE is NOT the JUCE that this Tracktion\n"
        "    Engine was tested against.\n"
        f"    ThirdParty/JUCE (ours)                        : {ours}\n"
        f"    tracktion_engine's own modules/juce gitlink   : {theirs}\n"
        "    Fix: move ThirdParty/JUCE to TE's SHA and commit BOTH gitlinks in one\n"
        "    commit, so a bisect can never land on a skewed pair:\n"
        f"        git -C ThirdParty/JUCE fetch origin && git -C ThirdParty/JUCE checkout {theirs}\n"
        "        git add ThirdParty/JUCE ThirdParty/tracktion_engine\n"
        "    If the skew is deliberate (a JUCE bump ahead of TE), re-run with\n"
        "    --allow-skew, or set WFG_ALLOW_PIN_SKEW=1."
    )
    if allow_skew:
        print(f"  WARN (b) {message}")
    else:
        failures.append(message)


def check_c(failures):
    """TE's vendored JUCE directory is empty on disk."""
    d = REPO_ROOT / TE_VENDORED_JUCE
    entries = sorted(p.name for p in d.iterdir()) if d.is_dir() else []
    if entries:
        failures.append(
            f"{TE_VENDORED_JUCE}/ is populated ({len(entries)} entries, e.g. "
            f"{', '.join(entries[:3])}).\n"
            "    Something ran `git submodule update --recursive`, or a checkout with\n"
            "    submodules: recursive. This build never enters that directory, but a\n"
            "    second JUCE checkout on disk means the same command WILL fail on any\n"
            "    machine or runner without an SSH key for git@github.com.\n"
            f"    Fix: rm -rf {TE_VENDORED_JUCE}/*   then run scripts/bootstrap.sh,\n"
            "    which sets submodule.modules/juce.update=none so it stays empty."
        )
    else:
        print(f"  ok  (c) {TE_VENDORED_JUCE}/ is empty")


def check_f(failures):
    """juce_simpleweb's nested asio IS populated — the inverse of check (c).

    The one nested submodule this build actually needs. juce_simpleweb.h
    includes asio.hpp unconditionally, so an unpopulated asio/ is not a
    degraded build, it is a compile error some way into the module — and on a
    fresh clone the cause looks like a broken module rather than a missing
    checkout step.

    Its URL is HTTPS (benkuper/asio), which is exactly why recursing into THIS
    path is safe when recursing into TE's vendored JUCE is not.
    """
    d = REPO_ROOT / SIMPLEWEB_ASIO
    entries = sorted(p.name for p in d.iterdir()) if d.is_dir() else []
    if not entries:
        failures.append(
            f"{SIMPLEWEB_ASIO}/ is empty.\n"
            "    juce_simpleweb includes asio.hpp unconditionally and will not\n"
            "    compile without it. `submodules: true` and a plain\n"
            "    `git submodule update --init` do NOT populate it, because it is\n"
            "    nested one level down.\n"
            f"    Fix: {FIX_HINT[SIMPLEWEB]}"
        )
    else:
        print(f"  ok  (f) {SIMPLEWEB_ASIO}/ is populated ({len(entries)} entries)")


def our_files(suffixes=None, names=None, under=None):
    """Walk the repo, pruning vendor and build trees."""
    root = REPO_ROOT if under is None else REPO_ROOT / under
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in PRUNE_DIRS]
        for f in filenames:
            if names and f in names:
                yield Path(dirpath) / f
            elif suffixes and any(f.endswith(s) for s in suffixes):
                yield Path(dirpath) / f


ADD_SUBDIR = re.compile(r"add_subdirectory\s*\(([^)]*)\)", re.IGNORECASE | re.DOTALL)


def check_d(failures):
    """No add_subdirectory() of tracktion_engine's ROOT anywhere in our CMake.

    TE's root CMakeLists does add_subdirectory(modules/juce) + enable_testing() +
    add_subdirectory(examples): a second JUCE over SSH, our test registry taken
    over, and DemoRunner / Benchmarks / TestRunner / EngineInPluginDemo added to
    `all`. Only .../tracktion_engine/modules is ever correct.
    """
    checked = 0
    for path in our_files(suffixes=(".cmake",), names={"CMakeLists.txt"}):
        checked += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in ADD_SUBDIR.finditer(text):
            args = m.group(1)
            if "tracktion_engine" in args and "tracktion_engine/modules" not in args:
                rel = path.relative_to(REPO_ROOT).as_posix()
                line = text[: m.start()].count("\n") + 1
                failures.append(
                    f"{rel}:{line} adds tracktion_engine's ROOT:\n"
                    f"        add_subdirectory({args.strip()})\n"
                    "    Only ThirdParty/tracktion_engine/modules may be added. TE's root\n"
                    "    pulls a second JUCE over SSH, calls enable_testing() in our tree,\n"
                    "    and adds its examples to `all`."
                )
    print(f"  ok  (d) {checked} CMake file(s) scanned, none adds TE's root")


def check_e(failures):
    """No workflow uses `submodules: recursive`."""
    wf = REPO_ROOT / ".github" / "workflows"
    if not wf.is_dir():
        print("  ok  (e) no .github/workflows/ yet")
        return
    hits = 0
    for path in sorted(wf.rglob("*.y*ml")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for n, line in enumerate(text.splitlines(), start=1):
            # Strip YAML comments before matching. ci.yml carries a long comment
            # explaining why `submodules: recursive` is wrong here, and a gate
            # that trips on its own explanation is a gate people delete.
            if re.search(r"submodules\s*:\s*recursive", line.split("#", 1)[0]):
                hits += 1
                failures.append(
                    f"{path.relative_to(REPO_ROOT).as_posix()}:{n} uses "
                    "`submodules: recursive`.\n"
                    "    That is WFS-DIY's setting and it breaks THIS repo: recursion\n"
                    "    descends into tracktion_engine's modules/juce, whose URL is\n"
                    "    git@github.com:juce-framework/JUCE.git, and no runner has an SSH\n"
                    "    key for it. Use `submodules: true`."
                )
    if not hits:
        print("  ok  (e) no workflow uses submodules: recursive")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--allow-skew",
        action="store_true",
        help="downgrade check (b), the JUCE/TE pin-parity check, to a warning",
    )
    args = ap.parse_args()
    allow_skew = args.allow_skew or os.environ.get("WFG_ALLOW_PIN_SKEW") == "1"

    if git("rev-parse", "--git-dir").returncode != 0:
        sys.exit(f"error: {REPO_ROOT} is not a git checkout")

    print(f"check-pins: {REPO_ROOT}")
    failures: list[str] = []
    check_a(failures)
    check_b(failures, allow_skew)
    check_c(failures)
    check_d(failures)
    check_e(failures)
    check_f(failures)

    if failures:
        print("\ncheck-pins: FAILED\n")
        for i, f in enumerate(failures, start=1):
            print(f"  {i}. {f}\n")
        return 1

    print("\ncheck-pins: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
