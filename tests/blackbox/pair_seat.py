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

"""A backup started late, seated where its main is, and in step - driven from
outside (namespace draft 58, RD.8).

WHAT THIS FILE IS FOR. The unit suite seats one engine at another's position in
one process, and joins two engines by the real stream and receiver over a link
that jitters. What none of it can prove is two shipped binaries doing it over
the port every client uses, each playing its sounds through a real Tracktion
graph:

  - a main plays a long sound and a scene; a backup STARTED LATE links, is
    seated at the main's position - the sound running under the main's run
    name, the scene in its round - and says it is in step;
  - its sound's playhead is within two ticks of the main's, read close together
    and allowing for the delay the backup applies the main's records at;
  - `pair.realign`, asked of the backup, seats it again and it stays in step;
  - the main killed, the backup takes over (RD.7) and goes on playing; the main
    started again follows the backup that took over - and is seated where the
    backup is, in step, its sound where the backup's is;
  - and the machine followed is told (RD.12): once its follower says it is in
    step, the leader's `pair/peerInStep` reads yes - the main's while the
    backup follows it, and the backup's once the main follows it.

SERVES ON ONE MACHINE, hosted - a Tracktion graph under a dummy clock - on
COPIES of the phase 2 fixture whose show is rewritten for this driver, each
with its own temporary engine folder. The sound's file is generated here, as
first_sound.py writes its tone, never committed.
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

# ON A SHARED CI RUNNER A TICK RUNS LATE (lane_level's rule, as pair_follow.py
# since c44ec75): the playheads are held within two ticks here and within four on
# CI, where a Debug runner's ticks and its disk are less even.
ON_CI = os.environ.get("GITHUB_ACTIONS") == "true"
PLAYHEAD_TICKS = 4 if ON_CI else 2
TAKEOVER_SLACK = 3.0 if ON_CI else 1.0
# A WAIT FOR SOMETHING TO COME ABOUT, not a claim of how soon, is three times as
# patient on CI: a Debug runner loaded enough to miss timecode and time-group checks
# took more than three seconds to answer its first node.set (run 38636177195).
PATIENCE = 3.0 if ON_CI else 1.0

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "phase2"

LIST = "7K2QM9X4"
BED = "B3N8R5TW"
SCENE = "G5H6J7K8"
RAIN = "P9XKC2WR"
LATER = "M5TQ7XVA"

TIMEOUT = 1.0
TICK = 0.02
RATE = 48000

# A long bed, and a timeline scene playing the same file at another level with
# a memo still to come: what a backup joining mid-show finds running.
SHOW = f"""<Show>
  <Lists goDebounce="0">
    <List id="{LIST}" name="Show">
      <Media id="{BED}" file="bed.wav" name="Bed" number="1">
        <Route id="Z04EH7PH" bus="J3MT5XYA" gains="1 1"/>
      </Media>
      <Group id="{SCENE}" mode="timeline" name="Scene" number="2">
        <Media id="{RAIN}" file="bed.wav" level="-6" name="Rain" number="2.1">
          <Route id="V3XY5Z7B" bus="J3MT5XYA" gains="1 1"/>
        </Media>
        <Cue id="{LATER}" name="Later" number="2.2" preWait="40"/>
      </Group>
      <Cue id="Q6RS8TW2" name="After" number="3"/>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="4">
    <Bus id="J3MT5XYA" name="Main L/R" width="2"/>
  </Audio>
  <MidiPorts/>
  <Network/>
  <Surfaces/>
  <Dcas/>
</Show>
"""

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


def write_bed(path: Path, seconds: float) -> None:
    """A steady tone, both channels, as first_sound.py writes its own."""
    path.parent.mkdir(parents=True, exist_ok=True)
    sample = int(0.25 * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<hh", sample, sample) * int(RATE * seconds))


def machine(scratch: Path, name: str):
    bundle = common.copy_bundle(FIXTURE, scratch / name / "show")
    (bundle / "show.xml").write_text(SHOW, encoding="utf-8")
    (bundle / "state.xml").unlink(missing_ok=True)
    write_bed(bundle / "media" / "bed.wav", 90.0)
    engine = scratch / name / "machine" / "engine"
    engine.mkdir(parents=True)
    return bundle, engine


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


def live_runs(read, cue):
    """The names of a cue's runs that are not over, oldest first."""
    names = []
    for run in read("/godot/run/order").split():
        if read(f"/godot/run/{run}/cue") == cue and read(f"/godot/run/{run}/state") not in ("done", "failed", ""):
            names.append(run)
    return names


def as_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return float("nan")


def as_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1


def playheads(read_leader, read_follower, run, delay_ticks):
    """The leader's playhead and the follower's, read close together: the
    leader's twice around the follower's, so the reading's own time is known,
    and the follower's moved on by the delay it applies the leader's records at
    - or not, from RD.9: a follower whose launches are on its leader's samples
    (`pair/mapped`) seats its sound where the leader's is, no delay behind. A
    seat that landed before the clock map was trusted is on the record clock
    still, so the nearer of the two readings is the one judged, and said."""
    address = f"/godot/run/{run}/position"
    before = as_float(read_leader(address))
    theirs = as_float(read_follower(address))
    after = as_float(read_leader(address))
    leader = (before + after) / 2.0
    skew = abs(after - before) / 2.0
    behind = abs(theirs + delay_ticks * TICK - leader)
    aligned = abs(theirs - leader)
    print(f"  note the follower's playhead {behind * 1000:.0f} ms from the leader's a delay behind,"
          f" {aligned * 1000:.0f} ms from it on the same sample")
    return leader, theirs, min(behind, aligned), skew


def run() -> int:
    report = Report(f"redundancy: a backup started late, seated and in step ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-pair-seat-") as scratch:
        scratch = Path(scratch)
        main_bundle, main_engine = machine(scratch, "main")
        backup_bundle, backup_engine = machine(scratch, "backup")
        servers = []

        try:
            # === A MAIN, ALONE, PLAYING ==============================================
            main = Server(main_bundle, locale=locale, engine_folder=main_engine, role="main", hosted=True,
                          extra_args=[f"--link-timeout={TIMEOUT:g}"])
            servers.append(main)
            port = main.http_port
            read_main, send_main = reader(main), sender(main)

            send_main("node.set", [f"/godot/list/{LIST}/standby", BED])
            report.check(wait_for(lambda: read_main(f"/godot/list/{LIST}/standby") == BED, 3.0 * PATIENCE), "the standby on the bed")
            send_main("go")
            report.check(wait_for(lambda: len(live_runs(read_main, BED)) == 1, 5.0 * PATIENCE), "the main plays the bed")
            send_main("node.set", [f"/godot/list/{LIST}/standby", SCENE])
            time.sleep(0.3)
            send_main("go")
            report.check(wait_for(lambda: len(live_runs(read_main, SCENE)) == 1 and len(live_runs(read_main, RAIN)) == 1, 5.0 * PATIENCE),
                         "and the scene, its sound playing")
            bed_run = (live_runs(read_main, BED) or [""])[0]
            scene_run = (live_runs(read_main, SCENE) or [""])[0]
            time.sleep(2.0)
            print(f"  note the main's bed {bed_run} at {read_main(f'/godot/run/{bed_run}/position')} s")

            # === A BACKUP STARTED LATE ===============================================
            backup = Server(backup_bundle, locale=locale, engine_folder=backup_engine, role="backup", hosted=True,
                            extra_args=[f"--peer=127.0.0.1:{port}", f"--link-timeout={TIMEOUT:g}"])
            servers.append(backup)
            read_backup, send_backup = reader(backup), sender(backup)

            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "following", 6.0 * PATIENCE),
                         "the backup started late follows its main",
                         f"state {read_backup('/godot/pair/state')!r}")
            report.check(wait_for(lambda: read_backup("/godot/pair/inStep") == "true", 8.0 * PATIENCE),
                         "it is seated at the main's position and says it is in step",
                         f"inStep {read_backup('/godot/pair/inStep')!r}, differs {read_backup('/godot/pair/differs')!r},"
                         f" alarm {read_backup('/godot/pair/alarm')!r}")
            report.equal(live_runs(read_backup, BED), [bed_run], "the bed runs on the backup under the main's run name")
            report.equal(live_runs(read_backup, SCENE), [scene_run], "and the scene under the main's")
            report.equal(len(live_runs(read_backup, LATER)), 1, "the scene's memo still to come waits there too")
            report.equal(read_backup(f"/godot/list/{LIST}/standby"), read_main(f"/godot/list/{LIST}/standby"),
                         "the standby is the main's")
            report.equal(read_backup("/godot/pair/alarm"), "", "and the alarm says nothing")
            report.equal(read_backup("/godot/pair/sending"), "false", "the backup holds")
            report.check(wait_for(lambda: read_main("/godot/pair/peerInStep") == "yes", 4.0 * PATIENCE),
                         "the main is told so by the backup: its pair/peerInStep reads yes (RD.12)",
                         f"peerInStep {read_main('/godot/pair/peerInStep')!r}, "
                         f"peerDiffers {read_main('/godot/pair/peerDiffers')!r}, alarm {read_main('/godot/pair/alarm')!r}")
            report.equal(read_main("/godot/pair/peerDiffers"), "", "with nothing said to differ")

            delay = as_int(read_backup("/godot/pair/delay"))
            leader, theirs, apart, skew = playheads(read_main, read_backup, bed_run, delay)
            print(f"  note delay {delay} tick(s); the main's bed at {leader:.3f} s, the backup's at {theirs:.3f} s,"
                  f" {apart * 1000:.0f} ms apart once the delay is allowed for, the reading's own {skew * 1000:.0f} ms")
            report.check(apart <= PLAYHEAD_TICKS * TICK + skew,
                         f"the backup's bed is within {PLAYHEAD_TICKS} ticks of the main's, allowing for the delay",
                         f"main {leader:.3f}, backup {theirs:.3f}, delay {delay}")

            # === REALIGNED ON DEMAND =================================================
            send_backup("pair.realign")
            report.check(wait_for(lambda: "position" in read_backup("/godot/pair/alarm"), 2.0 * PATIENCE)
                         or read_backup("/godot/pair/inStep") == "true",
                         "pair.realign is taken by the backup")
            time.sleep(1.0)
            report.check(wait_for(lambda: read_backup("/godot/pair/inStep") == "true", 4.0 * PATIENCE),
                         "after pair.realign the backup is still in step",
                         f"inStep {read_backup('/godot/pair/inStep')!r}, differs {read_backup('/godot/pair/differs')!r}")
            report.equal(live_runs(read_backup, BED), [bed_run], "the bed still under the main's name")
            leader, theirs, apart, skew = playheads(read_main, read_backup, bed_run, delay)
            report.check(apart <= PLAYHEAD_TICKS * TICK + skew,
                         "and still within the playhead's tolerance", f"main {leader:.3f}, backup {theirs:.3f}")
            send_main("pair.realign")
            report.check(wait_for(lambda: "not-following" in read_main("/godot/engine/lastError"), 3.0 * PATIENCE),
                         "pair.realign on the main, which leads, is refused not-following",
                         f"lastError {read_main('/godot/engine/lastError')!r}")

            # === THE MAIN KILLED; STARTED AGAIN, IT FOLLOWS AND IS SEATED ============
            main.process.terminate()
            killed = time.monotonic()
            took_over = wait_for(lambda: read_backup("/godot/pair/state") == "leading", TIMEOUT + 3.0)
            report.check(took_over and time.monotonic() - killed <= TIMEOUT + TAKEOVER_SLACK + 1.0,
                         "the main killed, the backup leads", f"state {read_backup('/godot/pair/state')!r}")
            main.stop()
            servers.remove(main)
            report.equal(live_runs(read_backup, BED), [bed_run], "and the bed plays on, on the backup")

            until = time.monotonic() + 10.0
            while True:
                try:
                    main = Server(main_bundle, locale=locale, engine_folder=main_engine, role="main", hosted=True,
                                  http_port=port, extra_args=[f"--link-timeout={TIMEOUT:g}"])
                    break
                except HarnessError as error:
                    if time.monotonic() >= until:
                        raise
                    print(f"  note port {port} did not bind yet: {str(error).splitlines()[0]}")
                    time.sleep(0.25)
            servers.append(main)
            read_main = reader(main)

            report.check(wait_for(lambda: read_main("/godot/pair/state") == "following", 10.0 * PATIENCE),
                         "the main started again follows the backup that took over",
                         f"main {read_main('/godot/pair/state')!r}")
            report.check(wait_for(lambda: read_main("/godot/pair/inStep") == "true", 8.0 * PATIENCE),
                         "and is seated where the backup is, in step",
                         f"inStep {read_main('/godot/pair/inStep')!r}, differs {read_main('/godot/pair/differs')!r},"
                         f" alarm {read_main('/godot/pair/alarm')!r}")
            report.check(wait_for(lambda: read_backup("/godot/pair/peerInStep") == "yes", 4.0 * PATIENCE),
                         "and the backup, which leads now, is told so by the main",
                         f"peerInStep {read_backup('/godot/pair/peerInStep')!r}, "
                         f"peerDiffers {read_backup('/godot/pair/peerDiffers')!r}")
            report.equal(live_runs(read_main, BED), [bed_run], "the bed under its name on the main again")
            report.equal(live_runs(read_main, SCENE), [scene_run], "and the scene")

            delay = as_int(read_main("/godot/pair/delay"))
            leader, theirs, apart, skew = playheads(read_backup, read_main, bed_run, delay)
            print(f"  note the main's delay behind the backup {delay} tick(s); the backup's bed at {leader:.3f} s,"
                  f" the main's at {theirs:.3f} s, {apart * 1000:.0f} ms apart")
            report.check(apart <= PLAYHEAD_TICKS * TICK + skew,
                         f"the main's bed is within {PLAYHEAD_TICKS} ticks of the backup's",
                         f"backup {leader:.3f}, main {theirs:.3f}, delay {delay}")
        finally:
            for server in reversed(servers):
                try:
                    server.stop()
                except HarnessError as error:
                    report.check(False, "a serve stopped cleanly", str(error))

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
