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

#include <wfg/client/model/Icons.h>

#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>

#include <string>
#include <vector>

namespace wfg::client::model
{
    Icon iconFor (const std::string& kind, const std::string& mode, const std::string& verb)
    {
        if (kind == "group")
        {
            if (mode == "timeline") return Icon::timeline;
            if (mode == "sampler")  return Icon::sampler;
            if (mode == "sequence") return Icon::sequence;

            return Icon::group;
        }

        /*  A TRANSPORT CUE IS RECOGNISED BY WHAT IT DOES, which is its verb:
            the list that makes one offers nine of them (2026-09-27), and a
            stop and a record are not the same cue to somebody reading down. */
        if (kind == "transport")
        {
            if (verb == "fade")                                   return Icon::stopFade;
            if (verb == "afterMember" || verb == "afterIteration") return Icon::stopAfter;
            if (verb == "advance")                                return Icon::advance;
            if (verb == "record")                                 return Icon::record;
            if (verb == "loop")                                   return Icon::loop;
            if (verb == "overdub")                                return Icon::overdub;
            if (verb == "clear")                                  return Icon::clear;

            return Icon::stop;      // hard, and a verb nobody has read yet
        }

        if (kind == "memo")    return Icon::memo;
        if (kind == "media")   return Icon::media;
        if (kind == "mic")     return Icon::mic;
        if (kind == "fade")    return Icon::fade;
        if (kind == "start")   return Icon::start;
        if (kind == "osc")     return Icon::osc;
        if (kind == "midi")    return Icon::midi;
        if (kind == "range")   return Icon::range;
        if (kind == "trigger") return Icon::trigger;

        return Icon::none;
    }

    std::string accentFor (const std::string& kind)
    {
        /*  ONE TOKEN PER KIND THAT HAS ONE, named after it, so a kind's accent
            is found in the theme file under the kind's own word. A start cue
            is one of the cues that act on a cue, and shares the transport's. */
        if (kind == "memo" || kind == "media" || kind == "mic" || kind == "fade"
              || kind == "transport" || kind == "osc" || kind == "midi" || kind == "group")
            return "kind-" + kind;

        if (kind == "start")
            return "kind-transport";

        return "ink-faint";
    }

    Icon iconForPanel (const std::string& subject)
    {
        if (subject == "waveform") return Icon::waveform;
        if (subject == "sends")    return Icon::sends;
        if (subject == "curve")    return Icon::curve;
        if (subject == "timeline") return Icon::timeline;
        if (subject == "take")     return Icon::take;

        return Icon::none;
    }

    Icon iconForDrawer (const std::string& heading, const std::string& kind, const std::string& mode)
    {
        if (heading == "what it is")   return Icon::identity;
        if (heading == "when")         return Icon::clock;
        if (heading == "what it does") return iconFor (kind, mode);
        if (heading == "sound")        return Icon::sound;
        if (heading == "speed")        return Icon::speed;
        if (heading == "sampler")      return Icon::sampler;
        if (heading == "sampling")     return Icon::take;
        if (heading == "in the list")  return Icon::list;
        if (heading == "details")      return Icon::info;

        return Icon::none;
    }

    namespace
    {
        /*  A SPEED AS A MARK SAYS IT: the window's own spelling ("×0.5"),
            and at nought - which is a frozen cue, not a very slow one - "×0". */
        std::string speedMark (const std::string& rate)
        {
            const auto value = osc::parseDouble (rate);

            if (! value.has_value())
                return {};

            if (! (*value > 0.0))
                return "\xc3\x97" "0";

            return speedText (*value);
        }
    }

    std::vector<Mark> marksFor (const Row& row)
    {
        std::vector<Mark> marks;

        /*  WHETHER IT PLAYS AT ALL comes first: a disabled row is dimmed, and
            this is the shape beside the dimming, for somebody who cannot tell
            one grey from another. */
        if (! row.enabled)
            marks.push_back ({ Icon::disabled, {}, "disabled: skipped, not deleted" });

        if (row.isGroup)
        {
            /*  HOW MANY ROUNDS: a number beside the loop, or for ever - which
                is what nought means for a group and is exactly what a bare
                count could not say. */
            if (row.loops == "0")
                marks.push_back ({ Icon::forever, {}, "loops for ever" });
            else if (! row.loops.empty() && row.loops != "1")
                marks.push_back ({ Icon::loop, row.loops, "plays " + row.loops + " rounds" });

            if (row.selection == "shuffle")
                marks.push_back ({ Icon::shuffle, {}, "shuffled: a fresh order each round" });

            /*  N OF M, when a round plays fewer members than the group has -
                which is what `play` means; nought and anything at or above the
                count are the ordinary "all of them". */
            if (const auto play = osc::parseDouble (row.play); play.has_value() && *play > 0.0)
            {
                const auto count = static_cast<std::size_t> (*play);

                if (row.members == 0 || count < row.members)
                    marks.push_back ({ Icon::subset,
                                       std::to_string (count)
                                           + (row.members > 0 ? " of " + std::to_string (row.members)
                                                              : std::string {}),
                                       "plays " + std::to_string (count) + " members a round" });
            }

            /*  ONE AFTER ANOTHER WITHOUT A GO, which only a sequence does -
                a timeline's members start together, and a sampler's wait for
                a hand. */
            if (row.advance == "auto" && (row.mode == "sequence" || row.mode.empty()))
                marks.push_back ({ Icon::follow, {}, "each member follows the one before, with no GO" });
        }

        if (row.kind == "media" && ! row.rate.empty())
        {
            if (const auto said = speedMark (row.rate); ! said.empty())
                marks.push_back ({ row.rateMode == "timestretch" ? Icon::stretch : Icon::speed, said,
                                   row.rateMode == "timestretch" ? "plays at " + said + ", its pitch kept"
                                                                 : "plays at " + said + ", its pitch moved with it" });
        }

        if (row.lane)
            marks.push_back ({ Icon::lane, {}, "a level lane is drawn over its file" });

        if (row.kind == "fade")
        {
            if (row.rateOn)
            {
                auto said = speedMark (row.rate);

                //  To one is a real destination for a fade, unlike for a cue's own speed.
                if (said.empty() && osc::parseDouble (row.rate).has_value())
                    said = "\xc3\x97" "1";

                marks.push_back ({ Icon::speed, said, "moves its target's speed to " + said });
            }

            if (row.stopWhenDone)
                marks.push_back ({ Icon::stop, {}, "stops what it faded, once it arrives" });
        }

        if (! row.dca.empty())
            marks.push_back ({ Icon::dca, row.dca, "answers to the DCA " + row.dca });

        /*  PREPARED BY AN ANCESTOR'S HEADER, on the cue's own row. The line
            the header draws for it already says "preset" in words. */
        if (! row.preset.empty() && ! row.derived)
            marks.push_back ({ Icon::preset, {}, "a preset: prepared by an enclosing group's header" });

        return marks;
    }
}
