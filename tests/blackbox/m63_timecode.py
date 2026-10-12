#!/usr/bin/env python3
"""M63 - outgoing timecode over minutes: how late MTC leaves, how clean LTC reads.

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

A MEASUREMENT, NOT A GATE (namespace draft 59.6). Two sessions of `wfg serve
--hosted --render --midi-render=TC=<file>` on a copy of the timecode fixture,
each running an LTC cue and an MTC cue that run until stopped, side by side in
one timeline group, for N minutes (ten unless told):

  (a) MTC: how late each message left against its moment, `sent_ns - due_ns`
      off the render file - the median, the 99th percentile, the worst, how
      many past 2 ms (what `sync/mtcLate` counts), how many quarter frames the
      lane dropped for lateness, and how far the due times wander off the
      straight line they should lie on. The first session alone on the port;
      the second with a 100-byte SysEx MIDI cue on the same port fired every
      second, the gate's cost (AHQ).
  (b) LTC: the whole of each session's code decoded by timecode.py's decoder -
      bit errors, frames with odd parity, a label that is not the one after
      the last (drop-frame's skips are the one after the last), and how far any
      frame's first edge strayed from `zero + k * frame` over the whole run.

WHAT THE RENDER CANNOT SHOW. `--midi-render` writes a line where a port would
send bytes, and a write costs microseconds where a DIN port takes 320 us a byte:
the second session shows what the lane does beside a SysEx on its thread and
its gate, not the 32 ms a 100-byte SysEx holds a real cable for. That is the
bench's, with a port and a reader.

Run by hand on a quiet machine; it writes nothing into the repository:

    WFG_BINARY=<wfg> python tests/blackbox/m63_timecode.py [--minutes=10]
        [--rate=29.97df] [--sample-rate=48000] [--alone-only]
"""

from __future__ import annotations

import os
import platform
import re
import statistics
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
import timecode as tc  # noqa: E402
from common import HarnessError, Server  # noqa: E402
from first_sound import run_for_cue, value_of, wait_for, wait_for_run_state, wait_for_render_tail  # noqa: E402

GROUP = "TC0G0003"
LTC_CUE = "TC0T0004"
MTC_CUE = "TC0T0005"
SYSEX_CUE = "TC0S0001"
LTC_FROM = (1, 0, 0, 0)
MTC_FROM = (10, 0, 0, 0)

# F0, 7D (the non-commercial manufacturer), 97 data bytes, F7: a hundred bytes.
SYSEX = " ".join(["F0", "7D"] + [f"{n % 0x60:02X}" for n in range(97)] + ["F7"])

LIST = f"""<List id="TC0A0001" name="M63">
      <Group id="{GROUP}" mode="timeline" name="Both codes" number="1">
        <Timecode id="{LTC_CUE}" format="ltc" from="01:00:00:00" name="LTC until stopped" number="1.1"/>
        <Timecode id="{MTC_CUE}" format="mtc" from="10:00:00:00" name="MTC until stopped" number="1.2"/>
      </Group>
      <Midi id="{SYSEX_CUE}" name="A hundred bytes" number="2" port="TC0P0001" sysex="{SYSEX}" type="sysex" wait="sent"/>
    </List>"""


def make_show(room: Path, rate: tc.Rate) -> Path:
    """The timecode fixture with its list replaced by M63's, and its tone's
    route gone with it: two codes until stopped, and the SysEx cue."""
    bundle = common.copy_bundle(tc.FIXTURE, room / "show")
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")
    text = re.sub(r"<List .*?</List>", LIST, text, count=1, flags=re.S)
    text = text.replace('frameRate="25"', f'frameRate="{rate.word}"')
    show.write_text(text, encoding="utf-8", newline="\n")
    (bundle / "state.xml").write_text(
        f'<State formatVersion="1">\n  <List id="TC0A0001" standby="{GROUP}"/>\n</State>\n',
        encoding="utf-8", newline="\n")
    return bundle


def session(room: Path, rate: tc.Rate, sample_rate: int, minutes: float, with_sysex: bool,
            locale: "str | None") -> dict:
    bundle = make_show(room, rate)
    render = room / "out.wav"
    midi = room / "mtc.txt"
    fired = 0

    with Server(bundle, locale=locale, sample_rate=sample_rate, buffer_size=tc.BLOCK, hosted=True,
                render=render, midi_render={"TC": str(midi)}) as server:
        if wait_for(server, "/godot/audio/status", "running", timeout=60.0) != "running":
            raise HarnessError("the audio side did not come up")

        common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/cue/fire", [GROUP]))
        ltc_run = run_for_cue(server, LTC_CUE, timeout=30.0)
        mtc_run = run_for_cue(server, MTC_CUE, timeout=30.0)

        if not ltc_run or not mtc_run:
            raise HarnessError(f"the codes did not start: LTC {ltc_run!r}, MTC {mtc_run!r}")

        began = time.monotonic()
        until = began + 60.0 * minutes
        next_report = began + 60.0

        while time.monotonic() < until:
            if with_sysex:
                common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/cue/fire", [SYSEX_CUE]))
                fired += 1
            if time.monotonic() >= next_report:
                print(f"    {(time.monotonic() - began) / 60.0:.0f} min: LTC at "
                      f"{value_of(server, f'/godot/run/{ltc_run}/timecode')}, MTC at "
                      f"{value_of(server, f'/godot/run/{mtc_run}/timecode')}", flush=True)
                next_report += 60.0
            time.sleep(max(0.0, min(1.0, until - time.monotonic())))

        readouts = {name: value_of(server, f"/godot/sync/{name}") for name in ("mtcLate", "mtcWorst", "ltcLate")}
        common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/run/stopAll"))
        wait_for_run_state(server, ltc_run, "done", 30.0)
        wait_for_run_state(server, mtc_run, "done", 30.0)
        wait_for_render_tail(render)

    return {"render": render, "midi": midi, "fired": fired, "readouts": readouts}


def mtc_figures(lines, rate: tc.Rate) -> dict:
    """The lane's lateness off the render file, and the quarter frames missing."""
    timed = [entry for entry in lines if entry[1] != 0]
    late = [(sent - due) / 1e6 for sent, due, _ in timed]
    sysex = sum(1 for entry in lines if entry[1] == 0 and entry[2][:1] == (0xF0,))

    quarters = [entry for entry in timed if tc.is_quarter_frame(entry[2])]
    quarter_ns = 1e9 * rate.den / (4 * rate.num)
    first_due = timed[0][1] if timed else 0
    indices = [round((due - first_due) / quarter_ns) for _, due, _ in quarters]
    expected = (indices[-1] + 1) if indices else 0

    #  How far the due times wander from the straight line through them: the
    #  lane's line between samples and moments, corrected by every tick.
    wander = 0.0
    if len(quarters) > 2:
        xs = indices
        ys = [due for _, due, _ in quarters]
        mean_x, mean_y = statistics.fmean(xs), statistics.fmean(ys)
        slope = (sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
                 / sum((x - mean_x) ** 2 for x in xs))
        wander = max(abs(y - (mean_y + slope * (x - mean_x))) for x, y in zip(xs, ys)) / 1e6

    return {
        "messages": len(late),
        "median": statistics.median(late) if late else float("nan"),
        "p99": tc.percentile(late, 0.99),
        "worst": max(late) if late else float("nan"),
        "over2": sum(1 for value in late if value > 2.0),
        "dropped": expected - len(quarters),
        "wander": wander,
        "sysex": sysex,
    }


def ltc_figures(render: Path, rate: tc.Rate, sample_rate: int) -> dict:
    """The whole code, decoded a piece at a time."""
    level = 10.0 ** (tc.LTC_LEVEL_DB / 20.0)
    decoder = tc.LtcDecoder(sample_rate, rate, level)
    zero = -1
    at = 0

    for piece in tc.channel_chunks(render, tc.LTC_CHANNEL):
        if zero < 0:
            found = tc.first_nonzero(piece)
            if found >= 0:
                zero = at + found
        decoder.feed(piece)
        at += len(piece)

    decoder.feed([0.0] * (decoder.quiet_limit + 1))
    frames = decoder.frames
    frame_len = rate.frame_samples(sample_rate)

    skips = 0
    for before, after in zip(frames, frames[1:]):
        if after.label != tc.step_label(before.label, rate):
            skips += 1

    offsets = [f.start - (zero + k * frame_len) for k, f in enumerate(frames)][1:]
    end = decoder.quiets[-1] if decoder.quiets else at
    played = int((end - zero) / frame_len + 0.5) if zero >= 0 else 0

    return {
        "frames": len(frames),
        "played": played,
        "errors": len(decoder.errors),
        "parity": sum(1 for f in frames if not f.parity_even or not f.bcd_ok),
        "skips": skips,
        "first": tc.label_text(frames[0].label, rate.drop) if frames else "-",
        "last": tc.label_text(frames[-1].label, rate.drop) if frames else "-",
        "drift": (max(offsets) - min(offsets)) if offsets else float("nan"),
        "drops_seen": sum(1 for a, b in zip(frames, frames[1:])
                          if rate.drop and b.label[2] == 0 and b.label[3] == 2 and b.label[1] % 10 != 0),
    }


def main() -> int:
    options = {arg.split("=", 1)[0]: (arg.split("=", 1)[1] if "=" in arg else "") for arg in sys.argv[1:]}
    minutes = float(options.get("--minutes", "10"))
    rate = tc.RATES[options.get("--rate", "29.97df")]
    sample_rate = int(options.get("--sample-rate", "48000"))
    locale = options.get("--wfg-locale")
    kinds = [False] if "--alone-only" in options else [False, True]

    try:
        binary = common.find_binary()
    except HarnessError as problem:
        print(f"m63: {problem}", file=sys.stderr)
        return 2

    print(f"M63 - outgoing timecode, {rate.word} at {sample_rate} Hz, {minutes:g} minutes a session")
    print(f"      {binary} on {platform.system()} {platform.release()}, {os.cpu_count()} cores")

    results = []

    for with_sysex in kinds:
        word = "beside a SysEx a second" if with_sysex else "alone on its port"
        print(f"  session: MTC {word}, LTC beside it ...", flush=True)

        with tempfile.TemporaryDirectory(prefix="wfg-m63-") as scratch:
            got = session(Path(scratch), rate, sample_rate, minutes, with_sysex, locale)
            mtc = mtc_figures(tc.read_midi_render(got["midi"]), rate)
            ltc = ltc_figures(got["render"], rate, sample_rate)
            results.append((word, got, mtc, ltc))

    print()
    print("(a) MTC: sent_ns - due_ns, milliseconds")
    print(f"    {'':26}{'messages':>9}{'median':>9}{'p99':>9}{'worst':>9}{'>2 ms':>7}{'dropped':>9}"
          f"{'due wander':>12}{'SysEx':>7}")
    for word, got, mtc, _ in results:
        print(f"    {word:26}{mtc['messages']:>9}{mtc['median']:>9.3f}{mtc['p99']:>9.3f}{mtc['worst']:>9.3f}"
              f"{mtc['over2']:>7}{mtc['dropped']:>9}{mtc['wander']:>12.3f}{mtc['sysex']:>7}")
        print(f"    {'':26}engine: sync/mtcLate {got['readouts']['mtcLate']}, "
              f"sync/mtcWorst {got['readouts']['mtcWorst']} ms, sync/ltcLate {got['readouts']['ltcLate']}"
              + (f"; {got['fired']} SysEx fired" if got["fired"] else ""))

    print()
    print("(b) LTC, decoded off the render")
    print(f"    {'':26}{'frames':>8}{'played':>8}{'bit err':>9}{'parity':>8}{'skips':>7}{'drift':>8}"
          f"{'drops':>7}  from - to")
    for word, _, _, ltc in results:
        print(f"    {word:26}{ltc['frames']:>8}{ltc['played']:>8}{ltc['errors']:>9}{ltc['parity']:>8}"
              f"{ltc['skips']:>7}{ltc['drift']:>8.3f}{ltc['drops_seen']:>7}  {ltc['first']} - {ltc['last']}")
    print("    skips: a label not the one after the last (drop-frame's own skips are not counted);")
    print("    drift: how far apart, in samples, the frames' first edges sit against zero + k * frame;")
    print("    drops: drop-frame minutes turned, each skipping ;00 and ;01")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as problem:
        print(f"m63: {problem}", file=sys.stderr)
        sys.exit(2)
