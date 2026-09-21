#!/usr/bin/env python3
# Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
# SPDX-License-Identifier: GPL-3.0-or-later
"""The output list packs its channels, and the patch follows it until a cue plays.

THE RULE THIS DRIVES, end to end and over the wire rather than in a unit test:

  - `Bus/@firstChannel` is the running sum of the widths before it, kept so by
    `bus.create`, `bus.delete`, `bus.move` and `bus.width`, and writable by
    nothing else.
  - While the show is fresh the interface patch stays EMPTY, which the device
    layer reads as identity, so the outputs follow the list somebody is
    arranging.
  - The first media run that launches with audio settles it. From then on every
    output keeps the interface channels it is on and a new one takes the next
    free ones.
  - And the flag is state: it survives a save and a reopen, because a rig
    sound-checked on Tuesday must not be re-patched by an edit on Wednesday.

Run under `--hosted` so a launch really happens: without it `launchIfDue`
returns early, the run sits at `armed`, and the latch this file is about would
never be reached (the lesson PR 5.4's driver recorded).
"""
import sys
import tempfile
import time
from pathlib import Path

import common
from first_sound import write_tone

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
report = common.Report("output layout")

with tempfile.TemporaryDirectory(prefix="godot-outputs-") as temporary:
    bundle = Path(temporary) / "show"
    bundle.mkdir()
    (bundle / "show.wfg").write_text('<Bundle formatVersion="1"/>', encoding="utf-8")

    #  ONE OUTPUT TO START WITH, and every other one made over the wire.
    #  A hosted graph refuses a show with tracks and no buses ("there is
    #  nowhere for them to go", Console.cpp), which is a fair refusal and
    #  means the empty case cannot be driven here.
    (bundle / "show.xml").write_text(
        '<Show><Lists><List id="7K2QM9X4">'
        '<Media id="B3N8R5TW" file="dropped.wav" name="Thunder"/>'
        '</List></Lists><Mounts/>'
        '<Audio tracks="2"><Bus id="J3MT5XYA" kind="mix" name="Main" width="2"/></Audio>'
        '</Show>', encoding="utf-8")
    write_tone(bundle / "media" / "dropped.wav")

    render = Path(temporary) / "render.wav"
    log = Path(temporary) / "events.log"
    original = (bundle / "show.xml").read_text(encoding="utf-8")

    with common.Server(bundle, hosted=True, render=render, log=log, locale=locale) as server:
        def send(command, args=None):
            common.send_udp(server.osc_port,
                            common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))

        def value(address):
            return common.http_json(server.http_port, address).get("VALUE", [None])[0]

        def buses():
            """Every bus id, in channel order — what the output list shows."""
            found = []
            for address, node in common.http_json(server.http_port, "/godot/bus").get("CONTENTS", {}).items():
                first = node.get("CONTENTS", {}).get("firstChannel", {}).get("VALUE", [0])[0]
                found.append((first, address))
            return [address for _, address in sorted(found)]

        def channels_of(bus):
            return (value(f"/godot/bus/{bus}/firstChannel"), value(f"/godot/bus/{bus}/width"))

        # ------------------------------------------------------------------
        # A list somebody writes, interleaved the way a rig is wired.
        # ------------------------------------------------------------------
        send("bus.create", ["direct", 1, 0])
        send("bus.create", ["direct", 1, -1])
        made = common.wait_until(lambda: buses() if len(buses()) == 3 else None)
        report.check(bool(made), "two more outputs were made over the wire")

        if not made:
            raise SystemExit(report.finish())

        first, middle, last = made
        report.equal(middle, "J3MT5XYA", "the output that was already there is in the middle")
        report.equal(channels_of(first), (0, 1), "the output made at the top starts at nought")
        report.equal(channels_of(middle), (1, 2), "the stereo mix moved along to make room")
        report.equal(channels_of(last), (3, 1), "and the channels keep packing")

        report.equal(value(f"/godot/bus/{first}/kind"), "direct", "a direct out says so")
        report.equal(value(f"/godot/bus/{middle}/kind"), "mix", "and a mix channel says so")
        report.equal(value(f"/godot/bus/{first}/name"), "Direct 1",
                     "an output arrives with a name it can be called by")

        #  THE PATCH IS NOT WRITTEN AT ALL while the show is fresh: empty is
        #  identity, so the outputs follow the order being arranged.
        report.equal(value("/godot/audio/outputPatch"), "", "the interface patch follows the list")
        report.equal(value("/godot/audio/patchSettled"), False, "and the show has not settled")

        #  Read-only at the door, and this is the reason the four commands
        #  exist: a client that could write it could overlap two outputs.
        send("node.set", [f"/godot/bus/{first}/firstChannel", 8])
        time.sleep(0.3)
        report.equal(channels_of(first), (0, 1), "a client cannot write a channel by hand")

        send("bus.move", [last, 0])
        common.wait_until(lambda: channels_of(last) == (0, 1))
        report.equal(channels_of(last), (0, 1), "a moved output takes the channels it moved to")
        report.equal(channels_of(first), (1, 1), "and everything after it repacks")
        report.equal(value("/godot/audio/outputPatch"), "",
                     "a fresh show still needs no patch after a move")

        send("bus.width", [first, 2])
        common.wait_until(lambda: channels_of(first) == (1, 2))
        report.equal(channels_of(middle), (3, 2), "widening an output moves the ones after it")

        send("bus.width", [first, 1])
        common.wait_until(lambda: channels_of(first) == (1, 1))
        report.equal(channels_of(middle), (2, 2), "and narrowing it gives the channel back")

        # ------------------------------------------------------------------
        # Then a cue plays, and what each output is plugged into becomes a fact.
        # ------------------------------------------------------------------
        send("route.default", ["B3N8R5TW", 2])
        time.sleep(0.5)
        send("cue.fire", ["B3N8R5TW"])
        settled = common.wait_until(
            lambda: value("/godot/audio/patchSettled") is True, timeout=10.0)
        report.check(bool(settled), "the first launch settles the interface patch")

        before = {bus: channels_of(bus) for bus in buses()}
        send("bus.create", ["mix", 2, 0])
        added = common.wait_until(lambda: buses() if len(buses()) == 4 else None)
        report.check(bool(added), "a fifth output can still be added")

        patch = common.wait_until(lambda: value("/godot/audio/outputPatch") or None)
        report.check(bool(patch), "and the patch is written rather than implied")

        #  THE LIST HAS CHANGED AND THE RIG HAS NOT. Every output that existed
        #  keeps the interface channels it was on, although all of them have
        #  moved in the list; the new one takes what is past them.
        mapping = [int(word) for word in (patch or "").split()]
        kept = True
        for bus, (was_first, width) in before.items():
            now_first = value(f"/godot/bus/{bus}/firstChannel")
            for offset in range(width):
                if mapping[now_first + offset] != was_first + offset:
                    kept = False
        report.check(kept, "every output keeps the interface channels it was plugged into")
        report.check(min(mapping[:2]) >= max(first for first, _ in before.values()) ,
                     "and the new output takes channels past the ones in use")

        send("document.save")
        report.check(bool(common.wait_until(lambda: value("/godot/document/dirty") is False)),
                     "the show saves")

    # ----------------------------------------------------------------------
    # Reopened: the flag is kept with the show, in state.xml and not in it.
    # ----------------------------------------------------------------------
    saved = (bundle / "show.xml").read_text(encoding="utf-8")
    report.check("patchSettled" not in saved, "the flag is not in the saved show")
    report.check("patchSettled" in (bundle / "state.xml").read_text(encoding="utf-8"),
                 "it is in state.xml, beside the standby")

    with common.Server(bundle, locale=locale) as reopened:
        report.equal(common.http_json(reopened.http_port, "/godot/audio/patchSettled")
                       .get("VALUE", [None])[0], True,
                     "a reopened show has not forgotten that it has been heard")

    #  REPLAY STARTS FROM THE SHOW THE SESSION STARTED FROM, not from the one
    #  it saved: every identifier the log hands back is already taken in the
    #  saved document, so a replay against it refuses its own first create.
    #  (`import_playback.py` records the same trap.)
    (bundle / "show.xml").write_text(original, encoding="utf-8")
    (bundle / "state.xml").unlink(missing_ok=True)

    code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                    f"--out={Path(temporary) / 'replayed'}",
                                    f"--wfg-locale={locale}")
    report.check(code == 0, "the session replays exactly", (out + err).strip()[:400])

raise SystemExit(report.finish())
