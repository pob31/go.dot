#!/usr/bin/env python3
# Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
# SPDX-License-Identifier: GPL-3.0-or-later
"""The native import command sequence must produce audible routed samples."""
import sys
import tempfile
import time
from pathlib import Path
import common
from first_sound import write_tone, read_render, frames_on_disk, RATE

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
with tempfile.TemporaryDirectory(prefix="godot-import-") as temporary:
    bundle = Path(temporary) / "show"
    bundle.mkdir()
    (bundle / "show.wfg").write_text('<Bundle formatVersion="1"/>', encoding="utf-8")
    (bundle / "show.xml").write_text(
        '<Show><Lists><List id="7K2QM9X4"/></Lists><Mounts/>'
        '<Audio tracks="2"><Bus id="J3MT5XYA" width="2"/></Audio></Show>', encoding="utf-8")
    render = Path(temporary) / "render.wav"
    log = Path(temporary) / "events.log"
    original = (bundle / "show.xml").read_text(encoding="utf-8")
    with common.Server(bundle, hosted=True, render=render, log=log, locale=locale) as server:
        def send(command, args):
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args))

        def value(address):
            return common.http_json(server.http_port, address).get("VALUE", [None])[0]

        # The file arrives after launch, just as a drag/drop import does.
        write_tone(bundle / "media" / "dropped.wav")
        send("cue.create", ["7K2QM9X4", 0, "media", "Dropped", "B3N8R5TW"])
        assert common.wait_until(lambda: value("/godot/cue/B3N8R5TW/kind") == "media")
        send("node.set", ["/godot/cue/B3N8R5TW/file", "dropped.wav"])
        send("route.default", ["B3N8R5TW", 2])
        assert common.wait_until(lambda: value("/godot/cue/B3N8R5TW/file") == "dropped.wav")
        # Give standby preparation time to react to the newly assigned file.
        time.sleep(0.5)
        send("cue.fire", ["B3N8R5TW"])
        before = frames_on_disk(render)
        assert common.wait_until(lambda: frames_on_disk(render) > before + RATE * 2, timeout=10)
        assert value("/godot/engine/errorCount") == 0

    channels, samples = read_render(render)
    assert channels == 2
    for channel in samples:
        assert sum(abs(sample) > 0.45 for sample in channel) > RATE // 2, "imported cue was silent"
    # Replay starts from the same unmodified show, not a saved final document.
    (bundle / "show.xml").write_text(original, encoding="utf-8")
    code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                  f"--out={Path(temporary) / 'replayed'}", f"--wfg-locale={locale}")
    assert code == 0, out + err
print("import playback: routed stereo audio and replay passed")
