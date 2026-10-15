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

"""The same show on a main and its backup, checked and not assumed - driven from
outside (namespace draft 58, RD.10; the author's AFU, and AFY).

WHAT THIS FILE IS FOR. The unit suite works out two shows' fingerprints in one
process, compares them at a summary's slot, asks and answers between two show
checks joined by a function, and takes one show over another in a folder. What
none of it can prove is two shipped binaries doing it over the port every
client uses, each fingerprinting its own copy of the show on a thread of its
own:

  - a main and a backup on copies of one show - each with its OWN audio
    interface in its show.xml, which is the machine's and not the show's - say
    their shows are the same;
  - an edit made at the main while the backup is away (the backup stopped, the
    main's level of cue 4 changed, the backup started again) is said on the
    backup once it follows: `pair/showMatch` false, `pair/showDiffers` naming
    the cue, the alarm saying so and what mends it;
  - `pair.adoptShow` on the backup takes the main's show: the level is the
    main's, `pair/showMatch` true, the backup's own interface row unchanged in
    the show and on its disk, its old files in a set-aside folder of the show;
    on the main the same command is refused `not-following`;
  - a sound removed from the backup's copy is named as missing here;
  - and the MAIN is told (RD.12): the backup says its verdict up the link, so
    the main's `pair/peerShowMatch` reads yes, then no with the backup's own
    sentence in `pair/peerShowDiffers` and its alarm naming the cue and what
    mends it on the backup, then yes again once the backup took its show.

TWO SERVES ON ONE MACHINE, on the dummy clock, on COPIES of the phase 2
fixture whose show is rewritten here, each with its own temporary engine
folder. Its one sound is generated here, as first_sound.py writes its tone,
into the main's copy, and the backup's copy is a copy of that - dates kept.
"""
import os
import struct
import sys
import tempfile
import time
import wave
from pathlib import Path

import common
from common import HarnessError, Report, Server

# ON A SHARED CI RUNNER A TICK RUNS LATE (lane_level's rule): the waits are
# longer there; nothing below is a timing claim.
ON_CI = os.environ.get("GITHUB_ACTIONS") == "true"
SETTLE = 20.0 if ON_CI else 10.0

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "phase2"

LIST = "7K2QM9X4"
RAIN = "P9XKC2WR"
TIMEOUT = 1.0
RATE = 48000

SHOW = """<Show>
  <Lists goDebounce="0">
    <List id="7K2QM9X4" name="Show">
      <Media id="B3N8R5TW" file="tone.wav" name="Thunder" number="1">
        <Route id="Z04EH7PH" bus="J3MT5XYA" gains="1 1"/>
      </Media>
      <Media id="P9XKC2WR" file="tone.wav" level="-6" name="Rain" number="4">
        <Route id="V3XY5Z7B" bus="J3MT5XYA" gains="1 1"/>
      </Media>
      <Cue id="M5TQ7XVA" name="And on" number="5"/>
    </List>
  </Lists>
  <Mounts/>
  <Audio outputDevice="{device}" tracks="2">
    <Bus id="J3MT5XYA" name="Main L/R" width="2"/>
  </Audio>
  <MidiPorts/>
  <Network/>
  <Surfaces/>
  <Dcas/>
</Show>
"""

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


def write_tone(path: Path, seconds: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sample = int(0.25 * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<h", sample) * int(RATE * seconds))


def wait_for(predicate, seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def reader(server):
    def read(address):
        """A node's first value as text, "" when it cannot be read; a flag as true or false."""
        try:
            values = common.http_json(server.http_port, address + "?VALUE").get("VALUE", [])
        except (HarnessError, OSError, ValueError):
            return ""
        if not values:
            return ""
        if isinstance(values[0], bool):
            return "true" if values[0] else "false"
        return str(values[0])
    return read


def sender(server):
    def send(command, args=None):
        common.send_udp(server.osc_port,
                        common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))
    return send


def as_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return float("nan")


def run() -> int:
    report = Report(f"redundancy: the same show, checked and taken ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-pair-show-") as scratch:
        scratch = Path(scratch)

        # ONE SHOW, TWO COPIES: the main's written with its sound, the backup's
        # copied from it - the same bytes and dates - and each given its own
        # interface, which is the machine's row and never the show's.
        main_bundle = common.copy_bundle(FIXTURE, scratch / "main" / "show")
        (main_bundle / "state.xml").unlink(missing_ok=True)
        write_tone(main_bundle / "media" / "tone.wav", 0.5)
        backup_bundle = common.copy_bundle(main_bundle, scratch / "backup" / "show")
        (main_bundle / "show.xml").write_text(SHOW.replace("{device}", "Main Interface"), encoding="utf-8")
        (backup_bundle / "show.xml").write_text(SHOW.replace("{device}", "Backup Interface"), encoding="utf-8")

        main_engine = scratch / "main" / "machine" / "engine"
        backup_engine = scratch / "backup" / "machine" / "engine"
        main_engine.mkdir(parents=True)
        backup_engine.mkdir(parents=True)

        main = backup = None

        def start_backup():
            return Server(backup_bundle, locale=locale, engine_folder=backup_engine, role="backup",
                          extra_args=[f"--peer=127.0.0.1:{main.http_port}", f"--link-timeout={TIMEOUT:g}"])

        try:
            main = Server(main_bundle, locale=locale, engine_folder=main_engine, role="main",
                          extra_args=[f"--link-timeout={TIMEOUT:g}"])
            backup = start_backup()
            read_main, read_backup = reader(main), reader(backup)
            send_main, send_backup = sender(main), sender(backup)

            # === ONE SHOW ON BOTH ==================================================
            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "following", SETTLE),
                         "the backup follows its main",
                         f"state {read_backup('/godot/pair/state')!r}")
            report.check(wait_for(lambda: read_backup("/godot/pair/showMatch") == "true", SETTLE),
                         "its show is the main's: the two interfaces in the two show.xml are each machine's own",
                         f"showMatch {read_backup('/godot/pair/showMatch')!r}, "
                         f"showDiffers {read_backup('/godot/pair/showDiffers')!r}")
            report.equal(read_backup("/godot/pair/showDiffers"), "", "and nothing is said to differ")
            report.check(wait_for(lambda: read_main("/godot/pair/peerShowMatch") == "yes", SETTLE),
                         "the main is told so by the backup: its pair/peerShowMatch reads yes (RD.12)",
                         f"peerShowMatch {read_main('/godot/pair/peerShowMatch')!r}")
            report.equal(read_main("/godot/audio/outputDevice"), "Main Interface", "the main's interface is its own")
            report.equal(read_backup("/godot/audio/outputDevice"), "Backup Interface",
                         "and the backup's is its own")

            # === AN EDIT WHILE THE BACKUP IS AWAY ==================================
            backup.stop()
            backup = None
            send_main("node.set", [f"/godot/cue/{RAIN}/level", "-12"])
            report.check(wait_for(lambda: as_float(read_main(f"/godot/cue/{RAIN}/level")) == -12.0, 5.0),
                         "the main's cue 4 is set to -12 dB while the backup is stopped",
                         f"level {read_main(f'/godot/cue/{RAIN}/level')!r}")

            backup = start_backup()
            read_backup, send_backup = reader(backup), sender(backup)

            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "following", SETTLE),
                         "started again, the backup follows")
            report.equal(as_float(read_backup(f"/godot/cue/{RAIN}/level")), -6.0,
                         "its show is still its own: cue 4 at -6 dB, the edit never streamed to it")
            report.check(wait_for(lambda: "Rain" in read_backup("/godot/pair/showDiffers"), SETTLE),
                         "on link-up the backup says its show differs, and names the cue",
                         f"showDiffers {read_backup('/godot/pair/showDiffers')!r}")
            report.equal(read_backup("/godot/pair/showDiffers"), "the show differs: cue 4 Rain changed",
                         "in one sentence")
            report.equal(read_backup("/godot/pair/showMatch"), "false", "and its show does not match")
            report.check("Take the other machine's show" in read_backup("/godot/pair/alarm"),
                         "the alarm says it, and what mends it",
                         f"alarm {read_backup('/godot/pair/alarm')!r}")

            # THE MAIN TOLD (RD.12): the machine the show is run on hears it from
            # the backup, in the backup's words, and its alarm says what mends it -
            # on the backup, whose button it is.
            report.check(wait_for(lambda: read_main("/godot/pair/peerShowMatch") == "no", SETTLE),
                         "the main is told by the backup: its pair/peerShowMatch reads no",
                         f"peerShowMatch {read_main('/godot/pair/peerShowMatch')!r}")
            report.equal(read_main("/godot/pair/peerShowDiffers"), "the show differs: cue 4 Rain changed",
                         "in the backup's own words")
            main_alarm = read_main("/godot/pair/alarm")
            report.check("cue 4 Rain changed" in main_alarm and "Take the main's show on the backup" in main_alarm,
                         "and the main's alarm names the cue, and says to take its show on the backup",
                         f"alarm {main_alarm!r}")

            # === TAKEN ==============================================================
            errors_before = read_main("/godot/engine/errorCount")
            send_main("pair.adoptShow")
            report.check(wait_for(lambda: read_main("/godot/engine/errorCount") != errors_before, 3.0)
                         and "not-following" in read_main("/godot/engine/lastError"),
                         "pair.adoptShow on the main, which sends, is refused not-following",
                         f"main lastError {read_main('/godot/engine/lastError')!r}")

            send_backup("pair.adoptShow")
            report.check(wait_for(lambda: as_float(read_backup(f"/godot/cue/{RAIN}/level")) == -12.0, SETTLE),
                         "pair.adoptShow on the backup takes the main's show: cue 4 at -12 dB",
                         f"level {read_backup(f'/godot/cue/{RAIN}/level')!r}, "
                         f"lastError {read_backup('/godot/engine/lastError')!r}")
            report.check(wait_for(lambda: read_backup("/godot/pair/showMatch") == "true", SETTLE),
                         "and the two shows match",
                         f"showMatch {read_backup('/godot/pair/showMatch')!r}, "
                         f"showDiffers {read_backup('/godot/pair/showDiffers')!r}")
            report.equal(read_backup("/godot/pair/showDiffers"), "", "with nothing said to differ")
            report.check(wait_for(lambda: read_main("/godot/pair/peerShowMatch") == "yes", SETTLE),
                         "and the main is told the shows match again",
                         f"peerShowMatch {read_main('/godot/pair/peerShowMatch')!r}, "
                         f"peerShowDiffers {read_main('/godot/pair/peerShowDiffers')!r}")
            report.check(wait_for(lambda: "show" not in read_main("/godot/pair/alarm"), SETTLE),
                         "its alarm no longer speaks of the show",
                         f"alarm {read_main('/godot/pair/alarm')!r}")
            report.equal(read_backup("/godot/audio/outputDevice"), "Backup Interface",
                         "the backup's own interface row is unchanged")
            report.equal(read_main("/godot/audio/outputDevice"), "Main Interface", "and the main's is its own still")

            written = (backup_bundle / "show.xml").read_text(encoding="utf-8")
            report.check('level="-12"' in written and 'outputDevice="Backup Interface"' in written,
                         "on the backup's disk, show.xml is the main's show with the backup's interface",
                         written)

            set_aside = sorted(path for path in backup_bundle.iterdir()
                               if path.is_dir() and path.name.startswith("set-aside."))
            report.equal(len(set_aside), 1, "its own files are set aside in one dated folder of the show",
                         f"folders {[path.name for path in backup_bundle.iterdir()]}")
            if set_aside:
                old = (set_aside[0] / "show.xml").read_text(encoding="utf-8") \
                    if (set_aside[0] / "show.xml").exists() else ""
                report.check('level="-6"' in old and 'outputDevice="Backup Interface"' in old,
                             "holding the show it had: cue 4 at -6 dB", old)
                report.check((set_aside[0] / "namespaces" / "desk.json").exists(),
                             "and its device descriptions")
            report.equal(read_backup("/godot/pair/state"), "following", "the backup follows still")

            # === A SOUND MISSING HERE ==============================================
            backup.stop()
            backup = None
            (backup_bundle / "media" / "tone.wav").unlink()
            backup = start_backup()
            read_backup = reader(backup)

            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "following", SETTLE),
                         "started again without its sound, the backup follows")
            report.check(wait_for(lambda: "tone.wav" in read_backup("/godot/pair/showDiffers"), SETTLE),
                         "it names the sound missing here",
                         f"showDiffers {read_backup('/godot/pair/showDiffers')!r}")
            report.equal(read_backup("/godot/pair/showDiffers"), "1 sound missing here: tone.wav", "in one sentence")
            report.equal(read_backup("/godot/pair/showMatch"), "false", "and its show does not match")
            report.check("Copy the files" in read_backup("/godot/pair/alarm"),
                         "the alarm says so, and that the sound is not sent",
                         f"alarm {read_backup('/godot/pair/alarm')!r}")
            report.check(wait_for(lambda: read_main("/godot/pair/peerShowDiffers") == "1 sound missing here: tone.wav",
                                  SETTLE),
                         "the main is told which sound the backup lacks",
                         f"peerShowDiffers {read_main('/godot/pair/peerShowDiffers')!r}")
            report.check("Copy the files" in read_main("/godot/pair/alarm"),
                         "and its alarm says to copy the files",
                         f"alarm {read_main('/godot/pair/alarm')!r}")
        finally:
            for server in (backup, main):
                if server is not None:
                    try:
                        server.stop()
                    except HarnessError as error:
                        report.check(False, "a serve stopped cleanly", str(error))

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
