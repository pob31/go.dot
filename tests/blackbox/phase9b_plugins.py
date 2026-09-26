#!/usr/bin/env python3
"""Finishing plugin hosting, stage 3 - the app scans this machine for plugins.

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
(the author's decision of 2026-09-26: a Scan button in the app): a show served
with no audio and an engine folder of its own knows no plugin; `plugin.scan
lv2` launches the command line's own scan as a child, `/godot/plugin/scan/...`
says where it is, and once it is over the machine's list - read again from
known.xml - offers the in-tree test bundle's two LV2 plugins, as LV2; a locked
show refuses the scan in the log; and `wfg replay` reproduces the session,
refusals and all, with no scan launched.

THE PLUGINS ARE FOUND IN A FOLDER NAMED TO THE SCAN, `plugin.scan lv2 <folder>`:
the test bundle is built beside the tests, never on a user's LV2 path. Not
through LV2_PATH, which on Windows JUCE cannot read (it splits it at every ':',
drive letters included) and which Go.dot therefore sets aside there. The folder
comes from --lv2-folder (ctest passes the one it built) or WFG_TEST_LV2_FOLDER.
"""

import argparse
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
from common import Report, Server  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "fx"


class Hand:
    def __init__(self, server: Server):
        import socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.port = server.osc_port

    def send(self, address: str, args: "list | None" = None) -> None:
        self.sock.sendto(common.osc_encode(address, args), (common.HOST, self.port))

    def close(self) -> None:
        self.sock.close()


def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")
    if status != 200:
        return None
    try:
        return common.json.loads(body)["VALUE"][0]
    except Exception:
        return None


def wait_for(server: Server, address: str, expected, timeout: float = 20.0):
    seen = None

    def ready():
        nonlocal seen
        seen = value_of(server, address)
        return seen == expected

    common.wait_until(ready, timeout=timeout)
    return seen


def run(locale: "str | None", lv2_folder: Path) -> int:
    report = Report(f"stage 3: the app scans this machine for plugins ({locale or 'C'})")

    if not (lv2_folder / "godot-test.lv2" / "manifest.ttl").is_file():
        report.check(False, "the in-tree test LV2 bundle is built", str(lv2_folder))
        return report.finish()

    with tempfile.TemporaryDirectory(prefix="wfg-phase9b-plugins-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "fx")
        engine = room / "engine"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        with Server(bundle, log=log, locale=locale, engine_folder=engine) as server:
            hand = Hand(server)
            try:
                report.equal(wait_for(server, "/godot/plugin/scan/state", "idle"), "idle",
                             "before any scan the scan reads idle")
                report.equal(value_of(server, "/godot/plugin/known/0/name"), None,
                             "and an engine folder nobody scanned into knows no plugin")

                hand.send("/godot/cmd/plugin/scan", ["lv2", str(lv2_folder)])
                report.equal(wait_for(server, "/godot/plugin/scan/state", "finished", timeout=90.0), "finished",
                             "plugin.scan lv2 runs the scan out of process and it finishes",
                             str(value_of(server, "/godot/plugin/scan/problem")))
                report.equal(value_of(server, "/godot/plugin/scan/format"), "lv2",
                             "the scan says which format it was asked for")
                report.check((value_of(server, "/godot/plugin/scan/found") or 0) >= 2,
                             "and that the machine now knows the bundle's two plugins",
                             str(value_of(server, "/godot/plugin/scan/found")))

                names = {value_of(server, f"/godot/plugin/known/{n}/name"): n for n in range(8)}
                report.check("Go.dot test LV2 gain" in names and "Go.dot test LV2 widen" in names,
                             "the known list, read again from known.xml, offers both", str(sorted(k for k in names if k)))

                if "Go.dot test LV2 gain" in names:
                    n = names["Go.dot test LV2 gain"]
                    report.equal(value_of(server, f"/godot/plugin/known/{n}/format"), "LV2",
                                 "as LV2, the show's word")
                    report.equal(value_of(server, f"/godot/plugin/known/{n}/path"), "urn:godot:test-lv2-gain",
                                 "named by its URI")

                report.check((engine / "plugins" / "known.xml").is_file(),
                             "the scan wrote the list into the session's own engine folder, not the machine's")

                hand.send("/godot/cmd/node/set", ["/godot/document/locked", True])
                report.equal(wait_for(server, "/godot/document/locked", True), True, "the show is locked")
                hand.send("/godot/cmd/plugin/scan", [])
                common.wait_until(lambda: False, timeout=0.5)
                report.equal(value_of(server, "/godot/plugin/scan/state"), "finished",
                             "a locked show starts no scan")
            finally:
                hand.close()

        text = log.read_text(encoding="utf-8")
        report.check(any(line.startswith("R ") and "locked plugin.scan" in line for line in text.splitlines()),
                     "the refusal is in the log, locked",
                     "\n".join(line for line in text.splitlines() if "plugin.scan" in line))
        report.check(any(line.startswith("A ") and " plugin.scanned " in line for line in text.splitlines()),
                     "and the scan's end is in it, as plugin.scanned from the engine")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session with no scan launched", out + err)

    return report.finish()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wfg-locale", default=None)
    parser.add_argument("--lv2-folder", default=os.environ.get("WFG_TEST_LV2_FOLDER", ""))
    args = parser.parse_args()

    folder = Path(args.lv2_folder) if args.lv2_folder else common.find_binary().parent.parent.parent / "tests" / "lv2"
    return run(args.wfg_locale, folder)


if __name__ == "__main__":
    sys.exit(main())
