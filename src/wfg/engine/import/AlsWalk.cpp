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

#include <wfg/engine/import/AlsWalk.h>

#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace wfg::import::als
{
    namespace
    {
        //======================================================================
        //  Numbers.

        bool same (double a, double b, double tolerance = 1.0e-9) noexcept
        {
            return std::abs (a - b) <= tolerance;
        }

        double gainOfDb (double db) noexcept
        {
            return std::pow (10.0, db / 20.0);
        }

        /*  Live's returns are named with their letter in front - "A-Face" - and
            the letter is Live's, not the designer's. */
        std::string withoutLetter (const std::string& name)
        {
            if (name.size() > 2 && name[1] == '-' && std::isupper (static_cast<unsigned char> (name[0])) != 0)
                return name.substr (2);

            return name;
        }

        //======================================================================
        //  An envelope as a curve of decibels against the CLIP's beats.

        /*  WHAT AN ENVELOPE'S POINT IS IN DECIBELS: a volume or a send is a
            gain; a modulation scales the clip from silence at -1 to unity at 1. */
        double pointDb (const Envelope& envelope, double value, const Laws& laws)
        {
            if (envelope.target.what == Parameter::modulation)
            {
                const auto factor = (value + 1.0) * 0.5;
                return factor <= 1.0e-6 ? laws.silenceDb : std::max (laws.silenceDb, 20.0 * std::log10 (factor));
            }

            return decibels (value, laws);
        }

        /*  The envelope's level at a beat of its clip, by the law that joins its
            points (QU): before its first point the first's, after its last the
            last's. */
        double envelopeDb (const Envelope& envelope, double beats, const Laws& laws)
        {
            const auto& points = envelope.points;

            if (points.empty())
                return 0.0;

            if (beats <= points.front().beats)
                return pointDb (envelope, points.front().value, laws);

            for (std::size_t at = 1; at < points.size(); ++at)
            {
                const auto& a = points[at - 1];
                const auto& b = points[at];

                if (beats > b.beats)
                    continue;

                const auto span = b.beats - a.beats;
                const auto fraction = span > 0.0 ? (beats - a.beats) / span : 1.0;

                if (envelope.target.what == Parameter::modulation
                      || laws.volume == Laws::Volume::straightInGain)
                {
                    const auto value = a.value + (b.value - a.value) * fraction;
                    return pointDb (envelope, value, laws);
                }

                const auto from = pointDb (envelope, a.value, laws);
                const auto to = pointDb (envelope, b.value, laws);
                return from + (to - from) * fraction;
            }

            return pointDb (envelope, points.back().value, laws);
        }

        bool moves (const Envelope& envelope)
        {
            for (std::size_t at = 1; at < envelope.points.size(); ++at)
                if (! same (envelope.points[at].value, envelope.points[0].value, 1.0e-7))
                    return true;

            return false;
        }

        double lastValue (const Envelope& envelope)
        {
            return envelope.points.empty() ? 0.0 : envelope.points.back().value;
        }

        //======================================================================
        //  The graph: which tracks feed which, and where it all comes out.

        /*  ONE FACTOR OF A PATH: a track's volume, or one of its sends. */
        struct Term
        {
            std::string trackId;
            Parameter what = Parameter::volume;
            int send = -1;

            bool operator== (const Term& other) const
            {
                return trackId == other.trackId && what == other.what && send == other.send;
            }
        };

        struct Path
        {
            std::string mix;
            std::vector<Term> terms;
            std::optional<double> pan;  ///< a direct out sent into an owner: its side (BDT)
        };

        /*  WHAT A PARAMETER STANDS AT, held between the clips that move it
            (QU's third law): a track's volume and sends, by track Id. */
        struct Held
        {
            double volume = 1.0;
            std::vector<double> sends;
            bool active = true;         ///< the track activator, as the scenes leave it
        };

        struct Graph
        {
            const LiveSet& set;
            const Laws& laws;
            std::map<std::string, const Track*> byId;
            std::map<std::string, int> returnIndex;
            std::vector<Mix> mixes;
            std::vector<Note>& notes;

            Graph (const LiveSet& setToUse, const Laws& lawsToUse, std::vector<Note>& notesToUse)
                : set (setToUse), laws (lawsToUse), notes (notesToUse)
            {
                for (const auto& track : set.tracks)
                    byId[track.id] = &track;

                for (std::size_t at = 0; at < set.returns.size(); ++at)
                {
                    byId[set.returns[at].id] = &set.returns[at];
                    returnIndex[set.returns[at].id] = static_cast<int> (at);
                }

                buildMixes();
            }

            const Track* track (const std::string& id) const
            {
                const auto found = byId.find (id);
                return found == byId.end() ? nullptr : found->second;
            }

            const Mix* mixOf (const std::string& key) const
            {
                for (const auto& one : mixes)
                    if (one.key == key)
                        return &one;

                return nullptr;
            }

            /*  THE MIXES: each return that reaches the interface, and the master
                when anything is sent to it, in the order of their pairs. A
                return carrying devices or sending into another is an aux
                (namespace draft §93, answering QE). Then every track wired
                straight to the interface, a direct out (§121, BDS), and the
                channels shared out among them (BDT). */
            void buildMixes()
            {
                const auto pairOf = [this] (const Routing& output, bool& mono) -> int
                {
                    mono = false;

                    if (output.kind == Routing::Kind::external)
                    {
                        mono = ! output.stereo;
                        return output.channel;
                    }

                    if (output.kind == Routing::Kind::master && set.master.has_value()
                          && set.master->output.kind == Routing::Kind::external)
                    {
                        mono = ! set.master->output.stereo;
                        return set.master->output.channel;
                    }

                    return -1;
                };

                for (std::size_t at = 0; at < set.returns.size(); ++at)
                {
                    const auto& ret = set.returns[at];

                    Mix mix;
                    mix.key = "return:" + std::to_string (at);
                    mix.name = withoutLetter (ret.name);
                    mix.pair = pairOf (ret.output, mix.mono);
                    mix.levelDb = decibels (ret.volume, laws);

                    /*  ITS DEVICES SORTED: an EQ Eight is the aux's EQ, a
                        plugin of somebody else's is carried onto its chain, and
                        Live's own are noted. A Utility (`StereoGain`) is passed
                        over, as it always was. */
                    for (const auto& device : ret.devices)
                    {
                        const auto called = device.name.empty() ? device.kind : device.name;
                        mix.effects.push_back (called);

                        if (device.kind == "StereoGain")
                            continue;

                        mix.aux = true;

                        if (device.kind == "Eq8")
                            mix.eqs.push_back (&device);
                        else if (device.plugin.has_value())
                            mix.plugins.push_back (&device);
                        else
                            mix.liveDevices.push_back (called);
                    }

                    if (mix.pair < 0 && ! mix.aux)
                        notes.push_back ({ Note::Kind::dropped, -1, ret.name,
                                           "the return reaches no output of the interface, so nothing sent to it "
                                           "is heard" });

                    if (ret.output.kind == Routing::Kind::master)
                        mix.levelDb += set.master.has_value() ? decibels (set.master->volume, laws) : 0.0;

                    mixes.push_back (std::move (mix));
                }

                if (set.master.has_value())
                {
                    Mix master;
                    master.key = "master";
                    master.name = "Master";
                    master.pair = pairOf (set.master->output, master.mono);
                    master.levelDb = decibels (set.master->volume, laws);
                    mixes.push_back (std::move (master));
                }

                /*  A TRACK WIRED STRAIGHT TO THE INTERFACE (namespace draft
                    §121, BDS): an output of its own, named after it. */
                for (const auto& track : set.tracks)
                {
                    if (track.output.kind != Routing::Kind::external || track.output.channel < 0)
                        continue;

                    Mix direct;
                    direct.key = "direct:" + track.id;
                    direct.name = track.name;
                    direct.direct = true;
                    direct.mono = ! track.output.stereo;
                    direct.pair = track.output.channel;
                    mixes.push_back (std::move (direct));
                }

                /*  A RETURN SENDING INTO ANOTHER (namespace draft §93, AVW): it
                    is an aux, and the send is its own - into an aux at the
                    send's level, into a mix with that mix's fader folded in, as
                    a sound's sends are. Live does not send a return into itself. */
                for (std::size_t at = 0; at < set.returns.size() && at < mixes.size(); ++at)
                    for (std::size_t into = 0; into < set.returns[at].sends.size() && into < set.returns.size(); ++into)
                        if (into != at && decibels (set.returns[at].sends[into], laws) > laws.floorDb + 0.01)
                            mixes[at].aux = true;

                shareChannels();

                for (std::size_t at = 0; at < set.returns.size() && at < mixes.size(); ++at)
                {
                    auto& mix = mixes[at];
                    const auto& ret = set.returns[at];

                    if (! mix.aux)
                    {
                        continue;
                    }

                    for (std::size_t into = 0; into < ret.sends.size() && into < set.returns.size(); ++into)
                    {
                        const auto db = decibels (ret.sends[into], laws);

                        if (into == at || db <= laws.floorDb + 0.01)
                            continue;

                        const auto& target = mixes[into];

                        if (! target.aux && target.pair < 0)
                            continue;

                        SendOut send;
                        send.mix = target.key;
                        send.levelDb = std::clamp (db + (target.aux ? 0.0 : target.levelDb), laws.silenceDb, 12.0);
                        mix.sends.push_back (send);
                    }

                    if (! mix.liveDevices.empty())
                        notes.push_back ({ Note::Kind::approximated, -1, ret.name,
                                           "is an aux in Go.dot, and its " + joined (mix.liveDevices)
                                           + (mix.liveDevices.size() == 1 ? " is" : " are")
                                           + " Live's own: the aux's chain is left for a plugin of yours"
                                             " (Show settings, Rack) - its sends come over on" });

                    for (const auto* plugin : mix.plugins)
                        notes.push_back ({ Note::Kind::info, -1, ret.name,
                                           "its " + (plugin->name.empty() ? plugin->kind : plugin->name)
                                           + " is carried onto the aux's chain, with the state Live saved" });

                    if (mix.eqs.size() > 1)
                        notes.push_back ({ Note::Kind::approximated, -1, ret.name,
                                           std::to_string (mix.eqs.size()) + " EQ Eights on the return; the aux's "
                                           "EQ takes the first" });

                    if (mix.pair < 0 && mix.into.empty() && mix.sends.empty())
                        notes.push_back ({ Note::Kind::info, -1, ret.name,
                                           "the aux reaches no output of the interface itself: only what it sends "
                                           "into another return is heard" });
                }
            }

            /*  TWO ON ONE PAIR (namespace draft §121, BDT). The owner of a set
                of channels is the widest mix on them, then the first in Live's
                order; another whose channels all lie inside one owner's gives
                up its output and goes into the owner at 0 dB - a return as an
                aux with a send, a direct out by its track's path - a mono one
                panned to the side it landed on. One straddling two owners
                keeps nothing: Go.dot's patch is one bus a channel. */
            void shareChannels()
            {
                const auto channelsOf = [] (const Mix& one)
                {
                    std::vector<int> out;

                    if (one.pair < 0)
                        return out;

                    if (one.mono)
                        out.push_back (one.pair);
                    else
                        out.insert (out.end(), { 2 * one.pair, 2 * one.pair + 1 });

                    return out;
                };

                std::vector<std::size_t> order;

                for (std::size_t at = 0; at < mixes.size(); ++at)
                    if (mixes[at].pair >= 0)
                        order.push_back (at);

                std::stable_sort (order.begin(), order.end(), [this] (std::size_t a, std::size_t b)
                {
                    return ! mixes[a].mono && mixes[b].mono;
                });

                std::map<int, std::size_t> ownerOf;

                for (const auto at : order)
                {
                    auto& one = mixes[at];
                    const auto channels = channelsOf (one);
                    std::set<std::size_t> owners;

                    for (const auto channel : channels)
                        if (const auto found = ownerOf.find (channel); found != ownerOf.end())
                            owners.insert (found->second);

                    if (owners.empty())
                    {
                        for (const auto channel : channels)
                            ownerOf[channel] = at;

                        continue;
                    }

                    const auto whole = owners.size() == 1
                                         && std::all_of (channels.begin(), channels.end(),
                                                         [&ownerOf] (int channel) { return ownerOf.count (channel) != 0; });

                    if (! whole)
                    {
                        std::string names;

                        for (const auto owner : owners)
                            names += (names.empty() ? "\"" : "\" and \"") + mixes[owner].name;

                        notes.push_back ({ Note::Kind::dropped, -1, one.name, "its outputs lie across " + names
                                           + "\"'s, which Go.dot's patch cannot share: it keeps no output of its own"
                                             " (namespace draft §121, BDT)" });
                        one.pair = -1;
                        continue;
                    }

                    const auto& owner = mixes[*owners.begin()];
                    std::optional<double> pan;

                    if (one.mono && ! owner.mono)
                        pan = channels.front() == 2 * owner.pair ? -1.0 : 1.0;

                    const auto side = pan.has_value() ? std::string (", panned to the side it was on") : std::string();
                    one.pair = -1;

                    if (one.direct)
                    {
                        one.into = owner.key;
                        one.pan = pan;
                        notes.push_back ({ Note::Kind::approximated, -1, one.name, "goes straight to the interface on "
                                           "channels \"" + owner.name + "\" holds: it is sent into that mix instead"
                                           + side + " (BDT)" });
                        continue;
                    }

                    one.aux = true;

                    SendOut send;
                    send.mix = owner.key;
                    send.levelDb = std::clamp (owner.aux ? 0.0 : owner.levelDb, laws.silenceDb, 12.0);
                    send.pan = pan;
                    one.sends.push_back (std::move (send));

                    notes.push_back ({ Note::Kind::approximated, -1, one.name, "shares its outputs with \"" + owner.name
                                       + "\": it is an aux with no output of its own, sent into that mix at 0 dB"
                                       + side + " (namespace draft §121, BDT)" });
                }
            }

            static std::string joined (const std::vector<std::string>& words)
            {
                std::string out;

                for (const auto& word : words)
                    out += (out.empty() ? "" : ", ") + word;

                return out;
            }

            /*  EVERY PATH FROM A TRACK TO A MIX, each the product of the faders
                and sends it crosses (QH). `prefix` is what the sound has already
                been through. A send before the fader leaves the fader out. */
            void paths (const std::string& trackId, std::vector<Term> prefix, std::vector<Path>& out,
                        int depth) const
            {
                const auto* node = track (trackId);

                if (node == nullptr || depth > 8)
                    return;

                const auto withVolume = [&prefix, &trackId]
                {
                    auto terms = prefix;
                    terms.push_back ({ trackId, Parameter::volume, -1 });
                    return terms;
                };

                /*  A RETURN IS A DESTINATION: what reaches it is its mix. */
                if (const auto found = returnIndex.find (trackId); found != returnIndex.end())
                {
                    out.push_back ({ "return:" + std::to_string (found->second), prefix });
                    return;
                }

                for (std::size_t send = 0; send < node->sends.size() && send < set.returns.size(); ++send)
                {
                    const auto pre = send < set.sendsPre.size() && set.sendsPre[send];
                    auto terms = pre ? prefix : withVolume();
                    terms.push_back ({ trackId, Parameter::send, static_cast<int> (send) });
                    out.push_back ({ "return:" + std::to_string (send), terms });
                }

                switch (node->output.kind)
                {
                    case Routing::Kind::track:
                        paths (node->output.trackId, withVolume(), out, depth + 1);
                        break;

                    case Routing::Kind::group:
                        paths (node->groupId, withVolume(), out, depth + 1);
                        break;

                    case Routing::Kind::master:
                        out.push_back ({ "master", withVolume() });
                        break;

                    case Routing::Kind::external:
                        /*  ITS OWN OUTPUT (BDS), or the mix that holds those
                            channels (BDT). */
                        if (const auto* direct = mixOf ("direct:" + trackId); direct != nullptr)
                        {
                            if (! direct->into.empty())
                                out.push_back ({ direct->into, withVolume(), direct->pan });
                            else if (direct->pair >= 0)
                                out.push_back ({ direct->key, withVolume() });
                        }
                        break;

                    case Routing::Kind::none:
                    case Routing::Kind::other:
                        break;
                }
            }
        };

        //======================================================================
        //  A curve's thinning, and its sampling over a clip.

        /*  RAMER-DOUGLAS-PEUCKER over (seconds, decibels), the error measured on
            levels clamped at -60 dB: below that a difference is one nobody
            hears, and a curve diving to silence would otherwise keep every
            sample of its tail. */
        void thinInto (const std::vector<CurvePoint>& points, std::size_t from, std::size_t to, double tolerance,
                       std::vector<bool>& keep)
        {
            if (to <= from + 1)
                return;

            const auto clamp = [] (double db) { return std::max (db, -60.0); };

            const auto& a = points[from];
            const auto& b = points[to];
            const auto span = b.seconds - a.seconds;

            double worst = 0.0;
            std::size_t worstAt = from;

            for (auto at = from + 1; at < to; ++at)
            {
                const auto fraction = span > 0.0 ? (points[at].seconds - a.seconds) / span : 0.0;
                const auto line = clamp (a.db) + (clamp (b.db) - clamp (a.db)) * fraction;
                const auto distance = std::abs (clamp (points[at].db) - line);

                if (distance > worst)
                {
                    worst = distance;
                    worstAt = at;
                }
            }

            if (worst > tolerance)
            {
                keep[worstAt] = true;
                thinInto (points, from, worstAt, tolerance, keep);
                thinInto (points, worstAt, to, tolerance, keep);
            }
        }

        bool flat (const std::vector<CurvePoint>& points, double tolerance = 0.05)
        {
            if (points.empty())
                return true;

            const auto [low, high] = std::minmax_element (points.begin(), points.end(),
                                                          [] (const CurvePoint& a, const CurvePoint& b)
                                                          { return a.db < b.db; });
            return high->db - low->db <= tolerance;
        }

        bool silentThroughout (const std::vector<CurvePoint>& points, const Laws& laws)
        {
            return std::all_of (points.begin(), points.end(),
                                [&laws] (const CurvePoint& point) { return point.db <= laws.floorDb + 0.01; });
        }
    }

    //==========================================================================
    double decibels (double gain, const Laws& laws)
    {
        const auto floorGain = gainOfDb (laws.floorDb);

        if (! (gain > floorGain * 1.0001))
            return laws.floorDb;

        return 20.0 * std::log10 (gain);
    }

    std::vector<CurvePoint> thin (const std::vector<CurvePoint>& points, double tolerance)
    {
        if (points.size() <= 2)
            return points;

        std::vector<bool> keep (points.size(), false);
        keep.front() = keep.back() = true;

        /*  WHERE A CURVE GOES IN OR OUT OF THE QUIET, both sides are kept: below
            -60 dB the error is measured as -60, so a corner there - a ramp
            arriving at the bottom of the fader, the silence it holds after -
            would otherwise be thinned away with the levels nobody hears. */
        for (std::size_t at = 1; at < points.size(); ++at)
            for (const auto quiet : { -60.0, -119.99 })
                if ((points[at].db <= quiet) != (points[at - 1].db <= quiet))
                    keep[at] = keep[at - 1] = true;

        thinInto (points, 0, points.size() - 1, tolerance, keep);

        std::vector<CurvePoint> out;

        for (std::size_t at = 0; at < points.size(); ++at)
            if (keep[at])
                out.push_back (points[at]);

        return out;
    }

    bool sceneDoesSomething (const LiveSet& set, int scene)
    {
        if (scene < 0)
            return false;

        const auto index = static_cast<std::size_t> (scene);

        for (const auto& track : set.tracks)
        {
            if (track.kind == Track::Kind::group || index >= track.slots.size())
                continue;

            const auto& slot = track.slots[index];

            if (slot.clip.has_value() || slot.hasStop)
                return true;
        }

        return false;
    }

    //==========================================================================
    namespace
    {
        /*  WHAT IS PLAYING, AND WHERE EVERYTHING STANDS, as the scenes go by. */
        struct State
        {
            std::map<std::string, std::string> playing;     ///< source track Id to the sound's key
            std::map<std::string, Sound*> sounds;           ///< a sound's key to the sound itself
            std::map<std::string, const Clip*> soundClip;   ///< a sound's key to its clip
            std::map<std::string, Held> held;               ///< track Id to where its mixer stands
            std::map<std::string, double> rate;             ///< a sound's key to the speed it plays at now (BDR)
        };

        struct Walker
        {
            const LiveSet& set;
            const WalkOptions& options;
            const Laws& laws;
            Walk out;
            Graph graph;
            State state;

            /*  THE TEMPO AS THE SCENES GO BY (namespace draft §121, BDR): the
                set's, then each scene's that sets one, from that scene on - a
                night runs them in order, and Live keeps the last it was given. */
            double beatsPerRealSecond;

            /*  THE CONTROLS THE SET'S OWN MIDI CLIPS SEND (namespace draft
                §121, BDX, BDY): channel and controller, from every MIDI clip on
                a track with an output port. A mapping on one of these is driven
                by the set, not by a hand. */
            std::set<std::pair<int, int>> midiFed;
            std::set<std::string> loopbackSaid;

            Walker (const LiveSet& setToUse, const WalkOptions& optionsToUse)
                : set (setToUse), options (optionsToUse), laws (optionsToUse.laws),
                  graph (setToUse, optionsToUse.laws, out.notes),
                  beatsPerRealSecond ((setToUse.tempo > 0.0 ? setToUse.tempo : 120.0) / 60.0)
            {
                for (const auto& track : set.tracks)
                    state.held[track.id] = { track.volume, track.sends, track.active };

                for (const auto& ret : set.returns)
                    state.held[ret.id] = { ret.volume, ret.sends, ret.active };

                for (const auto& track : set.tracks)
                {
                    if (track.kind != Track::Kind::midi || track.midiOut.channel <= 0)
                        continue;

                    for (const auto& slot : track.slots)
                        if (slot.clip.has_value())
                            for (const auto& envelope : slot.clip->envelopes)
                                if (const auto cc = controllerOf (envelope); cc >= 0)
                                    midiFed.insert ({ track.midiOut.channel, cc });
                }

                out.mixes = graph.mixes;
            }

            /** The controller an envelope of a MIDI clip sends, or -1. */
            static int controllerOf (const Envelope& envelope)
            {
                if (envelope.target.what != Parameter::controller)
                    return -1;

                const auto number = osc::parseDouble (envelope.target.path).value_or (-1.0);
                return std::isfinite (number) && number >= 0.0 && number <= 127.0 ? static_cast<int> (number) : -1;
            }

            bool fedByClip (const Mapping& mapping) const
            {
                return ! mapping.note && midiFed.count ({ mapping.channel, mapping.number }) != 0;
            }

            /*  A CONTROLLER'S VALUE ON WHAT IT MOVES (namespace draft §121, BDW),
                provisional until the probe set measures it: a fader straight in
                decibels from the mapping's floor to 0 dB at 109 of 127 and on
                to its top at 127 - a control whose top is 0 dB, a send, straight
                from its floor to the top; a switch on at 64; anything else
                straight between the mapping's ends. In the parameter's own
                units: a gain for a fader or a send. */
            double controllerValue (double value, const Mapping& mapping) const
            {
                const auto x = std::clamp (value, 0.0, 127.0);

                switch (mapping.target.what)
                {
                    case Parameter::volume:
                    case Parameter::send:
                    {
                        const auto floorDb = decibels (mapping.rangeMin, laws);
                        const auto topDb = decibels (mapping.rangeMax, laws);
                        double db;

                        if (topDb <= 0.01)
                            db = floorDb + (topDb - floorDb) * x / 127.0;
                        else if (x <= 109.0)
                            db = floorDb + (0.0 - floorDb) * x / 109.0;
                        else
                            db = topDb * (x - 109.0) / 18.0;

                        return db <= laws.floorDb + 0.01 ? gainOfDb (laws.floorDb) : gainOfDb (db);
                    }

                    case Parameter::active:
                        return x >= 64.0 ? 1.0 : 0.0;

                    case Parameter::pan:
                    case Parameter::device:
                    case Parameter::modulation:
                    case Parameter::otherModulation:
                    case Parameter::controller:
                    case Parameter::unknown:
                        return mapping.rangeMin + (mapping.rangeMax - mapping.rangeMin) * x / 127.0;
                }

                return 0.0;
            }

            /*  A LIVE INPUT (namespace draft §121, BDU): a track listening with
                its monitoring In to an input of the interface. Its clips are
                carriers still (isCarrier), shaping what it hears. */
            static bool isLive (const Track& track)
            {
                return track.kind == Track::Kind::audio && track.monitoring == Track::Monitoring::in
                         && track.input.kind == Routing::Kind::external && track.input.channel >= 0;
            }

            const Mix* mixNamed (const std::string& key) const
            {
                for (const auto& mix : out.mixes)
                    if (mix.key == key)
                        return &mix;

                return nullptr;
            }

            /*  A TRACK WHOSE MONITORING IS IN keeps its own clips silent, whatever
                its input says - Lazzi's "6 track in" listens to None and is still
                fed, by track 6's output - so its clips are carriers. */
            static bool isCarrier (const Track& track)
            {
                return track.kind == Track::Kind::audio && track.monitoring == Track::Monitoring::in;
            }

            /*  A CLIP'S LOOP POINTS IN ITS BEATS (namespace draft §121, BDQ):
                Live stores an unwarped clip's as SECONDS of the file - the
                Sturbzep organ's end at 402.55 with its end marker at 912.46
                beats of the markers' 136 BPM, both the file's 402.55 s - and
                the markers' slope, the clip's own tempo, is what turns them
                into the beats its envelopes are drawn in. A warped clip's are
                beats already. */
            static double clipBeats (const Clip& clip, double loopValue)
            {
                if (clip.warped)
                    return loopValue;

                const auto perFileSecond = beatsPerSecond (clip);
                return perFileSecond > 0.0 ? loopValue * perFileSecond : loopValue;
            }

            /** The beat a clip starts playing from. */
            static double startBeatOf (const Clip& clip)
            {
                return clipBeats (clip, clip.loopOn ? clip.loopStart + clip.startRelative : clip.loopStart);
            }

            /*  WHETHER ONE TRACK FEEDS ANOTHER: its output is that track, or that
                track's input is it. */
            static bool feeds (const Track& source, const Track& into)
            {
                return (source.output.kind == Routing::Kind::track && source.output.trackId == into.id)
                         || (into.input.kind == Routing::Kind::track && into.input.trackId == source.id);
            }

            /** The sounding track a carrier shapes, or none. */
            const Track* sourceFor (const Track& carrier) const
            {
                for (const auto& track : set.tracks)
                    if (! isCarrier (track) && track.kind == Track::Kind::audio && feeds (track, carrier))
                        return &track;

                return nullptr;
            }

            static const Envelope* envelopeOn (const Clip& clip, const std::string& trackId, Parameter what,
                                               int send = -1)
            {
                for (const auto& envelope : clip.envelopes)
                    if (envelope.target.trackId == trackId && envelope.target.what == what
                          && ! envelope.target.relative && (send < 0 || envelope.target.sendIndex == send))
                        return &envelope;

                return nullptr;
            }

            /*  ONE TERM'S CURVE OVER THE SOUND: its own clip's envelope, or the
                envelope of the clip launched on that term's track in the same
                scene - shifted onto the sound's beats, both clips running from
                one GO at one tempo - or where its track's mixer stands. */
            struct Source
            {
                const Envelope* envelope = nullptr;
                double beatOffset = 0.0;    ///< added to the sound's beat to give the envelope's clip's
                double heldDb = 0.0;
            };

            Source sourceOf (const Term& term, const Clip& clip, const std::string& soundTrack, double soundStart,
                             const std::map<std::string, std::pair<const Clip*, double>>& launchedHere) const
            {
                Source source;
                const auto& held = state.held.at (term.trackId);
                const auto value = term.what == Parameter::volume ? held.volume
                                 : term.send >= 0 && static_cast<std::size_t> (term.send) < held.sends.size()
                                     ? held.sends[static_cast<std::size_t> (term.send)] : 0.0;
                source.heldDb = decibels (value, laws);

                if (term.trackId == soundTrack)
                {
                    source.envelope = envelopeOn (clip, term.trackId, term.what, term.send);
                    return source;
                }

                if (const auto found = launchedHere.find (term.trackId); found != launchedHere.end())
                {
                    source.envelope = envelopeOn (*found->second.first, term.trackId, term.what, term.send);
                    source.beatOffset = found->second.second - soundStart;
                }

                return source;
            }

            double valueOf (const Source& source, double beat) const
            {
                return source.envelope != nullptr ? envelopeDb (*source.envelope, beat + source.beatOffset, laws)
                                                  : source.heldDb;
            }

            /*  A PATH'S LEVEL: its terms' decibels summed - and silence when any
                one of them rests on the bottom of its fader, which in Live is
                minus infinity, not minus seventy. A send at the floor plus a
                fader a little up is no sound at all. */
            double sumDb (const std::vector<Source>& sources, double beat) const
            {
                double db = 0.0;

                for (const auto& source : sources)
                {
                    const auto value = valueOf (source, beat);

                    if (value <= laws.floorDb + 0.01)
                        return laws.floorDb;

                    db += value;
                }

                return db;
            }

            /*  THE BEATS A CURVE IS SAMPLED AT over a sound: every point of every
                envelope that shapes it, either side by a hair so a jump is a
                five-millisecond ramp rather than a lost step, and a grid where
                any of them moves. */
            std::vector<double> sampleBeats (const std::vector<Source>& sources, double from, double to,
                                             bool grid) const
            {
                std::vector<double> beats { from, to };
                const auto hair = 0.0025 * beatsPerRealSecond;

                for (const auto& source : sources)
                {
                    if (source.envelope == nullptr)
                        continue;

                    for (const auto& point : source.envelope->points)
                    {
                        const auto at = point.beats - source.beatOffset;

                        if (at > from && at < to)
                        {
                            beats.push_back (std::max (from, at - hair));
                            beats.push_back (at);
                            beats.push_back (std::min (to, at + hair));
                        }
                    }
                }

                if (grid)
                {
                    /*  BOUNDED: a loop end of a million years, or a step of nothing, is a
                        file that lies, and a grid with no end is an import that never
                        returns. At most `maxGrid` points, the step widened to fit; none
                        where the span or the step is not a number. */
                    constexpr double maxGrid = 100000.0;
                    auto step = laws.sampleSeconds * beatsPerRealSecond;

                    if (std::isfinite (from) && std::isfinite (to) && std::isfinite (step) && step > 0.0)
                    {
                        step = std::max (step, (to - from) / maxGrid);

                        for (auto at = from + step; at < to; at += step)
                            beats.push_back (at);
                    }
                }

                std::sort (beats.begin(), beats.end());
                beats.erase (std::unique (beats.begin(), beats.end(),
                                          [] (double a, double b) { return std::abs (a - b) < 1.0e-9; }),
                             beats.end());
                return beats;
            }

            /*  A CURVE OVER THE CLIP'S BEATS, moved onto the FILE's seconds by
                the clip's warp - nothing before the file's start, which a lane
                cannot name, the value there taken for it - and thinned. */
            std::vector<CurvePoint> onFile (const Clip& clip, const std::vector<std::pair<double, double>>& byBeat) const
            {
                std::vector<CurvePoint> points;

                //  A point at no second, or at no level, is no point (tests/fuzz).
                for (const auto& [beat, db] : byBeat)
                    if (const auto seconds = sampleSecondsAt (clip, beat, set.tempo); std::isfinite (seconds) && std::isfinite (db))
                        points.push_back ({ seconds, db });

                std::vector<CurvePoint> kept;

                for (std::size_t at = 0; at < points.size(); ++at)
                {
                    const auto& point = points[at];

                    if (point.seconds < 0.0)
                    {
                        /*  The point where the curve crosses the file's start. */
                        if (at + 1 < points.size() && points[at + 1].seconds > 0.0)
                        {
                            const auto& next = points[at + 1];
                            const auto fraction = -point.seconds / (next.seconds - point.seconds);
                            kept.push_back ({ 0.0, point.db + (next.db - point.db) * fraction });
                        }

                        continue;
                    }

                    if (! kept.empty() && point.seconds <= kept.back().seconds + 0.0005)
                        continue;

                    kept.push_back (point);
                }

                if (kept.empty() && ! points.empty())
                    kept.push_back ({ 0.0, points.back().db });

                return kept;
            }

            /*  A CURVE READY FOR A LANE: silence spelled as Go.dot spells it,
                then thinned. */
            std::vector<CurvePoint> finished (const std::vector<CurvePoint>& points) const
            {
                return thin (withSilence (points), laws.thinningDb);
            }

            static double clampDb (double db, double silenceDb)
            {
                return std::clamp (db, silenceDb, 12.0);
            }

            /*  SILENCE AS GO.DOT SPELLS IT: Live's bottom of the fader, where a
                curve rests on it, is -120 - a true nought - and a curve passing
                through it on a slope keeps the -70 it was joined at. */
            std::vector<CurvePoint> withSilence (std::vector<CurvePoint> points) const
            {
                const auto atFloor = [this] (double db) { return db <= laws.floorDb + 0.01; };
                std::vector<bool> floor;

                for (const auto& point : points)
                    floor.push_back (atFloor (point.db));

                for (std::size_t at = 0; at < points.size(); ++at)
                {
                    const auto before = at == 0 || floor[at - 1];
                    const auto after = at + 1 >= points.size() || floor[at + 1];

                    if (floor[at] && before && after)
                        points[at].db = laws.silenceDb;
                    else
                        points[at].db = clampDb (points[at].db, laws.silenceDb);
                }

                return points;
            }

            //==================================================================
            /*  A NEW SOUND: a clip launched on a track that plays its clips. */
            Sound soundFor (int sceneIndex, const Scene& scene, const Track& track, const Clip& clip,
                            const std::map<std::string, std::pair<const Clip*, double>>& launchedHere)
            {
                Sound sound;
                sound.key = "sound:" + scene.id + ":" + track.id;
                sound.scene = sceneIndex;
                sound.trackId = track.id;
                sound.trackName = track.name;
                sound.name = clip.name.empty() ? clip.file.name : clip.name;
                sound.file = clip.file;
                sound.colour = clip.colour;

                const auto note = [&] (Note::Kind kind, const std::string& text)
                {
                    out.notes.push_back ({ kind, sceneIndex, track.name, text });
                };

                //  --- Where in the file, and how fast ------------------------------
                const auto startBeat = startBeatOf (clip);
                const auto endBeat = clipBeats (clip, clip.loopEnd);
                const auto fileSeconds = clip.file.sampleRate > 0.0 && clip.file.frames > 0
                                           ? static_cast<double> (clip.file.frames) / clip.file.sampleRate : 0.0;

                const auto perFileSecond = beatsPerSecond (clip);

                if (clip.warped && perFileSecond > 0.0)
                {
                    const auto rate = beatsPerRealSecond / perFileSecond;
                    sound.beatsPerFileSecond = perFileSecond;

                    if (std::abs (rate - 1.0) > 0.001)
                    {
                        sound.rate = rate;
                        sound.timestretch = clip.warpMode != 3;
                        note (Note::Kind::info, "warped to play at " + osc::formatDouble (std::round (rate * 1000.0) / 1000.0)
                                                  + " of its speed, "
                                                  + (sound.timestretch ? "pitch held (timestretch)" : "pitch moved (varispeed)"));
                    }

                    if (! warpIsStraight (clip))
                        note (Note::Kind::approximated, "its warp map bends; one speed, the whole map's, is used");
                }
                else if (! clip.warped && std::any_of (clip.envelopes.begin(), clip.envelopes.end(), moves))
                {
                    note (Note::Kind::approximated, "the clip is not warped: its loop points are read as seconds of the "
                                                    "file and its envelopes in its own beats, which the probe set is to "
                                                    "confirm (BDQ)");
                }

                /*  WHAT THE CLIP MOVES THAT THE IMPORT DOES NOT READ, said once. */
                {
                    int unread = 0;

                    for (const auto& envelope : clip.envelopes)
                        if (moves (envelope)
                              && (envelope.target.what == Parameter::unknown || envelope.target.what == Parameter::otherModulation
                                    || envelope.target.what == Parameter::pan
                                    || (envelope.target.relative && envelope.target.what != Parameter::modulation)))
                            ++unread;

                    if (unread > 0)
                        note (Note::Kind::dropped, std::to_string (unread) + " envelope(s) on the clip move something "
                                                   "the import does not read - a pan (Go.dot has no pan lanes yet), a "
                                                   "modulation other than the volume's, or a parameter the set does not name");
                }

                if (clip.pitchCoarse != 0 || std::abs (clip.pitchFine) > 0.01)
                    note (Note::Kind::dropped, "transposed " + std::to_string (clip.pitchCoarse) + " semitones in Live; "
                                               "Go.dot moves pitch only with speed, so it plays untransposed");

                const auto fileStart = sampleSecondsAt (clip, startBeat, set.tempo);
                const auto fileEnd = sampleSecondsAt (clip, endBeat, set.tempo);

                sound.preWait = fileStart < 0.0 ? -fileStart / sound.rate : 0.0;
                sound.in = std::max (0.0, fileStart);
                sound.out = fileSeconds > 0.0 ? std::min (fileEnd, fileSeconds) : fileEnd;

                if (clip.loopOn)
                {
                    sound.loops = true;
                    sound.loopIn = std::max (0.0, sampleSecondsAt (clip, clipBeats (clip, clip.loopStart), set.tempo));
                    sound.loopOut = sound.out;
                }

                if (sound.preWait > 0.0)
                    note (Note::Kind::info, "starts " + osc::formatDouble (std::round (sound.preWait * 100.0) / 100.0)
                                              + " s before its file does: a pre-wait of silence");

                //  --- Where it goes ------------------------------------------------
                std::vector<Path> every;
                graph.paths (track.id, {}, every, 0);

                const auto from = std::min (startBeat, clip.loopOn ? clipBeats (clip, clip.loopStart) : startBeat);
                const auto to = std::max (endBeat, from + 1.0e-3);

                /*  ONLY THE PATHS EVER HEARD COUNT. A send that stays at the bottom
                    of its fader carries nothing, and leaving it in would keep a
                    fader every heard path shares off the level - Lazzi's sources
                    send nowhere themselves and are heard through their "track in".
                    The sum of curves straight in decibels is loudest at one of its
                    points, so the points are where it is asked. */
                std::vector<Path> routes;

                for (const auto& path : every)
                {
                    std::vector<Source> sources;

                    for (const auto& term : path.terms)
                        sources.push_back (sourceOf (term, clip, track.id, startBeat, launchedHere));

                    for (const auto beat : sampleBeats (sources, from, to, laws.volume != Laws::Volume::straightInDb))
                    {
                        if (sumDb (sources, beat) > laws.floorDb + 0.01)
                        {
                            routes.push_back (path);
                            break;
                        }
                    }
                }

                /*  WHAT EVERY HEARD PATH SHARES goes on the level; the rest on the
                    sends. */
                std::vector<Term> common;

                if (! routes.empty())
                {
                    for (const auto& term : routes.front().terms)
                    {
                        const auto everywhere = std::all_of (routes.begin(), routes.end(), [&term] (const Path& path)
                        {
                            return std::find (path.terms.begin(), path.terms.end(), term) != path.terms.end();
                        });

                        if (everywhere && term.what == Parameter::volume)
                            common.push_back (term);
                    }
                }

                //  --- The level: the clip's gain, its modulation, the shared faders --
                std::vector<Source> levelSources;

                for (const auto& term : common)
                    levelSources.push_back (sourceOf (term, clip, track.id, startBeat, launchedHere));

                Source modulation;
                for (const auto& envelope : clip.envelopes)
                    if (envelope.target.what == Parameter::modulation && envelope.target.trackId == track.id)
                        modulation.envelope = &envelope;

                std::vector<Source> shaping = levelSources;

                if (modulation.envelope != nullptr)
                    shaping.push_back (modulation);

                const auto bends = modulation.envelope != nullptr && moves (*modulation.envelope);
                std::vector<std::pair<double, double>> levelByBeat;

                for (const auto beat : sampleBeats (shaping, from, to, bends || laws.volume != Laws::Volume::straightInDb))
                {
                    auto db = sumDb (levelSources, beat);

                    if (modulation.envelope != nullptr && db > laws.floorDb + 0.01)
                    {
                        const auto modulated = envelopeDb (*modulation.envelope, beat, laws);
                        db = modulated <= laws.silenceDb + 0.01 ? laws.silenceDb : db + modulated;
                    }

                    levelByBeat.emplace_back (beat, db);
                }

                const auto gainDb = decibels (clip.gain, laws);
                auto levelLane = finished (onFile (clip, levelByBeat));

                if (flat (levelLane))
                {
                    sound.levelDb = clampDb (gainDb + (levelLane.empty() ? 0.0 : levelLane.front().db), laws.silenceDb);
                }
                else
                {
                    sound.levelDb = clampDb (gainDb, laws.silenceDb);
                    sound.levelLane = std::move (levelLane);
                }

                //  --- The sends: what reaches each mix, past the shared faders -----
                std::map<std::string, std::vector<const Path*>> byMix;

                for (const auto& path : routes)
                    byMix[path.mix].push_back (&path);

                for (const auto& [mixKey, mixPaths] : byMix)
                {
                    const auto* mix = mixNamed (mixKey);

                    if (mix == nullptr || (mix->pair < 0 && ! mix->aux))
                        continue;

                    /*  Each path's own terms, the shared ones taken off. */
                    std::vector<std::vector<Source>> pathSources;
                    std::vector<Source> everySource;
                    bool anyMoving = false;

                    for (const auto* path : mixPaths)
                    {
                        std::vector<Source> sources;

                        for (const auto& term : path->terms)
                        {
                            if (std::find (common.begin(), common.end(), term) != common.end())
                                continue;

                            sources.push_back (sourceOf (term, clip, track.id, startBeat, launchedHere));

                            if (sources.back().envelope != nullptr && moves (*sources.back().envelope))
                                anyMoving = true;
                        }

                        everySource.insert (everySource.end(), sources.begin(), sources.end());
                        pathSources.push_back (std::move (sources));
                    }

                    /*  TWO PATHS INTO ONE MIX SUM AS GAINS, sampled on the grid. */
                    const auto summing = pathSources.size() > 1;
                    std::vector<std::pair<double, double>> sendByBeat;

                    for (const auto beat : sampleBeats (everySource, from, to,
                                                        anyMoving && (summing || laws.volume != Laws::Volume::straightInDb)))
                    {
                        double gain = 0.0;

                        for (const auto& sources : pathSources)
                            if (const auto db = sumDb (sources, beat); db > laws.floorDb + 0.01)
                                gain += gainOfDb (db);

                        /*  AN AUX'S FADER IS ITS OWN LEVEL (§93), not folded in. */
                        sendByBeat.emplace_back (beat, gain > 0.0 ? decibels (gain, laws) + (mix->aux ? 0.0 : mix->levelDb)
                                                                  : laws.floorDb);
                    }

                    auto lane = onFile (clip, sendByBeat);

                    if (silentThroughout (lane, laws))
                        continue;

                    /*  ITS DIRECT OUT (BDS): named on the cue, at the cue's
                        level - a path's own terms beyond the shared faders have
                        nowhere to go there. */
                    if (mix->direct)
                    {
                        sound.directOut = mixKey;

                        if (! flat (finished (lane)) || std::abs (lane.front().db) > 0.05)
                            note (Note::Kind::approximated, "its own output to the interface carries "
                                                            + osc::formatDouble (std::round (lane.front().db * 10.0) / 10.0)
                                                            + " dB beside the cue's level, which a direct out cannot: "
                                                              "it plays there at the cue's level");
                        continue;
                    }

                    if (summing)
                    {
                        int heard = 0;

                        for (const auto& sources : pathSources)
                        {
                            const auto loud = std::any_of (sendByBeat.begin(), sendByBeat.end(), [&] (const auto& point)
                            {
                                return sumDb (sources, point.first) > laws.floorDb + 0.01;
                            });

                            heard += loud ? 1 : 0;
                        }

                        if (heard > 1)
                            note (Note::Kind::info, "reaches \"" + mix->name + "\" by " + std::to_string (heard)
                                                      + " paths at once, summed into one send");
                    }

                    SendOut send;
                    send.mix = mixKey;
                    send.on = true;

                    for (const auto* path : mixPaths)
                        if (path->pan.has_value())
                            send.pan = path->pan;

                    lane = finished (lane);

                    if (flat (lane))
                        send.levelDb = clampDb (lane.front().db, laws.silenceDb);
                    else
                        send.lane = std::move (lane);

                    sound.sends.push_back (std::move (send));
                }

                if (sound.sends.empty() && sound.directOut.empty())
                    note (Note::Kind::dropped, "\"" + sound.name + "\" reaches no output of the interface: imported "
                                               "with no send, silent");

                //  --- Its devices, and the ones on its path ------------------------
                std::vector<const Track*> along { &track };

                for (const auto& path : routes)
                    for (const auto& term : path.terms)
                        if (const auto* crossed = graph.track (term.trackId);
                              crossed != nullptr && std::find (along.begin(), along.end(), crossed) == along.end())
                            along.push_back (crossed);

                for (const auto* crossed : along)
                {
                    for (std::size_t at = 0; at < crossed->devices.size(); ++at)
                    {
                        const auto& device = crossed->devices[at];

                        if (device.kind == "Eq8")
                        {
                            sound.eqs.push_back (&device);
                            continue;
                        }

                        if (device.kind.rfind ("MxDevice", 0) == 0)   // said once for the set (BEA)
                            continue;

                        const auto named = (device.name.empty() ? device.kind : device.name) + " on \""
                                             + crossed->name + "\"";
                        const auto switched = deviceSwitchedOn (clip, launchedHere, *crossed, static_cast<int> (at));

                        if (device.on || switched)
                        {
                            sound.effects.push_back (named + (device.on ? "" : ", switched on by the clip"));
                            note (Note::Kind::dropped, named + " is a Live device Go.dot has no equivalent for: "
                                                       "the sound plays without it");
                        }
                    }
                }

                if (! sound.eqs.empty() && sound.eqs.size() > 1)
                    note (Note::Kind::approximated, std::to_string (sound.eqs.size()) + " EQ Eights on its path; "
                                                    "the first is the cue's EQ, the others are reported");

                /*  ITS PAN RIDES INTO ITS SENDS (namespace draft §113, BCG): a
                    pan on a mono file, a balance on a stereo one - which the
                    build decides, knowing the file. Approximated rather than
                    exact: Live's pan is constant-power, 3 dB down at the
                    centre, and Go.dot's is full there. */
                sound.pan = std::abs (track.pan) > 0.005 ? std::clamp (track.pan, -1.0, 1.0) : 0.0;

                if (std::abs (track.pan) > 0.01)
                    note (Note::Kind::approximated, "panned " + osc::formatDouble (std::round (track.pan * 100.0) / 100.0)
                                                    + " in Live: its sends carry it as a pan (a mono file) or a balance "
                                                      "(a stereo one), full at the centre where Live's is 3 dB down");

                //  --- A carrier's movement past a loop's first pass ---------------
                if (clip.loopOn)
                    for (const auto& [carrierTrack, launched] : launchedHere)
                        if (carrierTrack != track.id)
                            for (const auto& envelope : launched.first->envelopes)
                                if (moves (envelope)
                                      && envelope.points.back().beats - launched.second > (endBeat - startBeat))
                                {
                                    note (Note::Kind::approximated,
                                          "a carrier on \"" + nameOf (carrierTrack) + "\" keeps moving past the "
                                          "loop's first pass; its first pass is what every pass now hears");
                                    break;
                                }

                return sound;
            }

            std::string nameOf (const std::string& trackId) const
            {
                const auto* found = graph.track (trackId);
                return found != nullptr ? found->name : trackId;
            }

            //==================================================================
            /*  A LIVE INPUT FIRED (BDU): a sound with no file, read from where
                the track's mixer stands - a clip launched with it shaping it as
                a carrier's would, though a mic cue has no file to draw a lane
                over, so a curve is taken at its first value and said. */
            Sound liveFor (int sceneIndex, const Scene& scene, const Track& track, const Clip* clip,
                           const std::map<std::string, std::pair<const Clip*, double>>& launchedHere)
            {
                Clip silent;
                silent.name = track.name;
                silent.loopEnd = 1.0;

                auto sound = soundFor (sceneIndex, scene, track, clip != nullptr ? *clip : silent, launchedHere);
                sound.key = "live:" + track.id;
                sound.name = track.name;
                sound.live = true;
                sound.inputChannel = track.input.channel;
                sound.inputStereo = track.input.stereo;
                sound.file = {};
                sound.preWait = sound.in = sound.out = 0.0;
                sound.loops = false;
                sound.rate = 1.0;
                sound.timestretch = false;
                sound.beatsPerFileSecond = 0.0;

                bool flattened = false;

                if (! sound.levelLane.empty())
                {
                    sound.levelDb = clampDb (sound.levelDb + sound.levelLane.front().db, laws.silenceDb);
                    sound.levelLane.clear();
                    flattened = true;
                }

                for (auto& send : sound.sends)
                    if (! send.lane.empty())
                    {
                        send.levelDb = clampDb (send.lane.front().db, laws.silenceDb);
                        send.lane.clear();
                        flattened = true;
                    }

                if (flattened)
                    out.notes.push_back ({ Note::Kind::approximated, sceneIndex, track.name, "the clip launched with the "
                                           "live input draws a curve, which a mic cue has no file to follow: its first "
                                           "value is used" });

                out.notes.push_back ({ Note::Kind::info, sceneIndex, track.name, "a live input on the interface's "
                                       + std::string (track.input.stereo ? "inputs " + std::to_string (2 * track.input.channel + 1)
                                                                              + "/" + std::to_string (2 * track.input.channel + 2)
                                                                          : "input " + std::to_string (track.input.channel + 1))
                                       + ": a mic cue, fired here and kept running (namespace draft §121, BDU)" });
                return sound;
            }

            static bool deviceSwitchedOn (const Clip& clip,
                                          const std::map<std::string, std::pair<const Clip*, double>>& launchedHere,
                                          const Track& track, int device)
            {
                const auto switches = [&] (const Clip& one)
                {
                    return std::any_of (one.envelopes.begin(), one.envelopes.end(), [&] (const Envelope& envelope)
                    {
                        return envelope.target.trackId == track.id && envelope.target.what == Parameter::device
                                 && envelope.target.deviceIndex == device && envelope.target.path == "On"
                                 && std::any_of (envelope.points.begin(), envelope.points.end(),
                                                 [] (const EnvelopePoint& point) { return point.value > 0.5; });
                    });
                };

                if (switches (clip))
                    return true;

                if (const auto found = launchedHere.find (track.id); found != launchedHere.end())
                    return switches (*found->second.first);

                return false;
            }

            //==================================================================
            /*  A CLIP THAT MOVES A TRACK'S MIXER WITHOUT LAUNCHING A SOUND: a
                carrier launched alone (QI), or a MIDI clip's curve landed on a
                mapping (§121, BDV). A fade on each sound the mover reaches - the
                sound its track is hearing for a carrier, its own for a live
                input or a plain track, every sound whose path crosses its
                fader for a group - and the mixer held where the clip leaves it. */
            std::vector<FadeOut> fadesFor (int sceneIndex, const Scene& scene, const Track& mover, const Clip& clip)
            {
                const auto note = [&] (Note::Kind kind, const std::string& text)
                {
                    out.notes.push_back ({ kind, sceneIndex, mover.name, text });
                };

                std::vector<Sound*> targets;

                if (mover.kind == Track::Kind::group)
                {
                    for (const auto& [trackId, key] : state.playing)
                    {
                        std::vector<Path> every;
                        graph.paths (trackId, {}, every, 0);

                        const auto crosses = std::any_of (every.begin(), every.end(), [&mover] (const Path& path)
                        {
                            return std::any_of (path.terms.begin(), path.terms.end(),
                                                [&mover] (const Term& term) { return term.trackId == mover.id; });
                        });

                        if (crosses)
                            targets.push_back (state.sounds.at (key));
                    }
                }
                else
                {
                    const auto* fed = isCarrier (mover) ? sourceFor (mover) : &mover;

                    //  A LIVE INPUT'S OWN CLIP shapes the mic cue it is (BDU).
                    if (fed == nullptr && isLive (mover))
                        fed = &mover;

                    if (fed == nullptr)
                    {
                        note (Note::Kind::info, "the track listens with its monitoring In and nothing feeds it: its clip is "
                                                "silent in Live, and is not imported");
                        hold (mover.id, clip);
                        return {};
                    }

                    if (const auto playing = state.playing.find (fed->id); playing != state.playing.end())
                        targets.push_back (state.sounds.at (playing->second));
                    else
                        note (Note::Kind::info, "nothing is playing on \"" + fed->name + "\" for the clip to shape: "
                                                "where it leaves the mixer is kept for what comes after");
                }

                std::vector<FadeOut> fades;

                for (auto* sound : targets)
                    fades.push_back (fadeOnto (sceneIndex, scene, mover, clip, *sound, mover.kind == Track::Kind::group));

                //  --- And where the clip leaves the mover's mixer ----------------
                hold (mover.id, clip);
                return fades;
            }

            FadeOut fadeOnto (int sceneIndex, const Scene& scene, const Track& carrier, const Clip& clip, Sound& soundMoved,
                              bool keyedBySound)
            {
                const auto note = [&] (Note::Kind kind, const std::string& text)
                {
                    out.notes.push_back ({ kind, sceneIndex, carrier.name, text });
                };

                auto* sound = &soundMoved;
                const auto startBeat = startBeatOf (clip);
                const auto endBeat = clipBeats (clip, clip.loopEnd);
                const auto& held = state.held.at (carrier.id);

                FadeOut fade;
                fade.key = "fade:" + scene.id + ":" + carrier.id + (keyedBySound ? ":" + sound->key : std::string());
                fade.scene = sceneIndex;
                fade.target = sound->key;
                fade.name = (clip.name.empty() ? std::string ("Fade") : clip.name) + " - " + carrier.name;

                /*  HOW LONG: to the carrier's last point that moves anything,
                    within the clip. */
                double lastMoving = startBeat;

                for (const auto& envelope : clip.envelopes)
                    if (envelope.target.trackId == carrier.id && moves (envelope))
                        for (const auto& point : envelope.points)
                            if (point.beats > lastMoving && point.beats <= endBeat)
                                lastMoving = point.beats;

                const auto spanBeats = std::max (lastMoving - startBeat, 0.0);
                fade.duration = std::max (spanBeats / beatsPerRealSecond, laws.sampleSeconds);

                //  --- The level: the carrier's volume against where it stood ------
                const auto* volume = envelopeOn (clip, carrier.id, Parameter::volume);
                const auto heldDb = decibels (held.volume, laws);
                double endDb = heldDb;

                if (volume != nullptr)
                {
                    std::vector<CurvePoint> points;
                    const Source source { volume, startBeat, heldDb };

                    for (const auto beat : sampleBeats ({ source }, 0.0, std::max (spanBeats, 1.0e-3),
                                                        laws.volume != Laws::Volume::straightInDb))
                        points.push_back ({ beat / beatsPerRealSecond, valueOf (source, beat) });

                    endDb = points.empty() ? heldDb : points.back().db;

                    for (auto& point : points)
                    {
                        point.seconds = fade.duration > 0.0 ? std::clamp (point.seconds / fade.duration, 0.0, 1.0) : 1.0;
                        point.db = sound->levelDb + (point.db - heldDb);
                    }

                    points = thin (withSilence (points), laws.thinningDb);

                    /*  A fade's drawing starts at nought and ends at one. */
                    if (! points.empty())
                    {
                        points.front().seconds = 0.0;
                        points.back().seconds = 1.0;
                    }

                    if (points.size() > 2 || (points.size() == 2 && ! same (points.front().db, points.back().db, 0.05)))
                        fade.points = points;

                    fade.levelDb = points.empty() ? sound->levelDb : points.back().db;
                }
                else
                {
                    fade.levelDb = sound->levelDb;
                }

                if (endDb <= laws.floorDb + 0.01)
                {
                    fade.stopWhenDone = true;
                    fade.levelDb = laws.silenceDb;

                    if (! fade.points.empty())
                        fade.points.back().db = laws.silenceDb;
                }

                /*  THE TRACK ACTIVATOR MOVED (§121, BDV): off is a fade to
                    silence that keeps the sound running, as Live's mute keeps
                    the track live; on again, after an off, a fade back to the
                    sound's level. */
                if (const auto* active = envelopeOn (clip, carrier.id, Parameter::active);
                      active != nullptr && ! active->points.empty())
                {
                    const auto on = lastValue (*active) >= 0.5;

                    if (! on)
                    {
                        fade.name = "Mute - " + sound->name;
                        fade.points.clear();
                        fade.levelDb = laws.silenceDb;
                        fade.stopWhenDone = false;
                        note (Note::Kind::info, "switched off by the clip: \"" + sound->name + "\" is faded to silence and "
                                                "kept running, as Live's mute keeps the track live (BDV)");
                    }
                    else if (! held.active)
                    {
                        fade.name = "Unmute - " + sound->name;

                        if (fade.points.empty())
                            fade.levelDb = sound->levelDb;

                        fade.stopWhenDone = false;
                        note (Note::Kind::info, "switched on by the clip: \"" + sound->name + "\" is faded back to its level (BDV)");
                    }
                }

                //  --- The sends the carrier moves --------------------------------
                for (std::size_t send = 0; send < carrier.sends.size(); ++send)
                {
                    const auto* moved = envelopeOn (clip, carrier.id, Parameter::send, static_cast<int> (send));

                    if (moved == nullptr || send >= held.sends.size())
                        continue;

                    const auto key = "return:" + std::to_string (send);
                    const auto from = decibels (held.sends[send], laws);
                    const auto to = decibels (lastValue (*moved), laws);

                    if (same (from, to, 0.05))
                        continue;

                    const auto existing = std::find_if (sound->sends.begin(), sound->sends.end(),
                                                        [&key] (const SendOut& one) { return one.mix == key; });
                    const auto base = existing != sound->sends.end() ? existing->levelDb : from;

                    fade.sends[key] = clampDb (to <= laws.floorDb + 0.01 ? laws.silenceDb : base + (to - from),
                                               laws.silenceDb);

                    if (moves (*moved) && moved->points.size() > 3)
                        note (Note::Kind::approximated, "the carrier's send to \"" + mixNameOf (key)
                                                        + "\" moves along a curve; the fade takes it to where it "
                                                        "ends, in one move");
                }

                //  --- The EQ and the devices it moves ----------------------------
                for (const auto& envelope : clip.envelopes)
                {
                    if (envelope.target.trackId != carrier.id || envelope.target.what != Parameter::device || ! moves (envelope))
                        continue;

                    const auto deviceIndex = static_cast<std::size_t> (envelope.target.deviceIndex);

                    if (deviceIndex >= carrier.devices.size())
                        continue;

                    const auto& device = carrier.devices[deviceIndex];
                    const auto& path = envelope.target.path;

                    if (device.kind == "Eq8" && path.rfind ("Bands.", 0) == 0)
                    {
                        const auto band = static_cast<int> (osc::parseDouble (path.substr (6, 1)).value_or (-1.0));
                        const auto what = path.substr (path.find_last_of ('/') + 1);

                        if (band >= 0 && (what == "Freq" || what == "Gain"))
                        {
                            fade.eq.push_back ({ &device, band, what, lastValue (envelope) });
                            continue;
                        }
                    }

                    note (Note::Kind::dropped, (device.name.empty() ? device.kind : device.name) + " " + path
                                               + " on \"" + carrier.name + "\" moves in this fade; Go.dot has no "
                                               "equivalent for the device");
                }

                if (const auto* pan = envelopeOn (clip, carrier.id, Parameter::pan); pan != nullptr && moves (*pan))
                    note (Note::Kind::dropped, "the pan of \"" + carrier.name + "\" moves in this fade; Go.dot has no "
                                               "pan lane, and the fade leaves it");

                return fade;
            }

            std::string mixNameOf (const std::string& key) const
            {
                const auto* mix = mixNamed (key);
                return mix != nullptr ? mix->name : key;
            }

            /*  WHERE A CLIP LEAVES ITS TRACK'S MIXER once it has played (QU's
                third law): each envelope's last value. */
            void hold (const std::string& trackId, const Clip& clip)
            {
                auto& held = state.held[trackId];

                for (const auto& envelope : clip.envelopes)
                {
                    if (envelope.target.trackId != trackId || envelope.target.relative || envelope.points.empty())
                        continue;

                    if (envelope.target.what == Parameter::volume)
                        held.volume = lastValue (envelope);
                    else if (envelope.target.what == Parameter::send && envelope.target.sendIndex >= 0
                               && static_cast<std::size_t> (envelope.target.sendIndex) < held.sends.size())
                        held.sends[static_cast<std::size_t> (envelope.target.sendIndex)] = lastValue (envelope);
                    else if (envelope.target.what == Parameter::active)
                        held.active = lastValue (envelope) >= 0.5;
                }
            }

            //==================================================================
            /*  A MIDI CLIP LAUNCHED (namespace draft §121, BDV): each of its
                controller curves landed on the mappings it feeds, as a move on
                what they move - the same move a carrier clip makes - and sent
                out as a MIDI cue where the set maps it nowhere. */
            void midiClipFor (int sceneIndex, const Scene& scene, const Track& track, const Clip& clip, Step& step)
            {
                const auto note = [&] (Note::Kind kind, const std::string& text)
                {
                    out.notes.push_back ({ kind, sceneIndex, track.name, text });
                };

                if (clip.notes > 0)
                    note (Note::Kind::dropped, "\"" + clip.name + "\" holds " + std::to_string (clip.notes)
                                               + " note(s), which are not imported: a MIDI note cue is a later thing");

                if (track.midiOut.channel <= 0)
                {
                    if (std::any_of (clip.envelopes.begin(), clip.envelopes.end(), moves))
                        note (Note::Kind::dropped, "\"" + clip.name + "\" moves controllers, and the track sends to no "
                                                   "MIDI port: they go nowhere");
                    return;
                }

                std::map<std::string, Clip> movesByTrack;   // the target track's Id to the clip of moves on it
                const auto startBeat = startBeatOf (clip);

                for (const auto& envelope : clip.envelopes)
                {
                    const auto cc = controllerOf (envelope);

                    if (cc < 0)
                    {
                        if (moves (envelope))
                            note (Note::Kind::dropped, "\"" + clip.name + "\" moves " + (envelope.target.path.empty()
                                                         ? std::string ("a controller the set does not name") : envelope.target.path)
                                                       + ", which the import does not send");
                        continue;
                    }

                    bool landed = false;

                    for (const auto& mapping : set.mappings)
                    {
                        if (mapping.note || mapping.channel != track.midiOut.channel || mapping.number != cc)
                            continue;

                        landed = true;
                        auto& synthetic = movesByTrack[mapping.target.trackId];

                        if (synthetic.envelopes.empty())
                        {
                            synthetic.name = clip.name;
                            synthetic.loopStart = clip.loopStart;
                            synthetic.loopEnd = clip.loopEnd;
                            synthetic.startRelative = clip.startRelative;
                            synthetic.loopOn = clip.loopOn;
                            synthetic.currentStart = clip.currentStart;
                            synthetic.currentEnd = clip.currentEnd;
                            synthetic.warped = true;
                        }

                        Envelope moved;
                        moved.target = mapping.target;
                        moved.switches = mapping.target.what == Parameter::active;

                        for (const auto& point : envelope.points)
                            moved.points.push_back ({ point.beats, controllerValue (point.value, mapping) });

                        synthetic.envelopes.push_back (std::move (moved));

                        if (mapping.target.what == Parameter::volume || mapping.target.what == Parameter::send
                              || mapping.target.what == Parameter::device)
                            note (Note::Kind::approximated, "\"" + clip.name + "\" sends controller " + std::to_string (cc)
                                                            + " to \"" + nameOf (mapping.target.trackId) + "\": its values "
                                                              "are taken along the provisional controller law, 0 dB at 109 "
                                                              "(BDW)");
                    }

                    if (landed)
                        continue;

                    /*  SENT OUT, in case something outside listens (BDV). */
                    MidiOut sent;
                    sent.key = "midi:" + scene.id + ":" + track.id + ":" + std::to_string (cc);
                    sent.scene = sceneIndex;
                    sent.name = (clip.name.empty() ? std::string ("MIDI") : clip.name) + " CC " + std::to_string (cc);
                    sent.port = track.midiOut.port;
                    sent.channel = track.midiOut.channel;
                    sent.controller = cc;

                    const auto& points = envelope.points;
                    const auto first = points.size() > 1 ? points[1] : points.empty() ? EnvelopePoint {} : points[0];
                    const auto last = points.empty() ? EnvelopePoint {} : points.back();
                    sent.from = static_cast<int> (std::clamp (std::round (first.value), 0.0, 127.0));
                    sent.to = static_cast<int> (std::clamp (std::round (last.value), 0.0, 127.0));
                    sent.seconds = std::max (0.0, (last.beats - startBeat) / beatsPerRealSecond);

                    if (! std::isfinite (sent.seconds))
                        sent.seconds = 0.0;

                    note (Note::Kind::info, "\"" + clip.name + "\" sends controller " + std::to_string (cc) + " on channel "
                                            + std::to_string (sent.channel) + " to nothing the set maps: sent out as a "
                                              "MIDI cue on \"" + sent.port + "\", in case something outside listens (BDV)");
                    step.midi.push_back (std::move (sent));
                }

                if (! movesByTrack.empty() && loopbackSaid.insert (track.id).second)
                    note (Note::Kind::info, "its controllers land on the set's own mappings over \"" + track.midiOut.port
                                            + "\": taken for a loopback Live reads back, which the set does not say (BDX)");

                for (auto& [targetId, synthetic] : movesByTrack)
                {
                    const auto* target = graph.track (targetId);

                    if (target == nullptr)
                        continue;

                    for (auto& fade : fadesFor (sceneIndex, scene, *target, synthetic))
                        step.fades.push_back (std::move (fade));
                }
            }

            //==================================================================
            void walkScenes()
            {
                int number = 0;

                for (std::size_t sceneIndex = 0; sceneIndex < set.scenes.size(); ++sceneIndex)
                {
                    const auto index = static_cast<int> (sceneIndex);

                    if (! options.scenes.empty() && options.scenes.count (index) == 0)
                        continue;

                    const auto& scene = set.scenes[sceneIndex];

                    /*  THE TEMPO FROM HERE ON (BDR), set even by a scene that
                        launches nothing: a night fires it, and Live keeps it. */
                    const auto tempoBefore = beatsPerRealSecond;

                    if (scene.tempoEnabled && std::isfinite (scene.tempo) && scene.tempo > 0.0)
                        beatsPerRealSecond = scene.tempo / 60.0;

                    const auto tempoChanges = ! same (beatsPerRealSecond, tempoBefore, 1.0e-6);

                    if (tempoChanges)
                        out.notes.push_back ({ Note::Kind::info, index, {}, "the scene sets the tempo to "
                                               + osc::formatDouble (scene.tempo) + ": what it launches is stretched "
                                               "to it, and a warped sound still playing is re-stretched (BDR)" });

                    if (! sceneDoesSomething (set, index) && ! (tempoChanges && ! state.playing.empty()))
                        continue;

                    Step step;
                    step.scene = index;
                    step.sceneId = scene.id;
                    step.name = scene.name;
                    step.notes = scene.annotation;
                    step.colour = scene.colour;

                    if (scene.followAction)
                        out.notes.push_back ({ Note::Kind::dropped, index, {}, "the scene has a follow action, which is "
                                               "not imported: a GO in Go.dot is a person's" });

                    /*  WHAT IS LAUNCHED HERE, by track: each clip with the beat it
                        starts from - a carrier launched with its sound is read
                        against that sound's beats. */
                    std::map<std::string, std::pair<const Clip*, double>> launchedHere;

                    for (const auto& track : set.tracks)
                    {
                        if (sceneIndex >= track.slots.size() || ! track.slots[sceneIndex].clip.has_value())
                            continue;

                        const auto& clip = *track.slots[sceneIndex].clip;

                        if (clip.deactivated)
                        {
                            out.notes.push_back ({ Note::Kind::dropped, index, track.name, "the clip \"" + clip.name
                                                   + "\" is deactivated in Live, and is not imported" });
                            continue;
                        }

                        launchedHere[track.id] = { &clip, startBeatOf (clip) };
                    }

                    /*  THE MIDI CLIPS FIRST (BDV): what they move - a fader, an
                        activator - is where the sounds launched here and the
                        live inputs fired here read the mixer from. */
                    for (const auto& track : set.tracks)
                        if (track.kind == Track::Kind::midi)
                            if (const auto launched = launchedHere.find (track.id); launched != launchedHere.end())
                                midiClipFor (index, scene, track, *launched->second.first, step);

                    //  --- Sounds first: they decide what the carriers fold into ----
                    std::set<std::string> carriersFolded;

                    /*  A LIVE INPUT NOT YET RUNNING, its track on, is fired here
                        (BDU) - with the clip launched on it, when there is one. */
                    for (const auto& track : set.tracks)
                    {
                        if (! isLive (track) || state.playing.count (track.id) != 0 || ! state.held.at (track.id).active)
                            continue;

                        const auto launched = launchedHere.find (track.id);
                        step.sounds.push_back (liveFor (index, scene, track,
                                                        launched != launchedHere.end() ? launched->second.first : nullptr,
                                                        launchedHere));

                        if (launched != launchedHere.end())
                            carriersFolded.insert (track.id);
                    }

                    for (const auto& track : set.tracks)
                    {
                        if (track.kind != Track::Kind::audio || isCarrier (track))
                            continue;

                        if (sceneIndex >= track.slots.size())
                            continue;

                        const auto& slot = track.slots[sceneIndex];
                        const auto launched = launchedHere.find (track.id);

                        /*  A NEW CLIP OR A STOP BUTTON ends what the track plays. */
                        if (launched != launchedHere.end() || slot.hasStop)
                        {
                            if (const auto playing = state.playing.find (track.id); playing != state.playing.end())
                            {
                                StopOut stop;
                                stop.key = "stop:" + scene.id + ":" + track.id;
                                stop.scene = index;
                                stop.target = playing->second;
                                stop.name = "Stop " + state.sounds.at (playing->second)->name;
                                step.stops.push_back (std::move (stop));
                                state.playing.erase (playing);
                            }
                        }

                        if (launched == launchedHere.end())
                            continue;

                        if (! state.held.at (track.id).active)
                        {
                            out.notes.push_back ({ Note::Kind::dropped, index, track.name, "the track is switched off "
                                                   "in Live; its clip is not imported" });
                            continue;
                        }

                        const auto& clip = *launched->second.first;
                        step.sounds.push_back (soundFor (index, scene, track, clip, launchedHere));

                        for (const auto& other : set.tracks)
                            if (isCarrier (other) && feeds (track, other) && launchedHere.count (other.id) != 0)
                                carriersFolded.insert (other.id);
                    }

                    //  --- Then carriers launched without their sound: fades ------
                    for (const auto& track : set.tracks)
                    {
                        if (! isCarrier (track) || launchedHere.count (track.id) == 0)
                            continue;

                        const auto& clip = *launchedHere.at (track.id).first;

                        if (carriersFolded.count (track.id) != 0)
                        {
                            hold (track.id, clip);
                            continue;
                        }

                        for (auto& fade : fadesFor (index, scene, track, clip))
                            step.fades.push_back (std::move (fade));
                    }

                    //  --- A tempo change re-stretches what is still playing (BDR) --
                    if (tempoChanges)
                    {
                        for (const auto& [trackId, key] : state.playing)
                        {
                            auto* sound = state.sounds.at (key);

                            if (sound->beatsPerFileSecond <= 0.0)
                                continue;

                            const auto rate = beatsPerRealSecond / sound->beatsPerFileSecond;
                            const auto was = state.rate.count (key) != 0 ? state.rate.at (key) : sound->rate;

                            if (std::abs (rate - was) <= 0.001)
                                continue;

                            FadeOut fade;
                            fade.key = "tempo:" + scene.id + ":" + trackId;
                            fade.scene = index;
                            fade.target = key;
                            fade.name = "Tempo " + osc::formatDouble (scene.tempo) + " - " + sound->name;
                            fade.duration = laws.sampleSeconds;
                            fade.levelDb = sound->levelDb;
                            fade.rate = rate;
                            step.fades.push_back (std::move (fade));
                            state.rate[key] = rate;

                            out.notes.push_back ({ Note::Kind::info, index, sound->trackName, "\"" + sound->name
                                                   + "\" is still playing when the tempo changes: Live re-stretches it, "
                                                     "so a speed fade takes it to "
                                                   + osc::formatDouble (std::round (rate * 1000.0) / 1000.0)
                                                   + " of its speed (BDR)" });
                        }
                    }

                    if (step.empty())
                        continue;

                    step.number = ++number;

                    /*  REGISTER THE SOUNDS where the next scenes will find them.
                        Pointers into the step that is about to move into the
                        walk's list are taken after it has moved. */
                    out.steps.push_back (std::move (step));
                    auto& kept = out.steps.back();

                    for (auto& sound : kept.sounds)
                    {
                        state.playing[sound.trackId] = sound.key;
                        state.sounds[sound.key] = &sound;
                        state.rate[sound.key] = sound.rate;

                        //  A live input fired with no clip leaves its mixer where it stood.
                        if (const auto launched = launchedHere.find (sound.trackId); launched != launchedHere.end())
                            hold (sound.trackId, *launched->second.first);
                    }
                }
            }

            /*  THE DCAS AND THE HANDS (QN, QO). */
            void readHands()
            {
                const auto control = [] (const Mapping& mapping)
                {
                    return (mapping.note ? "note " : "CC ") + std::to_string (mapping.number) + ", channel "
                             + std::to_string (mapping.channel);
                };

                for (const auto& mapping : set.mappings)
                {
                    const auto& target = mapping.target;
                    const auto trackName = nameOf (target.trackId);
                    Hand hand;
                    hand.control = control (mapping);

                    /*  DRIVEN BY THE SET'S OWN MIDI CLIPS (§121, BDY): no hand,
                        no DCA - a row of its own, saying what the clips do. */
                    if (fedByClip (mapping))
                    {
                        hand.moved = target.what == Parameter::volume ? "the volume of \"" + trackName + "\""
                                   : target.what == Parameter::send   ? "a send from \"" + trackName + "\""
                                   : target.what == Parameter::active ? "the track activator of \"" + trackName + "\""
                                   : target.what == Parameter::device ? target.deviceKind + " " + target.path + " on \"" + trackName + "\""
                                   : target.what == Parameter::pan    ? "the pan of \"" + trackName + "\""
                                                                      : "a parameter of \"" + trackName + "\"";
                        hand.goDot = target.what == Parameter::volume ? "a fade on each sound it reaches, and the value held for "
                                                                        "the GOs after (BDV)"
                                   : target.what == Parameter::send   ? "a move of that send on each sound it reaches (BDV)"
                                   : target.what == Parameter::active ? "its sounds muted and unmuted - a mic cue fired "
                                                                        "when first switched on (BDV)"
                                   : target.what == Parameter::device ? (target.deviceKind == "Eq8" ? "an EQ move (BDV)"
                                                                                                    : "nothing: Go.dot has no "
                                                                                                      "equivalent for the device")
                                                                      : "nothing";
                        out.clipFed.push_back (std::move (hand));
                        continue;
                    }

                    switch (target.what)
                    {
                        case Parameter::volume:
                            hand.moved = "the volume of \"" + trackName + "\"";
                            hand.goDot = "the DCA \"" + trackName + "\" on a DCA strip: its trim rides every cue "
                                         "from that track, as the fader rode the track";
                            out.dcas[target.trackId] = trackName;
                            break;

                        case Parameter::send:
                        {
                            const auto key = "return:" + std::to_string (target.sendIndex);
                            const auto* mix = mixNamed (key);
                            hand.moved = "the send from \"" + trackName + "\" to \"" + mixNameOf (key) + "\"";
                            hand.goDot = "the D700's Send page on the aimed cue, the \"" + mixNameOf (key)
                                           + "\" rotary - under the lock a ride heard and not saved"
                                           + (mix != nullptr && mix->aux ? std::string (" (an aux)") : std::string());
                            break;
                        }

                        case Parameter::device:
                            hand.moved = target.deviceKind + " " + target.path + " on \"" + trackName + "\"";
                            hand.goDot = target.deviceKind == "Eq8"
                                           ? "the D700's EQ page on the aimed cue"
                                           : "nothing: Go.dot has no equivalent for the device";
                            break;

                        case Parameter::pan:
                            hand.moved = "the pan of \"" + trackName + "\"";
                            hand.goDot = "the D700's Pan page on the aimed cue (namespace draft 113)";
                            break;

                        case Parameter::active:
                            hand.moved = "the track activator of \"" + trackName + "\" - its sounds on and off";
                            hand.goDot = "nothing of its own: a cue's GO and its stop, or Esc, are where Go.dot "
                                         "switches a sound (namespace draft 121, BEA)";
                            break;

                        case Parameter::modulation:
                        case Parameter::otherModulation:
                        case Parameter::controller:
                        case Parameter::unknown:
                            hand.moved = "a parameter of \"" + trackName + "\" the import does not read";
                            hand.goDot = "nothing";
                            break;
                    }

                    out.hands.push_back (std::move (hand));
                }

                if (set.fireScene.has_value())
                    out.hands.push_back ({ control (*set.fireScene), "fire the selected scene", "GO" });
                if (set.sceneUp.has_value())
                    out.hands.push_back ({ control (*set.sceneUp), "select the scene above", "the standby arrow up" });
                if (set.sceneDown.has_value())
                    out.hands.push_back ({ control (*set.sceneDown), "select the scene below", "the standby arrow down" });

                /*  A KEY ON A SCENE ITSELF (§121, BEA): the GO it would fire. A
                    MIDI trigger on that cue does the same, and is not written -
                    the controller arrives knowing nothing (QO, PRD §4.9). */
                for (std::size_t at = 0; at < set.scenes.size(); ++at)
                {
                    const auto& scene = set.scenes[at];

                    if (! scene.key.has_value())
                        continue;

                    const auto step = std::find_if (out.steps.begin(), out.steps.end(), [at] (const Step& one)
                    {
                        return one.scene == static_cast<int> (at);
                    });

                    out.hands.push_back ({ control (*scene.key),
                                           "launches scene " + std::to_string (at + 1)
                                             + (scene.name.empty() ? std::string() : " \"" + scene.name + "\""),
                                           step != out.steps.end()
                                             ? "a GO from cue " + std::to_string (step->number) + " - a MIDI trigger on it "
                                               "would do the same, and is not written (QO)"
                                             : "nothing: the scene is not imported" });
                }

                for (const auto& hand : out.hands)
                    out.notes.push_back ({ Note::Kind::info, -1, {}, hand.control + ": " + hand.moved + " - in Go.dot, "
                                                                     + hand.goDot });

                /*  A SOUND FROM A TRACK WHOSE FADER A HAND RODE answers to its DCA,
                    and where its clip also drew that fader the drawing is kept:
                    the import takes the set as it is stopped, not as a night runs
                    it (the author, QW) - a hand taking over in Live is the show,
                    and the DCA is the hand. Said, so the operator knows where the
                    level starts. */
                for (auto& step : out.steps)
                    for (auto& sound : step.sounds)
                        if (out.dcas.count (sound.trackId) != 0)
                        {
                            sound.dcaTrack = sound.trackId;

                            if (! sound.levelLane.empty())
                                out.notes.push_back ({ Note::Kind::info, sound.scene, sound.trackName,
                                                       "\"" + sound.name + "\" has its fader drawn in Live and a hand "
                                                       "on it too: the drawing is imported, as the set is when stopped, "
                                                       "and the DCA is the hand - its lane starts at "
                                                       + osc::formatDouble (std::round (sound.levelLane.front().db * 10.0) / 10.0)
                                                       + " dB, so a DCA at 0 dB plays the drawing (QW)" });
                        }
            }

            /*  THE RESTING VALUES A HAND MAY HAVE LEFT (QP): a track's volume or
                send mapped to a control, used where no envelope covers it. */
            void flagResting()
            {
                for (const auto& mapping : set.mappings)
                {
                    const auto& target = mapping.target;

                    if ((target.what != Parameter::volume && target.what != Parameter::send) || fedByClip (mapping))
                        continue;

                    const auto* mapped = graph.track (target.trackId);

                    if (mapped == nullptr)
                        continue;

                    bool covered = true;

                    for (const auto& step : out.steps)
                        for (const auto& sound : step.sounds)
                            if (sound.trackId == target.trackId)
                            {
                                /*  TWO TRACKS CAN CARRY ONE ID in a set that was damaged or edited by
                                    hand, and `mapped` is the first of them: its slot for this scene may
                                    be absent or empty. No clip found is no envelope found. */
                                const auto at = static_cast<std::size_t> (sound.scene);
                                const auto* clip = at < mapped->slots.size() && mapped->slots[at].clip.has_value()
                                                     ? &*mapped->slots[at].clip : nullptr;

                                if (clip == nullptr || envelopeOn (*clip, target.trackId, target.what, target.sendIndex) == nullptr)
                                    covered = false;
                            }

                    if (! covered)
                        out.notes.push_back ({ Note::Kind::approximated, -1, mapped->name,
                                               "a hand rode " + std::string (target.what == Parameter::volume ? "its fader" : "a send")
                                               + ", and some of its clips have no envelope on it: the value the set was "
                                               "saved with is used there, which may be where a hand left it "
                                               "(namespace draft §29, QP)" });
                }
            }

            /*  A LIVE INPUT NO SCENE SWITCHED ON (BDU): saved off, and no clip
                of the set turns it on - a hand does, on the night, or nobody.
                No mic cue is made, and the report says so. */
            void noteLivesNeverFired()
            {
                for (const auto& track : set.tracks)
                {
                    if (! isLive (track) || state.playing.count (track.id) != 0)
                        continue;

                    const auto hand = std::any_of (set.mappings.begin(), set.mappings.end(), [&track] (const Mapping& mapping)
                    {
                        return mapping.target.trackId == track.id && mapping.target.what == Parameter::active;
                    });

                    out.notes.push_back ({ Note::Kind::dropped, -1, track.name, "a live input saved switched off, which no "
                                           "scene switches on" + std::string (hand ? " - a hand does, on the night" : "")
                                           + ": no mic cue is made for it; add one if the night needs it (BDU)" });
                }
            }

            void noteTheSet()
            {
                if (set.arrangementClips > 0)
                    out.notes.push_back ({ Note::Kind::dropped, -1, {}, std::to_string (set.arrangementClips)
                                           + " clip(s) in Arrangement view are not imported: Session view only" });

                /*  A MAX FOR LIVE DEVICE, wherever it sits (§121, BEA): said once
                    for the set, since the group it is on may be on no path a
                    sound takes - Sturbzep's sits on a group whose output is None. */
                for (const auto& track : set.tracks)
                    for (const auto& device : track.devices)
                        if (device.kind.rfind ("MxDevice", 0) == 0)
                            out.notes.push_back ({ Note::Kind::dropped, -1, track.name, "a Max for Live device, "
                                                   + (device.name.empty() ? device.kind : device.name)
                                                   + ", is not imported: Go.dot has no Max" });

                for (const auto& problem : set.problems)
                    out.notes.push_back ({ Note::Kind::dropped, -1, {}, problem });
            }
        };
    }

    //==========================================================================
    Walk walk (const LiveSet& set, const WalkOptions& options)
    {
        Walker walker (set, options);
        walker.noteTheSet();
        walker.walkScenes();
        walker.readHands();
        walker.flagResting();
        walker.noteLivesNeverFired();
        return std::move (walker.out);
    }
}
