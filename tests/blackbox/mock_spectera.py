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

"""A Sennheiser Spectera base station, as far as SSCv2 goes (namespace draft
§124, BET; WR.5).

Written from Sennheiser's "Sennheiser Sound Control (SSCv2) Specification"
2.3 and the Spectera OpenAPI 18.3, and nothing of Go.dot's: HTTPS only, with a
self-signed certificate (tests/fixtures/certs), HTTP/1.1 kept alive, Basic
authentication as `api` - the third-party user - and 401 without it. `GET /api/ssc/state/subscriptions`
answers a Server-Sent Events stream, chunked, whose first event is `open`
with the session's path and `sessionUUID`; `PUT .../{sessionUUID}/add` takes a
JSON array of resource paths and reports each one's values at once; after
that every change is an event keyed by the resource's path. `PUT` of a
resource takes a partial object - transactional: one unknown field and nothing
changes, 400 - and answers the resource as it now is; as the OpenAPI's
PutSchemas require, it must carry the element's identifier, and a pack's
its `type` (SEK or SKM, the schema's discriminator), or it is 400 too. The
site has no PUT (405). A path it lacks is 404.

A driver scripts it over plain HTTP on a second port, `{"VALUE": [...]}`:

    /_mock/requests        every request taken, "METHOD path body", in order -
                           authorised or not, so a driver can see none came
    /_mock/state?/api/mts/paired/all/7   a resource as the base station holds it
    /_mock/hand?/api/mts/paired/all/7/batteryFillLevel=20   a change nobody's
                           request made, told to every subscriber
    /_mock/subscribers     the subscriptions open
    /_mock/close           ends every subscription with a `close` event

The ports go out on one line, `osc <https> query <http>`. Stdlib only.
"""

import json
import queue
import ssl
import sys
import threading
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HOST = "127.0.0.1"
USER = "api"
PASSWORD = "mockpass"
CERTS = Path(__file__).resolve().parent.parent / "fixtures" / "certs"


def initial():
    return {
        "/api/device/identity": {"product": "Spectera Base Station", "hardwareRevision": "B", "serial": "1234567890",
                                 "vendor": "Sennheiser"},
        "/api/device/state": {"state": "Normal", "warnings": []},
        "/api/device/site": {"deviceName": "Spectera", "location": "", "position": ""},
        "/api/rf/channels/0": {"rfChannelId": 0, "frequency": 602000, "txPower": 50, "bandwidthMode": 8000,
                               "rfState": "RfActive"},
        "/api/rf/channels/1": {"rfChannelId": 1, "frequency": 610000, "txPower": 50, "bandwidthMode": 8000,
                               "rfState": "RfActive"},
        "/api/mts/paired/all/7": {"mtUid": 7, "type": "SKM", "name": "Lead", "state": "Connected",
                                  "batteryFillLevel": 90, "batteryRuntime": 420, "batteryLow": False, "rssi": -48,
                                  "sleep": False, "micPreampGain": 30},
        "/api/mts/paired/all/9": {"mtUid": 9, "type": "SEK", "name": "Drums", "state": "Connected",
                                  "batteryFillLevel": 75, "batteryRuntime": 360, "batteryLow": False, "rssi": -52,
                                  "sleep": False, "headphoneVolume": 12},
    }


COLLECTIONS = ("/api/rf/channels", "/api/mts/paired/all")
IDS = {"/api/rf/channels": "rfChannelId", "/api/mts/paired/all": "mtUid"}


class Station:
    def __init__(self):
        self.lock = threading.Lock()
        self.resources = initial()
        self.requests = []
        self.sessions = {}          # uuid -> {"queue": Queue, "paths": set}

    def value_of(self, path):
        if path in self.resources:
            return self.resources[path]
        if path in COLLECTIONS:
            return [v for k, v in sorted(self.resources.items()) if k.startswith(path + "/")]
        return None

    def watched_by(self, session, path):
        return any(path == p or path.startswith(p + "/") for p in session["paths"])

    def tell(self, path):
        for session in self.sessions.values():
            if self.watched_by(session, path):
                session["queue"].put(("message", {path: self.resources[path]}))


def handler_for(station):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def body(self):
            length = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(length).decode("utf-8") if length else ""

        def answer(self, status, payload=None):
            data = b"" if payload is None else json.dumps(payload, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def authorised(self):
            expected = "Basic " + __import__("base64").b64encode(f"{USER}:{PASSWORD}".encode()).decode()
            if self.headers.get("Authorization") == expected:
                return True
            self.answer(401, {"error": "unauthorized"})
            return False

        def chunk(self, text):
            data = text.encode("utf-8")
            self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
            self.wfile.flush()

        def stream(self):
            session_id = str(uuid.uuid4())
            session = {"queue": queue.Queue(), "paths": set()}
            with station.lock:
                station.sessions[session_id] = session
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Location", "/api/ssc/state/subscriptions/" + session_id)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            try:
                self.chunk("event: open\ndata: " + json.dumps(
                    {"path": "/api/ssc/state/subscriptions/" + session_id, "sessionUUID": session_id}) + "\n\n")
                while True:
                    try:
                        name, data = session["queue"].get(timeout=1.0)
                    except queue.Empty:
                        continue
                    self.chunk(("event: %s\n" % name if name != "message" else "")
                               + "data: " + json.dumps(data, separators=(",", ":")) + "\n\n")
                    if name == "close":
                        self.chunk("")
                        break
            except OSError:
                pass
            finally:
                with station.lock:
                    station.sessions.pop(session_id, None)
                self.close_connection = True

        def do_GET(self):  # noqa: N802
            with station.lock:
                station.requests.append("GET " + self.path)
            if not self.authorised():
                return
            if self.path == "/api/ssc/state/subscriptions":
                return self.stream()
            with station.lock:
                value = station.value_of(self.path)
            self.answer(404 if value is None else 200, value)

        def do_PUT(self):  # noqa: N802
            text = self.body()
            with station.lock:
                station.requests.append("PUT " + self.path + " " + text)
            if not self.authorised():
                return
            try:
                payload = json.loads(text) if text else None
            except ValueError:
                return self.answer(400, {"error": "not JSON"})

            if self.path.startswith("/api/ssc/state/subscriptions/") and self.path.endswith("/add"):
                session_id = self.path.split("/")[-2]
                with station.lock:
                    session = station.sessions.get(session_id)
                    if session is None:
                        return self.answer(404, {"error": "no such session"})
                    for path in payload or []:
                        value = station.value_of(path)
                        if value is None:
                            continue
                        session["paths"].add(path)
                        session["queue"].put(("message", {path: value}))
                return self.answer(200)

            with station.lock:
                resource = station.resources.get(self.path)
                if resource is None:
                    return self.answer(404, {"error": "not found"})
                if not isinstance(payload, dict) or any(key not in resource for key in payload):
                    return self.answer(400, {"error": "unknown field"})
                if self.path == "/api/device/site":
                    return self.answer(405, {"error": "the site is read-only"})
                for collection, field in IDS.items():
                    if self.path.startswith(collection + "/") and payload.get(field) != resource.get(field):
                        return self.answer(400, {"error": "missing or wrong " + field})
                if self.path.startswith("/api/mts/paired/all/") and payload.get("type") != resource.get("type"):
                    return self.answer(400, {"error": "type picks the schema: SEK or SKM"})
                resource.update(payload)
                station.tell(self.path)
                return self.answer(200, resource)

    return Handler


def control_for(station, fingerprint):
    class Control(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, values):
            data = json.dumps({"VALUE": values}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):  # noqa: N802
            path, _, query = self.path.partition("?")
            query = urllib.parse.unquote(query)
            with station.lock:
                if path == "/_mock/requests":
                    return self.reply(list(station.requests))
                if path == "/_mock/state":
                    value = station.value_of(query)
                    return self.reply([] if value is None else [value])
                if path == "/_mock/subscribers":
                    return self.reply([len(station.sessions)])
                if path == "/_mock/fingerprint":
                    return self.reply([fingerprint])
                if path == "/_mock/hand":
                    where, _, text = query.partition("=")
                    resource, _, field = where.rpartition("/")
                    station.resources[resource][field] = json.loads(text)
                    station.tell(resource)
                    return self.reply([where])
                if path == "/_mock/close":
                    for session in station.sessions.values():
                        session["queue"].put(("close", {}))
                    return self.reply([len(station.sessions)])
            self.send_response(404)
            self.end_headers()

    return Control


def main() -> int:
    station = Station()
    secure = ThreadingHTTPServer((HOST, 0), handler_for(station))
    secure.daemon_threads = True
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(str(CERTS / "spectera-mock.crt"), str(CERTS / "spectera-mock.key"))
    secure.socket = context.wrap_socket(secure.socket, server_side=True)

    der = ssl.PEM_cert_to_DER_cert((CERTS / "spectera-mock.crt").read_text(encoding="ascii"))
    digest = __import__("hashlib").sha256(der).hexdigest().upper()
    fingerprint = ":".join(digest[i:i + 2] for i in range(0, len(digest), 2))

    control = ThreadingHTTPServer((HOST, 0), control_for(station, fingerprint))
    control.daemon_threads = True
    threading.Thread(target=secure.serve_forever, daemon=True).start()
    print("osc %d query %d" % (secure.server_address[1], control.server_address[1]), flush=True)
    control.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
