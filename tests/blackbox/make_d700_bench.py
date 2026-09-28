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

"""A bench show for the Asparion D700: everything an afternoon at the desk needs, generated.

WHAT IT IS FOR. Phase 6's done-when has four clauses only the unit on the desk
can settle - a sample started from a D700 fader with the audio armed, a group
DCA followed by the motor faders through a fade, provenance on the strip
displays, and a bank change that lets a playing clip finish before its strip
switches - and M27/M28 want the unit too. This writes one show that walks
through all of them with GO, so the session is spent looking at the desk and
not declaring ports.

WHAT IS IN IT. The D700 declared on its two ports, named the way Windows names
them (docs/D700_CONTROL_GUIDE.md §1.1: "D 700" is bank 1, "MIDIIN2 (D 700)" /
"MIDIOUT2 (D 700)" bank 2), expecting the Mackie preset. Its first fader is a
DCA strip on "Band"; the rest are sampler strips. Bank A is four samples with
four initial levels - 0, -6, -12 and -18 dB - so the faders fly to four
different places when it is armed; three are tones at different pitches, and
one is noise, so a ring shows what the saturation carries (a tone vivid, noise
pale). Bank B is two more, armed over A with a group takeover. "Bed" plays on
the Band DCA, and two fades move the DCA, which the first fader follows.
"Low" and "High" carry an EQ already, so the EQ page has something to show:
"Low" a low shelf lifted and its high-pass in, "High" a cut in band three.
Three mix channels - Foldback, Wedges, Reverb - on outputs 3-8, and "Low"
sends to Foldback at -6 and Reverb at -12, so the Send page has two sends and
one channel it does not reach yet.

AUDIBLE, UNLIKE THE FIXTURES' MEDIA, which are constants made for arithmetic:
these are sine tones, a noise burst and a slow sweep, each with a short fade in
and out so nothing clicks, at -12 dBFS.

    python tests/blackbox/make_d700_bench.py [folder]      (default: build/bench/d700)

Not a test and not registered anywhere: a thing to open.
"""

from __future__ import annotations

import argparse
import math
import random
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common                                    # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
RATE = 48000
PEAK = 0.25          # -12 dBFS: loud enough to hear a fader, gentle on a monitor
EDGE = 0.02          # seconds of fade at each end, so a start or a stop never clicks


# =============================================================================
# The sounds
# =============================================================================

def write_wav(path: Path, seconds: float, voice) -> None:
    """A stereo 16-bit file of `voice(t)` in -1..1, faded in and out at the edges."""
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = int(RATE * seconds)
    edge = int(RATE * EDGE)
    body = bytearray()

    for n in range(frames):
        gain = min(1.0, n / edge, (frames - 1 - n) / edge) if edge > 0 else 1.0
        value = int(max(-1.0, min(1.0, voice(n / RATE) * PEAK * gain)) * 32767)
        body += struct.pack("<hh", value, value)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(body))


def tone(*frequencies: float):
    count = len(frequencies)
    return lambda t: sum(math.sin(2.0 * math.pi * f * t) for f in frequencies) / count


def noise(seed: int):
    draw = random.Random(seed)
    return lambda t: draw.uniform(-1.0, 1.0)


def sweep(low: float, high: float, seconds: float):
    rate = math.log(high / low) / seconds
    return lambda t: math.sin(2.0 * math.pi * low * (math.exp(rate * t) - 1.0) / rate)


SOUNDS = {
    "low.wav":   (6.0, tone(110.0)),
    "mid.wav":   (6.0, tone(440.0)),
    "high.wav":  (6.0, tone(1760.0)),
    "noise.wav": (6.0, noise(7)),
    "chord.wav": (8.0, tone(330.0, 415.3)),
    "sweep.wav": (8.0, sweep(200.0, 2000.0, 8.0)),
    "bed.wav":   (30.0, tone(220.0, 277.2)),
}


# =============================================================================
# The show
# =============================================================================

def strips() -> str:
    """Sixteen strips, two banks of eight: the first a DCA strip on Band."""
    digits = "123456789ABCDEFG"
    lines = ['      <Strip id="DBNT0001" dca="DBND0001" role="dca"/>']
    lines += [f'      <Strip id="DBNT000{d}"/>' for d in digits[1:]]
    return "\n".join(lines)


def media(cue_id: str, number: str, name: str, file: str, colour: str, route: str,
          extra: str = "", sends: str = "") -> str:
    return (f'        <Media id="{cue_id}" colour="{colour}" file="{file}"{extra} name="{name}" '
            f'number="{number}">\n'
            f'          <Route id="{route}" bus="DBNB0001" gains="1 1"/>\n'
            f'{sends}'
            f'        </Media>')


SHOW = f"""<Show>
  <Lists>
    <List id="DBN00001" name="D700 bench">
      <Group id="DBN00002" mode="sampler" name="Bank A" number="1" takeover="group">
{media("DBN00003", "1.1", "Low", "low.wav", "#C04040", "DBNR0003",
        ' eqB1Gain="4" eqB1Shape="lowShelf" eqHpf="true" eqHpfFreq="60"',
        '          <Send id="DBNX0001" bus="DBNB0002" level="-6"/>\n'
        '          <Send id="DBNX0002" bus="DBNB0004" level="-12"/>\n')}
{media("DBN00004", "1.2", "Mid", "mid.wav", "#D0A030", "DBNR0004", ' initialLevel="-6" release="hold"')}
{media("DBN00005", "1.3", "High", "high.wav", "#40A0E0", "DBNR0005",
        ' eqB3Freq="1500" eqB3Gain="-9" eqB3Q="2" initialLevel="-12"')}
{media("DBN00006", "1.4", "Noise", "noise.wav", "#909090", "DBNR0006", ' initialLevel="-18"')}
      </Group>
      <Media id="DBN00007" colour="#50C080" dca="DBND0001" file="bed.wav" name="Bed" number="2">
        <Route id="DBNR0007" bus="DBNB0001" gains="1 1"/>
      </Media>
      <Fade id="DBN00008" dca="DBND0001" duration="3" level="-20" name="Band down" number="3"/>
      <Fade id="DBN00009" dca="DBND0001" duration="3" level="0" name="Band up" number="4"/>
      <Group id="DBN0000A" mode="sampler" name="Bank B" number="5" takeover="group">
{media("DBN0000B", "5.1", "Chord", "chord.wav", "#A060D0", "DBNR000B")}
{media("DBN0000C", "5.2", "Sweep", "sweep.wav", "#E07030", "DBNR000C", ' initialLevel="-6"')}
      </Group>
      <Transport id="DBN0000D" name="Disarm B" number="6" target="DBN0000A"/>
      <Cue id="DBN0000E" name="End" number="7"/>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="8">
    <Bus id="DBNB0001" name="Main L/R" width="2"/>
    <Bus id="DBNB0002" firstChannel="2" kind="mix" name="Foldback" width="2"/>
    <Bus id="DBNB0003" firstChannel="4" kind="mix" name="Wedges" width="2"/>
    <Bus id="DBNB0004" firstChannel="6" kind="mix" name="Reverb" width="2"/>
  </Audio>
  <MidiPorts>
    <Port id="DBNP0001" inputDevice="D 700" name="D700 bank 1" outputDevice="D 700"/>
    <Port id="DBNP0002" inputDevice="MIDIIN2 (D 700)" name="D700 bank 2" outputDevice="MIDIOUT2 (D 700)"/>
  </MidiPorts>
  <Network/>
  <Surfaces>
    <Surface id="DBNS0001" name="D700" ports="DBNP0001 DBNP0002" preset="Mackie" profile="d700">
{strips()}
    </Surface>
  </Surfaces>
  <Dcas>
    <Dca id="DBND0001" name="Band" shortName="Band"/>
  </Dcas>
</Show>
"""

STATE = """<State formatVersion="1">
  <List id="DBN00001" standby="DBN00002"/>
</State>
"""

WALK = """
BEFORE STARTING
  1. Asparion Configurator: put the D700 on preset 2 (Mackie), then CLOSE it -
     it has been seen overriding colour writes.
  2. Close any DAW or Max patch holding the D700's ports (Windows MIDI is exclusive).
  3. {wfg} midi
     should list "D 700" and "MIDIIN2 (D 700)" / "MIDIOUT2 (D 700)".

START
  {wfg} serve {bundle} --hosted --window --sample-rate=48000 --buffer=128 --device="<your interface>"
  ({wfg} devices lists the interface names.)

  Show settings > Surfaces should say the D700 is connected. If it names a port with no device
  behind it, correct the device in Show settings > MIDI, save, and restart serve: a port binds
  at start (rebinding while running is not built yet). Show > Surfaces... opens the panel,
  which mirrors all sixteen strips.

THE WALK (GO on the desk is PLAY; STOP is Esc, STOP twice is double Esc)
  GO   Bank A arms: faders 2-5 fly to 0, -6, -12 and -18 dB; the rotaries' surrounds show the
       cues' colours; the displays show the names and levels.
       Touch a fader: that sample starts at the fader's level. Ride it.
       "Mid" is a hold clip: pull it to the bottom and let go, and it stops.
       "Noise" should light a pale ring while it sounds, the tones vivid ones.
  GO   Bed plays, marked with the Band DCA - fader 1 rides it.
  GO   Band down: fader 1 follows the three-second fade to -20 dB.
  GO   Band up, back to 0.
  GO   Bank B takes over the desk: a clip of Bank A still sounding finishes before its strip
       changes hands; idle strips change at once.
  GO   Disarm B.

THE EQ AND SEND PAGES (2026-09-25)
  With Bank A armed:
  SELECT on fader 2 ("Low"): its white bar lights and its third row reads "picked".
  EQ   the sixteen rotaries are Low's EQ, in the author's order: HP freq, B1 shape, B1 freq,
       B1 gain, B1 Q, B2 freq/gain/Q | B3 freq/gain/Q, B4 shape/freq/gain/Q, LP freq.
       Each surround wears its band's colour (the EQ panel's), dimmed while the band is out;
       the ring is the value; the screens say the control and the value, "off" beside a band
       that is out. EQ is lit.
       Turn B1 gain: Low's shelf moves (listen while it plays). Press it: band one out, "off".
       Press HP freq: the high-pass out; again, in. Press B1 shape: shelf to peak and back.
  EQ   again: the surface's own page. `*` also leaves at once.
  SELECT on fader 4 ("High"), EQ: High's EQ, band three cut at 1.5 kHz.
  SELECT on the lit strip again: nothing picked, and an EQ page closes.
  SELECT on fader 2 again ("Low"), then Send: three rotaries - Foldback -6, Wedges "no send",
       Reverb -12 - in Low's colour, dimmed where out. Turn Foldback; press it: off. Turn
       Wedges up: the send is made. Send again: the surface's own page.
  LOCK the show (the padlock), SELECT Low, EQ and turn: heard, not saved (/godot/document/live
       counts it). Unlock: keep it with `live.keep` or let it go with `live.drop` - the window's
       Keep / Discard bar is its next stage.
  Worth watching for: which port the EQ button's light answers on (it is sent to the port the
  press came from); whether the colours read at a glance; how fast a detent should be.

  Worth watching for: a fader brushed while reaching for the master section - does it fire a
  sample? (FaderEdge::touchDwellTicks in src/wfg/engine/cue/Runner.h is the lever.)
  M27 and M28 need `pip install python-rtmidi` and serve stopped: see their own help.
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder", nargs="?", default=str(REPO_ROOT / "build" / "bench" / "d700"))
    options = parser.parse_args()

    bundle = Path(options.folder).resolve()
    bundle.mkdir(parents=True, exist_ok=True)

    (bundle / f"{bundle.name}.wfg").write_text('<Bundle formatVersion="1"/>\n', encoding="utf-8")
    (bundle / "show.xml").write_text(SHOW, encoding="utf-8", newline="\n")
    (bundle / "state.xml").write_text(STATE, encoding="utf-8", newline="\n")

    for name, (seconds, voice) in SOUNDS.items():
        write_wav(bundle / "media" / name, seconds, voice)

    try:
        binary = common.find_binary()
    except common.HarnessError:
        binary = None

    if binary is not None:
        for verb in ("validate", "analyse"):
            done = subprocess.run([str(binary), verb, str(bundle)], capture_output=True, text=True)
            print(f"wfg {verb}: {(done.stdout + done.stderr).strip() or 'ok'}")

    release = REPO_ROOT / "build" / "vs" / "src" / "Release" / "wfg.exe"
    wfg = release if release.exists() else (binary or Path("wfg"))

    print(f"\nBench show written to {bundle}")
    print(WALK.format(wfg=wfg, bundle=bundle))
    return 0


if __name__ == "__main__":
    sys.exit(main())
