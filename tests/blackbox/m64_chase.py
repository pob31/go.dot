#!/usr/bin/env python3
"""M64 - time groups from outside: how close to its frame a member starts, and how each chaser keeps up.

    This file is part of Go.dot - https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later

A MEASUREMENT, NOT A GATE (namespace draft 60.6). Two sessions of `wfg serve
--hosted --render --input-wav=<LTC> --midi-play-loop=<port>=<MTC>`, on shows
written into a copy of the timegroup fixture, the code written by
timegroup.py's own LTC and MTC writers and the render read by its reader:

  (a) FIRING. A time group on LTC and one on MTC, each holding ten free
      runners 3 s apart, each a second of LTC from 00:00:00:00 on its
      group's own output; the code loops, and every pass fires them all again,
      until two hundred have fired on each. For each: where its sound began
      against the sample the input's frame began on (LTC), against where the
      port's pass put the frame - the median over the pass's `code.heard` of
      where each put its frame (MTC) - and against the sample the engine's own
      map gave its time when it asked for it (both). The median, the 99th
      percentile and the worst.
  (b) CHASE. A time group on LTC holding a free runner, a chaser by jump, by
      varispeed and by timestretch at 01:00:02:00, ten minutes of sound each,
      against ten minutes of code: +50 ppm for four minutes, +0.1 % for three,
      a stop of two seconds, back 120 ms further on than where it would have
      been, two minutes at +50 ppm, a locate back thirty seconds, one more
      minute. For each: the error in each steady stretch (median and worst),
      the speed it ran at, how long it took to be back in step after the
      return and after the locate, how many times it jumped, and the worst.

Run by hand on a quiet machine; it writes nothing into the repository:

    WFG_BINARY=<wfg> python tests/blackbox/m64_chase.py [--passes=20]
        [--chase-minutes=10] [--firing-only] [--chase-only] [--keep=<folder>]
        [--analyse=<folder kept before>] [--fires=10] [--spacing=75] [--tracks=8]

What it found on the author's box (sixteen cores, Debug), 2026-10-13, is in
namespace draft 60.6: the hosted pump made no gap; the tick thread fell tens to
hundreds of milliseconds behind its audio as the firing sessions made runs, and
members asked for then launched late, as soon as they could - which the firing
figures separate out (`--spacing=33` puts the members 1.32 s apart, a sound
ending, the next loading and two firing in the same half second).
"""

from __future__ import annotations

import bisect
import math
import shutil
import statistics
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
import timegroup as tg  # noqa: E402
from common import HarnessError, Server  # noqa: E402
from first_sound import go, run_for_cue, value_of, wait_for, frames_on_disk, wait_for_render_tail  # noqa: E402

S = tg.SAMPLE_RATE
ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"   # Crockford's base 32: no I, L, O or U


def ident(prefix: str, n: int) -> str:
    out = ""
    for _ in range(4):
        out = ALPHABET[n % 32] + out
        n //= 32
    return prefix + out


def option(name: str, default=None):
    return next((arg.split("=", 1)[1] for arg in sys.argv[1:] if arg.startswith(f"--{name}=")), default)


def percentile(values, fraction: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))] if ordered else float("nan")


def show_xml(groups, buses) -> str:
    """A show of time groups: groups is [(id, name, source, [(cue, at, file, bus, chase)])],
    buses [(id, name)] one channel each."""
    lists = []
    for number, (group, name, source, members) in enumerate(groups, start=1):
        lines = [f'      <Group id="{group}" mode="time" name="{name}" number="{number}" timeSource="{source}">']
        for k, (cue, at, file, bus, chase) in enumerate(members, start=1):
            chased = f' chase="{chase}"' if chase != "free" else ""
            lines.append(f'        <Media at="{at}"{chased} file="{file}" id="{cue}" name="{cue}"'
                         f' number="{number}.{k}">')
            lines.append(f'          <Route bus="{bus}" gains="1" id="TG0R{cue[4:]}"/>')
            lines.append("        </Media>")
        lines.append("      </Group>")
        lists.extend(lines)
    bus_lines = [f'    <Bus{"" if k == 0 else f" firstChannel={chr(34)}{k}{chr(34)}"} id="{bus}" name="{name}"/>'
                 for k, (bus, name) in enumerate(buses)]
    return "\n".join(["<Show>", '  <Lists goDebounce="0">', '    <List id="TG0A0001" name="M64">', *lists,
                      "    </List>", "  </Lists>", "  <Mounts/>", f'  <Audio tracks="{option("tracks", str(max(8, 2 * len(buses) + 4)))}">',
                      *bus_lines, "    <Inputs>", '      <Input id="TG0N0001" name="LTC in"/>', "    </Inputs>",
                      "  </Audio>", "  <MidiPorts>", '    <Port id="TG0P0001" name="MTC in"/>', "  </MidiPorts>",
                      "  <Network/>", "  <Surfaces/>", "  <Dcas/>",
                      '  <Sync frameRate="25" ltcIn="TG0N0001" mtcIn="TG0P0001"/>', "</Show>", ""])


def make_bundle(room: Path, groups, buses) -> Path:
    if option("analyse"):
        return room / "timegroup"
    bundle = common.copy_bundle(tg.FIXTURE, room / "timegroup")
    (bundle / "show.xml").write_text(show_xml(groups, buses), encoding="utf-8", newline="\n")
    (bundle / "state.xml").write_text(f'<State formatVersion="1">\n  <List id="TG0A0001" standby="{groups[0][0]}"/>\n'
                                      "</State>\n", encoding="utf-8", newline="\n")
    code, out, err = common.run_wfg("validate", str(bundle))
    if code != 0:
        raise HarnessError(f"the M64 show does not validate: {out}{err}")
    return bundle


def run_story(room: Path, bundle: Path, story: tg.Story, loop: int, groups, until: float):
    """Serves the show with the story on the input and the port until the
    render holds `until` samples past the pass the GOs came in time for.
    (pass, log, render). With `--analyse`, what a room kept before holds."""
    if option("analyse"):
        return (int((room / "pass.txt").read_text(encoding="ascii")), tg.Log(room / "session.wfglog"),
                tg.Render(room / "out.wav"))
    tg.write_levels(room / "ltc-in.wav", tg.ltc_edges(story), loop)
    (room / "mtc-in.txt").write_text("\n".join(tg.mtc_lines(story)) + "\n", encoding="ascii", newline="\n")
    render, log = room / "out.wav", room / "session.wfglog"
    with Server(bundle, log=log, sample_rate=S, buffer_size=tg.BLOCK, hosted=True, render=render,
                input_wav=room / "ltc-in.wav", midi_play_loop={"MTC in": str(room / "mtc-in.txt")}) as server:
        if wait_for(server, "/godot/audio/status", "running", timeout=60) != "running":
            raise HarnessError("the audio side did not come up")
        for group in groups:
            go(server)
            run = run_for_cue(server, group[0], timeout=30)
            if not run or not common.wait_until(lambda: value_of(server, f"/godot/run/{run}/state") == "playing"):
                raise HarnessError(f"GO did not arm {group[0]}")
        armed = (value_of(server, "/godot/engine/tick") or 0) * tg.TICK
        first_at = min(story.sample_of(tg.frame_of(m[1])) for g in groups for m in g[3])
        story_pass = 0 if armed < first_at - 2.5 * S else armed // loop + 1
        target = story_pass * loop + until
        print(f"  armed by {armed / S:.2f} s; reading from pass {story_pass}; {target / S:.0f} s of render to wait for")
        (room / "pass.txt").write_text(str(story_pass), encoding="ascii")
        started = time.monotonic()
        while frames_on_disk(render) < target + S:
            if time.monotonic() - started > 3 * target / S + 120:
                raise HarnessError("the render stopped growing")
            time.sleep(1.0)
        notes = [line for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                 if line.startswith("# note")] if log.is_file() else []
        print(f"  rtViolations {value_of(server, '/godot/engine/rtViolations')}, audioGaps "
              f"{value_of(server, '/godot/engine/audioGaps')}, tick lateness at worst "
              f"{(value_of(server, '/godot/engine/latenessMax') or 0) / S * 1000:.1f} ms")
        wait_for_render_tail(render)
    for line in notes[:8]:
        print(f"  {line}")
    return story_pass, tg.Log(log), tg.Render(render)


def r0_of(log: tg.Log, truth: tg.Truth) -> int:
    first = next(h for h in log.heard if h.source == tg.LTC_ORIGIN and h.what == "jumped")
    exact = truth.of(first.frame)
    return tg.BLOCK * int(round((exact - first.sample) / tg.BLOCK))


def summary(values) -> str:
    if not values:
        return "none"
    absolute = [abs(v) for v in values]
    return (f"median {statistics.median(values):+.3f} ms, p99 |{percentile(absolute, 0.99):.3f}| ms, "
            f"worst |{max(absolute):.3f}| ms over {len(values)}")


# =============================================================================
# (a) Firing
# =============================================================================

SPACING = int(option("spacing", "75"))        # frames between two members: 3 s
FIRES = int(option("fires", "10"))            # members a group
FIRST = tg.frame_of("01:00:00:00")
FIRE_STORY = tg.Story([("code", FIRST - 75, 75 + FIRES * SPACING + 40, 1.00005)])
FIRE_LOOP = int(math.ceil(FIRE_STORY.length))


def bursts_of(render: tg.Render, channel: int):
    """[onset] of every second of LTC on a channel: frames whose number does
    not follow the one before begin a new burst, and its onset is where a line
    through its frames puts frame nought."""
    out, current = [], []
    for start, frame in tg.ltc_frames(render, channel):
        if current and frame <= current[-1][1]:
            out.append(current)
            current = []
        current.append((start, frame))
    if current:
        out.append(current)
    onsets = []
    for frames in out:
        if len(frames) >= 5:
            onset = tg.Track([(t, k / tg.FPS) for t, k in frames]).onset()
            if onset is not None:
                onsets.append(onset)
    return onsets


def firing(room: Path, passes: int) -> None:
    print(f"(a) firing: {FIRES} members on LTC and {FIRES} on MTC, {passes} passes of {FIRE_LOOP / S:.2f} s")
    groups = []
    for g, (group, source, bus) in enumerate((("TG0G0001", "ltc", "TG0B0001"), ("TG0G0003", "mtc", "TG0B0002"))):
        members = [(ident("TG0M", 1 + g * 100 + k), tg.label_of(FIRST + k * SPACING), "short.wav", bus, "free")
                   for k in range(FIRES)]
        groups.append((group, f"On {source.upper()}", source, members))
    bundle = make_bundle(room, groups, [("TG0B0001", "LTC members"), ("TG0B0002", "MTC members")])
    if not option("analyse"):
        tg.write_member(bundle / "media" / "short.wav", 1.0)
    story_pass, log, render = run_story(room, bundle, FIRE_STORY, FIRE_LOOP, groups, passes * FIRE_LOOP)
    truth = tg.Truth(story_pass * FIRE_LOOP, FIRE_STORY)
    r0 = r0_of(log, truth)
    last = story_pass * FIRE_LOOP + passes * FIRE_LOOP

    def nearest(onset, wants):
        want = min(wants, key=lambda w: abs(w - onset))
        return (onset - want) / tg.MS, want

    # Each run's `run.fire`, the map of its group's code applied before it,
    # and the burst nearest where that map put the member's time: how far
    # from its map's sample each fire began, and which began late - after it,
    # a launch the engine placed as soon as it could once asked.
    members = {m[0]: (tg.frame_of(m[1]), tg.LTC_ORIGIN if g[2] == "ltc" else tg.MTC_ORIGIN)
               for g in groups for m in g[3]}
    cue_of = {r.args[2]: r.args[1] for r in log.of("run.spawn") if len(r.args) >= 3}

    def on_map(source: str, onsets):
        out = {}
        for record in log.of("run.fire"):
            member = members.get(cue_of.get(record.args[0], ""))
            heard = log.map_before(source, record.seq) if member and member[1] == source else None
            if heard is None or not onsets:
                continue
            want = heard.sample_of(member[0]) + r0
            if story_pass * FIRE_LOOP <= want < last:
                onset = min(onsets, key=lambda o: abs(o - want))
                out[onset] = (onset - want) / tg.MS
        return out

    # Where each member's time fell, pass by pass, on the input.
    ltc_onsets = [o for o in bursts_of(render, 0) if story_pass * FIRE_LOOP <= o < last]
    ltc_wants = [story_pass * FIRE_LOOP + m * FIRE_LOOP + FIRE_STORY.sample_of(FIRST + k * SPACING)
                 for m in range(passes) for k in range(FIRES)]
    ltc_map = on_map(tg.LTC_ORIGIN, ltc_onsets)
    ltc = {o: nearest(o, ltc_wants)[0] for o in ltc_onsets}

    # MTC's passes, from the `jumped` that opens each, and where each puts its frames.
    mtc_heard = [h for h in log.heard if h.source == tg.MTC_ORIGIN]
    opens = [i for i, h in enumerate(mtc_heard) if h.what == "jumped" and h.frame <= FIRST - 70]
    origins = []
    for a, b in zip(opens, opens[1:] + [len(mtc_heard)]):
        offsets = [h.sample + r0 - FIRE_STORY.sample_of(h.frame) for h in mtc_heard[a:b]
                   if h.what != "stopped" and FIRE_STORY.sample_of(h.frame) is not None]
        if offsets:
            origins.append(statistics.median(offsets))
    mtc_onsets = [o for o in bursts_of(render, 1) if story_pass * FIRE_LOOP <= o < last]
    mtc_wants = [o + FIRE_STORY.sample_of(FIRST + k * SPACING) for o in origins for k in range(FIRES)]
    mtc_map = on_map(tg.MTC_ORIGIN, mtc_onsets)
    mtc = {o: nearest(o, mtc_wants)[0] for o in mtc_onsets}

    for name, against, by_map in (("LTC", ltc, ltc_map), ("MTC", mtc, mtc_map)):
        late = {o for o, d in by_map.items() if d * tg.MS > 2.5}
        frame = "the input's frame" if name == "LTC" else "where the port's pass put the frame"
        print(f"  {name}, against {frame}: {summary(list(against.values()))}")
        print(f"  {name}, against the map it was asked by: {summary(list(by_map.values()))}")
        print(f"    of which {len(late)} began after their map's sample, launched as soon as they could be; the"
              f" others against the frame: {summary([d for o, d in against.items() if o not in late])}")
    behind = [ms for _, ms in log.late_ticks]
    print(f"  the tick thread noted behind its audio {len(behind)} times"
          + (f", up to {max(behind):.0f} ms" if behind else ""))
    print(f"  code.heard: LTC {sum(1 for h in log.heard if h.source == tg.LTC_ORIGIN)}, "
          f"MTC {len(mtc_heard)} over {len(origins)} passes of the port")


# =============================================================================
# (b) Chase
# =============================================================================

def chase_story(minutes: float) -> tg.Story:
    """Ten minutes, or the same shape scaled: four tenths at +50 ppm, three at
    +0.1 %, a two-second stop, back 120 ms on, two tenths at +50 ppm, a locate
    back thirty seconds, one tenth more."""
    total = minutes * 60.0
    a, b, c, d = (int(round(total * f)) * tg.FPS for f in (0.4, 0.3, 0.2, 0.1))
    start = tg.frame_of("00:59:50:00")
    lead = 250
    after_b = start + lead + a + b
    back = after_b + 50 + 3
    return tg.Story([("code", start, lead + a, 1.00005), ("code", start + lead + a, b, 1.001), ("quiet", 2.0),
                     ("code", back, c, 1.00005), ("code", back + c - 750, d, 1.00005)])


def chase(room: Path, minutes: float) -> None:
    story = chase_story(minutes)
    loop = int(math.ceil(story.length))
    length = math.ceil(story.length / S - 5.0)
    print(f"(b) chase: {story.length / S:.1f} s of code, four members of {length} s")
    members = [(tg.FREE.cue, "01:00:02:00", "ltc.wav", "TG0B0001", "free"),
               (tg.JUMP.cue, "01:00:02:00", "ltc.wav", "TG0B0002", "jump"),
               (tg.VARI.cue, "01:00:02:00", "ltc.wav", "TG0B0003", "varispeed"),
               (tg.STRETCH.cue, "01:00:02:00", "bursts.wav", "TG0B0004", "timestretch")]
    groups = [("TG0G0001", "On LTC", "ltc", members)]
    bundle = make_bundle(room, groups, [("TG0B0001", "Free"), ("TG0B0002", "Jump"), ("TG0B0003", "Varispeed"),
                                        ("TG0B0004", "Timestretch")])
    if not option("analyse"):
        tg.write_member(bundle / "media" / "ltc.wav", length)
        tg.write_bursts(bundle / "media" / "bursts.wav", length)
    story_pass, log, render = run_story(room, bundle, story, loop, groups, story.length + 1.0 * S)
    truth = tg.Truth(story_pass * loop, story)
    step, stop, back, locate = (story.pieces[k][1] for k in (1, 2, 3, 4))
    end = story.length
    at = truth.of(tg.frame_of("01:00:02:00"))
    shape = tg.burst_shape()

    print("  | method | +50 ppm | +0.1 % | in step after the return | then | in step after the locate | then"
          " | jumps (A/B/C/D) | worst steady | speeds (A/B, then catching up) |")
    for member, settle in ((tg.FREE, 1.5), (tg.JUMP, 10.0), (tg.VARI, 1.5), (tg.STRETCH, tg.STRETCHED_MS)):
        if member.sound == "bursts":
            track, _ = tg.bursts_track(render, member, truth, shape)
        else:
            track = tg.ltc_track(render, member)
        errs = truth.errors(track, member, at, truth.at(end))

        def caught(event):
            """Seconds after `event` until it is within `settle` and stays so,
            heard throughout, for two seconds; None if never."""
            pts = [(t, e) for t, e in errs if truth.at(event) <= t < truth.at(end)]
            times = [t for t, _ in pts]
            for i, (t, e) in enumerate(pts):
                if abs(e) > settle:
                    continue
                ahead = pts[i:bisect.bisect_left(times, t + 2 * S, i)]
                if ahead[-1][0] >= t + 1.5 * S and all(abs(x) <= settle for _, x in ahead):
                    return (t - truth.at(event)) / S
            return None

        after_back, after_locate = caught(back), caught(locate)
        stretches = [(at - truth.origin + 5 * S, step), (step + 5 * S, stop),
                     (back + (after_back if after_back is not None else 20.0) * S + 1 * S, locate),
                     (locate + (after_locate if after_locate is not None else 5.0) * S + 1 * S, end)]

        def stretch(first, last):
            values = [abs(e) for t, e in errs if truth.at(first) <= t < truth.at(last)]
            return f"{statistics.median(values):.2f}/{max(values):.2f}" if values else "silent"

        # Its own jumps: where it moved in its file beyond what its speed
        # explains, frame to frame - or burst to burst, half a second apart, at
        # up to 1.2 % (the stretcher's 259/256).
        threshold = 8.0 if member.sound == "bursts" else 3.0
        bounds = [at - truth.origin, step, stop, locate, end]
        jumps = [len(track.moves(truth.at(a), truth.at(b), 1.0, threshold)) for a, b in zip(bounds, bounds[1:])]
        steady = [abs(e) for t, e in errs if any(truth.at(a) <= t < truth.at(b) for a, b in stretches)]
        speeds = "/".join("-" if r is None else f"{r:.6f}" for r in
                          (track.rate(at + 5 * S, truth.at(step), 3), track.rate(truth.at(step) + 5 * S, truth.at(stop), 3),
                           track.rate(truth.at(back) + 1 * S, truth.at(back) + 8 * S, 3)))
        print(f"  | {member.chase} | {stretch(*stretches[0])} | {stretch(*stretches[1])} "
              f"| {'never' if after_back is None else f'{after_back:.1f} s'} | {stretch(*stretches[2])} "
              f"| {'never' if after_locate is None else f'{after_locate:.1f} s'} | {stretch(*stretches[3])} "
              f"| {'/'.join(map(str, jumps))} | {max(steady) if steady else float('nan'):.2f} | {speeds} |")
    print("  (each stretch: median/worst |error| in ms against the code; in step: within 1.5 ms for varispeed (the law lets"
          f" go within one, the map is half a millisecond) and the free runner's reference, 10 for jump, {tg.STRETCHED_MS:.1f} for timestretch; A from 5 s after the"
          " members' time to the step, B from 5 s after the step to the stop, C from a second after it is back in"
          " step to the locate, D from a second after it is in step again)")
    heard = [h for h in log.heard if h.source == tg.LTC_ORIGIN]
    print(f"  code.heard from LTC: {len(heard)} - " + ", ".join(
        f"{sum(1 for h in heard if h.what == w)} {w}" for w in ("jumped", "running", "stopped")))


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"m64: {problem}", file=sys.stderr)
        return 2
    kept = option("keep") or option("analyse")
    rooms = []
    try:
        for name, wanted in (("firing", "--chase-only" not in sys.argv), ("chase", "--firing-only" not in sys.argv)):
            if not wanted:
                continue
            if option("analyse"):
                room = Path(kept) / name
            elif kept:
                room = Path(kept) / name
                if room.exists():
                    shutil.rmtree(room)
                room.mkdir(parents=True)
            else:
                room = Path(tempfile.mkdtemp(prefix=f"wfg-m64-{name}-"))
                rooms.append(room)
            if name == "firing":
                firing(room, int(option("passes", "20")))
            else:
                chase(room, float(option("chase-minutes", "10")))
    except HarnessError as problem:
        print(f"m64: {problem}", file=sys.stderr)
        return 2
    finally:
        for room in rooms:
            shutil.rmtree(room, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
