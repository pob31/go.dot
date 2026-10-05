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
        };

        /*  WHAT A PARAMETER STANDS AT, held between the clips that move it
            (QU's third law): a track's volume and sends, by track Id. */
        struct Held
        {
            double volume = 1.0;
            std::vector<double> sends;
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

            /*  THE MIXES: each return that reaches the interface, and the master
                when anything is sent to it, in the order of their pairs. A
                return carrying effects Go.dot does not have is parked (QE). */
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

                    bool effects = false;

                    for (const auto& device : ret.devices)
                    {
                        mix.effects.push_back (device.name.empty() ? device.kind : device.name);

                        if (device.kind != "Eq8" && device.kind != "StereoGain")
                            effects = true;
                    }

                    mix.parked = effects;

                    if (mix.pair < 0)
                        notes.push_back ({ Note::Kind::dropped, -1, ret.name,
                                           "the return reaches no output of the interface, so nothing sent to it "
                                           "is heard" });
                    else if (mix.parked)
                        notes.push_back ({ Note::Kind::dropped, -1, ret.name,
                                           "the return carries effects Go.dot has no equivalent for ("
                                           + joined (mix.effects) + "): its sends are imported switched off, "
                                           "their levels kept (namespace draft §29, QE)" });
                    else if (! mix.effects.empty())
                        notes.push_back ({ Note::Kind::dropped, -1, ret.name,
                                           "the return's own " + joined (mix.effects)
                                           + " is not imported: a mix in Go.dot carries no EQ" });

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
                        notes.push_back ({ Note::Kind::dropped, -1, node->name,
                                           "the track goes straight to the interface rather than through a "
                                           "return or the master; that output is not imported" });
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
        };

        struct Walker
        {
            const LiveSet& set;
            const WalkOptions& options;
            const Laws& laws;
            Walk out;
            Graph graph;
            State state;
            double beatsPerRealSecond;

            Walker (const LiveSet& setToUse, const WalkOptions& optionsToUse)
                : set (setToUse), options (optionsToUse), laws (optionsToUse.laws),
                  graph (setToUse, optionsToUse.laws, out.notes),
                  beatsPerRealSecond ((setToUse.tempo > 0.0 ? setToUse.tempo : 120.0) / 60.0)
            {
                for (const auto& track : set.tracks)
                    state.held[track.id] = { track.volume, track.sends };

                for (const auto& ret : set.returns)
                    state.held[ret.id] = { ret.volume, ret.sends };

                out.mixes = graph.mixes;
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
                    const auto step = laws.sampleSeconds * beatsPerRealSecond;

                    for (auto at = from + step; at < to; at += step)
                        beats.push_back (at);
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

                for (const auto& [beat, db] : byBeat)
                    points.push_back ({ sampleSecondsAt (clip, beat, set.tempo), db });

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
                const auto startBeat = clip.loopOn ? clip.loopStart + clip.startRelative : clip.loopStart;
                const auto endBeat = clip.loopEnd;
                const auto fileSeconds = clip.file.sampleRate > 0.0 && clip.file.frames > 0
                                           ? static_cast<double> (clip.file.frames) / clip.file.sampleRate : 0.0;

                const auto perFileSecond = beatsPerSecond (clip);

                if (clip.warped && perFileSecond > 0.0)
                {
                    const auto rate = beatsPerRealSecond / perFileSecond;

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
                else if (! clip.warped)
                {
                    note (Note::Kind::approximated, "the clip is not warped: its markers are read as beats at the "
                                                    "set's tempo, which the probe set is to confirm");
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
                                                   "the import does not read - a pan, a modulation other than the "
                                                   "volume's, or a parameter the set does not name");
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
                    sound.loopIn = std::max (0.0, sampleSecondsAt (clip, clip.loopStart, set.tempo));
                    sound.loopOut = sound.out;
                }

                if (sound.preWait > 0.0)
                    note (Note::Kind::info, "starts " + osc::formatDouble (std::round (sound.preWait * 100.0) / 100.0)
                                              + " s before its file does: a pre-wait of silence");

                //  --- Where it goes ------------------------------------------------
                std::vector<Path> every;
                graph.paths (track.id, {}, every, 0);

                const auto from = std::min (startBeat, clip.loopOn ? clip.loopStart : startBeat);
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

                    if (mix == nullptr || mix->pair < 0)
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

                        sendByBeat.emplace_back (beat, gain > 0.0 ? decibels (gain, laws) + mix->levelDb
                                                                  : laws.floorDb);
                    }

                    auto lane = onFile (clip, sendByBeat);

                    if (silentThroughout (lane, laws))
                        continue;

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
                    send.on = ! mix->parked;
                    lane = finished (lane);

                    if (flat (lane))
                        send.levelDb = clampDb (lane.front().db, laws.silenceDb);
                    else
                        send.lane = std::move (lane);

                    sound.sends.push_back (std::move (send));
                }

                if (sound.sends.empty())
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

                if (std::abs (track.pan) > 0.01)
                    note (Note::Kind::dropped, "panned " + osc::formatDouble (std::round (track.pan * 100.0) / 100.0)
                                               + " in Live; Go.dot has no pan, so it plays centred");

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
            /*  A CARRIER LAUNCHED WITHOUT ITS SOUND (QI): a fade on the sound
                its track is hearing. */
            std::optional<FadeOut> fadeFor (int sceneIndex, const Scene& scene, const Track& carrier, const Clip& clip)
            {
                const auto note = [&] (Note::Kind kind, const std::string& text)
                {
                    out.notes.push_back ({ kind, sceneIndex, carrier.name, text });
                };

                const auto* fed = sourceFor (carrier);

                if (fed == nullptr)
                {
                    note (carrier.input.kind == Routing::Kind::external ? Note::Kind::dropped : Note::Kind::info,
                          carrier.input.kind == Routing::Kind::external
                            ? "the track listens to the interface's input: a live input is a mic cue in Go.dot, "
                              "which this import does not make"
                            : "the track listens with its monitoring In and nothing feeds it: its clip is silent in "
                              "Live, and is not imported");
                    return std::nullopt;
                }

                const auto playing = state.playing.find (fed->id);

                if (playing == state.playing.end())
                {
                    note (Note::Kind::info, "a carrier with no sound on \"" + fed->name + "\" to shape: nothing to "
                                            "fade");
                    return std::nullopt;
                }

                auto* sound = state.sounds.at (playing->second);
                const auto startBeat = clip.loopOn ? clip.loopStart + clip.startRelative : clip.loopStart;
                const auto& held = state.held.at (carrier.id);

                FadeOut fade;
                fade.key = "fade:" + scene.id + ":" + carrier.id;
                fade.scene = sceneIndex;
                fade.target = sound->key;
                fade.name = (clip.name.empty() ? std::string ("Fade") : clip.name) + " - " + carrier.name;

                /*  HOW LONG: to the carrier's last point that moves anything,
                    within the clip. */
                double lastMoving = startBeat;

                for (const auto& envelope : clip.envelopes)
                    if (envelope.target.trackId == carrier.id && moves (envelope))
                        for (const auto& point : envelope.points)
                            if (point.beats > lastMoving && point.beats <= clip.loopEnd)
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

                //  --- The sends the carrier moves --------------------------------
                for (std::size_t send = 0; send < carrier.sends.size(); ++send)
                {
                    const auto* moved = envelopeOn (clip, carrier.id, Parameter::send, static_cast<int> (send));

                    if (moved == nullptr || send >= held.sends.size())
                        continue;

                    const auto key = "return:" + std::to_string (send);
                    const auto from = decibels (held.sends[send], laws);
                    const auto to = decibels (lastValue (*moved), laws);

                    if (const auto* mix = mixNamed (key); mix != nullptr && mix->parked && moves (*moved))
                        note (Note::Kind::dropped, "the carrier moves its send to \"" + mix->name + "\", a return "
                                                   "whose effects are parked: that movement is not heard");

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

                //  --- And where the carrier leaves its track ---------------------
                hold (carrier.id, clip);
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

                    if (! sceneDoesSomething (set, index))
                        continue;

                    const auto& scene = set.scenes[sceneIndex];

                    Step step;
                    step.scene = index;
                    step.sceneId = scene.id;
                    step.name = scene.name;
                    step.notes = scene.annotation;
                    step.colour = scene.colour;

                    if (scene.tempoEnabled && ! same (scene.tempo, set.tempo, 0.01))
                        out.notes.push_back ({ Note::Kind::dropped, index, {}, "the scene changes the tempo to "
                                               + osc::formatDouble (scene.tempo) + "; Go.dot has no tempo, the set's "
                                               "is kept" });

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

                        launchedHere[track.id] = { &clip, clip.loopOn ? clip.loopStart + clip.startRelative
                                                                      : clip.loopStart };
                    }

                    //  --- Sounds first: they decide what the carriers fold into ----
                    std::set<std::string> carriersFolded;

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

                        if (! track.active)
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

                        if (auto fade = fadeFor (index, scene, track, clip))
                            step.fades.push_back (std::move (*fade));
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

                        const auto& launched = launchedHere.at (sound.trackId);
                        hold (sound.trackId, *launched.first);
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
                            hand.goDot = mix != nullptr && mix->parked
                                           ? "nothing yet: \"" + mix->name + "\" carries effects Go.dot does not have"
                                           : "the D700's Send page on the aimed cue, the \"" + mixNameOf (key)
                                               + "\" rotary - under the lock a ride heard and not saved";
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
                            hand.goDot = "nothing: Go.dot has no pan";
                            break;

                        case Parameter::modulation:
                        case Parameter::otherModulation:
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

                for (const auto& hand : out.hands)
                    out.notes.push_back ({ Note::Kind::info, -1, {}, hand.control + ": " + hand.moved + " - in Go.dot, "
                                                                     + hand.goDot });

                /*  A SOUND FROM A TRACK WHOSE FADER A HAND RODE answers to its DCA. */
                for (auto& step : out.steps)
                    for (auto& sound : step.sounds)
                        if (out.dcas.count (sound.trackId) != 0)
                            sound.dcaTrack = sound.trackId;
            }

            /*  THE RESTING VALUES A HAND MAY HAVE LEFT (QP): a track's volume or
                send mapped to a control, used where no envelope covers it. */
            void flagResting()
            {
                for (const auto& mapping : set.mappings)
                {
                    const auto& target = mapping.target;

                    if (target.what != Parameter::volume && target.what != Parameter::send)
                        continue;

                    const auto* mapped = graph.track (target.trackId);

                    if (mapped == nullptr)
                        continue;

                    bool covered = true;

                    for (const auto& step : out.steps)
                        for (const auto& sound : step.sounds)
                            if (sound.trackId == target.trackId)
                            {
                                const auto& clip = *mapped->slots[static_cast<std::size_t> (sound.scene)].clip;

                                if (envelopeOn (clip, target.trackId, target.what, target.sendIndex) == nullptr)
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

            void noteTheSet()
            {
                if (set.arrangementClips > 0)
                    out.notes.push_back ({ Note::Kind::dropped, -1, {}, std::to_string (set.arrangementClips)
                                           + " clip(s) in Arrangement view are not imported: Session view only" });

                for (const auto& track : set.tracks)
                    if (track.kind == Track::Kind::midi)
                        out.notes.push_back ({ Note::Kind::dropped, -1, track.name, "a MIDI track is not imported" });

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
        return std::move (walker.out);
    }
}
