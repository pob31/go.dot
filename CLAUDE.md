# CLAUDE.md

This file is **PRD §4, reproduced verbatim** from `docs/godot-prd-draft-0.8.md`. It is the
review criterion for every pull request.

**If this file and the PRD disagree, the PRD wins and this file is stale. Fix it by copying,
never by editing.** The section number below is the PRD's, kept so a diff against the source
is exact. Every `§` reference points into the PRD, which is where §3.3 and the rest live.

---

## 4. Constraints as law

1. **The GO path is small, boring and merciless.** GO never blocks.
2. **The audio thread is a lipogram**: no allocation, no locks, no exceptions,
   no syscalls, no logging.
3. **Undo, crash-safe autosave, and a GO button that never blocks** are why
   anyone trusts show software.
4. **Three levels of stop, three distinct guarantees:**
   - **Esc — graceful abort.** Stops running cues and **runs footers**. Same code
     path as normal completion, entered early: a group aborted at 04:12 releases
     its channels and kills its LFOs exactly as it would have at 06:00.
   - **Double Esc — immediate.** Drops all actions, **skips footers**, kills all
     internal processing including live effects. Scope is **everything Go.dot
     originates** — it does not mute the PA or shut down the projector. The world
     may be left in a state nobody declared; that is the price of an emergency.
   - **Go Doh! — recovery.** Aimed at the failure that actually happens weekly:
     a trigger fired ahead of time. Recovery-oriented, not destructive.
     *Specification deferred* until the full inventory of in-flight objects
     exists.
5. **Undo of a GO is *revert*, not undo.** Restores standby, releases bindings,
   re-asserts pre-GO state; honest that the audio already escaped. A double-GO
   caught inside the anticipation window is fully recoverable and is the common
   case.
6. **Every parameter has a defined resting state** (§3.3 panic value).
7. **Easter eggs live where idle eyes wander** — splash, About, empty states,
   idle strings. Never in the error path.
8. **Colour is never the sole carrier of information.**
9. **The controller arrives knowing nothing and leaves knowing nothing.**
10. **The document holds what someone decided**, never what the machine happened
    to be doing.
11. **Every gesture-reachable action exists as a named command.**
12. **Containers describe behaviour; content describes output.** Nothing
    inherits downward. Groups organise time, order and lifetime; cues own
    outputs, media and parameters. Repetition of an assignment is multi-select
    edit, not inheritance.

---

## Where to look

- **`docs/godot-prd-draft-0.8.md`** — the full Product Requirements Document; the source of
  everything above, and of the `§` references inside it.
- **`docs/godot-devplan-draft-0.1.md`** — the phase plan: what is being built now, what is
  deferred, and which decisions are the author's rather than the implementer's.
- **`README.md`** § Contributing — the conventions this repo enforces in practice: GPL-3 file
  headers, the `fr_FR` locale rule, submodule pins, and how to build.
