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

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Dual.h>

#include <wfg/client/model/NewCue.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::client::model
{
    std::string wordFor (Subject::Kind kind)
    {
        switch (kind)
        {
            case Subject::Kind::waveform:  return "waveform";
            case Subject::Kind::sends:     return "sends";
            case Subject::Kind::timeline:  return "timeline";
            case Subject::Kind::curve:     return "curve";
            case Subject::Kind::eq:        return "eq";
            case Subject::Kind::fx:        return "fx";
            case Subject::Kind::take:      return "take";
            case Subject::Kind::fade:      return "fade";
            case Subject::Kind::messages:  return "messages";
            case Subject::Kind::none:      break;
        }

        return {};
    }

    Subject::Kind subjectKindFor (const std::string& word)
    {
        for (const auto kind : { Subject::Kind::waveform, Subject::Kind::sends, Subject::Kind::timeline,
                                 Subject::Kind::curve, Subject::Kind::eq, Subject::Kind::fx,
                                 Subject::Kind::take, Subject::Kind::fade, Subject::Kind::messages })
            if (wordFor (kind) == word)
                return kind;

        return Subject::Kind::none;
    }

    bool followsPick (Subject::Kind kind)
    {
        switch (kind)
        {
            /*  A WAVEFORM FOLLOWS. Clicking the next cue while looking at one
                file's shape means "show me that one", every time - it is the
                same question asked of a different cue, and making somebody
                shut and reopen the panel to ask it would be the panel getting
                in the way of the work.

                What will NOT follow, when they exist, are the subjects that
                are not about the picked cue at all: a rack channel's plugins
                belong to the rack. Each says so here rather than in its own
                editor, so the rule is one list rather than a habit. */
            /*  AND SO DO THE SENDS, for the same reason: "how much of this
                cue goes to the reverb" is a question about whichever cue is
                in hand, and a mixer that stayed pointed at the last one would
                be a mixer that lies. */
            case Subject::Kind::waveform:  return true;
            case Subject::Kind::sends:     return true;

            /*  AND THE TIMELINE FOLLOWS TOO, but only as far as a group: a
                cue picked inside the group being arranged is not a new
                subject, and re-pointing at it would shut the very thing
                somebody is dragging in. `Client` holds it on the group. */
            case Subject::Kind::timeline:  return true;

            /*  AND A CURVE FOLLOWS, which is what makes it able to open by
                itself: picking a fade is the gesture that asks for it. */
            case Subject::Kind::curve:     return true;

            /*  AND THE EQ FOLLOWS, for the sends' reason: how this cue is
                shaped is a question about whichever cue is in hand. */
            case Subject::Kind::eq:        return true;

            /*  AND THE CHAIN, for the same reason, and so does the plugin's
                own window opened from it (author, 2026-09-25): which inserts
                this cue has is a question about the cue in hand. */
            case Subject::Kind::fx:        return true;

            /*  AND THE TAKE (Phase 9c): which take a mic cue plays through is
                a question about the cue in hand, and a later cue on the same
                channel opens on the same take - which is the point. */
            case Subject::Kind::take:      return true;

            /*  AND A FADE'S MIXER, for the curve's reason: it opens when a
                fade is picked (namespace draft §26, PG). */
            case Subject::Kind::fade:      return true;

            /*  AND AN OSC CUE'S MESSAGES, for the waveform's reason: which
                messages this cue sends is a question about the cue in hand
                (namespace draft 45). */
            case Subject::Kind::messages:  return true;
            case Subject::Kind::none:      break;
        }

        return false;
    }

    bool servesMany (Subject::Kind kind)
    {
        return kind == Subject::Kind::sends || kind == Subject::Kind::eq;
    }

    std::vector<std::string> footCues (const tree::TreeSnapshot& snapshot, Subject::Kind kind,
                                       const std::string& anchor, const std::vector<std::string>& picked)
    {
        if (! servesMany (kind) || picked.size() < 2)
            return {};

        const auto serves = [&snapshot] (const std::string& id)
        {
            const auto kindWord = text (snapshot, "/godot/cue/" + id + "/kind");
            return kindWord == "media" || kindWord == "mic";
        };

        std::vector<std::string> out;

        /*  THE ANCHOR FIRST, when the panel serves it: it is the cue clicked
            last on purpose, and the one whose values the panel draws. */
        if (std::find (picked.begin(), picked.end(), anchor) != picked.end() && serves (anchor))
            out.push_back (anchor);

        for (const auto& id : picked)
            if (id != anchor && serves (id))
                out.push_back (id);

        return out;
    }

    std::string partForPanel (Subject::Kind kind)
    {
        switch (kind)
        {
            case Subject::Kind::waveform:  return "time";
            case Subject::Kind::sends:     return "sends";
            case Subject::Kind::eq:        return "eq";
            case Subject::Kind::fx:        return "fx";

            /*  A timeline is a group's members, a curve and a fade's mixer
                are a fade's own, a take is the channel's: none is a part of a
                cue another cue could take. */
            case Subject::Kind::timeline:
            case Subject::Kind::curve:
            case Subject::Kind::take:
            case Subject::Kind::fade:
            case Subject::Kind::messages:
            case Subject::Kind::none:      break;
        }

        return {};
    }

    PartClip readPartClip (const std::string& text)
    {
        /*  READ BY ITS TWO LANDMARKS AND NOTHING MORE, because the engine reads
            the rest and refuses what is wrong: `<Fragment part="…">` and the
            first element after it, which the canonical writer puts on the
            next line with its attributes in name order. */
        PartClip clip;

        const auto start = text.find_first_not_of (" \t\r\n");

        if (start == std::string::npos || text.compare (start, 16, "<Fragment part=\"") != 0)
            return clip;

        const auto wordsFrom = start + 16;
        const auto wordsTo = text.find ('"', wordsFrom);

        if (wordsTo == std::string::npos)
            return clip;

        const auto open = text.find ('<', wordsTo);

        if (open == std::string::npos)
            return clip;

        const auto nameTo = text.find_first_of (" />", open + 1);

        if (nameTo == std::string::npos)
            return clip;

        clip.element = text.substr (open + 1, nameTo - open - 1);

        //  ITS ID, which every cue's element carries.
        const auto tagTo = text.find ('>', open);
        const auto idAt = text.find (" id=\"", open);

        if (idAt != std::string::npos && idAt < tagTo)
        {
            const auto idFrom = idAt + 5;
            const auto idTo = text.find ('"', idFrom);

            if (idTo != std::string::npos)
                clip.sourceId = text.substr (idFrom, idTo - idFrom);
        }

        if (clip.element != "Media" && clip.element != "Mic" && clip.element != "Video")
            return {};

        clip.parts = text.substr (wordsFrom, wordsTo - wordsFrom);
        return clip;
    }

    bool takesPart (const PartClip& clip, const std::string& kind)
    {
        if (! clip.isPart())
            return false;

        //  The kind as the cue's element: "media" is a Media.
        const auto element = kind == "media" ? std::string ("Media")
                            : kind == "mic"   ? std::string ("Mic")
                            : kind == "video" ? std::string ("Video")
                                              : std::string {};

        if (element.empty())
            return false;

        for (const auto& part : words (clip.parts))
        {
            const auto sound = element == "Media" || element == "Mic";
            const auto timed = element == "Media" || element == "Video";

            if ((part == "eq" || part == "sends") && ! sound)
                return false;

            //  WU: a chain only between cues of one kind.
            if (part == "fx" && element != clip.element)
                return false;

            if ((part == "time" || part == "speed") && ! timed)
                return false;

            if (part == "play" && element != "Media")
                return false;

            if (part == "picture" && element != "Video")
                return false;
        }

        return true;
    }

    std::vector<std::string> pasteTargets (const tree::TreeSnapshot& snapshot, const PartClip& clip,
                                           const std::string& panelCue, const std::vector<std::string>& picked)
    {
        const auto takes = [&snapshot, &clip] (const std::string& id)
        {
            return id != clip.sourceId && takesPart (clip, text (snapshot, "/godot/cue/" + id + "/kind"));
        };

        const auto many = std::find (picked.begin(), picked.end(), panelCue) != picked.end() && picked.size() > 1;

        std::vector<std::string> out;

        if (! many)
        {
            if (! panelCue.empty() && takes (panelCue))
                out.push_back (panelCue);

            return out;
        }

        for (const auto& id : picked)
            if (takes (id))
                out.push_back (id);

        return out;
    }

    std::string manyCuesWords (std::size_t acting, std::size_t picked)
    {
        if (acting >= picked)
            return std::to_string (acting) + " cues";

        return std::to_string (acting) + " of " + std::to_string (picked) + " cues";
    }

    std::string footCueForPick (const tree::TreeSnapshot& snapshot, Subject::Kind kind, const std::string& picked)
    {
        const auto dual = dualOf (snapshot, picked);

        if (! dual.isPair())
            return picked;

        switch (kind)
        {
            case Subject::Kind::eq:
            case Subject::Kind::fx:
            case Subject::Kind::sends:
                return dual.sound;

            case Subject::Kind::waveform:
                return dual.movie;

            case Subject::Kind::none:
            case Subject::Kind::timeline:
            case Subject::Kind::curve:
            case Subject::Kind::take:
            case Subject::Kind::fade:
            case Subject::Kind::messages:
                break;
        }

        return picked;
    }

    FootReading readFoot (const tree::TreeSnapshot& snapshot, const Subject& subject,
                          const std::vector<std::string>& picked)
    {
        FootReading out;
        out.subject = subject;

        if (! subject.isOpen() || subject.objectId.empty())
            return out;

        const auto at = [&snapshot] (const std::string& address)
        {
            return text (snapshot, address);
        };

        /*  SEVERAL CUES AT ONCE (namespace draft §30.11, the author's report:
            "it didn't spread to the complete selection"): a send mixer or an
            EQ over a selection acts on every media and mic cue in it, and
            draws the lead's values - the anchor's, when the anchor is one of
            them. One such cue among several picked is simply that cue. */
        if (const auto acting = footCues (snapshot, subject.kind, subject.objectId, picked); ! acting.empty())
        {
            out.subject.objectId = acting.front();

            if (acting.size() > 1)
            {
                out.cues = acting;
                out.picked = picked.size();
            }
        }

        const auto cue = "/godot/cue/" + out.subject.objectId + "/";

        out.cueName = at (cue + "name");
        out.cueKind = at (cue + "kind");

        if (subject.kind == Subject::Kind::waveform)
        {
            out.file = at (cue + "file");
            out.startOffset = osc::parseDouble (at (cue + "startOffset")).value_or (0.0);
            out.fileLength = osc::parseDouble (at (cue + "duration")).value_or (0.0);
            out.rate = osc::parseDouble (at (cue + "rate")).value_or (1.0);
            out.rateMode = at (cue + "rateMode");
            out.ranges = readRanges (snapshot, out.subject.objectId);
            out.lane = readLane (snapshot, out.subject.objectId);
            out.locked = isYes (flag (snapshot, "/godot/document/locked"));

            /*  A SOUND LOCKED TO ITS MOVIE takes its Ranges from the movie
                (namespace draft 37.5, WL): drawn, and edited on the movie. */
            if (const auto movie = at (cue + "lockedTo"); ! movie.empty() && at ("/godot/cue/" + movie + "/kind") == "video")
                out.locked = true;

            if (out.cueKind == "media")
                for (const auto& strip : readSends (snapshot, out.subject.objectId))
                    if (! strip.sendId.empty())
                        out.sendLanes.push_back ({ strip.sendId, strip.name,
                                                   readLaneAt (snapshot, sendLaneAddress (strip.sendId)),
                                                   strip.busId, strip.levelDb });

            /*  AND THE CUE'S OWN LEVEL, which the level lane is an offset on
                (namespace draft §34): a flipped fader rides the number as
                heard, and the waveform draws its ride on the lane's axis. */
            out.cueLevel = osc::parseDouble (at (cue + "level")).value_or (0.0);
            out.laneRecord = readLaneRecord (snapshot);

            /*  WHY THERE IS NOTHING TO DRAW, when there is nothing to draw, in
                the words that say what to do about it. A panel that just sat
                blank would leave somebody wondering whether the file is silent,
                missing, or still being looked at - three different situations
                with three different answers. */
            const auto movie = out.cueKind == "video" && at (cue + "source") == "movie";

            /*  A MOVIE'S OWN SOUND, drawn under its time (§47, AAC): the cue
                locked to it on the line below, as the list draws the pair. */
            out.movie = movie;

            if (movie)
                if (const auto sound = dualOf (snapshot, out.subject.objectId).sound; ! sound.empty())
                    out.soundFile = at ("/godot/cue/" + sound + "/file");

            /*  AND A CUE WITH NO SOUND TO DRAW SAYS PLAINLY THAT THE PANEL
                IS NOT FOR IT (author, 2026-10-07: "make it plain obvious that
                for files that don't have anything to show in the foot panel
                this is not relevant to them ... Check of notes, fades and such
                don't have the same notice") - by its kind, and never in words
                that sound like something still being worked on. */
            if (out.cueKind != "media" && ! movie)
            {
                const auto source = out.cueKind == "video" ? at (cue + "source") : std::string {};
                const auto what = ! source.empty() ? source : out.cueKind.empty() ? std::string ("cue")
                                                                                   : kindWord (out.cueKind);
                const auto article = ! what.empty() && std::string_view ("aeiou").find (what[0]) != std::string_view::npos
                                       ? "an " : "a ";

                out.notice = std::string ("Nothing to show here for ") + article + what
                           + ": only an audio cue or a movie has a waveform and ranges.";
            }
            else if (out.file.empty())
                out.notice = "This cue names no file yet.";
            else if (! (out.fileLength > 0.0))
                out.notice = "The length of this file is not known yet - it is read when the show "
                             "opens, so a file imported in this session has none until the show is "
                             "reopened.";
        }

        if (subject.kind == Subject::Kind::timeline)
            out.timeline = readTimeline (snapshot, out.subject.objectId);

        if (subject.kind == Subject::Kind::curve)
            out.curve = readCurve (snapshot, out.subject.objectId);

        /*  AN OSC CUE'S MESSAGES (namespace draft 45), with the lock read
            here, as the waveform reads its own: nothing on the table is
            written while the show is. */
        if (subject.kind == Subject::Kind::messages)
        {
            out.oscMessages = readOscMessages (snapshot, out.subject.objectId);
            out.oscCurves = readOscCurves (snapshot, out.subject.objectId);
            out.locked = out.oscMessages.locked;

            if (out.oscMessages.rows.empty())
                out.notice = "Not an OSC cue";
        }

        if (subject.kind == Subject::Kind::fade)
        {
            out.fadeMix = readFadeMix (snapshot, out.subject.objectId);

            if (! out.fadeMix.present)
                out.notice = out.fadeMix.notice;
        }

        if (subject.kind == Subject::Kind::eq)
        {
            out.eq = out.cueKind == "fade" ? readFadeEq (snapshot, out.subject.objectId)
                                           : readEq (snapshot, out.subject.objectId);

            //  Over several, every one's - the lead's is `eq`, read again here as the first.
            for (const auto& id : out.cues)
                out.eqs.push_back (id == out.subject.objectId ? out.eq : readEq (snapshot, id));

            if (! out.eq.present)
                out.notice = out.eq.notice;
        }

        if (subject.kind == Subject::Kind::fx)
        {
            out.fx = readFx (snapshot, out.subject.objectId);
            out.eq = readEq (snapshot, out.subject.objectId);

            if (! out.fx.present)
                out.notice = out.fx.notice;
        }

        if (subject.kind == Subject::Kind::take)
        {
            out.take = readTake (snapshot, out.subject.objectId);

            if (! out.take.present)
                out.notice = out.take.notice;
        }

        if (subject.kind == Subject::Kind::sends)
        {
            out.cueLevel = osc::parseDouble (at (cue + "level")).value_or (0.0);
            out.sends = out.many() ? readSendsMany (snapshot, out.cues)
                                   : readSends (snapshot, out.subject.objectId);

            for (const auto& id : out.cues)
                out.cueLevels.push_back (osc::parseDouble (at ("/godot/cue/" + id + "/level")).value_or (0.0));

            if (out.cueKind != "media" && out.cueKind != "mic")
                out.notice = "Only an audio or a mic cue has send levels.";
            else if (out.sends.empty())
                out.notice = "This show declares no mix channels yet - Show, Audio settings, "
                             "Outputs, add a mix channel.";
        }

        /*  "6 CUES" IN THE HEAD, where one cue's name would stand (§30.11). */
        if (out.many())
            out.cueName = manyCuesWords (out.cues.size(), out.picked);

        /*  AND THE PLAYHEAD, from whichever run is sounding this cue. Read from
            the run half rather than from the document, because where a cue has
            got to is a fact about a performance and not about a show. */
        for (const auto* node : snapshot.all())
        {
            constexpr std::string_view runs = "/godot/run/";

            if (node->address.rfind (runs, 0) != 0)
                continue;

            const auto rest = node->address.substr (runs.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos || rest.substr (slash + 1) != "cue")
                continue;

            if (text (node) != out.subject.objectId)
                continue;

            const auto run = "/godot/run/" + rest.substr (0, slash) + "/";
            const auto state = at (run + "state");

            if (state == "playing" || state == "stopping")
            {
                out.running = true;
                out.runId = rest.substr (0, slash);
                out.position = osc::parseDouble (at (run + "position")).value_or (0.0);
                break;
            }
        }

        return out;
    }

    Subject footForSurface (bool pageUp, const std::string& pageWord, const std::string& aim)
    {
        if (! pageUp || aim.empty())
            return {};

        if (pageWord == "eq")
            return { Subject::Kind::eq, aim };

        if (pageWord == "send")
            return { Subject::Kind::sends, aim };

        //  The FX page (2026-09-26): the cue's chain at the foot.
        if (pageWord == "fx")
            return { Subject::Kind::fx, aim };

        //  The Loop page (Phase 9c): the take it rides, at the foot.
        if (pageWord == "loop")
            return { Subject::Kind::take, aim };

        return {};
    }
}
