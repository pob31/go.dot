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

"""The machine's pairing, driven from outside the process (namespace draft 58,
RD.4; AGC, AFV).

WHAT THIS FILE IS FOR. The unit suite holds the file, the commands and the
rows each on its own. What none of it can prove is the shipped binary end to
end: that `wfg serve --role=backup --peer=... --link-timeout=...` publishes
what it was told without writing it anywhere, that a command sent over OSC
writes `pairing.xml` in Go.dot's own folder - beside the engine folder, which
`--engine-folder` moves - off the tick thread, with the file's own values for
what the command line overrode, and that the next serve on that folder reads
it back, a fraction of a second included, in either locale. And that a role
nobody knows stops the start with a sentence rather than starting unpaired.

NEVER THE REAL FOLDER: every serve here is given `--engine-folder` inside a
temporary directory, and its pairing.xml is that folder's sibling there.
"""
import sys
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import common
from common import Report, Server

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "minimal"

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


def wait_for(predicate, seconds=5.0):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def reader(server):
    def read(address):
        """A node's first value, "" for none; a flag as true or false."""
        try:
            values = common.http_json(server.http_port, address).get("VALUE", [])
        except common.HarnessError:
            return ""
        if not values:
            return ""
        if isinstance(values[0], bool):
            return "true" if values[0] else "false"
        return values[0]
    return read


def sender(server):
    def send(command, args=None):
        common.send_udp(server.osc_port,
                        common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))
    return send


def file_says(path: Path):
    """pairing.xml's three attributes, or None while it is not there or not whole."""
    try:
        root = ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError):
        return None
    if root.tag != "Pairing":
        return None
    return {key: root.get(key) for key in ("role", "peers", "timeout")}


def run() -> int:
    report = Report(f"redundancy: the machine's pairing ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-pairing-") as scratch:
        scratch = Path(scratch)
        machine = scratch / "machine"            # Go.dot's own folder, on this test's machine
        engine_folder = machine / "engine"       # what --engine-folder names
        engine_folder.mkdir(parents=True)
        pairing = machine / "pairing.xml"        # the engine folder's sibling

        # --- a backup started with every override, and no file -----------------
        bundle = common.copy_bundle(FIXTURE, scratch / "first")
        log = scratch / "first.wfglog"
        with Server(bundle, log=log, locale=locale, engine_folder=engine_folder,
                    extra_args=["--role=backup", "--peer=127.0.0.1:5999", "--link-timeout=2.5"]) as server:
            read, send = reader(server), sender(server)

            report.equal(read("/godot/pair/role"), "backup", "the role on the command line is published")
            report.equal(read("/godot/pair/peers"), "127.0.0.1:5999", "and the peer")
            report.equal(read("/godot/pair/timeout"), 2.5, "and the link's timeout, 2.5 s")
            report.equal(read("/godot/pair/sending"), "true",
                         "a backup alone does not hold yet - only --hold does until RD.6")
            report.check(any("paired as backup" in notice for notice in server.notices),
                         "serve says how it is paired as it starts", f"notices {server.notices}")
            report.check(not pairing.exists(), "the overrides are not written to pairing.xml")

            send("pair.setRole", ["boss"])
            report.check(wait_for(lambda: "bad-value pair.setRole" in str(read("/godot/engine/lastError"))),
                         "a role nobody knows is refused bad-value",
                         f"lastError {read('/godot/engine/lastError')!r}")
            report.equal(read("/godot/pair/role"), "backup", "and the role is as it was")

            send("pair.setRole", ["main"])
            report.check(wait_for(lambda: read("/godot/pair/role") == "main"),
                         "pair.setRole main over OSC changes the role in force", f"role {read('/godot/pair/role')!r}")
            report.check(wait_for(lambda: (file_says(pairing) or {}).get("role") == "main"),
                         "and writes pairing.xml beside the engine folder", f"file {file_says(pairing)}")

            written = file_says(pairing) or {}
            report.equal(written.get("peers"), "", "the file keeps its own peers, never the --peer override")
            report.equal(written.get("timeout"), "1", "and its own timeout, never --link-timeout's")
            report.equal(read("/godot/pair/peers"), "127.0.0.1:5999", "while the overrides stay in force this run")
            report.check("write-failed" not in str(read("/godot/engine/lastError")),
                         "no write failed", f"lastError {read('/godot/engine/lastError')!r}")

        #  THE SESSION'S OWN WORD for the write, and a replay that reproduces it:
        #  the commands are registered where a log is read back, and the file is
        #  the machine's, so the replay writes none and applies the engine's
        #  `pair.saved` as the session heard it.
        report.equal(common.logged_before(log, "pair.saved", containing='s:""'), 1,
                     "the write is answered by one pair.saved, with no error")
        report.equal(common.logged_before(log, "pair.setRole"), 2, "both pair.setRole records are in the log")
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={scratch / 'replayed'}",
                                        f"--wfg-locale={locale}")
        report.equal(code, 0, "`wfg replay` reproduces the session, the pairing commands included",
                     (out + err).strip()[-2000:])

        # --- the same folder again, no overrides: the file is read ---------------
        # role=None: no --role on the line, which a Server otherwise adds (RD.5).
        bundle = common.copy_bundle(FIXTURE, scratch / "second")
        with Server(bundle, locale=locale, engine_folder=engine_folder, role=None) as server:
            read, send = reader(server), sender(server)

            report.equal(read("/godot/pair/role"), "main", "a second serve on the folder reads the role back")
            report.equal(read("/godot/pair/peers"), "", "and no peer")
            report.equal(read("/godot/pair/timeout"), 1, "and the default timeout")

            send("pair.setPeers", ["10.0.0.2 10.1.0.2:6010"])
            send("pair.setTimeout", [1.5])
            report.check(wait_for(lambda: (file_says(pairing) or {}).get("timeout") == "1.5"
                                  and (file_says(pairing) or {}).get("peers") == "10.0.0.2:5010 10.1.0.2:6010"),
                         "two peers and 1.5 s written, a port given where it was left off, a point and never a comma",
                         f"file {file_says(pairing)}")
            report.equal(read("/godot/pair/peers"), "10.0.0.2:5010 10.1.0.2:6010", "the peers published as written")

        # --- and a third reads the fraction back ----------------------------------
        bundle = common.copy_bundle(FIXTURE, scratch / "third")
        with Server(bundle, locale=locale, engine_folder=engine_folder, role=None) as server:
            read = reader(server)
            report.equal(read("/godot/pair/timeout"), 1.5, "a third serve reads 1.5 s back exactly")
            report.equal(read("/godot/pair/peers"), "10.0.0.2:5010 10.1.0.2:6010", "and both peers")
            report.equal(read("/godot/pair/role"), "main", "and the role")

        # --- a value typed wrong stops the start ----------------------------------
        for option, word in (("--role=boss", "--role"), ("--peer=10.0.0.2:http", "--peer"),
                             ("--link-timeout=2,5", "--link-timeout")):
            code, _, err = common.run_wfg("serve", str(bundle), "--sample-rate=48000", "--buffer=128",
                                          "--http-port=0", "--osc-port=0", f"--engine-folder={engine_folder}",
                                          f"--wfg-locale={locale}", option)
            report.equal(code, 2, f"{option} stops the start")
            report.check(word in err, f"{option}: and says which option", f"stderr {err.strip()!r}")

        report.equal((file_says(pairing) or {}).get("role"), "main", "none of it touched the file")

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
