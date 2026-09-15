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

"""A large show, written to disk: the 500-cue bundle a console is measured on.

Usage:   python3 tests/fixtures/make_large_show.py <folder> [--cues N] [--force]
Produces: a bundle folder - show.xml, state.xml, <folder name>.wfg and
         namespaces/wfs.json - and one line on stdout saying what is in it.
Exits:   0 written, 2 refused (a count below one; a folder inside
         tests/fixtures/bundles/ or holding it; or a folder that already has
         something in it - with --force, anything but this script's own
         earlier output), and in every refusal nothing on disk is touched.

STDLIB ONLY, for the reason tests/blackbox/common.py gives: this runs on a
contributor's machine at the moment somebody needs a number, and every
dependency is one more thing that can be missing then.

THE SHAPE IS M18's (§13.14) AND M23's (§14.14), because the measurements are
meant to be quoted side by side. `tests/AnalysisTests.cpp` ("M18: the analysis
is a cache asked and not told") and `tests/BundleTests.cpp` ("M23: an autosave
of a 500-cue show") build it straight into a tree: five hundred `Media` cues,
each with one `Feed` into one of twenty slots declared on a mount. This writes
the same thing as a bundle, because the thing measured here - M24, the web
console's `render()` - reads a running engine, and a running engine opens a
folder.

PLUS WHAT A CONSOLE HAS TO DRAW, which M18 never needed because an analysis
does not draw anything:

  - every 25th top-level position is a `Group` holding the next four media
    cues, cycling manual, automatic and timeline, so that nesting, folding
    and a group's flag all render - and a manual one, so that a claim inside
    it is not ended by the group;
  - a `Trigger` on every 25th media cue - twenty for five hundred - mostly
    osc, some midi, a few clock and one disabled, so the row's lightning mark
    and the inspector's trigger list have something to show;
  - some cues with a pre-wait or a post-wait, which the row writes as a word;
  - an `Audio` section with a two-wide bus beside the one the slots sit on;
  - a `state.xml` parking the standby on the first cue.

THE GROUP MEMBERS ARE MEDIA CUES FROM THE SAME COUNT, so `--cues 500` is five
hundred media cues with five hundred feeds, exactly as M18 has them, and the
groups are extra rows on top: seventeen of them at the default, 517 rows.

THE MEDIA FILE IS NAMED AND NOT WRITTEN. `tone.wav` is absent, and that is
allowed: a missing file never fails the load (show.rng, on Media/@file), so the
bundle opens, validates and serves; it fails the cue at the arm, with
`run.failed media-missing`. With no audio side, which is how
scripts/measure-console-render.py serves it, nothing is armed at all, so
nothing is reported either. The consequence is worth knowing before reading a
measurement taken on it: with no length for any cue, the slot analysis falls
back to rows, and every two top-level cues on one slot are an overlap - about
three hundred pairs per slot here, which is the over-report M18 was written to
count, and which the console draws as a "shares" flag on every row.

CANONICAL, byte for byte. Attributes are written `id` first and the rest in
name order, and a value equal to its default is left out, which is what
`CanonicalXml::write` does - so `wfg canon show.xml` gives back this file
unchanged, and a diff against a saved copy shows what the session changed and
nothing about how this script spells things.

IDENTIFIERS are eight characters of Crockford base32, upper case, no I, L, O
or U (§1): a two-letter prefix saying what the object is, then a counter. Every
one is checked for shape and for uniqueness before anything is written.

NEVER INSIDE tests/fixtures/bundles/. `scripts/validate-show.py` with no
arguments validates every folder there as a committed fixture, and a generated
show left behind in it would become one by accident - and a 500-cue fixture
nobody reviewed, with a few thousand overlap warnings, in every run. Nor a
folder that holds it, which is the repository or on the way to it.

--force REPLACES ONLY WHAT THIS SCRIPT WROTE. It deletes the folder whole,
because the engine writes into a bundle it serves - a `recovery/` left from an
earlier session would be offered at the next open - and a folder is taken for
this script's own only when it holds `<folder name>.wfg` and a
namespaces/wfs.json whose description says it was written here. Every other
folder with something in it is refused, --force or not: a mistyped path is
somebody's work, and a flag that deletes whatever it is pointed at is one
typing mistake from deleting the repository. The show is built before the
disk is touched, so a refusal from the build leaves the old folder standing.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
FIXTURE_BUNDLES = REPO / "tests" / "fixtures" / "bundles"

CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

SLOT_COUNT = 20          # M18's
GROUP_EVERY = 25         # a group at every 25th top-level position
GROUP_SIZE = 4
TRIGGER_EVERY = 25       # a trigger on every 25th media cue
TRIGGER_OFFSET = 12      # ...starting at the thirteenth, so none is on the standby

MEDIA_FILE = "tone.wav"  # absent, see above

# Written into the namespace's description, and looked for there by --force:
# one string in both places, so the writing and the recognising cannot drift.
WRITTEN_BY = "Written by tests/fixtures/make_large_show.py."

# Names an operator might read down a list. Deterministic, so two runs write
# the same bytes and a measurement taken on one can be retaken on the other.
WORDS = ("Walk-in", "House to half", "Rain", "Thunder", "Crowd", "Voice",
         "Bed", "Wind", "Bells", "Door", "Footsteps", "Birds", "Traffic",
         "Organ", "Choir", "Siren", "Clock", "Sea", "Radio", "Train")


class Refused(Exception):
    """Something this script will not do. Distinct from a bug in it."""


# =============================================================================
# Identifiers
# =============================================================================

class Identifiers:
    """Eight-character Crockford identifiers, each handed out once."""

    def __init__(self) -> None:
        self.used: "set[str]" = set()

    def make(self, prefix: str, n: int) -> str:
        digits = ""
        value = n

        while True:
            digits = CROCKFORD[value % 32] + digits
            value //= 32
            if value == 0:
                break

        ident = prefix + digits.rjust(8 - len(prefix), "0")

        if len(ident) != 8 or any(c not in CROCKFORD for c in ident):
            raise AssertionError(f"{ident!r} is not an identifier")
        if ident in self.used:
            raise AssertionError(f"{ident} handed out twice")

        self.used.add(ident)
        return ident


# =============================================================================
# Canonical XML, the part of it this show needs
# =============================================================================

def escape(value: str) -> str:
    return (value.replace("&", "&amp;").replace("<", "&lt;")
                 .replace(">", "&gt;").replace('"', "&quot;"))


def element(name: str, ident: "str | None", attributes: dict, depth: int,
            children: "list[str] | None" = None) -> str:
    """One element, `id` first and the rest sorted by name - byte order, as
    CanonicalXml.cpp's std::map sorts them. A None value is an attribute left
    at its default, which canonical form does not write."""
    indent = "  " * depth
    text = indent + "<" + name

    if ident is not None:
        text += f' id="{escape(ident)}"'

    for key in sorted(k for k, v in attributes.items() if v is not None):
        text += f' {key}="{escape(str(attributes[key]))}"'

    if not children:
        return text + "/>\n"

    return text + ">\n" + "".join(children) + indent + "</" + name + ">\n"


# =============================================================================
# The show
# =============================================================================

def build(cues: int) -> "tuple[dict, str]":
    """(files, summary): every file of the bundle by relative path, and the
    one line that says what is in it."""
    if cues < 1:
        raise Refused("--cues must be at least 1")

    ids = Identifiers()

    list_id = ids.make("CQ", 1)
    mount_id = ids.make("MN", 1)
    main_bus = ids.make("BS", 1)
    processor_bus = ids.make("BS", 2)

    slots = [ids.make("SA", n + 1) for n in range(SLOT_COUNT)]

    # --- cues ---------------------------------------------------------------
    counts = {"media": 0, "grouped": 0, "groups": 0, "triggers": 0, "rows": 0}
    first_cue = None

    def media(n: int, number: str, depth: int, pre: "str | None" = None,
              post: "str | None" = None) -> str:
        """Media cue n (from nought), with its feed and perhaps a trigger."""
        nonlocal first_cue

        cue_id = ids.make("MA", n + 1)
        if first_cue is None:
            first_cue = cue_id

        children = [element("Feed", ids.make("FA", n + 1),
                            {"gains": "1", "slot": slots[n % SLOT_COUNT]}, depth + 1)]

        if n % TRIGGER_EVERY == TRIGGER_OFFSET:
            children.append(trigger(counts["triggers"], depth + 1))
            counts["triggers"] += 1

        counts["media"] += 1
        counts["rows"] += 1

        return element("Media", cue_id,
                       {"file": MEDIA_FILE,
                        "name": f"{WORDS[n % len(WORDS)]} {n + 1}",
                        "number": number,
                        "postWait": post,
                        "preWait": pre},
                       depth, children)

    def trigger(k: int, depth: int) -> str:
        """The k-th trigger: osc mostly, then midi, then clock, one of them off.

        The osc addresses sit under neither /godot nor the mount's prefix,
        which show.rng says a load refuses - a trigger there would be a message
        that both wrote a value and fired a cue."""
        ident = ids.make("TA", k + 1)
        kind = ("osc", "osc", "osc", "midi", "clock")[k % 5]
        attributes: dict = {"enabled": "false" if k == 7 else None}

        if kind == "osc":
            attributes["address"] = f"/stage/trigger/{k + 1}"
            # `osc` is the default kind, so canonical form does not write it.
        elif kind == "midi":
            attributes.update({"kind": "midi", "channel": "1", "number": str(60 + k)})
        else:
            # An evening's times of day. A run of an instrument that happens to
            # cross one fires that cue by trigger, which the list's history
            # records as `t` and never as a GO.
            hour, minute = 19 + (k // 10) % 4, (k * 7) % 60
            attributes.update({"kind": "clock", "at": f"{hour:02d}:{minute:02d}:00"})

        return element("Trigger", ident, attributes, depth)

    body: "list[str]" = []
    position = 0
    n = 0

    while n < cues:
        at_group = (position + 1) % GROUP_EVERY == 0 and cues - n >= GROUP_SIZE

        if at_group:
            group_number = str(position + 1)
            flavour = counts["groups"] % 3
            attributes = {"name": f"Scene {counts['groups'] + 1}", "number": group_number,
                          "advance": "auto" if flavour == 1 else None,
                          "mode": "timeline" if flavour == 2 else None}

            members = []

            for member in range(GROUP_SIZE):
                # A timeline spaces its members by their pre-waits; the others
                # carry none, which is the ordinary case.
                pre = f"{member * 1.5:g}" if flavour == 2 and member else None
                members.append(media(n, f"{group_number}.{member + 1}", 4, pre=pre))
                n += 1
                counts["grouped"] += 1

            body.append(element("Group", ids.make("GA", counts["groups"] + 1),
                                attributes, 3, members))
            counts["groups"] += 1
            counts["rows"] += 1
        else:
            pre = "0.5" if position % 7 == 3 else None
            post = "2" if position % 11 == 5 else None
            body.append(media(n, str(position + 1), 3, pre=pre, post=post))
            n += 1

        position += 1

    lists = element("Lists", None, {}, 1,
                    [element("List", list_id, {"name": "Large show"}, 2, body)])

    # --- the mount and its slots --------------------------------------------
    # One mono slot per processor input, each on its own channel of the
    # twenty-wide bus that carries audio to the processor.
    slot_elements = [
        element("Slot", slot,
                {"address": f"/wfs/input/{k + 1}", "bus": processor_bus,
                 "firstChannel": str(k) if k else None,
                 "name": f"Input {k + 1}", "width": None},
                3)
        for k, slot in enumerate(slots)]

    mounts = element("Mounts", None, {}, 1,
                     [element("Mount", mount_id,
                              {"namespace": "namespaces/wfs.json", "port": "9000",
                               "prefix": "/wfs"},
                              2, slot_elements)])

    # --- audio ----------------------------------------------------------------
    audio = element("Audio", None, {"tracks": "8"}, 1,
                    [element("Bus", main_bus, {"name": "Main L/R", "width": "2"}, 2),
                     element("Bus", processor_bus,
                             {"firstChannel": "2", "name": "To the processor",
                              "width": str(SLOT_COUNT)}, 2)])

    show = element("Show", None, {}, 0, [lists, mounts, audio])

    state = ('<State formatVersion="1">\n'
             f'  <List id="{list_id}" standby="{first_cue}"/>\n'
             '</State>\n')

    files = {
        "show.xml": show,
        "state.xml": state,
        "namespaces/wfs.json": namespace(),
    }

    summary = (f"{counts['media']} media cues ({counts['grouped']} of them in "
               f"{counts['groups']} groups), {counts['triggers']} triggers, "
               f"{SLOT_COUNT} slots, {counts['rows']} rows; standby on {first_cue}")

    return files, summary


def namespace() -> str:
    """What the mount says the processor has: twenty inputs with a position
    each. Hand-written in the shape of the slots fixture's, because most
    devices' descriptions are. Nothing on the console reads it - GET /godot
    does not reach under /wfs - but a mount has to describe something or it
    does not load."""
    inputs = {}

    for k in range(SLOT_COUNT):
        base = f"/wfs/input/{k + 1}"
        inputs[str(k + 1)] = {
            "FULL_PATH": base,
            "DESCRIPTION": f"Input {k + 1}.",
            "CONTENTS": {
                axis: {
                    "FULL_PATH": f"{base}/{axis}",
                    "DESCRIPTION": f"Position along {axis}, in metres.",
                    "TYPE": "f",
                    "ACCESS": 3,
                    "RANGE": [{"MIN": -20.0, "MAX": 20.0}],
                }
                for axis in ("x", "y")
            },
        }

    description = {
        "FULL_PATH": "/wfs",
        "DESCRIPTION": "A WFS processor's inputs, as far as a large generated show needs "
                       "them: twenty, each with a position. " + WRITTEN_BY,
        "CONTENTS": {
            "input": {
                "FULL_PATH": "/wfs/input",
                "DESCRIPTION": "Input channels.",
                "CONTENTS": inputs,
            }
        },
    }

    return json.dumps(description, indent=2) + "\n"


# =============================================================================
# Writing it
# =============================================================================

def inside(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def written_here(folder: Path) -> bool:
    """Whether a folder is this script's own earlier output: the manifest named
    after it, and the namespace description carrying WRITTEN_BY. Both, because
    a bundle somebody made by hand has the first, and the second alone could
    be a copied file."""
    if not (folder / (folder.name + ".wfg")).is_file():
        return False

    try:
        description = json.loads((folder / "namespaces" / "wfs.json")
                                 .read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False

    return (isinstance(description, dict)
            and WRITTEN_BY in str(description.get("DESCRIPTION", "")))


def write(folder: Path, cues: int = 500, force: bool = False) -> str:
    """Writes the bundle and returns the summary line."""
    # First, before anything on disk is looked at or touched: a count the
    # build refuses must not arrive after the old folder has been deleted.
    files, summary = build(cues)

    folder = folder.resolve()
    bundles = FIXTURE_BUNDLES.resolve()

    if inside(folder, bundles):
        raise Refused(f"{folder} is inside tests/fixtures/bundles/, where "
                      "scripts/validate-show.py would take it for a committed fixture")

    if inside(REPO, folder):
        raise Refused(f"{folder} is the repository or holds it")

    if inside(bundles, folder):
        raise Refused(f"{folder} holds tests/fixtures/bundles/")

    if folder.exists():
        if not folder.is_dir():
            raise Refused(f"{folder} exists and is not a folder")

        if any(folder.iterdir()):
            if not written_here(folder):
                raise Refused(f"{folder} is not empty and was not written by this script; "
                              "name an empty or new folder")

            if not force:
                raise Refused(f"{folder} holds an earlier show written by this script; "
                              "pass --force to replace it")

            shutil.rmtree(folder)

    # The manifest is named after the folder, which is how Bundle::open finds it.
    files[folder.name + ".wfg"] = '<Bundle formatVersion="1"/>\n'

    for relative, text in files.items():
        target = folder / relative
        target.parent.mkdir(parents=True, exist_ok=True)

        # Bytes, so LF stays LF on Windows: the canonical form says "\n"
        # everywhere, and a show whose bytes depend on the platform is not.
        target.write_bytes(text.encode("utf-8"))

    return summary


def main(argv: "list[str]") -> int:
    parser = argparse.ArgumentParser(
        description="Writes the large generated show M24 is measured on.")
    parser.add_argument("folder", type=Path, help="the bundle folder to write")
    parser.add_argument("--cues", type=int, default=500,
                        help="how many media cues (default 500, M18's)")
    parser.add_argument("--force", action="store_true",
                        help="replace the folder if it holds this script's own earlier "
                             "output (any other folder with something in it is refused)")
    args = parser.parse_args(argv)

    try:
        summary = write(args.folder, args.cues, args.force)
    except Refused as why:
        print(f"make_large_show: {why}", file=sys.stderr)
        return 2

    print(f"wrote {args.folder}: {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
