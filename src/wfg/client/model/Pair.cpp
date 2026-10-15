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

#include <wfg/client/model/Pair.h>

#include <wfg/client/model/Gestures.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/pair/Leadership.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        /*  The dash the page and the transport draw for a reading nobody has
            published, and for a line that means nothing on this seat. */
        constexpr const char* unsaid = "—";

        /*  A SENTENCE OF THE ENGINE'S IS BOUNDED HERE, before any label sees
            it - the transport's rule since a show's warnings hung the window
            (model/Transport.h): `pair/differs` names cues, and a cue's name is
            whatever somebody typed. Cut on a character's first byte, never
            inside one. */
        std::string clipped (std::string sentence)
        {
            constexpr std::size_t longest = 400;

            if (sentence.size() <= longest)
                return sentence;

            auto cut = longest;

            while (cut > 0 && (static_cast<unsigned char> (sentence[cut]) & 0xC0u) == 0x80u)
                --cut;

            sentence.resize (cut);
            return sentence + "…";
        }

        double number (const std::string& text)
        {
            return osc::parseDouble (text).value_or (0.0);
        }

        std::int64_t count (const std::string& text)
        {
            return static_cast<std::int64_t> (std::llround (number (text)));
        }

        /*  ONE DECIMAL, in no locale's spelling: a millisecond to a tenth, parts
            per million to a tenth - what an operator compares, not the
            engine's full precision. */
        std::string oneDecimal (double value)
        {
            return osc::formatDouble (std::round (value * 10.0) / 10.0);
        }

        std::string milliseconds (double seconds)
        {
            return oneDecimal (seconds * 1000.0) + " ms";
        }

        std::string capitalised (std::string sentence)
        {
            if (! sentence.empty() && sentence[0] >= 'a' && sentence[0] <= 'z')
                sentence[0] = static_cast<char> (sentence[0] - 'a' + 'A');

            return sentence;
        }

        std::string ticks (std::int64_t many)
        {
            //  Fifty ticks a second: each one is 20 ms.
            return std::to_string (many) + (many == 1 ? " tick (" : " ticks (") + std::to_string (many * 20) + " ms)";
        }

        /*  A FOLLOWER'S VERDICT AS THE LEADER PUBLISHES IT (RD.12): `yes`, `no`,
            and anything else - `unknown`, or a row not there - not known. */
        Flag verdictFlag (const std::string& word)
        {
            return word == "yes" ? Flag::yes : word == "no" ? Flag::no : Flag::unsaid;
        }
    }

    //==========================================================================
    bool PairReading::paired() const
    {
        return role == "main" || role == "backup";
    }

    std::string PairReading::other() const
    {
        return role == "main" ? "backup" : "main";
    }

    std::string PairReading::seatWords() const
    {
        if (! paired() || state.empty())
            return {};

        const auto isMain = role == "main";

        /*  AGM's three as the author chose them, and the implementer's for the
            seats he was not asked about, in the same shape: the role, then
            what it does. Two machines both sending each say they send; the
            banner's "Two mains!" is what says there are two. */
        if (state == "following")
            return isMain ? pairWords::mainFollowing : pairWords::backupFollowing;

        if (state == "waiting")
            return isMain ? std::string ("Main - waiting") : std::string (pairWords::backupWaiting);

        if (state == "alone" && isMain)
            return pairWords::mainAlone;

        return isMain ? pairWords::mainSending : pairWords::backupSending;
    }

    std::string PairReading::seatSentence() const
    {
        if (! paired())
            return "Not paired: this computer plays the show on its own.";

        const auto them = other();

        if (state == "following")
            return role == "main"
                     ? "The backup took over: this computer follows it a moment behind and sends nothing to the devices."
                     : "This computer follows the main a moment behind and sends nothing to the devices, ready to take over.";

        if (state == "waiting")
            return "This computer has not found its " + them + " since it started: it holds, and sends nothing to the"
                   " devices until it finds it or is told to take over.";

        if (state == "bothSending")
            return "Both computers are sending: every command reaches the devices from both.";

        if (state == "leading")
            return role == "main"
                     ? std::string ("This computer sends to the devices, and the backup follows it.")
                     : "This computer took over and sends to the devices" + std::string (isYes (linked)
                           ? ", and the main follows it." : "; the main is not there.");

        if (state == "alone")
            return "This computer sends to the devices; " + std::string (role == "main" ? "no backup follows it."
                                                                                        : "the main is not there.");

        return unsaid;
    }

    //==========================================================================
    namespace
    {
        bool showTextDiffers (const PairReading& reading)
        {
            /*  THE SHOW ITSELF, OR ONLY ITS FILES: the engine's sentence opens
                with "the show differs" when the show's text does
                (pair/ShowCheck.cpp, `showDifferenceOf`) and with the files
                otherwise - and only the first has a show to take. */
            return std::string_view (reading.showDiffers).starts_with ("the show differs");
        }
    }

    bool PairReading::bannerShowing() const
    {
        return paired() && ! alarm.empty();
    }

    bool PairReading::bannerAlarming() const
    {
        return state == "bothSending";
    }

    std::string PairReading::bannerHeading() const
    {
        return bannerAlarming() ? std::string (pairWords::twoMains) : seatWords();
    }

    std::optional<PairAct> PairReading::bannerAct() const
    {
        /*  THE ONE BUTTON THAT ANSWERS THE ALARM, read from the rows rather
            than from the alarm's words, which are the engine's to change. */
        if (state == "bothSending")
            return PairAct::keep;

        if (state == "waiting")
            return PairAct::takeOver;

        if (state == "following")
        {
            if (showTextDiffers (*this))
                return PairAct::adoptShow;

            if (! differs.empty() && ! isYes (inStep))
                return PairAct::realign;
        }

        return std::nullopt;
    }

    std::string PairReading::bannerSentence() const
    {
        /*  AND WHEN ITS BUTTON WOULD BE REFUSED, WHY - in words after the
            alarm's, since a greyed button alone says only that something is
            unavailable (§4.8). */
        if (const auto act = bannerAct(); act.has_value())
            if (const auto offered = offer (*act); ! offered.open)
                return alarm + " " + offered.words;

        return alarm;
    }

    //==========================================================================
    std::string PairReading::buttonWords (PairAct act) const
    {
        switch (act)
        {
            case PairAct::takeOver:  return pairWords::takeOver;
            case PairAct::handOver:  return pairWords::handOver;
            case PairAct::keep:      return pairWords::makeThisTheMain;
            case PairAct::realign:   return pairWords::realign;
            case PairAct::adoptShow: return pairWords::takeTheShowOf + other() + pairWords::takeTheShowEnd;
        }

        return {};
    }

    PairOffer PairReading::offer (PairAct act) const
    {
        /*  THE ENGINE'S OWN RULE for the three that move a seat - the function
            `pair.takeOver`, `pair.handOver` and `pair.keep` answer with
            (pair/Leadership.h) - asked with what the rows say: the seat the
            log last said, and the other machine there or not. */
        const auto seat = pair::seatNamed (state);
        const pair::Seated now { pair::roleNamed (role), seat.value_or (pair::Seat::alone), {} };

        /*  The other machine's seat as this one sees it: the rule reads only
            whether there is one, and the rows say only whether it is there. */
        const std::string peerSeat = isYes (linked) ? std::string ("linked") : std::string {};
        const auto following = seat == pair::Seat::following;
        const auto them = other();

        std::string refused;

        switch (act)
        {
            case PairAct::takeOver: refused = pair::refusal (now, pair::Ask::takeOver, peerSeat); break;
            case PairAct::handOver: refused = pair::refusal (now, pair::Ask::handOver, peerSeat); break;
            case PairAct::keep:     refused = pair::refusal (now, pair::Ask::keep, peerSeat); break;

            //  `pair.realign`'s and `pair.adoptShow`'s own (PairSettings.cpp): from the seat alone.
            case PairAct::realign:  refused = following ? std::string {} : std::string (reason::notFollowing); break;

            case PairAct::adoptShow:
            {
                if (! following)
                    refused = reason::notFollowing;
                else if (busy)
                    refused = reason::busy;

                /*  AND WHAT THE ROWS SAY BEFORE THE ENGINE WOULD: a show the
                    comparison found the same, or has not compared yet, or whose
                    files alone differ - each `not-fetched` at the engine, said
                    here as what it is. */
                else if (isYes (showMatch))
                    refused = "same-show";
                else if (showDiffers.empty())
                    refused = "not-compared";
                else if (! showTextDiffers (*this))
                    refused = "files-only";

                break;
            }
        }

        if (refused.empty())
        {
            switch (act)
            {
                case PairAct::takeOver:
                    return { true, {}, "This computer sends to the devices from now on; the " + them
                                         + " holds and follows it, if it is there." };
                case PairAct::handOver:
                    return { true, {}, "The " + them + " sends to the devices from now on; this computer holds and"
                                         " follows it." };
                case PairAct::keep:
                    return { true, {}, "The show stays on this computer; the " + them + " holds and follows it." };
                case PairAct::realign:
                    return { true, {}, "Takes the " + them + "'s position again: its standby, and every cue running"
                                         " at its playhead." };
                case PairAct::adoptShow:
                    return { true, {}, "Takes the " + them + "'s show - every cue and setting - and keeps this"
                                         " computer's interface and ports. This computer's own files are set aside in"
                                         " a dated folder of the show; sounds are not copied." };
            }
        }

        /*  WHY NOT, in words an operator acts on. A machine with no role is
            told where a role is given, whichever command it pressed - and that
            is said only on the Redundancy tab, under its role menu: the banner
            and the Show menu's submenu are a paired machine's alone. */
        std::string why;

        if (refused == reason::notPaired || (! paired() && refused == reason::notFollowing))
            why = "This computer is not paired: choose Main or Backup above.";
        else if (refused == reason::notHolding)
            why = "This computer already sends.";
        else if (refused == reason::notSending)
            why = "This computer holds: there is nothing to hand over.";
        else if (refused == reason::notLinked)
            why = "The " + them + " is not there to take the show.";
        else if (refused == reason::notBothSending)
            why = "Only while both computers send.";
        else if (refused == reason::notFollowing)
            why = "Only on a computer that follows the other.";
        else if (refused == reason::busy)
            why = "Not while a cue runs here: stop it first.";
        else if (refused == "same-show")
            why = "The two shows are the same.";
        else if (refused == "not-compared")
            why = "The two shows have not been compared yet.";
        else if (refused == "files-only")
            why = "Only files differ, and files never travel: copy them onto this computer.";
        else
            why = refused;

        return { false, refused, why };
    }

    //==========================================================================
    std::vector<PairLine> PairReading::lines() const
    {
        std::vector<PairLine> said;

        const auto them = other();
        const auto following = state == "following";
        const auto leads = state == "leading" || state == "bothSending";
        const auto there = isYes (linked);

        //  THIS COMPUTER, and whether what it sends leaves.
        said.push_back ({ "This computer", paired() ? seatWords() + ". " + seatSentence() : seatSentence() });

        if (! paired())
            return said;

        said.push_back ({ "Sends to the devices",
                          sending == Flag::unsaid ? std::string (unsaid)
                          : isYes (sending) ? std::string ("Yes")
                                            : std::string ("No - everything meant for a device (network, MIDI, serial)"
                                                           " is held, and counted as sent") });

        //  THE OTHER COMPUTER: there, or where it is looked for.
        std::string otherLine;

        if (there)
            otherLine = "There" + (peer.empty() ? std::string() : ", heard at " + peer);
        else if (peers.empty())
            otherLine = "Not there - no address is given for it";
        else
            otherLine = "Not there - looking for it at " + peers;

        said.push_back ({ "The " + them, otherLine });

        //  THE TIMING (RD.6, RD.9), each only where it means something.
        const auto trip = number (roundTrip);
        said.push_back ({ "Round trip", there && trip > 0.0 ? milliseconds (trip) + " there and back over the network"
                                                            : std::string (unsaid) });

        const auto delta = count (delay);
        std::string delayLine = unsaid;

        if (following && delta > 0)
            delayLine = "Applies the " + them + "'s commands " + ticks (delta) + " after it";
        else if (leads && delta > 0)
            delayLine = "Starts each sound " + ticks (delta) + " later than it would alone, so the " + them
                      + " has every GO in time";

        said.push_back ({ "Delay", delayLine });

        std::string sampleLine = unsaid;

        if (following)
            sampleLine = isYes (mapped)
                           ? "Yes - each sound starts on the same sample as the " + them + "'s"
                           : "Not yet - each sound starts " + (delta > 0 ? std::to_string (delta * 20) + " ms" : std::string ("a moment"))
                               + " after the " + them + "'s until the two clocks are matched";
        else if (leads && there)
            sampleLine = "Matched on the " + them;

        said.push_back ({ "Same sample", sampleLine });

        const auto ppm = number (drift);
        const auto residual = number (clockResidual);
        std::string driftLine = unsaid, matchLine = unsaid;

        if (following && residual > 0.0)
        {
            /*  PARTS PER MILLION SAID AS TIME TOO: 3.6 ms an hour for each one,
                which is what an operator can picture. */
            if (std::abs (ppm) < 0.05)
                driftLine = "None measured";
            else
                driftLine = "This computer's sound clock runs " + oneDecimal (std::abs (ppm)) + " parts per million "
                          + (ppm > 0.0 ? "faster" : "slower") + " than the " + them + "'s - about "
                          + oneDecimal (std::abs (ppm) * 3.6) + " ms an hour. Shown, never corrected: one word"
                            " clock for both interfaces is the cure";

            matchLine = "Known to within " + std::to_string (std::llround (residual)) + " µs";
        }
        else if (following)
        {
            driftLine = "Not measured yet";
            matchLine = "Not measured yet";
        }
        else if (leads && there)
        {
            driftLine = "Measured on the " + them;
            matchLine = "Measured on the " + them;
        }

        said.push_back ({ "Clock drift", driftLine });
        said.push_back ({ "Clock match", matchLine });

        /*  THE OUTPUT DELAYS AND THE JUMP A CHANGEOVER MAKES (AGQ, the
            author's): nothing makes up for the difference, and the screen
            says so plainly. */
        const auto own = number (outputLatency);
        const auto theirs = number (peerOutputLatency);

        said.push_back ({ "Output delay", "This computer " + (own > 0.0 ? milliseconds (own) : std::string ("not said"))
                                          + " · the " + them + " "
                                          + (theirs > 0.0 ? milliseconds (theirs) : std::string ("not said yet")) });

        std::string jumpLine;

        if (own <= 0.0 || theirs <= 0.0)
            jumpLine = "Not known until both computers have said their output delay";
        else if (std::abs (theirs - own) < 0.00005)
            jumpLine = "None - the two interfaces take the same time";
        else
            jumpLine = milliseconds (std::abs (theirs - own)) + " - the " + them + "'s sound comes out "
                     + milliseconds (std::abs (theirs - own)) + (theirs > own ? " later" : " earlier")
                     + " than this computer's. Nothing makes up for it: both start each sound on the same sample,"
                       " and each interface takes its own time";

        said.push_back ({ "Changeover jump", jumpLine });

        //  IN STEP AND THE SAME SHOW (RD.8, RD.10): judged on the machine that follows.
        std::string stepLine = unsaid, showLine = unsaid;

        if (following)
        {
            stepLine = isYes (inStep) ? "Yes - the same standby and the same cues, at the same place, as the " + them
                     : ! differs.empty() ? "No - " + differs
                                         : std::string ("Not compared yet");

            showLine = isYes (showMatch) ? "Yes - the same cues, settings and files as the " + them
                     : ! showDiffers.empty() ? capitalised (showDiffers)
                                             : std::string ("Not compared yet");
        }
        else if (leads && there)
        {
            /*  A MACHINE THAT LEADS IS TOLD (RD.12): the follower compares and
                says its verdicts up the link, in its own words - reported, so
                its "here" reads as the follower's. Where to look only while
                nothing is known: before it has said, or once it has not said
                for the link's timeout. */
            const auto lookThere = "Checked on the " + them + " - look at its screen";
            const auto reported = [&them] (const std::string& sentence)
            {
                return sentence.empty() ? std::string ("No") : "No - the " + them + " says " + sentence;
            };

            stepLine = isYes (peerInStep) ? "Yes - the " + them + " has the same standby and the same cues, at the same place"
                     : peerInStep == Flag::no ? reported (peerDiffers)
                                              : lookThere;

            showLine = isYes (peerShowMatch) ? "Yes - the " + them + " has the same cues, settings and files"
                     : peerShowMatch == Flag::no ? reported (peerShowDiffers)
                                                 : lookThere;
        }

        said.push_back ({ "In step", stepLine });
        said.push_back ({ "Same show", showLine });

        //  WHAT WENT DOWN THE LINK AND WHAT DID NOT (RD.5-RD.7).
        if (following)
            said.push_back ({ "Records", "Received from the " + them + ": " + std::to_string (count (received))
                                         + " · missed: " + std::to_string (count (gaps))
                                         + " · applied late: " + std::to_string (count (late)) });
        else if (leads)
            said.push_back ({ "Records", "Sent to the " + them + ": " + std::to_string (count (received))
                                         + " · missed by it: " + std::to_string (count (gaps)) });

        said.push_back ({ "Commands", "Passed on to the " + them + ": " + std::to_string (count (relayed))
                                      + " · ignored from this computer's devices while following: "
                                      + std::to_string (count (dropped))
                                      + " · passed on and never answered: " + std::to_string (count (unconfirmed)) });

        return said;
    }

    bool PairReading::operator== (const PairReading& o) const noexcept
    {
        const auto tie = [] (const PairReading& r)
        {
            return std::tie (r.role, r.peers, r.timeout, r.state, r.sending, r.linked, r.peer, r.alarm,
                             r.inStep, r.differs, r.showMatch, r.showDiffers, r.mapped,
                             r.peerInStep, r.peerDiffers, r.peerShowMatch, r.peerShowDiffers,
                             r.roundTrip, r.delay, r.drift, r.clockResidual, r.outputLatency, r.peerOutputLatency,
                             r.received, r.gaps, r.late, r.relayed, r.dropped, r.unconfirmed, r.busy);
        };

        return tie (*this) == tie (o);
    }

    //==========================================================================
    bool anyRunLive (const tree::TreeSnapshot& snapshot)
    {
        /*  STARTED AND NOT ENDED: a run the horizon made ready has not started
            (`run/started` nought) and does not count, as Console.cpp's
            `busyNow` leaves out a preparation. */
        for (const auto& id : words (text (snapshot, "/godot/run/order")))
        {
            const auto state = text (snapshot, "/godot/run/" + id + "/state");

            if (state.empty() || state == "preparing" || state == "done" || state == "failed")
                continue;

            if (const auto started = text (snapshot, "/godot/run/" + id + "/started"); ! started.empty() && started != "0")
                return true;
        }

        return false;
    }

    PairReading readPair (const tree::TreeSnapshot& snapshot)
    {
        PairReading reading;

        reading.role = text (snapshot, "/godot/pair/role");
        reading.peers = text (snapshot, "/godot/pair/peers");
        reading.timeout = text (snapshot, "/godot/pair/timeout");
        reading.state = text (snapshot, "/godot/pair/state");
        reading.sending = flag (snapshot, "/godot/pair/sending");
        reading.linked = flag (snapshot, "/godot/pair/linked");
        reading.peer = clipped (text (snapshot, "/godot/pair/peer"));
        reading.alarm = clipped (text (snapshot, "/godot/pair/alarm"));
        reading.inStep = flag (snapshot, "/godot/pair/inStep");
        reading.differs = clipped (text (snapshot, "/godot/pair/differs"));
        reading.showMatch = flag (snapshot, "/godot/pair/showMatch");
        reading.showDiffers = clipped (text (snapshot, "/godot/pair/showDiffers"));
        reading.mapped = flag (snapshot, "/godot/pair/mapped");

        //  RD.12's: a word, not a flag - `unknown` is the third answer, read as unsaid.
        reading.peerInStep = verdictFlag (text (snapshot, "/godot/pair/peerInStep"));
        reading.peerDiffers = clipped (text (snapshot, "/godot/pair/peerDiffers"));
        reading.peerShowMatch = verdictFlag (text (snapshot, "/godot/pair/peerShowMatch"));
        reading.peerShowDiffers = clipped (text (snapshot, "/godot/pair/peerShowDiffers"));

        reading.roundTrip = text (snapshot, "/godot/pair/roundTrip");
        reading.delay = text (snapshot, "/godot/pair/delay");
        reading.drift = text (snapshot, "/godot/pair/drift");
        reading.clockResidual = text (snapshot, "/godot/pair/clockResidual");
        reading.outputLatency = text (snapshot, "/godot/pair/outputLatency");
        reading.peerOutputLatency = text (snapshot, "/godot/pair/peerOutputLatency");
        reading.received = text (snapshot, "/godot/pair/received");
        reading.gaps = text (snapshot, "/godot/pair/gaps");
        reading.late = text (snapshot, "/godot/pair/late");
        reading.relayed = text (snapshot, "/godot/pair/relayed");
        reading.dropped = text (snapshot, "/godot/pair/dropped");
        reading.unconfirmed = text (snapshot, "/godot/pair/unconfirmed");

        /*  ONLY WHERE IT DECIDES SOMETHING: the walk of the run table is for
            the one button it greys. */
        if (reading.state == "following" && ! reading.showDiffers.empty())
            reading.busy = anyRunLive (snapshot);

        return reading;
    }

    Event eventFor (PairAct act)
    {
        switch (act)
        {
            case PairAct::takeOver:  return gesture::takeOver();
            case PairAct::handOver:  return gesture::handOver();
            case PairAct::keep:      return gesture::keepTheShow();
            case PairAct::realign:   return gesture::realign();
            case PairAct::adoptShow: return gesture::adoptShow();
        }

        return gesture::takeOver();
    }
}
