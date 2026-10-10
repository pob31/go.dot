"""A processor writing cues into the show, driven from outside the process
(namespace draft §56, docs/godot-authoring-protocol-0.1.md).

THIS FILE IS THE PROCESSOR. It binds a UDP socket the way WFS-DIY or
S21_HiJack binds its receive port, declares itself from it, captures cues from
it, and reads Go.dot's answers off it - and, once a cue it wrote is fired, the
cue's own messages, since the device the cue aims at is this very socket. So
"it landed" is a datagram this file received, not a counter the engine keeps.

What it walks: the declare (made, answered `created`, the host taken from the
datagram); a capture after the standby (answered `created`), fired by GO and
heard; the same identifier captured again (answered `updated`, in place); a
capture aimed under no device (answered `unknown-id`); a locked show (answered
`locked`, and counted as a refusal); a declare again, from a new port, which
moves the device; and `wfg replay` of the session's log, which has no socket
and must make the same show.
"""
import http.server
import json
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

import common

SHOW = ('<Show><Lists goDebounce="0"><List id="7K2QM9X4" name="Cues"/></Lists><Mounts/><Network/></Show>')
LIST = "7K2QM9X4"

#  WHAT THE PROCESSOR SAYS IT ACCEPTS, served at GET /proc as WFS-DIY serves
#  GET /wfs: a value of each kind and a command that takes nothing.
DESCRIPTION = {
    "FULL_PATH": "/proc", "CONTENTS": {
        "x": {"FULL_PATH": "/proc/x", "TYPE": "i", "ACCESS": 3},
        "y": {"FULL_PATH": "/proc/y", "TYPE": "s", "ACCESS": 3},
        "go": {"FULL_PATH": "/proc/go", "ACCESS": 2},
    },
}


class Describer(http.server.BaseHTTPRequestHandler):
    """HTTP/1.0: the connection closes after the answer, which is what Go.dot's
    client reads to."""
    def do_GET(self):
        body = json.dumps(DESCRIPTION).encode("utf-8") if self.path == "/proc" else b""
        self.send_response(200 if body else 404)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def run(locale: str) -> int:
    report = common.Report(f"authoring from a processor ({locale})")

    processor = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    processor.bind(("127.0.0.1", 0))
    processor.settimeout(3.0)
    port = processor.getsockname()[1]

    describer = http.server.HTTPServer(("127.0.0.1", 0), Describer)
    query_port = describer.server_address[1]
    threading.Thread(target=describer.serve_forever, daemon=True).start()

    def receive(address, tries=20):
        """The next datagram at `address`, its arguments; anything else is skipped."""
        for _ in range(tries):
            try:
                data, _ = processor.recvfrom(65536)
            except socket.timeout:
                return None
            decoded = common.osc_decode(data)
            if decoded and decoded[0] == address:
                return decoded[1]
        return None

    with tempfile.TemporaryDirectory(prefix="wfg-authoring-") as scratch:
        room = Path(scratch)
        bundle = room / "Authoring"
        bundle.mkdir()
        (bundle / "Authoring.wfg").write_text('<Bundle formatVersion="1"/>', encoding="utf-8")
        (bundle / "show.xml").write_text(SHOW, encoding="utf-8")
        log = room / "session.wfglog"
        replayed = room / "replayed"

        with common.Server(bundle, log=log, locale=locale) as server:
            def to_godot(address, args):
                processor.sendto(common.osc_encode(address, args), ("127.0.0.1", server.osc_port))

            def read(address):
                """A node's value as the document spells it. The answer leaves at
                the end of the tick that applied the command, before the tree
                that publishes its result - so a node just made is waited for."""
                reply = common.wait_until(lambda: common.http_json(server.http_port, address))
                values = (reply or {}).get("VALUE", [])
                if not values:
                    return ""
                if isinstance(values[0], bool):
                    return "true" if values[0] else "false"
                return str(values[0])

            def settled(address, wanted):
                """The value once it is `wanted`, else the last one read: a node that
                already exists still reads its old value until the next publish."""
                common.wait_until(lambda: read(address) == wanted)
                return read(address)

            def capture(where, target, cue_id, pairs, name="Snap"):
                args = [where, target, cue_id, name, "", "", ""]
                for address, value in pairs:
                    args += [address, value]
                to_godot("/godot/cmd/cue/capture", args)
                return receive("/godot/captured")

            # --- the processor declares itself ------------------------------
            to_godot("/godot/cmd/mount/declare", ["/proc", port, query_port, "Processor"])
            declared = receive("/godot/declared")
            report.check(declared is not None and declared[1] == "created",
                         "a declare is answered `created` at the port it named", repr(declared))
            device = declared[0] if declared else ""

            report.equal(read(f"/godot/mount/{device}/prefix"), "/proc", "the device holds the processor's root")
            report.equal(read(f"/godot/mount/{device}/host"), "127.0.0.1", "its host is the datagram's")
            report.equal(read(f"/godot/mount/{device}/port"), str(port), "its port is the one declared")
            report.equal(read(f"/godot/mount/{device}/rx"), "true", "and it is heard")

            # --- its description, fetched and adopted -----------------------
            described = receive("/godot/described")
            report.check(described is not None and described[0] == device and described[1] >= 3 and described[2] == "",
                         "Go.dot fetches the description from the query port and says how many nodes it read",
                         repr(described))
            report.equal(settled(f"/godot/mount/{device}/namespace", f"namespaces/{device}.json"),
                         f"namespaces/{device}.json", "the device now names the fetched file")
            report.check((bundle / "namespaces" / f"{device}.json").is_file(), "which is in the bundle")
            status, _ = common.http_get(server.http_port, "/proc/x")
            report.equal(status, 200, "and its nodes are published under its root")

            # --- a capture after the standby, fired and heard -------------------
            answer = capture("standby", "", "", [("/proc/x", "i:3"), ("/proc/y", 's:"hello"')])
            report.check(answer is not None and answer[1] == "created" and len(answer[0]) == 8,
                         "a capture is answered `created` with the cue's identifier", repr(answer))
            cue = answer[0] if answer else ""

            report.equal(read(f"/godot/cue/{cue}/kind"), "osc", "the cue is an OSC cue")
            report.equal(read(f"/godot/cue/{cue}/address"), "/proc/x", "its own message is the first pair")
            report.equal(read(f"/godot/cue/{cue}/name"), "Snap", "and it carries the name sent")
            report.check(len(read(f"/godot/cue/{cue}/messages").split()) == 1, "the second pair is a further message")

            to_godot("/godot/cmd/standby/set", [cue])
            report.check(common.wait_until(lambda: read(f"/godot/list/{LIST}/standby") == cue) is not None,
                         "the operator parks the standby on it")
            to_godot("/godot/cmd/go", [])

            first = receive("/proc/x")
            second = receive("/proc/y")
            report.equal(first, [3], "GO sends the cue's own message to the processor")
            report.equal(second, ["hello"], "and its further message after it")

            # --- the same identifier again: updated in place -------------------
            answer = capture("standby", "", cue, [("/proc/x", "i:7")], name="Snap again")
            report.check(answer is not None and answer[1] == "updated" and answer[0] == cue,
                         "the same identifier is answered `updated`", repr(answer))
            report.equal(settled(f"/godot/cue/{cue}/value", "i:7"), "i:7", "the cue's value is the new one")
            report.equal(settled(f"/godot/cue/{cue}/messages", ""), "", "and the old further message is gone")
            report.equal(read(f"/godot/list/{LIST}/order"), cue, "still one cue in the list, where it was")

            # --- a command that takes nothing, fired by its cue's number ---------
            answer = capture("list", "", "", [("/proc/go", "")], name="Go")
            report.check(answer is not None and answer[1] == "created", "a capture of a command that takes nothing", repr(answer))
            go_cue = answer[0] if answer else ""
            to_godot("/godot/cmd/node/set", [f"/godot/cue/{go_cue}/number", "7"])
            settled(f"/godot/cue/{go_cue}/number", "7")
            to_godot("/godot/cmd/cue/fireNumber", ["7"])
            report.equal(receive("/proc/go"), [], "cue.fireNumber fires it, and it leaves as a bare message")
            report.equal(read(f"/godot/list/{LIST}/standby"), "", "and the standby did not move")

            # --- refusals, answered ----------------------------------------------
            errors_before = read("/godot/engine/errorCount")
            answer = capture("standby", "", "", [("/nowhere/x", "i:1")])
            report.check(answer is not None and answer[1] == "unknown-id",
                         "an address under no device is answered `unknown-id`", repr(answer))

            to_godot("/godot/cmd/node/set", ["/godot/document/locked", "true"])
            report.check(common.wait_until(lambda: read("/godot/document/locked") == "true") is not None,
                         "the show is locked")
            answer = capture("standby", "", "", [("/proc/x", "i:1")])
            report.check(answer is not None and answer[1] == "locked", "a locked show is answered `locked`", repr(answer))
            report.check(common.wait_until(lambda: read("/godot/engine/errorCount") != errors_before) is not None,
                         "and each refusal is counted", f"{errors_before} -> {read('/godot/engine/errorCount')}")
            to_godot("/godot/cmd/node/set", ["/godot/document/locked", "false"])
            common.wait_until(lambda: read("/godot/document/locked") == "false")

            # --- declared again from a new port: the device moves --------------
            moved = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            moved.bind(("127.0.0.1", 0))
            moved.settimeout(3.0)
            try:
                to_godot("/godot/cmd/mount/declare", ["/proc", moved.getsockname()[1], 0, "Processor"])
                data, _ = moved.recvfrom(4096)
                decoded = common.osc_decode(data) or ("", [])
                report.equal([decoded[0]] + list(decoded[1]), ["/godot/declared", device, "updated"],
                             "a second declare is answered `updated` at the new port")
                report.equal(settled(f"/godot/mount/{device}/port", str(moved.getsockname()[1])), str(moved.getsockname()[1]),
                             "and the device's port moved with it")
            except socket.timeout:
                report.check(False, "a second declare is answered at the new port")
            finally:
                moved.close()

            time.sleep(0.2)

        processor.close()
        describer.shutdown()

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        f"--wfg-locale={locale}")
        report.equal(code, 0, "`wfg replay` makes the same show with no socket and nothing drawn",
                     (out + err).strip()[-2000:])

    return report.finish()


if __name__ == "__main__":
    locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
    sys.exit(run(locale))
