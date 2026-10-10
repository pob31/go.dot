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
"""The analysis cache swept: at launch by itself, and when somebody asks (namespace draft section 52).

The author, 2026-10-10: "Is there a clean up routine to remove the .timbre
files if a media file is removed from the project?", then "Autosweep at launch
sound like the best option with a manual command in the show menu."

Driven through the shipped serve with nothing but the media folder, a named
command over OSC and the tree:

 1. A show whose media/ holds two sounds, and whose .timbre holds the analysis
    of one of them, of a sound that is gone, and somebody's note. Opened, the
    launch sweep takes away the gone sound's and nothing else, and says so on
    /godot/engine/mediaCacheSweep as `auto`.
 2. The second sound taken out of the folder, `media.cleanCache` asked for: its
    analysis goes, and the readout says `asked`.
 3. The session's log, holding that command, replays record for record.
"""
import hashlib
import os
import sys
import tempfile
import time
from pathlib import Path

import common
from first_sound import write_tone

SHOW = ('<Show><Lists><List id="7K2QM9X4"/></Lists><Mounts/>'
        '<Audio tracks="2"><Bus id="J3MT5XYA" width="2"/></Audio></Show>')


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def aged(path: Path, text: str = "x") -> Path:
    """A cache file written two hours ago - well before any sweep this run makes."""
    path.write_text(text, encoding="utf-8")
    then = time.time() - 2 * 60 * 60
    os.utime(path, (then, then))
    return path


def sweep_row(server) -> "list[str]":
    text = common.http_json(server.http_port, "/godot/engine/mediaCacheSweep").get("VALUE", [""])[0] or ""
    return text.split("\t") if text else []


def run(locale: str) -> int:
    report = common.Report(f"analysis cache swept ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-sweep-") as scratch:
        room = Path(scratch)
        bundle = room / "Sweep"
        media = bundle / "media"
        cache = media / ".timbre"
        cache.mkdir(parents=True)
        (bundle / "Sweep.wfg").write_text('<Bundle formatVersion="1"/>', encoding="utf-8")
        (bundle / "show.xml").write_text(SHOW, encoding="utf-8")

        write_tone(media / "kept.wav", seconds=1.0)
        write_tone(media / "later.wav", seconds=2.0)

        kept = sha256_of(media / "kept.wav")
        later = sha256_of(media / "later.wav")
        gone = "a" * 64

        for stem in (kept, later, gone):
            aged(cache / f"{stem}.tpy")
            aged(cache / f"{stem}.tpk")

        aged(cache / "notes.txt", "somebody's note")

        log = room / "session.wfglog"
        replayed = room / "replayed"

        with common.Server(bundle, log=log, locale=locale) as server:
            #  1. THE LAUNCH SWEEP, by itself.
            first = common.wait_until(lambda: (lambda row: row if len(row) == 6 and row[1] != "sweeping" else None)(sweep_row(server)),
                                      timeout=30)
            report.check(first is not None, "the launch sweep ends and says so", repr(sweep_row(server)))

            if first is not None:
                report.equal(first[:4], ["1", "done", "auto", "2"], "it was the launch's own, and removed two files")

            report.check(not (cache / f"{gone}.tpy").exists() and not (cache / f"{gone}.tpk").exists(),
                         "the analysis of a sound that is gone is taken away")
            report.check(all((cache / f"{stem}.{kind}").exists() for stem in (kept, later) for kind in ("tpy", "tpk")),
                         "the analysis of both sounds still in the folder is kept, though no cue names either")
            report.check((cache / "notes.txt").exists(), "and nothing it does not know is touched")
            report.check((cache / "sounds.index").exists(), "the sounds' hashes are kept for the next launch")

            #  2. ASKED FOR, once a sound has gone.
            (media / "later.wav").unlink()
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/media/cleanCache", []))

            second = common.wait_until(lambda: (lambda row: row if len(row) == 6 and row[0] == "2" and row[1] != "sweeping" else None)(sweep_row(server)),
                                       timeout=30)
            report.check(second is not None, "media.cleanCache sweeps again and says so", repr(sweep_row(server)))

            if second is not None:
                report.equal(second[:4], ["2", "done", "asked", "2"], "asked for, and removed the two files of the sound taken out")

            report.check(not (cache / f"{later}.tpy").exists() and not (cache / f"{later}.tpk").exists(),
                         "the analysis of the sound taken out is gone")
            report.check((cache / f"{kept}.tpy").exists(), "and the other sound's is still there")

        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        report.check(" media.cleanCache" in text, "the ask is in the log as media.cleanCache")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        f"--wfg-locale={locale}")
        report.equal(code, 0, "`wfg replay` reproduces the session record for record", (out + err).strip()[-2000:])

    return report.finish()


if __name__ == "__main__":
    locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
    sys.exit(run(locale))
