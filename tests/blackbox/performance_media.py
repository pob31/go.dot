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
"""A performance plays the show's sound: found in its own media/ first, then the show's around it.

A Show is the piece and a Performance each event of it, folded inside the
show's folder (author, 2026-10-01):

    Hamlet/media/storm.wav      the piece's sound
    Hamlet/Paris/               a performance, with no copy of it

A cue in Paris naming "storm.wav" must sound, through the one resolver every
part of the engine asks (src/wfg/engine/audio/MediaInfo.h, resolveMediaPath).
"""
import sys
import tempfile
import time
from pathlib import Path
import common
from first_sound import write_tone, read_render, frames_on_disk, RATE

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
with tempfile.TemporaryDirectory(prefix="godot-performance-") as temporary:
    show = Path(temporary) / "Hamlet"
    performance = show / "Paris"
    performance.mkdir(parents=True)
    write_tone(show / "media" / "storm.wav")
    (performance / "Paris.wfg").write_text('<Bundle formatVersion="1"/>', encoding="utf-8")
    (performance / "show.xml").write_text(
        '<Show><Lists><List id="7K2QM9X4"/></Lists><Mounts/>'
        '<Audio tracks="2"><Bus id="J3MT5XYA" width="2"/></Audio></Show>', encoding="utf-8")
    render = Path(temporary) / "render.wav"
    with common.Server(performance, hosted=True, render=render, locale=locale) as server:
        def send(command, args):
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args))

        def value(address):
            return common.http_json(server.http_port, address).get("VALUE", [None])[0]

        send("cue.create", ["7K2QM9X4", 0, "media", "Storm", "B3N8R5TW"])
        assert common.wait_until(lambda: value("/godot/cue/B3N8R5TW/kind") == "media")
        send("node.set", ["/godot/cue/B3N8R5TW/file", "storm.wav"])
        send("route.default", ["B3N8R5TW", 2])
        assert common.wait_until(lambda: value("/godot/cue/B3N8R5TW/file") == "storm.wav")
        time.sleep(0.5)
        send("cue.fire", ["B3N8R5TW"])
        before = frames_on_disk(render)
        assert common.wait_until(lambda: frames_on_disk(render) > before + RATE * 2, timeout=10)
        assert value("/godot/engine/errorCount") == 0, "the performance did not find the show's sound"

    assert not (performance / "media" / "storm.wav").exists(), "the performance should not hold a copy"
    channels, samples = read_render(render)
    assert channels == 2
    for channel in samples:
        assert sum(abs(sample) > 0.45 for sample in channel) > RATE // 2, "the show's sound was silent in the performance"
print("performance media: the show's sound plays in its performance")
