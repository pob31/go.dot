# Go.dot — An MCP server: notes before the work starts

**Draft 0.1**, 2026-11-24. Written from a conversation with the author and a read of three
checkouts that day: Go.dot at `c3e0263a`, WFS-DIY at `v1.0.0beta68-4-g035fc8dc`, spatcore at
`v0.4.0-50-g7bdf4ab` (Go.dot vendors `v0.4.0-48-g880af3d`). Nothing here is built. It is kept so
that the 0.2.x work starts from what was found instead of finding it again.

Line numbers are those of that day. **Check a claim against the code before relying on it.**

What is the author's and what is not:

- **Decided by the author** (2026-11-24): version 0.2.x brings an MCP server (already PRD §3.33);
  in its first version an agent may **read the show and edit it, and may run it only behind a
  switch the operator opens**. The wording of that sentence is this note's, the choice is his.
- Everything else is a finding or a recommendation, marked *(proposed)* where it is a choice.
- The PRD is not amended by this file.

---

## 1. Is MCP still the way to connect an agent?

Yes. The Model Context Protocol is what the agent programs speak (Claude's desktop app and
Claude Code, ChatGPT, the editors). The argument of 2026 is MCP against "a skill and a
command-line tool", and the usual conclusion is that they do different jobs:

| Way in | Good for | For Go.dot |
|---|---|---|
| A command-line tool | one-shot work on files, on one machine | `wfg` has no verb that talks to a *running* Go.dot (`serve`, `replay`, `tree`, `validate`... all work on a bundle or start an engine). It cannot stand in for a live connection. |
| A skill (a folder of instructions an agent loads when it needs them) | know-how: what a cue is, how routing works, what not to do | The cheap way to carry knowledge. Worth shipping *beside* the server. |
| MCP | a live connection to a running program that has state | Go.dot's case. |

**Recommended:** an MCP server inside the application, on the port already reserved for it
(**7410**; WFS-DIY is on 7400), and a short set of instructions for the agent beside it.

### The protocol's own state

- spatcore's server announces revision `2025-06-18` and also accepts `2025-03-26` and
  `2024-11-05` (`control/mcp/MCPProtocolVersions.h`).
- A revision dated **2026-07-28** removes protocol sessions (`Mcp-Session-Id`), the `GET` stream
  and the `initialize` handshake, adds a `server/discover` call, and requires
  `MCP-Protocol-Version`, `Mcp-Method` and `Mcp-Name` headers on every POST. The search of
  2026-11-24 did **not** settle whether it is final or still a draft: read
  <https://modelcontextprotocol.io/specification/latest/changelog> before choosing.
- Either way it suits spatcore's transport, which is already plain request and answer on one
  `POST /mcp` with no stream. WFS-DIY's own notes list "adopt 2026-07-28" as open, so the work
  belongs in spatcore and serves both applications.

---

## 2. What Go.dot already has

Every way in from outside ends in one place: `Engine::submit (Event{origin, command, args})`
into the registry of named commands. An MCP server is one more client of that, which is what
PRD §3.2 asks ("Desktop UI, tablet, surface bridge, MCP and external scripts are all clients of
the same surface").

- **The commands describe themselves.** `Command` (`src/wfg/engine/command/Command.h:107-136`)
  carries a dotted name, a one-sentence description and typed parameters (`CommandParam`: name,
  type tag, optional, variadic). About 235 are registered. `CommandRegistry` lists them in order
  (`all()`, `find()`, `revision()`).
- **The tree describes itself.** A GET on `/godot` at the OSCQuery port (5010) gives every node
  with its type, range, unit, access and description. The source is
  `docs/parameters/godot-parameters.csv` (864 rows), from which the schema table, the RELAX NG
  schema and the OSCQuery answers are all generated.
- **Editing verbs exist.** `node.set`, `node.setMany` (several values, all or none, one undo
  step), `cue.create`, `cue.createFrom`, `object.delete`, `object.move`, `object.moveMany`,
  `document.copy` / `paste`, and the other `*.create` commands
  (`src/wfg/engine/document/DocumentCommands.cpp`).
- **Every change carries its origin** (`window`, `ws:<ip>:<port>`, `udp:...`, `script:<run>`,
  `mqtt:<id>`...), so an `mcp` origin is one more value, not a new mechanism.
- **Undo** is one transaction per applied command; `undo` and `redo` are commands; the state is
  readable (`document/canUndo`, `undoName`, `undoHistory`, `revision`).
- **A show lock** exists (`/godot/document/locked`): it refuses every write to what the show
  saves, and never blocks GO, standby, runs or save.

### The server code is already in the tree

`ThirdParty/spatcore/control/mcp/` is vendored and **unused** by Go.dot (the only "MCP" in
`src/` is a comment at `ShowDocument.h:898`). `ThirdParty/juce_simpleweb`, which it is built on,
is already a submodule. Usable as they are:

- `MCPTransport` — one `POST /mcp`, bound to 127.0.0.1, port tested before starting.
- `MCPRequestGuards.h` — refuses what only a web page would send: the Host must be a loopback
  name, an Origin if present must be loopback, the body must be `application/json`, duplicate
  headers are refused.
- `MCPDispatcher` — `initialize`, `tools/list`, `tools/call`, `resources/*`, `prompts/*`; each
  tool call is handed to the message thread and waited for (5 s); JSON nested too deep is
  refused before parsing. The server's name, version and welcome text come from the application.
- `MCPToolRegistry`, `MCPTierEnforcement` (three confirmation tiers and a master switch that is
  **off by default**), `MCPChangeRecords`, `MCPUndoHooks.h`, `MCPLogSink.h`.

---

## 3. How WFS-DIY did it, and what not to copy

WFS-DIY's server has been in use since v1.0.0beta12 (2026-04-30). What it teaches:

- **Hundreds of tools is too many.** It generates 520 tools from its parameter CSVs, plus 187
  "nudge" tools and about 51 written by hand. `tools/list` weighed about 240 KB until beta40
  hid all but the most guarded generated tools and brought it to about 34 KB. An agent pays for
  every listed tool on every conversation.
- **The AI's changes have their own history.** A write whose origin is MCP skips the ordinary
  undo history (otherwise one Ctrl+Z would take back everything the agent did), and is kept as
  a record of before and after values in a ring of 100. The operator has a window listing them,
  undo per row, and Ctrl+Alt+Z. An undo is refused when somebody else has since written a
  different value. There is no single "undo everything the AI did".
- **Three tiers.** Tier 1 runs at once. Tier 2 answers with a token good for 30 s and bound to
  the exact arguments; the agent must call again to confirm. Tier 3 is refused unless the
  operator has opened a gate, which closes by itself (300 s in the code; the README says ten
  minutes). The token stops accidents, not an attacker.
- **No password, this machine only.** The transport can listen wider; the application does not
  let it.
- **Knowledge travels with the server**: 17 markdown resources and 6 prompts, and a welcome
  text in `initialize`.
- **Fixed since**: version negotiation, a failed bind reported as success, tools that did
  nothing, values that were not numbers, web pages able to reach it, a crash on nested JSON.
  All of that is in the spatcore Go.dot vendors.

**What stops spatcore being used as it is:** `MCPResourceRegistry.cpp` hard-codes the
`wfs://knowledge/*` list and `MCPPromptRegistry.cpp` hard-codes six WFS prompts, and the
dispatcher's constructor wants both. They have to take their content from the application.
That is a change in spatcore (`docs/core-boundary-proposal-control.md:122-145` already plans the
seams), to be made there and pinned, not patched in Go.dot.

---

## 4. A shape for Go.dot *(proposed)*

### A handful of tools, not one per command

Go.dot is better placed than WFS-DIY because its commands and its tree are already data. Five
tools would cover it; the names are placeholders:

| Tool | Does | Stands on |
|---|---|---|
| read the tree | the nodes under an address, with values and descriptions; a depth limit, because `/godot` whole is large | the tree snapshot the OSCQuery server reads |
| find | cues by number, name or kind; nodes by a word in their description | new, small |
| list / describe commands | the registry as JSON, with parameter names, optional and variadic marks | `CommandRegistry::all()` |
| run a command | any named command, arguments checked by the registry | `Engine::submit` with origin `mcp` |
| set values | several address/value pairs as one undo step | `node.setMany` |

One tool per command would be 235 tools and WFS-DIY's weight problem again. The tier of a call
then belongs to the *command* being run, not to the tool: the tool that runs commands looks the
command's tier up.

### What is missing in Go.dot

1. **An answer to a command.** Outside clients get none today: a refusal shows only as
   `/godot/engine/lastError` and `errorCount`. A tool call must return what happened — the id of
   the cue just made, or the reason code of the refusal (`Command.h:153-459` is the list, and
   it is a contract). In-process this is the applied record; it must be waited for without the
   message thread ever waiting on the GO path (constraint 1).
2. **Commands as JSON.** `wfg commands` prints text; OSCQuery publishes each command at
   `/godot/cmd/<a>/<b>` with a description and type tags but without parameter names.
3. **A tier per command.** Proposed default: a command that only reads or only edits the
   document is tier 1; one that deletes or moves many things is tier 2; one that starts or
   stops anything (`mutates` is not the right flag — this is about *runs*) is refused unless
   the operator's switch is open. This is the author's ruling of 2026-11-24 put into the
   tier machinery. The list of running commands should come from the same place Pause's hold
   check uses (`Engine::setHoldCheck`), so the two cannot drift apart.
4. **A lock an agent cannot lift.** Any client can clear `/godot/document/locked` with
   `node.set` today (decision W says the lock is honoured by every client; nothing says who may
   lift it). For an agent the lock must hold: a write to `document/locked` from origin `mcp` is
   refused.
5. **Commands that are the engine's own.** `mount.described`, `run.started` and others
   "sent by the engine" sit in the same registry with no list of who may call them. An agent
   needs that list.
6. **Undo of the agent's work.** Go.dot has one shared history, and a run of `node.set` from
   one origin on one address already coalesces. Two ways:
   - *Shared history, steps marked with their origin* — the smallest: the operator's Ctrl+Z
     takes back the agent's last step like anyone's, and the undo history names it. A command
     "undo back to before the agent's first change of this conversation" is then a count of
     steps.
   - *A separate history as WFS-DIY has* — more protection against one undo taking a great
     deal, but a second history beside `ShowDocument`'s, which was assessed and recommended
     against once already (`TreeParameterStore`, reuse map).
   **Recommended:** the first, with one rule taken from WFS-DIY: a whole tool call is one undo
   step. **The author's to decide.**
7. **A place in the window**: the switch (off by default), the gate for running, the address to
   copy into an agent's settings, and a way to see what the agent did.
8. **The backup machine.** A following backup passes its clients' commands to the main
   (`Engine.h:91-117`). Say whether an agent may connect to a backup at all.

### Knowledge for the agent

The parameters CSV and the command descriptions already say *what* everything is. What an
agent lacks is *how a show is made*: what a cue list, a group, a route, a send, a fade are, and
the constraints of `CLAUDE.md` in a paragraph. Proposed as MCP resources under `godot://`,
written once and also usable as a skill's text, so there is one source.

### What the constraints say

- **1 and 3 (GO never blocks).** The server is a client. Its threads wait for answers; the
  engine never waits for the server.
- **4 (stops).** Esc and double Esc from an agent are running commands like GO, behind the same
  switch — except that a stop asked while the switch is shut should perhaps still be *allowed*:
  refusing a stop is the dangerous direction. **The author's to decide.**
- **7 (easter eggs never in the error path)** applies to every sentence a tool returns.
- **9 (the controller arrives knowing nothing and leaves knowing nothing).** No state of a
  conversation is kept in the show; the stateless revision of the protocol agrees with this.
- **10 (the document holds what someone decided).** An agent's edit is a decision somebody
  asked for; nothing about the connection is saved in the show. The switch is the machine's,
  not the show's.
- **11 (every action is a named command).** This is what makes five tools enough.

---

## 5. Open questions

For the author:

1. Undo: shared history with origins marked, or a separate history (§4, item 6).
2. May an agent stop a show while the running switch is shut?
3. Does the switch for running close by itself after a time, as WFS-DIY's gate does, or stay
   open until shut? A rehearsal wants it open for an hour; a gate that closes mid-sentence is a
   nuisance, one left open into a performance is a risk.
4. May an agent connect to a backup machine; may it reach mounted devices (writes outside
   `/godot` go to WFS-DIY, a console, a broker)?
5. This machine only, as WFS-DIY — or a tablet's agent on the network one day? There is no
   password anywhere in Go.dot today (decision AGB accepts a GO from anyone on the network).

For whoever builds it:

6. Which protocol revision to announce (§1), and whether the change is made in spatcore first.
7. How a tool call waits for the applied record.
8. How big an answer of "read the tree" may be, and how it is cut.
9. Tests: spatcore has unit tests of the guards and a transport on loopback; WFS-DIY has
   `tools/validation/control-replay/mcp_replay.py` (handshake, list, tier 2 round trip, tier 3
   refusal, batch undo, a web page refused). Go.dot's blackbox drivers are the place for the
   same.

---

## 6. Where things are

| What | Where |
|---|---|
| Command, parameters, reason codes | `src/wfg/engine/command/Command.h`, `CommandRegistry.h` |
| How a write is routed, origins | `src/wfg/engine/tree/EngineNamespace.cpp:67-188`, `command/Event.h` |
| What a refusal looks like today | `src/wfg/engine/Engine.cpp:70-178` |
| Document commands | `src/wfg/engine/document/DocumentCommands.cpp` |
| Undo transactions, the lock | `src/wfg/engine/document/ShowDocument.h:893-977`, `cue/UndoStep.cpp` |
| OSCQuery and WebSocket server | `src/wfg/engine/oscquery/OscQueryServer.cpp` |
| Commands printed | `wfg commands`, `src/wfg/engine/Console.cpp:366-513` |
| The schema's source | `docs/parameters/godot-parameters.csv`, `scripts/generate-schema.py` |
| The MCP server to reuse | `ThirdParty/spatcore/control/mcp/` |
| WFS-DIY's use of it | `D:\dev\WFS_DIY_v1\Source\Network\MCP\`, `MainComponent.cpp:1033-1038`, README "AI Control (MCP Server)" |
| WFS-DIY's specifications | `D:\dev\WFS_DIY_v1\Documentation\MCP\specs\` |
| What the docs already say | PRD §3.2, §3.33; namespace draft §8 (port 7410), decision W, §14.11; reuse map, "MCP — a client the PRD names but no phase schedules" |
