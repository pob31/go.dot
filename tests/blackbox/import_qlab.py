#!/usr/bin/env python3
#
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

"""A QLab workspace imported by the shipped binary (namespace draft §46.3, QL.6).

WHAT THIS IS FOR. The unit suite builds workspaces into a document in memory.
What it cannot say is that `wfg import-qlab`, the verb a person or a script
runs, finds a workspace's sounds on the disk and copies them into `media/`,
writes a bundle that both opinions of the grammar accept - the engine's own
`wfg validate` and `scripts/validate-show.py`'s RELAX NG through somebody
else's validator - imports the same workspace to the same bytes twice (ZY),
refuses a folder already holding a show untouched, refuses a QLab version it
does not read, and writes the report beside the show (ZZ).

THE WORKSPACE IS WRITTEN HERE, with Python's own plistlib, in the shape QLab 5
writes (§46.5) - an outer keyed archive of settings holding the cues' archive
as data - into a folder whose name is not ASCII, beside a stereo WAV made here
too: nothing of a real production is committed (ZQ), and an accented path is
what a French show folder is. Its one script cue is not imported, so the exit
code is 1, "imported, with something to read".

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import math
import plistlib
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
VALIDATE_SHOW = REPO_ROOT / "scripts" / "validate-show.py"


class Keyed:
    """What NSKeyedArchiver writes: `$objects`, each archived object naming its
    class through `$class`, fields pointing at one another by UID."""

    def __init__(self) -> None:
        self.objects: list = ["$null"]
        self.classes: dict = {}

    def add(self, value) -> plistlib.UID:
        self.objects.append(value)
        return plistlib.UID(len(self.objects) - 1)

    def object(self, class_name: str, **fields) -> plistlib.UID:
        if class_name not in self.classes:
            self.classes[class_name] = self.add({"$classname": class_name, "$classes": [class_name, "NSObject"]})
        return self.add({"$class": self.classes[class_name], **fields})

    def string(self, text: str) -> plistlib.UID:
        return self.object("NSString", **{"NS.string": text})

    def array(self, uids: list) -> plistlib.UID:
        return self.object("NSMutableArray", **{"NS.objects": list(uids)})

    def dictionary(self, pairs: dict) -> plistlib.UID:
        keys = [self.add(k) for k in pairs]
        return self.object("NSMutableDictionary", **{"NS.keys": keys, "NS.objects": list(pairs.values())})

    def archive(self, root: plistlib.UID) -> bytes:
        return plistlib.dumps({"$version": 100000, "$archiver": "NSKeyedArchiver", "$top": {"root": root},
                               "$objects": self.objects}, fmt=plistlib.FMT_BINARY, sort_keys=False)


def workspace_bytes(version: str = "5.6.3") -> bytes:
    """A small QLab 5 workspace: one list, a scene of a playlist of two messages,
    a timeline of a sound and its fade, an OSC fade, a script cue."""
    cues = Keyed()
    made = [0]

    def cue(class_name: str, name: str, **fields) -> plistlib.UID:
        made[0] += 1
        base = {"uniqueID": f"BB-{made[0]:04d}", "armed": True, "preWait": 0.0, "postWait": 0.0, "continueMode": 0}
        if name:
            base["name"] = cues.string(name)
        base.update(fields)
        return cues.object(class_name, **base)

    def group(name: str, mode: int, members: list) -> plistlib.UID:
        return cue("GroupCue", name, groupMode=mode, cues=cues.array(members))

    def matrix(cells: list) -> plistlib.UID:
        entries = {}
        for row, column, db in cells:
            entries[str(row * 1025 + column)] = cues.object("AudioLevelKnobs", row=row, column=column,
                                                            initialLevel=0.0 if db is None else 10 ** (db / 20),
                                                            trimLevel=1.0)
        return cues.object("AudioLevelMatrix", rows=1025, columns=1025, entries=cues.dictionary(entries))

    def shape(kind: int) -> plistlib.UID:
        a = cues.object("FadeShapeEntry", t=0.0, v=0.0)
        b = cues.object("FadeShapeEntry", t=1.0, v=1.0)
        return cues.object("FadeShape", type=kind, curveParameter=1.0, shapeEntries=cues.array([a, b]))

    mute = cue("OSCCue", "UnMute CG Monitors", oscString="/channel/118/mute \\F", networkPatchID="PATCH-S21")
    place = cue("OSCCue", "", oscString="/wfs/input/21/positionXYZ -3.0 1.0 3.", networkPatchID="PATCH-WFS")
    doors = group("Doors open", 6, [mute, place])

    alias = cues.object("F53Alias", lastKnownPath="/Volumes/Gone/audio/rain.wav", relativePath="audio/rain.wav")
    sound = cue("AudioCue", "", fileTarget=alias, audioOutputPatchID="PATCH-AUDIO", rate=1.0, doPitchShift=True,
                startTime=0.0, endTime=1.0, playCount=1, infiniteLoop=True,
                levels=matrix([(0, 0, None), (0, 1, 0.0), (0, 2, 0.0), (1, 0, 0.0), (1, 1, 0.0),
                               (2, 0, 0.0), (2, 2, -6.0)]))
    entry = cues.object("FadeValueEntry", row=0, column=0, startValue=1e-4, endValue=10 ** (-30 / 20))
    fade_object = cues.object("Fade", fadeMode=1, fadeType=1, duration=2.0,
                              entries=cues.dictionary({"0": entry}),
                              shapes=cues.object("FadeShapesFunction", upShape=shape(1)))
    fade = cue("FadeCue", "", cueTargetUniqueID="BB-0004", duration=2.0, stopTargetWhenDone=False, fade=fade_object)
    rain = group("Rain", 3, [sound, fade])

    fader_fade = cues.object("Fade", shapes=cues.object("FadeShapesFunction", upShape=shape(3)))
    fader = cue("OSCCue", "CG QLab Fader -18 > 0dB", oscString="/channel/114/fader #v#", networkPatchID="PATCH-S21",
                fadeType=1, startValue=-18.0, endValue=0.0, duration=5.0, fadingFloats=True, fade=fader_fade)
    script = cue("ScriptCue", "a script", source="tell application id \"com.figure53.QLab.5\"")

    scene = group("1 - Doors", 1, [doors, rain, fader, script])
    the_list = group("Main", 0, [scene])
    root = cues.object("GroupCue", groupMode=3, cues=cues.array([the_list]))
    inner = cues.archive(root)

    outer = Keyed()
    names = outer.dictionary({"1": "FOH L", "2": "FOH R"})
    audio_patch = outer.object("AudioOutputPatch", uniqueID="PATCH-AUDIO", name="Desk", cueOutputChannels=2,
                               cueOutputNames=names)

    def destination(identifier: str, name: str, host: str, port: int) -> plistlib.UID:
        state = outer.dictionary({"host": host, "port": port, "useTcp": False})
        data = outer.dictionary({"uniqueID": identifier, "name": name, "deviceIdentifier": "com.figure53.oscmessage",
                                 "clientStates": outer.array([state])})
        return outer.dictionary({"data": data})

    settings = outer.dictionary({
        "Audio": outer.dictionary({"audioOutputPatches": outer.array([audio_patch]), "minVolume": -80.0}),
        "Network": outer.dictionary({"networkPatches": outer.array([
            destination("PATCH-WFS", "WFS", "192.168.1.32", 8051),
            destination("PATCH-S21", "S21", "192.168.1.221", 8024)])}),
    })
    top = outer.dictionary({"QLabShortVersionString": version, "QLabBuildNumber": 5603, "workspaceName": "Doors",
                            "settings": settings,
                            "cueLists": outer.object("NSMutableData", **{"NS.data": inner})})
    return outer.archive(top)


def write_wav(path: Path) -> None:
    """A second of a stereo sine, sixteen bits at 48 kHz."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(48000)
        frames = bytearray()
        for n in range(48000):
            sample = int(8000 * math.sin(2 * math.pi * 440 * n / 48000))
            frames += struct.pack("<hh", sample, sample)
        out.writeframes(bytes(frames))


def run(*args: str) -> "tuple[int, str]":
    """The binary once, its output read as the UTF-8 it writes."""
    done = subprocess.run([str(common.find_binary()), *args], capture_output=True,
                          encoding="utf-8", errors="replace", timeout=120)
    return done.returncode, done.stdout + done.stderr


def main(argv: "list[str]") -> int:
    locale = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    report = Report(f"a QLab workspace, imported ({locale or 'C'})")
    with_locale = [f"--wfg-locale={locale}"] if locale else []

    try:
        with tempfile.TemporaryDirectory(prefix="wfg-qlab-") as scratch:
            room = Path(scratch)
            show_folder = room / "Histoire(s) du Théâtre"
            show_folder.mkdir()
            workspace = show_folder / "Doors.qlab5"
            workspace.write_bytes(workspace_bytes())
            write_wav(show_folder / "audio" / "rain.wav")

            first, second = room / "Doors", room / "Again"

            code, said = run("import-qlab", str(workspace), f"--into={first}", *with_locale)
            report.equal(code, 1, "the workspace imports, with something in its report to read", said.strip()[-300:])
            report.check("1 cue list(s), 9 cue(s)" in said, "one list and nine cues", said.strip()[-300:])
            report.check("0 sound(s) not found" in said, "its sound found beside it, through an accented folder",
                         said.strip()[-300:])

            for name in ("Doors.wfg", "show.xml", "state.xml", "import-report.md", "media/rain.wav"):
                report.check((first / name).exists(), f"the folder holds {name}")

            code, said = run("validate", str(first), *with_locale)
            report.check(code == 0 and "is valid" in said, "the engine opens the show it wrote", said.strip()[-300:])

            done = subprocess.run([sys.executable, str(VALIDATE_SHOW), str(first)], capture_output=True,
                                  encoding="utf-8", errors="replace", timeout=120)
            report.equal(done.returncode, 0, "and somebody else's validator accepts it against show.rng",
                         (done.stdout + done.stderr).strip()[-300:])

            show = (first / "show.xml").read_text(encoding="utf-8")
            for words, what in (('advance="auto"', "the playlist an automatic group"),
                                ('value="f:-3 f:1 f:3"', "a message of three values"),
                                ('<Mount', "a device for each patch"),
                                ('prefix="/channel"', "the S21 under /channel"),
                                ('mode="timeline"', "the timeline"),
                                ('channels="2"', "the sound's channels read from its file"),
                                ('gains="1 0"', "input one routed to output one"),
                                ('loops="0"', "the infinite loop a range for ever"),
                                ('<Fade', "the fade"),
                                ('curve="sCurve"', "QLab's S-curve"),
                                ('points="0 -18 5 0"', "the #v# fade a curve"),
                                ('[QLab] a script', "the script a memo in its place")):
                report.check(words in show, f"the show holds {what}")

            # The same workspace, again: the same show, byte for byte (ZY).
            run("import-qlab", str(workspace), f"--into={second}", "--no-media", *with_locale)
            report.check((first / "show.xml").read_bytes() == (second / "show.xml").read_bytes(),
                         "the same workspace imports to the same show.xml twice")

            # A folder that holds a show is refused, and left as it was.
            before = (first / "show.xml").read_bytes()
            code, said = run("import-qlab", str(workspace), f"--into={first}", *with_locale)
            report.equal(code, 2, "a folder already holding a show is refused", said.strip()[-200:])
            report.check((first / "show.xml").read_bytes() == before, "and its show is untouched")

            # A QLab it does not read is refused in words (ZR).
            old = show_folder / "Old.qlab5"
            old.write_bytes(workspace_bytes("3.2.14"))
            code, said = run("import-qlab", str(old), f"--into={room / 'Old'}", *with_locale)
            report.equal(code, 2, "a QLab 3 workspace is refused", said.strip()[-200:])
            report.check("QLab 4 and QLab 5" in said, "and says which it reads", said.strip()[-200:])

            # The report says what the import could not carry.
            text = (first / "import-report.md").read_text(encoding="utf-8")
            for words in ("## The outputs", "FOH L", "## The devices", "192.168.1.221:8024", "## Not imported",
                          "a script is never run"):
                report.check(words in text, f"the report says \"{words}\"")
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2

    return report.finish()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
