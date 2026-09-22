"""The devices a show talks to, driven from outside the process.

WHAT THIS FILE IS FOR, and why it is not a unit test. Everything here is a
claim about the whole machine rather than about a class: that a device with no
description file survives the load, that retyping its port from a client
reaches the socket without reopening the show, that a cue aimed under its
prefix puts real bytes on a real wire, and that the two switches on it - `tx`
and the show's strict-senders toggle - do what the settings window says they
do. Each one crosses the document, the tick thread, the mount table, the
sender and a UDP socket; a test that held any of those still would be testing
something else.

THE FAR END IS A SOCKET THIS FILE OWNS. It binds port 0 and reads what
arrives, so "it was sent" is the datagram rather than a counter the engine
keeps about itself. The counter is checked too, beside it, because the counter
is what a tech rehearsal actually reads.

THE REFUSAL CASE IS DRIVEN OVER THE WEBSOCKET ON PURPOSE. Strict senders gates
the OSC port and deliberately not the WebSocket: a client is a client and a
device is a device, and gating clients would lock an operator out of the very
setting that locked them out. So the last section turns the filter on, proves a
datagram from an undeclared sender is dropped, and then turns it off again
through the door that is never gated - which is also the proof that the door
exists.
"""
import socket
import sys
import tempfile
import time
from pathlib import Path

import common

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
fixture = Path(next(arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--bundle=")))

report = common.Report("devices")

#  The lighting desk the fixture declares, and the cue aimed at it.
DESK = "K4QW8YSC"          # opaque: no namespace file
DESCRIBED = "K3PV7WRB"     # the one with a description
CUE = "B3N8R5TW"           # /light/go i:1
STRAY = "M5TQ7XVA"         # aimed under no device at all

desk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
desk.bind(("127.0.0.1", 0))
desk.settimeout(3.0)
desk_port = desk.getsockname()[1]

#  A WRITABLE COPY, because everything below is an EDIT: retyping a port,
#  renaming a cue, switching the filter. Run against the fixture itself, the
#  autosave would write a recovery folder into the repository - which is
#  exactly what happened the first time this file was run.
temporary = tempfile.TemporaryDirectory(prefix="godot-devices-")
bundle = common.copy_bundle(fixture, Path(temporary.name) / "show")

try:
    with common.Server(bundle, locale=locale) as server:
        def send(command, args=None):
            common.send_udp(server.osc_port,
                            common.osc_encode("/godot/cmd/" + command.replace(".", "/"),
                                              args or []))

        def read(address):
            """A node's value, as the document spells it.

            A `T` row comes back as a JSON boolean, so `str()` would answer
            "True" and no comparison against the document's own "true" would
            ever hold - a whole afternoon of green-looking checks that assert
            nothing. Normalised here, once."""
            values = common.http_json(server.http_port, address).get("VALUE", [])

            if not values:
                return ""

            if isinstance(values[0], bool):
                return "true" if values[0] else "false"

            return str(values[0])

        def settle(address, wanted, tries=40):
            """Wait for a node to reach a value. The tick is 50 Hz and a write
            travels document -> tick -> published tree, so a driver that read
            straight back would be timing the machine rather than testing it."""
            for _ in range(tries):
                if read(address) == wanted:
                    return True
                time.sleep(0.05)
            return False

        def waited(seconds=0.6):
            """Whether the desk socket stayed silent for this long."""
            desk.settimeout(seconds)
            try:
                desk.recvfrom(4096)
                return False
            except socket.timeout:
                return True

        # --- a device that describes nothing ----------------------------
        report.equal(read(f"/godot/mount/{DESK}/prefix"), "/light",
                     "a device with no description file is loaded, not refused")
        report.equal(read(f"/godot/mount/{DESK}/name"), "Lighting desk",
                     "and carries the name somebody gave it")
        report.equal(read(f"/godot/mount/{DESK}/problem"), "",
                     "with nothing wrong to report")
        report.equal(read(f"/godot/mount/{DESK}/loaded"), "true",
                     "the engine holds its declaration")
        report.equal(read(f"/godot/mount/{DESK}/nodeCount"), "0",
                     "and publishes no nodes under it, which is what opaque means")

        #  Nothing is published at its prefix. A described device is the
        #  control: the same read under /desk answers.
        status, _ = common.http_get(server.http_port, "/light")
        report.equal(status, 404, "nothing answers at an opaque device's prefix")
        status, _ = common.http_get(server.http_port, "/desk")
        report.equal(status, 200, "and the described device beside it still does")

        # --- the rows are writable, and the engine follows them ---------
        send("node.set", [f"/godot/mount/{DESK}/port", str(desk_port)])
        report.check(settle(f"/godot/mount/{DESK}/port", str(desk_port)),
                     "a client can retype the port",
                     "the mount rows were read-only until 2026-09-22")

        # --- a cue aimed under its prefix reaches the wire --------------
        send("cue.fire", [CUE])

        arrived = b""
        try:
            arrived, sender = desk.recvfrom(4096)
        except socket.timeout:
            pass

        report.check(b"/light/go" in arrived,
                     "a cue aimed at it sends, to the port that was just typed",
                     f"received {arrived!r}")

        #  AS WRITTEN. There is no declared type to coerce to, so the atom in
        #  the cue is what goes out - which is why a cue spells its own type.
        if arrived:
            address, values = common.osc_decode(arrived)
            report.equal(address, "/light/go",
                         "with the address exactly as the cue spells it")
            report.equal(values, [1],
                         "and the value as typed, uncoerced")

        report.check(settle(f"/godot/mount/{DESK}/sent", "1"),
                     "and the device's own sent count moved")

        # --- a cue aimed under no device is still refused ---------------
        send("cue.fire", [STRAY])
        time.sleep(0.4)
        report.check(waited(0.4),
                     "a cue aimed under no device sends nothing")

        # --- tx off: the show runs, the wire stays quiet ----------------
        send("node.set", [f"/godot/mount/{DESK}/tx", "false"])
        report.check(settle(f"/godot/mount/{DESK}/tx", "false"), "tx can be switched off")

        send("cue.fire", [CUE])
        report.check(waited(), "with tx off nothing leaves the machine")
        report.equal(read(f"/godot/mount/{DESK}/sent"), "1",
                     "and the sent count stands still")

        #  THIS CUE'S RUNS ONLY. The roster still holds the stray cue fired
        #  above, which failed `bad-address` and was meant to - reading every
        #  run here would fail this check for the right reason in the wrong
        #  place.
        runs = [run for run in read("/godot/run/order").split()
                if read(f"/godot/run/{run}/cue") == CUE]
        warnings = [read(f"/godot/run/{run}/warning") for run in runs]
        states = [read(f"/godot/run/{run}/state") for run in runs]

        report.check("not-sent" in warnings,
                     "the run says so in a word: not-sent",
                     f"warnings were {warnings}")
        report.check("failed" not in states,
                     "and the cue did not FAIL - a rehearsal without the desk plays",
                     f"states were {states}")

        send("node.set", [f"/godot/mount/{DESK}/tx", "true"])
        report.check(settle(f"/godot/mount/{DESK}/tx", "true"), "and it can be switched back on")

        # --- strict senders ---------------------------------------------
        report.equal(read("/godot/network/strictSenders"), "false",
                     "a show hears everybody until it is told otherwise")

        send("node.set", ["/godot/network/strictSenders", "true"])
        report.check(settle("/godot/network/strictSenders", "true"),
                     "the filter can be switched on")

        #  The fixture's device declares 127.0.0.1 with rx set, which is where
        #  this driver's own datagrams come from - so they are still heard.
        send("node.set", [f"/godot/cue/{CUE}/name", "Heard"])
        report.check(settle(f"/godot/cue/{CUE}/name", "Heard"),
                     "a declared device with rx on is still heard")

        refused_before = read("/godot/network/refused")

        #  Now take this driver off the list. Over the WEBSOCKET, which the
        #  filter does not gate: a client is not a device.
        client = common.WSClient(server.http_port)

        try:
            client.send_osc("/godot/cmd/node/set", [f"/godot/mount/{DESK}/rx", "false"])
            report.check(settle(f"/godot/mount/{DESK}/rx", "false"),
                         "the WebSocket is never gated, so a client can always reach the switch")

            send("node.set", [f"/godot/cue/{CUE}/name", "Refused"])
            time.sleep(0.6)

            report.equal(read(f"/godot/cue/{CUE}/name"), "Heard",
                         "a datagram from a sender nobody declared is dropped")
            report.check(read("/godot/network/refused") != refused_before,
                         "and it is counted rather than silently ignored",
                         f"{refused_before} -> {read('/godot/network/refused')}")

            #  And off again, through the same ungated door, so the OSC port
            #  answers for the last check and the show is left as it was.
            client.send_osc("/godot/cmd/node/set", ["/godot/network/strictSenders", "false"])
            report.check(settle("/godot/network/strictSenders", "false"),
                         "and the filter can be switched off from a client")
        finally:
            client.close()

        send("node.set", [f"/godot/cue/{CUE}/name", "Desk go"])
        report.check(settle(f"/godot/cue/{CUE}/name", "Desk go"),
                     "and with the filter off the OSC port answers again")
finally:
    desk.close()
    temporary.cleanup()

sys.exit(report.finish())
