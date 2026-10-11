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

#pragma once

/*
    THE NAMES THE ENGINE GIVES WHAT IT MAKES WHILE A SHOW RUNS, derived from
    what made them (namespace draft §58, RD.2, AGE).

    A backup follows its main by applying the main's operator records with
    their arguments as applied - a GO carries the run names the main gave it,
    as a replay re-supplies them. But much of what runs is made by the engine
    itself, on each machine: a group launching its members, the standby's
    look-ahead preparing a scene before anybody presses GO, a Doh! putting a
    scene back. Drawn from the system's entropy, as they were, the two machines
    called the same run by two names, and a `run.stop` a client sent named a
    run on one of them only - and worse, the backup's GO adopts the run its own
    look-ahead prepared, and lit a run whose name was not the one the main's GO
    record carried.

    So a name is a function of its CAUSE: the session's salt, the run it was
    made under (or `root:<cue>` for a run with no parent), the cue it is a run
    of, and how many runs of that cue that cause has made before it. Two
    machines applying the same records to the same show make the same runs in
    the same order under the same causes, and so give them the same names,
    with nothing on the wire.

    COUNTED WHEN A RUN IS MADE, NOT WHEN A NAME IS ASKED FOR. `next` only says
    what the next run of a cue under a cause WOULD be called; `made` is what
    counts it - and reserves the name - once the run exists, whether its name
    was drawn here or handed in by a record. That is the brief's "a supplied
    name advances the counter", put where it cannot be got wrong: the count is
    then a fact about the run table rather than about which road drew, so a
    name drawn and not used - the GO that drew one and then adopted the arm
    the standby had made, a member whose spawn failed - moves nothing on the
    machine that drew it, and the machine that was handed the record (which
    names no such run) agrees with it.

    REDRAWN PAST A NAME IN USE: one a run already has, one the registry holds,
    and one Pure Data would read as a number (namespace draft §51) - by an
    attempt number mixed into the hash, the same on both machines since both
    hold the same runs.

    THE SALT is drawn once a session, by `IdRegistry::drawSalt`, which keeps
    "the only place in the engine that consumes randomness" literally true. It
    is what makes tonight's first GO on a cue a different name from last
    night's, so a client still holding a name from before a restart cannot stop
    the wrong run with it. RD.8 hands the backup the main's salt and counters
    with the position (`Position`, below); this stage builds no copying.

    NOTHING NEW IN THE LOG, AND THAT WAS CHECKED. Every name drawn here with the
    salt is written into a record as applied - `go`, `cue.fire`,
    `trigger.fire`, `audio.arm`, `run.assert`, `run.spawn`, `run.prepare`,
    `list.loadToTime`, `run.seek`, `go.doh`, `go.dohRelaunch`, and for the
    show's own objects `take.kept`, `lane.write` and `media.converted`'s cue -
    and a shuffled run's seed is written into its `run.round`; so `wfg
    replay`, whose own salt is anybody's, is handed every one of them and draws
    none. Two names no record carries: a movie's locked sound
    (`Runner::fireLockedSounds`), made in the handler that fires the movie, and
    the route `media.converted` gives the sound cue it makes. A replay makes
    both again in the same handler, so both are derived WITHOUT the salt
    (`nextUnsalted`, `nextInUnsalted`) from a cause whose name IS in the record
    and is already this session's own - the movie's run, the sound's cue - so
    the salt would add nothing but a number the replay does not have. Logging
    the salt was therefore not needed, and the log is unchanged.

    TICK THREAD ONLY, like the Runner that owns it.
*/

#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/Ids.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace wfg::cue
{
    class RunNames
    {
    public:
        RunNames (const RunTable& runs, doc::IdRegistry& ids);

        /*  WHAT RD.8 COPIES, as a plain value: the salt, and how many runs
            each cause has made of each cue. A backup seated with the main's
            position names what it makes from then on as the main does. */
        struct Position
        {
            std::uint64_t salt = 0;
            std::map<std::pair<std::string, std::string>, std::uint64_t> counts;

            bool operator== (const Position&) const = default;
        };

        //==============================================================================
        /*  The cause of a run: the run it is made under, or `root:<cue>` for
            one with none - a GO at the top of a list, a cue fired by name,
            the standby's arm. Both machines have the parent under the same
            name, and both count the cue's root runs alike. */
        static std::string causeOf (std::string_view parentRun, std::string_view cueId);

        /*  The name the next run of `cue` made under `cause` will have, if it
            is made now: free, not one Pd reads as a number. Advances nothing
            and reserves nothing - `made` does both, once the run exists.
            Empty only if a thousand attempts were all in use. */
        std::string next (std::string_view cause, std::string_view cue) const;

        /*  The same, without the salt: for a name no record carries, which a
            replay must derive again from the same cause - see the note at the
            top of this file. */
        std::string nextUnsalted (std::string_view cause, std::string_view cue) const;

        /*  A run of `cue` was made under `cause`, named `name` - drawn by
            `next`, or handed in by a record: counted, and the name reserved.
            Called by the Runner where a run is created, and nowhere else. */
        void made (std::string_view cause, std::string_view cue, const std::string& name);

        //==============================================================================
        /*  A DOCUMENT OBJECT THE ENGINE MAKES BY ITSELF (AFX): the next name
            for `what` under `cause`, free in the document's own registry.
            Reserves nothing - the document's create does, given the name - and
            `count` is what advances it, drawn or handed in. */
        std::string nextIn (const doc::IdRegistry& registry,
                            std::string_view cause, std::string_view what) const;

        /*  The same without the salt, for an object no record names - its
            cause must then be one a record does name, such as the cue it is
            made for, so a replay derives it again. */
        std::string nextInUnsalted (const doc::IdRegistry& registry,
                                    std::string_view cause, std::string_view what) const;

        void count (std::string_view cause, std::string_view what);

        /*  A shuffled group's seed, when the group writes none: from the salt
            and the group's run, so the two machines shuffle alike. Never
            nought, which means "no seed"; never negative, which the row
            refuses. */
        std::int32_t seedFor (std::string_view cause) const;

        //==============================================================================
        std::uint64_t salt() const noexcept { return currentSalt; }
        void setSalt (std::uint64_t salt) noexcept { currentSalt = salt; }

        /*  The counters forgotten, the salt kept: names start again from the
            first of every cause. */
        void reset() { counts.clear(); }

        Position position() const { return { currentSalt, counts }; }
        void seat (Position wanted);

        /*  ONE CANDIDATE, the pure function the rest is built on: FNV-1a over
            the salt, the cause, the cue, the count and the attempt, finished
            with splitmix64's mixer so the forty bits a name keeps are mixed
            from every byte, and spelled as an identifier. Public so a test can
            say which name a draw passed over. */
        static std::string candidate (std::uint64_t salt, std::string_view cause, std::string_view cue,
                                      std::uint64_t n, std::uint64_t attempt);

    private:
        std::uint64_t countOf (std::string_view cause, std::string_view cue) const;

        std::string drawn (std::uint64_t salt, std::string_view cause, std::string_view cue) const;

        std::string drawnIn (std::uint64_t salt, const doc::IdRegistry& registry,
                             std::string_view cause, std::string_view what) const;

        const RunTable& runs;
        doc::IdRegistry& ids;
        std::uint64_t currentSalt = 0;
        std::map<std::pair<std::string, std::string>, std::uint64_t> counts;
    };
}
