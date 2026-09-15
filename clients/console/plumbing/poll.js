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

/*  THE READ HALF: the poll of `GET /godot`, one in flight at a time, and the
    link readout that says what came of it. It draws through `view.render`,
    the one door every render goes through (views/view.js). */

import { flatten, tree } from "./tree.js";
import { socketIsUp } from "./link.js";
import { el } from "../views/common.js";
import { view } from "../views/view.js";
import { table } from "../gestures/table.js";

const POLL_MS = 100;               // at most this often; `poll` says why it can be less

let failures = 0;
let heard = performance.now();     // when the engine last answered, or the page opened

function link(state, text) {
  el("link").className = state;
  el("link-text").textContent = text;
}

/*  LIVE, OR READING ONLY, and why. The page writes by two things together: a
    socket that is up, and the gesture table that says which command each key
    and click sends (gestures/commands.json). Without either, the tree is
    still read and drawn and nothing a gesture would send goes anywhere - which
    is "reading only" whichever one is missing, and the words say which. Said
    on every poll, so it cannot be drawn over by the next one. */
function liveWords() {
  if (!socketIsUp()) return ["half", "reading only"];
  if (table.problem) return ["half", "reading only · " + table.problem];
  return ["up", "live"];
}

/*  ONE POLL IN FLIGHT, and the next one asked for only when it is over.

    The page used to start a poll every hundred milliseconds with
    `setInterval`, whether or not the last one had answered. On a small show
    nobody could tell: the tree came back in a few milliseconds. On a 500-cue
    show `GET /godot` is 8.6 MB, which a Debug engine takes about a second to
    send, so requests were asked for ten times faster than they were answered
    and queued in the browser - about 1350 were counted unanswered at once -
    and past the browser's cap every new one failed the moment it was made. In
    four measured runs of seven the page drew once in four minutes, or never:
    a show of a real size could not be looked at, and the poll was why. A
    Release engine sends the same tree in under a tenth of a second, which is
    inside the interval, but only just - a busier machine, a slower tablet or
    a longer show, and it would not be.

    So each poll asks for the next when it ends, answered or failed: after
    what is left of a hundred milliseconds since it began, or at once if it
    took longer than that. The cadence is at most ten a second and never
    faster than the engine can answer, which is what a poll of the whole tree
    can honestly promise. What a 500-cue tree costs to serve is recorded
    beside M24's answer (namespace draft §14.14).

    STILL WAITING IS SAID OUT LOUD TOO, and the chain is why it has to be.
    With one poll in flight, a request the engine never answers - a process
    stopped in a debugger, a network gone quiet without saying so - is a page
    that asks nothing more and draws nothing more, and it would go on saying
    "live" over a view that had frozen: the one thing §3.17 says a client must
    never do. So while a poll waits for its answer, the link counts the
    seconds, from two: about twice what a Debug engine takes to answer for the
    500-cue tree, so that an ordinary slow answer is never taken for silence.
    The request is not abandoned at a deadline: a slow answer is still an
    answer, and a page that gave up after so many seconds would never draw a
    show that took longer than that to serve.

    THE ANSWER IS THE HEADERS, NOT THE LAST BYTE. The engine builds the whole
    reply before it writes any of it, then writes the headers and the body
    together, so when `fetch` resolves the engine has already answered in
    full. What comes after is the network and this page. Counting silence up
    to the last byte meant that a working engine on a slow link - a tablet
    across the room, an 8.6 MB tree - was reported in red as not answering,
    on every poll. So silence is counted only until the headers arrive. A
    body that is slow to arrive is reported as exactly that, without the red
    of an engine that has gone, and with the seconds beside it, so that a
    transfer stalled halfway shows as a count that keeps climbing. */
async function poll() {
  const began = performance.now();
  let spoke = null;                // when this poll's headers came: the engine had answered

  const waiting = setInterval(() => {
    const now = performance.now();

    if (spoke === null) {
      const quiet = Math.floor((now - Math.max(heard, began)) / 1000);

      if (quiet >= 2) link("down", "no answer for " + quiet + " s");
      return;
    }

    const arriving = Math.floor((now - spoke) / 1000);

    if (arriving >= 2) {
      const [state, words] = liveWords();
      link(state, words + " · receiving the tree, " + arriving + " s");
    }
  }, 1000);

  try {
    const reply = await fetch("/godot", { cache: "no-store" });

    spoke = heard = performance.now();

    if (!reply.ok) throw new Error("the engine answered " + reply.status);

    tree.at = flatten(await reply.json(), {});
    failures = 0;

    /*  TWO STATES OF LIVE, because they are different capabilities and a page
        that cannot write looks exactly like one that can until somebody tries.
        The tree is read over HTTP and every edit goes out over the socket, so
        a socket that is down is a page that reads perfectly and changes
        nothing - which is worth saying before a note goes missing.

        AND SAID ONLY ONCE THE TREE IS DRAWN. A render that throws leaves
        every pane after the one that threw as it was - the running pane's
        kill buttons included - so it is said on the link and written to the
        console, apart from the engine's failures: the engine did answer, and
        counting a drawing that failed as a poll that failed would say
        "not connected" about an engine that is, three polls late. */
    try {
      view.render();
      link(...liveWords());
    } catch (problem) {
      console.error(problem);
      link("down", "drawing failed: " +
                   (problem && problem.message ? problem.message : String(problem)));
    }
  } catch (problem) {
    failures += 1;

    /*  STALE IS SAID OUT LOUD rather than left to be inferred from numbers that
        stopped moving. §3.17 asks for exactly this of the tablet, and the
        reason is the same here: a view that quietly freezes is worse than one
        that has gone, because somebody will act on it. And the two reasons it
        can be stale are told apart. A 404 means something answered and it was
        not Go.dot - a static server somebody pointed at this folder - and that
        is a different mistake from an engine that has gone. */
    if (failures > 2) {
      link("down", /404/.test(problem.message)
                     ? "no engine here (something else is serving this page)"
                     : "not connected (" + problem.message + ")");
    }
  } finally {
    clearInterval(waiting);

    /*  The next one, whatever became of this one: a failure that did not ask
        again would be a page that stopped trying. */
    setTimeout(poll, Math.max(0, POLL_MS - (performance.now() - began)));
  }
}

export { link, poll };
