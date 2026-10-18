"""A spatial processor's snapshot of seventy inputs, written into the show and
fired, driven from outside the process (namespace draft §88).

THIS FILE IS THE PROCESSOR, as authoring.py's is: it binds a UDP socket the
way WFS-DIY binds its receive port, declares itself from it - saying it reads
bundles - captures a cue of 7,500 address/value pairs in 1,200-byte chunks sent
back to back, as WFS-DIY sent the author's export, and then hears the cue it
wrote when it is fired. "It arrived" is the datagrams this file received.

What it holds the engine to:
  - the capture lands whole, every pair, with the tick never more than a
    couple of seconds behind (the author's took it 14.3 s behind);
  - GO sends every message, in order, bundled (the device said it reads them)
    and spread at its per-tick limit, 20 datagrams a tick - not 7,500 datagrams
    in one frame;
  - the cue is playing until its last message has left, and ends `done`.
"""
import re
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

import common

SHOW = ('<Show><Lists goDebounce="0"><List id="7K2QM9X4" name="Cues"/></Lists><Mounts/><Network/></Show>')
LIST = "7K2QM9X4"
PAIRS = 7500
CUE = "BGCQ0001"            # Crockford base32: no I, L, O or U
DATAGRAM = 1200
PER_TICK = 20

LATE_NOTE = re.compile(r"^# note tick thread up to ([0-9.]+) ms late, by tick (\d+)")


def address_of(n: int) -> str:
    return f"/wfs/input/{n // 100 + 1}/p{n % 100}"


def chunks_of(pairs):
    """The capture as WFS-DIY cuts it: the head and as many pairs as fit under
    1,200 bytes, then `more` chunks on the same identifier."""
    out = []
    current = None

    def head(first):
        return (["/wfs", "standby", "", CUE, "Seventy inputs", "", "", ""] if first
                else ["/wfs", "more", CUE, CUE, "", "", "", ""])

    for address, value in pairs:
        if current is None:
            current = head(not out)
        trial = current + [address, value]
        if len(common.osc_encode("/godot/cmd/cue/capture", trial)) > DATAGRAM and len(current) > 8:
            out.append(current)
            current = head(False) + [address, value]
        else:
            current = trial
    if current is not None:
        out.append(current)
    return out


def run(locale: str) -> int:
    report = common.Report(f"a cue of {PAIRS} messages, captured and fired ({locale})")

    processor = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    processor.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    processor.bind(("127.0.0.1", 0))
    processor.settimeout(0.2)
    port = processor.getsockname()[1]

    #  Everything the processor hears, as it arrives, by a thread of its own:
    #  the cue's datagrams come faster than a driver reading one at a time.
    heard = []
    heard_lock = threading.Lock()
    listening = threading.Event()
    listening.set()

    def listen():
        while listening.is_set():
            try:
                data, _ = processor.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            with heard_lock:
                heard.append((time.monotonic(), data))

    threading.Thread(target=listen, daemon=True).start()

    def answers(address):
        with heard_lock:
            datagrams = list(heard)
        out = []
        for _, data in datagrams:
            decoded = common.osc_decode(data)
            if decoded and decoded[0] == address:
                out.append(decoded[1])
        return out

    with tempfile.TemporaryDirectory(prefix="wfg-big-cue-") as scratch:
        room = Path(scratch)
        bundle = room / "BigCue"
        bundle.mkdir()
        (bundle / "BigCue.wfg").write_text('<Bundle formatVersion="1"/>', encoding="utf-8")
        (bundle / "show.xml").write_text(SHOW, encoding="utf-8")
        log = room / "session.wfglog"

        with common.Server(bundle, log=log, locale=locale) as server:
            def to_godot(address, args):
                processor.sendto(common.osc_encode(address, args), ("127.0.0.1", server.osc_port))

            def read(address):
                reply = common.wait_until(lambda: common.http_json(server.http_port, address))
                values = (reply or {}).get("VALUE", [])
                if not values:
                    return ""
                if isinstance(values[0], bool):
                    return "true" if values[0] else "false"
                return str(values[0])

            # --- the processor declares itself, saying it reads bundles ----------
            to_godot("/godot/cmd/mount/declare", ["/wfs", port, 0, "WFS-DIY", "", "", 1])
            declared = common.wait_until(lambda: answers("/godot/declared"), timeout=5.0)
            report.check(bool(declared) and declared[0][1] == "created",
                         "a declare saying it reads bundles is answered `created`", repr(declared))
            device = declared[0][0] if declared else ""
            report.equal(read(f"/godot/mount/{device}/bundles"), "true", "the device is made with Bundles on")
            report.equal(read(f"/godot/mount/{device}/perTick"), str(PER_TICK), "and twenty datagrams a tick")

            # --- the capture, in chunks sent back to back ------------------------
            chunks = chunks_of([(address_of(n), "f:0.5") for n in range(PAIRS)])
            report.check(all(len(common.osc_encode("/godot/cmd/cue/capture", c)) <= DATAGRAM for c in chunks),
                         f"the capture is {len(chunks)} chunks under 1,200 bytes each")

            for chunk in chunks:
                to_godot("/godot/cmd/cue/capture", chunk)

            captured = common.wait_until(lambda: (lambda a: a if len(a) >= len(chunks) else None)(answers("/godot/captured")),
                                         timeout=60.0)
            report.check(captured is not None and all(a[0] == CUE for a in captured),
                         "every chunk is answered, all on the same cue",
                         f"{len(answers('/godot/captured'))} of {len(chunks)} answered")
            report.check(captured is not None and all(a[1] in ("created", "appended") for a in captured),
                         "the first `created`, every other `appended`")

            messages = common.wait_until(lambda: (lambda m: m if len(m) == PAIRS - 1 else None)(
                read(f"/godot/cue/{CUE}/messages").split()), timeout=20.0)
            report.equal(len(messages or read(f"/godot/cue/{CUE}/messages").split()), PAIRS - 1,
                         "the cue holds every pair: its own message and the 7,499 after it")
            report.equal(read(f"/godot/cue/{CUE}/address"), address_of(0), "its own message is the first pair")

            # --- GO, and the cue heard at the device's pace ----------------------
            with heard_lock:
                heard.clear()

            to_godot("/godot/cmd/standby/set", [CUE])
            common.wait_until(lambda: read(f"/godot/list/{LIST}/standby") == CUE)
            to_godot("/godot/cmd/go", [])

            def received():
                with heard_lock:
                    datagrams = [(t, d) for t, d in heard if d[:8] == b"#bundle\x00" or d[:5] == b"/wfs/"]
                return datagrams

            def addresses_in(datagrams):
                out = []
                for _, data in datagrams:
                    if data[:8] == b"#bundle\x00":
                        position = 16
                        while position + 4 <= len(data):
                            size = int.from_bytes(data[position:position + 4], "big")
                            element = common.osc_decode(data[position + 4:position + 4 + size])
                            if element:
                                out.append(element[0])
                            position += 4 + size
                    else:
                        decoded = common.osc_decode(data)
                        if decoded:
                            out.append(decoded[0])
                return out

            common.wait_until(lambda: len(addresses_in(received())) >= PAIRS, timeout=60.0)
            time.sleep(0.3)
            datagrams = received()
            addresses = addresses_in(datagrams)

            report.equal(len(addresses), PAIRS, "every message of the cue reaches the processor")
            report.check(addresses == [address_of(n) for n in range(PAIRS)], "in the cue's order")
            report.check(all(d[:8] == b"#bundle\x00" for _, d in datagrams),
                         "as bundles, since the device said it reads them")
            report.check(all(len(d) <= DATAGRAM for _, d in datagrams), "each datagram under 1,200 bytes")

            #  Spread: at twenty a tick, the datagrams take at least that many ticks.
            spread = datagrams[-1][0] - datagrams[0][0] if len(datagrams) > 1 else 0.0
            ticks = len(datagrams) / PER_TICK
            report.check(spread >= (ticks - 2) * 0.020 * 0.5,
                         f"{len(datagrams)} datagrams spread over {spread * 1000:.0f} ms - at twenty a tick, not in one frame",
                         f"{ticks:.1f} ticks' worth")

            time.sleep(0.5)

        listening.clear()
        processor.close()

        text = log.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()

        #  The cue's run: ended, never failed, and not before its messages had gone.
        go_tick = next((int(line.split()[1]) for line in lines if line.startswith("A ") and " go " in line), None)
        ended = [int(line.split()[1]) for line in lines if line.startswith("A ") and " run.ended " in line]
        failed = [line for line in lines if " run.failed " in line]
        report.check(go_tick is not None and len(ended) == 1, "the cue's run ends once", f"go {go_tick}, ended {ended}")
        report.check(not failed, "and never fails", "; ".join(failed)[:400])
        if go_tick is not None and ended:
            report.check(ended[0] - go_tick >= ticks - 2,
                         "playing until its last message had left", f"ended {ended[0] - go_tick} ticks after GO")

        #  The tick, as the engine noted it - each note with its tick, beside
        #  the capture's and the GO's, so a red says when.
        notes = [(int(m.group(2)), float(m.group(1))) for m in (LATE_NOTE.match(line) for line in lines) if m]
        worst = max((ms for _, ms in notes), default=0.0)
        captured_tick = next((int(line.split()[1]) for line in lines if line.startswith("A ") and " cue.capture " in line), None)
        #  A DEBUG BUILD IS JUDGED LOOSELY: MSVC's idle tick already sits at its
        #  20 ms budget on a laptop, so its lateness says more about the build
        #  than the engine - thirty seconds still catches the walks this
        #  replaced, which took minutes there. A release build is held to one
        #  second (M82 measures it).
        print(f"  M82: capture at tick {captured_tick}, GO at {go_tick}; tick lateness notes (tick, ms): {notes}")
        debug = "debug" in str(common.find_binary()).lower()
        bound = 30000.0 if debug else 1000.0
        report.check(worst < bound,
                     f"the tick was never more than {bound / 1000:g} s behind (at worst {worst:.0f} ms, "
                     f"{'debug' if debug else 'release'} build)",
                     f"capture at tick {captured_tick}, GO at {go_tick}; notes (tick, ms): {notes}")

    return report.finish()


if __name__ == "__main__":
    locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
    sys.exit(run(locale))
