# How Go.dot was built

Go.dot was written in about two months, from 30 August to 31 October 2026, by one
person working with Claude, Anthropic's AI model, through Claude Code. This page is
for anyone who comes across the repository and wonders how that happened, and which
part of it was human.

## Who did what

I am a freelance theatre sound and video engineer. Go.dot is the show controller I
wanted for my own shows. It also conducts the other tools I have written:
[WFS-DIY](https://github.com/pob31/WFS-DIY), a wave-field synthesis processor for
sound reinforcement; [S21_HiJack](https://github.com/pob31/S21_HiJack), a companion
app for DiGiCo's S21 and S31 consoles; and [spatcore](https://github.com/pob31/spatcore),
the base they share.

The work was split like this:

- **Mine:** what the software must do and must never do, every design decision, looking
  at each screen and saying what is wrong with it, and standing in front of the hardware.
- **Claude's:** the code, the tests, the documents that keep track of both, reviewing
  its own work with separate review agents, and chasing every red CI run to its cause.

I wrote very little of the code. I wrote, or ruled on, nearly everything the code is
required to do.

## The rules came from the desk

The core of the specification is twelve constraints, in
[§4 of the PRD](godot-prd-draft-0.8.md) and copied word for word into
[CLAUDE.md](../CLAUDE.md), the file Claude Code reads at the start of every session.
A few of them:

- **GO never blocks.** Whatever else is happening (a disk read, a network timeout, a
  plugin that has hung), the next cue goes when the operator presses the button.
- **Three levels of stop, three different promises.** Esc stops gracefully and still
  runs the cleanup a group would have run at its end. Double Esc stops everything
  Go.dot started, at once, and skips the cleanup. **Doh!** is for the mistake that
  actually happens every week: a cue fired too early. It puts things back.
- **Undoing a GO is a revert, not an undo.** The sound has already gone out into the
  room, and the software is honest about that.
- **Every parameter has a defined resting state**, so "stop everything" always means
  something precise.
- **Colour is never the only way information is shown.** Stage lighting shifts
  colours, and not every operator sees them the same way.

None of these is a programming idea. They come from running shows. A model will
happily write a GO button that waits for a file to load. It takes someone who has
run shows to forbid it, and to keep forbidding it at every review.

## How the work was organised

**The specification is law.** The [PRD](godot-prd-draft-0.8.md) and the
[phase plan](godot-devplan-draft-0.1.md) come before the code. CI fails if
CLAUDE.md drifts from the PRD's §4.

**Every decision has an owner.** Anything marked *(proposed)* in the PRD is a design
put forward and not yet confirmed, and it is not built without an explicit yes. When
a question was mine to answer, Claude asked with a recommended option and I picked
one. Most days that was a few minutes of my time for several hours of building.

**Decisions are remembered.** Claude Code keeps short notes between sessions: my
rulings, the traps found along the way, what is still owed to the hardware bench.
By the end there were about 150 of them. They are why a session on day 50 does not
reopen a question settled on day 12.

**Briefs, an implementer and a reviewer.** For larger stages Claude wrote a brief,
one agent built it, a second agent that had not written the code reviewed it
read-only, and the fixes went back to the first. Then came the full test suite, the
push and CI. The reviewer regularly found real bugs: hangs, regressions, a fader
that would have written a trim during an emergency stop.

**Tests as the safety net.** The tests are 247,000 lines, a little over a third of
the code. Every suite runs twice: once in the C locale and once in French, because a
decimal comma has broken things more than once. CI builds and tests on Linux, macOS
and Windows. It also runs a strict build that fails on any warning in our own code,
a real-time safety job (Clang's RTSan watching the audio thread for allocations and
locks), and [fuzzing](fuzzing.md) of the code that reads OSC packets and QLab and
Ableton files. A Mac mini and an Ubuntu NUC on my desk serve as a test farm that
Claude reaches over SSH.

**Something to look at, early.** I need to see a thing before I can have an opinion
on it. So a web console was built in phase 3, ahead of the phase that owned the
interface, and the interface was designed by my looking at it and saying what was
wrong.

## Overnight

Once a plan is written down and the decisions behind it are made, the agents can
keep going without me. One evening in early October I left the second half of the
stop-and-Doh! hardening running with *"You can keep going throughout the night."* By
6:30 the next morning seven more stages had each been reviewed, fixed, tested and
pushed, and CI was green.

That was the exception, though, not the method. Only about a tenth of the work
happened at night. The rest happened while I was around to answer questions. The
lever was not that the agents ran unsupervised. It was that each instruction from me
turned into a lot of work: roughly 580 messages typed by me drove about 72,700
model requests.

## The numbers

| | |
|---|---|
| Time | 30 Aug – 31 Oct 2026, in my spare time |
| Commits | 1,182 |
| Code | ~645,000 lines (app 367k, tests 247k, scripts, web console and build 31k), not counting the libraries it builds on |
| Documentation | ~52,000 lines of Markdown, about 700,000 words |
| Releases | first test build 29 September; v0.1.22 on 31 October; Windows, macOS and Linux |

These figures come from Claude Code's own logs for 22 September to 31 October. Logs
from before 22 September had already been deleted.

| | |
|---|---|
| Model requests | ~72,700 (mostly Claude Opus 5.5, some Claude Fable 5.1 and Claude Opus 5) |
| Messages typed by me | ~580 |
| Tokens written by the model | ~44 million |
| Tokens read back from cache | ~22 billion |

**What it would have cost at API prices:** about $7,800 for the logged period at
October 2026 list prices. With the first three weeks estimated, it comes to
**$10,000–11,000**. More than half of that is the model re-reading a large context
on every turn, not writing new code.

**What it actually cost:** a Claude Max subscription, one month at the Max 20 tier
and one at Max 5, so **about $300**.

**What it would have cost by hand:** at a tenth of the speed, this would be twenty
months of a senior C++ audio engineer, €165,000 or more at French rates. Honestly,
that is generous to the human: the same scope written by hand is a team working for
years. Treat the line count as a measure of volume, not of value.

## What it is not, yet

Go.dot is ready to try, **not yet ready for opening night**; the
[README](../README.md) says the same. Much of what touches hardware has been built
and tested in software only. The phase plan ends each piece of work with what is
still owed to the bench, and nothing on that list counts as done until someone has
stood in front of the real device. Some items have been through it: a full 64-input
WFS-DIY configuration recalled from a single cue of 5,300 messages passed on the
bench on 18 October. Many have not.

## What I learned

- **The model writes code faster than anyone can read it.** What keeps it honest is
  a specification it can be held to, tests, and a reviewer that did not write the
  code. Without those, the speed would only produce mistakes faster.
- **Timing tests on shared CI runners lie, and a laptop is not a show machine.** A
  lot of red runs were the machine, not the code. Timing is judged on the real
  bench.
- **Some surprises are only found by testing, not by reading.** Tracktion Engine
  turns on flush-to-zero for the whole process, for example. Claude found that by
  measuring, not from any documentation.
- **The human job did not shrink. It moved,** from typing code to deciding, looking
  and checking. My experience as a sound engineer is the part of Go.dot that could
  not have come from anywhere else.

*Pierre-Olivier Boulant, October 2026. Claude drafted this page from the repository
and its logs, and I checked it.*
