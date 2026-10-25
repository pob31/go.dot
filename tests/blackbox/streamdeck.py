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

"""A Stream Deck surface, served (namespace draft §95, SD.8).

WHAT THIS IS FOR. The unit suite proves the layout, the bridge's commands and
its pictures against a deck of the test's own making. What none of it says is
that the SHIPPED BINARY wires them: a surface made `streamDeck` by command,
with no strips, its keys laid out over the show's cart and published at
/godot/surface/<id>/keys - plugged in or not - its status saying in words why
no deck drives it, the show's keys arriving when they are switched on, and a
strip refused on it.

NO DECK IS NEEDED, and none is on a CI runner: the keys are laid out as an
MK.2's while no deck is found. On a machine with one plugged in, the checks
that say "no deck" are voided, and the rest still hold.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common                                    # noqa: E402
import first_sound                               # noqa: E402
from common import HarnessError, Report, Server  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "minimal"

CART = "CRT00001"
PADS = [("PAD00001", "Rain"), ("PAD00002", "Thunder"), ("PAD00003", "Backup tracker")]
DECK = "SDK00001"
BASE = f"/godot/surface/{DECK}"


def send(server: Server, command: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))


def keys_of(server: Server) -> "list[list[str]]":
    text = first_sound.value_of(server, BASE + "/keys")
    return [line.split("\t") for line in text.split("\n")] if isinstance(text, str) and text else []


def keys_until(server: Server, wanted, timeout: float = 10.0) -> "list[list[str]]":
    deadline = time.monotonic() + timeout
    keys: "list[list[str]]" = []

    while time.monotonic() < deadline:
        keys = keys_of(server)

        if wanted(keys):
            return keys

        time.sleep(0.05)

    return keys


def run(locale: "str | None") -> int:
    report = Report(f"a Stream Deck surface, served ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-streamdeck-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "minimal")
        log = room / "session.wfglog"

        with Server(bundle, log=log, locale=locale) as server:
            send(server, "list.create", ["Pads", CART, "kind", "cart"])

            for index, (cue, name) in enumerate(PADS):
                send(server, "cue.create", [CART, index, "memo", name, cue])

            send(server, "surface.create", ["streamDeck", "Desk deck", DECK])

            report.equal(first_sound.wait_for(server, BASE + "/profile", "streamDeck"), "streamDeck",
                         "the surface is made a Stream Deck")
            report.equal(first_sound.value_of(server, BASE + "/strips"), 0, "with no strips")

            #  Its keys: the cart's pads from the top left, fifteen keys while no deck is found.
            keys = keys_until(server, lambda k: len(k) >= 3 and k[2][0] == f"cue {PADS[2][0]}")
            report.equal(len(keys), 15, "its keys are published, as an MK.2's with no deck found")

            if len(keys) >= 3:
                for at, (cue, name) in enumerate(PADS):
                    report.equal(keys[at][0], f"cue {cue}", f"key {at} presses {name}")
                    report.equal(keys[at][2], name, f"key {at} says {name}")

                report.equal(keys[3][0], "", "the key after the cart's last does nothing")

            plugged = first_sound.value_of(server, "/godot/engine/streamDecks")

            if plugged:
                report.void("its status says no deck is plugged in", f"this machine has one: {plugged!r}")
            else:
                report.equal(first_sound.value_of(server, BASE + "/connected"), False, "it is not connected")
                report.equal(first_sound.value_of(server, BASE + "/problem"), "no Stream Deck is plugged in",
                             "and says why, in words")
                report.equal(first_sound.value_of(server, BASE + "/model"), "", "no model is claimed")

            #  The show's keys, switched on: GO at the bottom right, Esc beside it.
            send(server, "node.set", [BASE + "/showKeys", "true"])
            keys = keys_until(server, lambda k: len(k) == 15 and k[14][0] == "go")
            report.equal(keys[14][0] if len(keys) == 15 else None, "go", "GO is the last key once the show's keys are on")
            report.equal(keys[13][0] if len(keys) == 15 else None, "esc", "Esc beside it")
            report.equal(keys[11][0] if len(keys) == 15 else None, "pause", "Pause / Play before Doh! (PRD 3.34)")
            report.equal(keys[9][0] if len(keys) == 15 else None, "standbyUp", "the standby's arrows first")

            #  A deck has no strips: a strip is refused on it.
            send(server, "strip.create", [DECK])
            time.sleep(0.5)
            report.equal(first_sound.value_of(server, BASE + "/strips"), 0, "a strip is refused on a deck")

            #  Elgato's app, closed only when asked: the command is there and answers.
            send(server, "streamdeck.closeApp")
            time.sleep(0.3)

        logged = log.read_text(encoding="utf-8")
        report.check("surface.create" in logged and "streamdeck.closeApp" in logged,
                     "the session's log carries the commands")

    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
