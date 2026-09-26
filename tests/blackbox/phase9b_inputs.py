#!/usr/bin/env python3
"""The live rack, stage 9b.2 - named inputs, heard before anything listens.

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

WHAT THIS PROVES, end to end and through nothing but the product's own doors
(namespace draft 18.2 and 18.10): a show served on the hosted interface with a
two-channel WAV at its inputs - a tone on the first, silence on the second -
names three inputs with `input.create`; the first reads the tone's level on its
meter and the second reads silence, whether or not anything listens, which is
the soundcheck's question; the third lies past the interface's two logical
inputs and says so in words rather than reading silent; the interface's own
delays read nought on the hosted interface; and the session replays.
"""

import argparse
import math
import struct
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
from common import Report, Server  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "persistent"

RATE = 48000
BLOCK = 128
TONE = 0.25                     # -12 dBFS
TONE_DB = 20.0 * math.log10(TONE)


def write_inputs(path: Path, seconds: float = 3.0) -> None:
    """A tone on the first channel and silence on the second."""
    frames = int(RATE * seconds)
    samples = []
    for n in range(frames):
        samples.append(int(TONE * 32767 * math.sin(2.0 * math.pi * 750.0 * n / RATE)))
        samples.append(0)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * len(samples), *samples))


def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")
    if status != 200:
        return None
    try:
        return common.json.loads(body)["VALUE"][0]
    except Exception:
        return None


def send(server: Server, address: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode(address, args or []))


def run(locale: "str | None") -> int:
    report = Report(f"stage 9b.2: named inputs and their meters ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-phase9b-inputs-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "persistent")
        inputs = room / "inputs.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        write_inputs(inputs)

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    input_wav=inputs, engine_folder=room / "engine") as server:
            for _ in range(3):
                send(server, "/godot/cmd/input/create", [1, -1])

            def three():
                ids = (value_of(server, "/godot/input/order") or "").split()
                return ids if len(ids) == 3 else None

            order = common.wait_until(three, timeout=10)
            report.check(order is not None and len(order) == 3, "three named inputs are made", f"{order}")

            if order:
                voice, silent, beyond = order

                report.equal(value_of(server, f"/godot/input/{voice}/name"), "Input 1",
                             "named on arrival, as an output is")
                report.equal(value_of(server, f"/godot/input/{beyond}/firstChannel"), 2,
                             "packed onto the logical inputs one after another")

                def near_tone():
                    level = value_of(server, f"/godot/input/{voice}/meter")
                    return level if isinstance(level, (int, float)) and abs(level - TONE_DB) < 1.5 else None

                heard = common.wait_until(near_tone, timeout=10)
                report.check(heard is not None,
                             "the first input's meter reads the tone at its inputs, with nothing listening",
                             f"read {value_of(server, f'/godot/input/{voice}/meter')} against {TONE_DB:.1f} dB")

                report.check((value_of(server, f"/godot/input/{silent}/meter") or 0) <= -119.0,
                             "the second reads silence",
                             f"read {value_of(server, f'/godot/input/{silent}/meter')}")
                report.equal(value_of(server, f"/godot/input/{silent}/problem"), "",
                             "and says nothing is wrong with it")

                problem = value_of(server, f"/godot/input/{beyond}/problem") or ""
                report.check("past the last" in problem,
                             "the third, past the interface's two, says so in words rather than reading silent",
                             problem)

            report.equal(value_of(server, "/godot/audio/inputLatency"), 0,
                         "the hosted interface has no input delay of its own")
            report.equal(value_of(server, "/godot/audio/outputLatency"), 0,
                         "nor an output one")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session", (out + err).strip()[-400:])

    return report.finish()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wfg-locale", default=None)
    args = parser.parse_args()
    return run(args.wfg_locale)


if __name__ == "__main__":
    sys.exit(main())
