/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

/*  THE WRITE HALF: one WebSocket to the engine, and every edit a named
    command sent down it as binary OSC (§4.11). */

import { oscEncode, str } from "./osc.js";

let socket = null;
let socketUp = false;

/*  SERVED BY THE ENGINE, OR NOT AT ALL.

    This page is a client for something, and the file is sitting in the
    repository where double-clicking it is the obvious thing to try. Opened that
    way it has nothing to talk to - and, worse than nothing, it used to stop
    dead: `location.host` is empty for a `file:` URL, `new WebSocket("ws:///")`
    throws a TypeError before the socket is even attempted, and that throw came
    out of `openSocket()`, which was called BEFORE the poll was started. So the
    page did not fall back to reading. It sat there with "connecting" in the
    corner and three empty panes and said nothing about why, which is a quarter
    of an hour of somebody's evening.

    Now it says so, in the one place a person is looking - and since PR 5.10 it
    is the shell that says it, because a browser will not load a page's modules
    from a `file:` URL at all, so this module would never run to say anything
    (index.html). The test stays here because the socket still has to know. */
const servedByEngine = location.protocol === "http:" || location.protocol === "https:";

function openSocket() {
  if (!servedByEngine) return;

  const scheme = location.protocol === "https:" ? "wss://" : "ws://";

  /*  GUARDED, because a constructor that throws here takes the whole page with
      it - and "the address is not one a WebSocket can be opened at" is a thing
      to recover from rather than to die of. */
  try {
    socket = new WebSocket(scheme + location.host + "/");
  } catch (problem) {
    socketUp = false;
    setTimeout(openSocket, 2000);
    return;
  }

  socket.binaryType = "arraybuffer";
  socket.onopen = () => { socketUp = true; };

  /*  Reopened rather than mourned. The engine is the thing that restarts during
      a rehearsal, and a page somebody has to remember to refresh is a page that
      will be stale at the moment it matters. */
  socket.onclose = () => { socketUp = false; setTimeout(openSocket, 700); };
  socket.onerror = () => { try { socket.close(); } catch (problem) { socketUp = false; } };
}

function send(address, args) {
  if (!socket || !socketUp) return false;

  try { socket.send(oscEncode(address, args)); return true; }
  catch (problem) { return false; }
}

/** A named command (§4.11): `standby.set` is published at /godot/cmd/standby/set. */
function command(name, args) {
  return send("/godot/cmd/" + name.replace(/\./g, "/"), args || []);
}

/*  ONE VALUE, AS TEXT. `node.set` declares its value argument as '*' - whatever
    the target says - and turns whatever arrives into canonical text before the
    schema parses it against the row the address resolves to. So text is not a
    shortcut: it is the road an integer takes anyway, one step earlier, and it
    means this page never has to guess a type it could get wrong. */
function setNode(address, text) {
  return send(address, [str(text)]);
}

/*  Whether the socket is up now. A function rather than the variable, because
    an importer reads a binding it cannot see change hands across modules the
    way a script-wide `let` could be read: this is asked each time. */
function socketIsUp() {
  return socketUp;
}

export { servedByEngine, openSocket, send, command, setNode, socketIsUp };
