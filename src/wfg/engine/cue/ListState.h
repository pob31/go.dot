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
    WHAT A CUE LIST IS DOING THAT NOBODY WROTE DOWN.

    PRD §4.10 keeps what the machine happens to be doing out of the document,
    and these are exactly that: where an operator is POINTING when they ask what
    the show would be at some position, and where the last jump actually landed.
    Neither is a decision anybody saved - a show reopened tomorrow has no aim,
    and that is correct rather than a loss.

    TWO POINTERS AND NOT ONE, which is §3.13's own shape. The AIM is where a
    finger is: a client drags it along the list and the solve follows. The STATE
    POSITION is where a `list.loadToTime` last put the show. After a jump the two
    agree; after a GO or a manual tweak they do not, and that divergence is the
    thing a running view has to show - it is the difference between where the
    operator asked to be and where the show has got to since.

    A CUE AND AN OFFSET, NEVER A WALL TIME. A manual list has no time in it, so
    "the state at 04:12" is a question the document cannot answer; "cue C has
    been running for `offset` seconds" is one it can. An offset of -1 means
    BEFORE C has fired - standby on it, nothing of it done - which is what "take
    it back to cue 12" means and is a different position from nought seconds in.

    THREADING: none of its own. The tick thread writes it through a command; the
    parameter tree reads it while publishing, on that same thread.
*/

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::cue
{
    /** A position in a list: a cue, and how far into it. */
    struct ListAim
    {
        std::string cue;

        /** Seconds, or -1 for "before this cue has fired at all". */
        double offset = -1.0;

        bool isSet() const noexcept { return ! cue.empty(); }
    };

    /** `"<cue> <offset>"`, which is how both nodes spell one. */
    std::string spellAim (const ListAim& aim);

    /** The other direction. An empty or malformed text is an unset aim. */
    ListAim readAim (const std::string& text);

    /*  ONE STEP A LIST TOOK: when, what, and how it was asked for.

        `origin` is a letter because the node spells sixty-four of these in one
        string and a word each would be a paragraph: `g` for a GO, `f` for a cue
        fired by name, `t` for a trigger, `p` for a press on a sampler strip
        (Phase 6), and `d` for a Doh! (2026-10-01, PRD §3.32) - the GO it
        took back is gone from the history, and this says it was taken back.
        A `d` is not a firing: every reader that places what fired skips it.

        `serial` IS THE GO THAT CAUSED THE STEP (namespace draft §24): the `go`
        handler's own `g`, and the `f` a start cue's target writes under the GO
        that fired the start cue. Nought for every other step. Not spelled in
        the node: it is how Doh! finds the steps of the GO it takes back,
        wherever a seek has moved them. */
    struct Step
    {
        std::int64_t tick = 0;
        std::string cue;
        char origin = 'g';
        std::uint64_t serial = 0;
    };

    /*  WHAT DOH! WOULD TAKE BACK, NOW (PRD §3.32): the list the last GO
        moved, the cue it fired and its tick; empty when there is nothing to
        take back. One for the whole runner and not one per list, because the
        Doh acts on the list of the last GO whichever list has the focus.
        Published as `/godot/list/doh`, "<list> <cue> <tick>". */
    struct DohOffer
    {
        std::string list, cue;
        std::int64_t tick = -1;

        bool isSet() const noexcept { return ! list.empty(); }
    };

    /** `"<list> <cue> <tick>"`, or empty for none. */
    std::string spellDohOffer (const DohOffer& offer);

    /** `<tick>:<cue>:<origin>`, which is how the node spells one. */
    std::string spellStep (const Step& step);

    //==============================================================================
    class ListState
    {
    public:
        void aimAt (const std::string& list, const ListAim& aim) { aims[list] = aim; }
        void landedAt (const std::string& list, const ListAim& aim) { positions[list] = aim; }

        ListAim aimOf (const std::string& list) const { return lookUp (aims, list); }
        ListAim positionOf (const std::string& list) const { return lookUp (positions, list); }

        /*  A step taken, newest kept last. Bounded at sixty-four because the
            node that publishes it is one string, and because the use is going
            BACK a few steps rather than reading an evening: an operator who
            wants act one again asks the solver for act one, not the history.

            §3.13's manual waypoints, kept for the operator rather than by them
            (decision R): each of these is a load-to-time target, `<cue> -1`. */
        void stepped (const std::string& list, const Step& step)
        {
            auto& steps = histories[list];
            steps.push_back (step);
            ++taken;

            /*  AND THE RECORDER KEEPS IT, unbounded, while it is on: the
                author's live recorder is the history dumped to a group, and
                a night is longer than sixty-four steps. */
            if (recordingSince >= 0)
                recorded.push_back (step);

            while (steps.size() > kept)
                steps.erase (steps.begin());
        }

        /*  A STEP PUT WHERE ITS TICK SAYS, not at the end (2026-10-02, Doh!
            D2, namespace draft §24): the corrected GO that carries a paused
            cue on writes its `g` back-dated - the step the Doh erased, moved
            forward by the time the cue spent paused - so a later jump places
            the cue where it is playing. After the last step whose tick is at
            or before it, in the history and in the recorder's take alike. */
        void steppedAt (const std::string& list, const Step& step)
        {
            const auto placeIn = [&step] (std::vector<Step>& steps)
            {
                auto at = steps.end();

                while (at != steps.begin() && std::prev (at)->tick > step.tick)
                    --at;

                steps.insert (at, step);
            };

            auto& steps = histories[list];
            placeIn (steps);
            ++taken;

            if (recordingSince >= 0)
                placeIn (recorded);

            while (steps.size() > kept)
                steps.erase (steps.begin());
        }

        /*  A JUMP RETIMES THE HISTORY (2026-09-19). The steps are on the wall
            clock, and a load to time puts the show where it was at `landedAt`:
            from now on a cue fired at tick t before that instant has been
            going for (now - landedAt) + (landedAt - t) ticks, so its step
            moves forward by (now - landedAt); and a step after the instant
            did not happen in the show the jump made, so it goes. Without this
            a second aim after a jump would read the abandoned steps as still
            in force, and the kept ones as older than the sound they describe. */
        void retimed (const std::string& list, std::int64_t landedAt, std::int64_t now)
        {
            auto& steps = histories[list];

            steps.erase (std::remove_if (steps.begin(), steps.end(),
                                         [landedAt] (const Step& step) { return step.tick > landedAt; }),
                         steps.end());

            for (auto& step : steps)
                step.tick += now - landedAt;
        }

        /*  A SCENE RE-SEATED AT A SECOND OF ITSELF (`run.seek` on a group)
            moves its most recent step to where that second says it was fired.
            Its most recent FIRING: a `d` step records a Doh and fired nothing,
            so it is passed over (2026-10-01). The step keeps its serial. */
        void refired (const std::string& list, const std::string& cue, std::int64_t firedAt)
        {
            auto& steps = histories[list];

            for (auto step = steps.rbegin(); step != steps.rend(); ++step)
                if (step->cue == cue && step->origin != 'd')
                {
                    step->tick = firedAt;
                    return;
                }
        }

        /*  THE STEPS OF A GO THAT DOH! TAKES BACK, gone (2026-10-01, PRD
            §3.32): every step carrying that GO's serial, from every list's
            history and from the live recorder's take - the GO's own `g`, and
            the `f` its start cues' targets wrote - wherever a `run.seek` moved
            them, since they are found by serial and not by tick. Answers the
            `g` among them, as it stood. `taken` does not move back: a hook
            watches it to notice a step, and the Doh's own `d` is the next one. */
        std::optional<Step> unstepped (std::uint64_t serial)
        {
            std::optional<Step> erased;

            if (serial == 0)
                return erased;

            const auto carries = [serial] (const Step& step) { return step.serial == serial; };

            for (auto& history : histories)
            {
                auto& steps = history.second;

                for (const auto& step : steps)
                    if (carries (step) && step.origin == 'g')
                        erased = step;

                steps.erase (std::remove_if (steps.begin(), steps.end(), carries), steps.end());
            }

            recorded.erase (std::remove_if (recorded.begin(), recorded.end(), carries), recorded.end());
            return erased;
        }

        /*  WHAT DOH! WOULD TAKE BACK (PRD §3.32), set by the `go` handler
            and cleared by the Doh, a jump on that list, or a GO that fires
            nothing to take back. */
        void setDohOffer (const DohOffer& offer) { doh = offer; }
        const DohOffer& dohOffer() const noexcept { return doh; }

        /*  WHAT THE NEXT GO ON A LIST CARRIES ON (2026-10-02, Doh! D2, PRD
            §3.32): `"<cue> <seconds>"` - the cue a Doh paused and where it will
            carry on from - or empty when the next GO starts whatever it fires
            from its top. Published as `/godot/list/<id>/resume`; written by the
            handlers that set and drop a Doh's resume, never by a hook. */
        /*  WHAT A PRESS OF DOH! WOULD FORGET NOW (D2's review): the list and
            the cue of the resume the last Doh left, when no GO a press could
            take back stands before it - published as `/godot/list/dohForget`,
            "<list> <cue>", for the button to offer the second press only where
            the engine would act on it. A readout: written by a hook. */
        struct DohForget
        {
            std::string list, cue;
        };

        void setDohForget (const DohForget& forget) { dohForgetNow = forget; }
        const DohForget& dohForget() const noexcept { return dohForgetNow; }

        void setResume (const std::string& list, const std::string& text)
        {
            if (text.empty())
                resumes.erase (list);
            else
                resumes[list] = text;
        }

        std::string resumeOf (const std::string& list) const
        {
            const auto found = resumes.find (list);
            return found == resumes.end() ? std::string {} : found->second;
        }

        /** Newest last. */
        const std::vector<Step>& historyOf (const std::string& list) const
        {
            static const std::vector<Step> none;
            const auto found = histories.find (list);
            return found == histories.end() ? none : found->second;
        }

        /** How many steps every list has taken in total, for a hook that wants
            to notice a new one without comparing lists. */
        std::uint64_t stepsTaken() const noexcept { return taken; }

        /*  THE LIVE RECORDER (author, 2026-09-18). On from a tick, keeping
            every step on every list; off with the steps handed back, oldest
            first, for `record.stop` to write into a take. */
        void startRecording (std::int64_t tick)
        {
            recordingSince = tick;
            recorded.clear();
        }

        std::vector<Step> stopRecording()
        {
            recordingSince = -1;
            return std::move (recorded);
        }

        bool isRecording() const noexcept { return recordingSince >= 0; }
        std::int64_t recordingSinceTick() const noexcept { return recordingSince; }

        /** A show being closed takes all of it with it: it is about a session. */
        void clear()
        {
            aims.clear();
            positions.clear();
            histories.clear();
            recorded.clear();
            recordingSince = -1;
            doh = {};
            resumes.clear();
            dohForgetNow = {};
        }

        static constexpr std::size_t kept = 64;

    private:
        static ListAim lookUp (const std::map<std::string, ListAim>& from,
                               const std::string& list)
        {
            const auto found = from.find (list);
            return found == from.end() ? ListAim {} : found->second;
        }

        std::map<std::string, ListAim> aims, positions;
        std::map<std::string, std::vector<Step>> histories;
        std::uint64_t taken = 0;
        std::int64_t recordingSince = -1;
        std::vector<Step> recorded;
        DohOffer doh;
        std::map<std::string, std::string> resumes;
        DohForget dohForgetNow;
    };
}
