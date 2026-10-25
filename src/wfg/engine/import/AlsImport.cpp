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

#include <wfg/engine/import/AlsImport.h>

#include <wfg/engine/cue/RangeEnd.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/Sequence.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::import::als
{
    namespace
    {
        //======================================================================
        //  Numbers and words as the document spells them.

        using import::number;

        std::string laneText (const std::vector<CurvePoint>& points)
        {
            std::string out;
            double last = -1.0;

            for (const auto& point : points)
            {
                const auto seconds = std::round (std::max (0.0, point.seconds) * 1.0e4) / 1.0e4;

                if (seconds <= last)
                    continue;

                last = seconds;
                out += (out.empty() ? "" : " ") + osc::formatDouble (seconds) + " "
                         + number (std::clamp (point.db, -120.0, 12.0), 2);
            }

            return out;
        }

        /*  THE SET'S IDENTIFIERS (QQ): the shared line of them, salted for Live. */
        struct Identities : import::Identities
        {
            Identities() : import::Identities ("wfg-als:") {}
        };

        //======================================================================
        //  EQ EIGHT ONTO THE CUE'S EQ (QL).

        struct EqFit
        {
            doc::ShowDocument::Attributes rows;
            std::map<int, std::string> bandRow;   ///< Live's band to the cue's row stem: "eqB2", "eqHpf"
            std::vector<std::string> unfit;
        };

        EqFit fitEq (const Device& eq)
        {
            EqFit fit;

            if (! eq.on)
                return fit;

            std::set<int> used;

            const auto clampTo = [] (double value, double low, double high) { return std::clamp (value, low, high); };

            const auto band = [&] (int liveBand, int ours, const char* shape, const EqBand& from)
            {
                const auto stem = "eqB" + std::to_string (ours);
                used.insert (ours);
                fit.bandRow[liveBand] = stem;
                fit.rows.push_back ({ stem + "On", "true" });

                if (shape != nullptr)
                    fit.rows.push_back ({ stem + "Shape", shape });

                fit.rows.push_back ({ stem + "Freq", number (clampTo (from.frequency, 20.0, 20000.0), 2) });
                fit.rows.push_back ({ stem + "Gain", number (clampTo (from.gain, -24.0, 24.0), 2) });
                fit.rows.push_back ({ stem + "Q", number (clampTo (from.q, 0.1, 10.0), 3) });
            };

            for (std::size_t at = 0; at < eq.bands.size(); ++at)
            {
                const auto& one = eq.bands[at];
                const auto liveBand = static_cast<int> (at);

                if (! one.on)
                    continue;

                switch (one.mode)
                {
                    case 0:
                    case 1:
                        fit.rows.push_back ({ "eqHpf", "true" });
                        fit.rows.push_back ({ "eqHpfFreq", number (clampTo (one.frequency, 20.0, 2000.0), 2) });
                        fit.bandRow[liveBand] = "eqHpf";

                        if (one.mode == 0)
                            fit.unfit.push_back ("a 48 dB low cut, made Go.dot's 12 dB high-pass");
                        break;

                    case 6:
                    case 7:
                        fit.rows.push_back ({ "eqLpf", "true" });
                        fit.rows.push_back ({ "eqLpfFreq", number (clampTo (one.frequency, 1000.0, 20000.0), 2) });
                        fit.bandRow[liveBand] = "eqLpf";

                        if (one.mode == 7)
                            fit.unfit.push_back ("a 48 dB high cut, made Go.dot's 12 dB low-pass");
                        break;

                    case 2:
                        if (used.count (1) == 0)
                            band (liveBand, 1, "lowShelf", one);
                        else
                            fit.unfit.push_back ("a second low shelf");
                        break;

                    case 5:
                        if (used.count (4) == 0)
                            band (liveBand, 4, "highShelf", one);
                        else
                            fit.unfit.push_back ("a second high shelf");
                        break;

                    case 3:
                    {
                        bool placed = false;

                        for (const auto ours : { 2, 3, 1, 4 })
                            if (used.count (ours) == 0)
                            {
                                band (liveBand, ours, (ours == 1 || ours == 4) ? "peak" : nullptr, one);
                                placed = true;
                                break;
                            }

                        if (! placed)
                            fit.unfit.push_back ("a fifth bell");
                        break;
                    }

                    default:
                        fit.unfit.push_back ("a notch");
                        break;
                }
            }

            return fit;
        }

        //======================================================================
        //  The media plan: each sound's file on this disk and in the bundle.

        using import::channelsOf;

        //======================================================================
        //  The report (QV).

        std::string sceneWord (const LiveSet& set, int scene)
        {
            if (scene < 0 || static_cast<std::size_t> (scene) >= set.scenes.size())
                return "the set";

            const auto& named = set.scenes[static_cast<std::size_t> (scene)].name;
            return "scene " + std::to_string (scene + 1) + (named.empty() ? std::string {} : " \"" + named + "\"");
        }

        std::string reportText (const juce::File& setFile, const LiveSet& set, const Walk& walked,
                                const std::vector<Note>& notes, const ImportOutcome& outcome)
        {
            std::ostringstream out;

            out << "# Import report: " << setFile.getFileNameWithoutExtension().toStdString() << "\n\n";
            out << "Imported by Go.dot from `" << setFile.getFileName().toStdString() << "` (" << set.creator
                << "), " << juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M").toStdString() << ".\n\n";
            out << "The import never hides an approximation: everything below came over changed, did not come "
                   "over, or is a fact to know before the show is run (namespace draft §29, QV). The laws used to "
                   "turn Live's curves into Go.dot's are the provisional ones until a probe set has measured "
                   "them (QU).\n\n";

            out << "## What came over\n\n";
            out << "- " << outcome.gos << " GO(s), " << outcome.sounds << " sound(s)"
                << (outcome.lives > 0 ? ", " + std::to_string (outcome.lives) + " live input(s) as mic cues (namespace draft §121, BDU)"
                                      : std::string())
                << ".\n";

            int fades = 0, speedFades = 0, stops = 0, lanes = 0, sendLanes = 0, midi = 0;

            for (const auto& step : walked.steps)
            {
                for (const auto& fade : step.fades)
                    (fade.rate.has_value() ? speedFades : fades) += 1;

                stops += static_cast<int> (step.stops.size());
                midi += static_cast<int> (step.midi.size());

                for (const auto& sound : step.sounds)
                {
                    lanes += sound.levelLane.empty() ? 0 : 1;

                    for (const auto& send : sound.sends)
                        sendLanes += send.lane.empty() ? 0 : 1;
                }
            }

            out << "- " << fades << " fade(s) from carrier and MIDI clips, " << speedFades
                << " speed fade(s) at tempo changes, " << stops << " stop(s), " << midi
                << " MIDI cue(s) for controllers the set maps nowhere.\n";
            out << "- " << lanes << " level lane(s), " << sendLanes << " send lane(s).\n\n";

            out << "| GO | Scene | Sounds | Fades | Stops |\n|---|---|---|---|---|\n";

            for (const auto& step : walked.steps)
            {
                std::string sounds;

                for (const auto& sound : step.sounds)
                    sounds += (sounds.empty() ? "" : ", ") + sound.name;

                out << "| " << step.number << " | " << (step.name.empty() ? "(unnamed)" : step.name) << " | "
                    << sounds << " | " << step.fades.size() << " | " << step.stops.size() << " |\n";
            }

            out << "\n## The mixes\n\n";

            std::set<std::string> sentTo;

            for (const auto& step : walked.steps)
                for (const auto& sound : step.sounds)
                {
                    for (const auto& send : sound.sends)
                        sentTo.insert (send.mix);

                    if (! sound.directOut.empty())
                        sentTo.insert (sound.directOut);
                }

            for (const auto& mix : walked.mixes)
            {
                if ((mix.pair < 0 && ! mix.aux) || sentTo.count (mix.key) == 0)
                    continue;

                std::string into;

                for (const auto& send : mix.sends)
                    if (const auto owner = std::find_if (walked.mixes.begin(), walked.mixes.end(),
                                                         [&send] (const Mix& one) { return one.key == send.mix; });
                          owner != walked.mixes.end())
                        into += (into.empty() ? ", sent into \"" : "\", \"") + owner->name;

                out << "- **" << mix.name << "**: " << (mix.pair < 0 ? std::string ("no output of its own")
                                                     : mix.mono ? "output " + std::to_string (mix.pair + 1)
                                                                 : "outputs " + std::to_string (2 * mix.pair + 1) + "/"
                                                                     + std::to_string (2 * mix.pair + 2))
                    << (mix.direct ? " - **a direct out**: a track's own output, named on its cues (namespace draft §121)"
                        : mix.aux ? " - **an aux**: a rack channel of its own, its sends heard (namespace draft §93)" + into
                                      + (into.empty() ? "" : "\"")
                                  : "")
                    << "\n";
            }

            out << "\n## The hands\n\nWhat a controller moved in Live, and what in Go.dot does the same. Nothing about "
                   "the controller is written into the show (PRD §4.9).\n\n";
            out << "| Control | In Live | In Go.dot |\n|---|---|---|\n";

            for (const auto& hand : walked.hands)
                out << "| " << hand.control << " | " << hand.moved << " | " << hand.goDot << " |\n";

            if (! walked.clipFed.empty())
            {
                out << "\n## Moved by the set's own MIDI clips\n\nControls the set's MIDI clips send over a port Live reads "
                       "back, mapped to its own faders and switches: driven by the set, not by a hand, so no DCA is made for "
                       "them (namespace draft §121, BDX, BDY).\n\n";
                out << "| Control | In Live | In Go.dot |\n|---|---|---|\n";

                for (const auto& hand : walked.clipFed)
                    out << "| " << hand.control << " | " << hand.moved << " | " << hand.goDot << " |\n";
            }

            for (const auto kind : { Note::Kind::approximated, Note::Kind::dropped, Note::Kind::info })
            {
                std::vector<const Note*> these;

                for (const auto& note : notes)
                    if (note.kind == kind && ! (kind == Note::Kind::info && note.scene < 0 && note.track.empty()))
                        these.push_back (&note);

                if (these.empty())
                    continue;

                out << "\n## " << (kind == Note::Kind::approximated ? "Approximated"
                                   : kind == Note::Kind::dropped     ? "Not imported"
                                                                     : "To know") << "\n\n";

                for (const auto* note : these)
                    out << "- " << sceneWord (set, note->scene) << (note->track.empty() ? "" : ", track \"" + note->track + "\"")
                        << ": " << note->text << "\n";
            }

            if (! outcome.missingMedia.empty())
            {
                out << "\n## Sounds not found\n\nEach keeps its cue, naming the file; put the file in the show's "
                       "`media/` and it plays.\n\n";

                for (const auto& missing : outcome.missingMedia)
                    out << "- `" << missing << "`\n";
            }

            return out.str();
        }
    }

    //==========================================================================
    std::string idFor (const std::string& key)
    {
        return import::idFor ("wfg-als:", key);
    }

    std::set<int> defaultScenes (const LiveSet& set)
    {
        std::set<int> out;

        for (std::size_t at = 0; at < set.scenes.size(); ++at)
            if (! set.scenes[at].name.empty() && sceneDoesSomething (set, static_cast<int> (at)))
                out.insert (static_cast<int> (at));

        return out;
    }

    std::optional<std::set<int>> parseScenes (const std::string& text)
    {
        std::set<int> out;
        juce::StringArray parts;
        parts.addTokens (juce::String (text), ",", {});
        parts.trim();
        parts.removeEmptyStrings();

        if (parts.isEmpty())
            return std::nullopt;

        for (const auto& part : parts)
        {
            const auto from = part.upToFirstOccurrenceOf ("-", false, false).trim();
            const auto to = part.contains ("-") ? part.fromFirstOccurrenceOf ("-", false, false).trim() : from;

            if (! from.containsOnly ("0123456789") || ! to.containsOnly ("0123456789") || from.isEmpty() || to.isEmpty())
                return std::nullopt;

            const auto low = from.getIntValue();
            const auto high = to.getIntValue();

            if (low < 1 || high < low)
                return std::nullopt;

            for (auto scene = low; scene <= high; ++scene)
                out.insert (scene - 1);
        }

        return out;
    }

    juce::File findMedia (const FileReference& reference, const juce::File& setFolder)
    {
        return import::findFile ({ reference.relativePath, reference.absolutePath, reference.name, reference.size },
                                 { setFolder });
    }

    //==========================================================================
    std::vector<std::uint8_t> presetBytesOf (const PluginInfo& info)
    {
        if (info.state.empty())
            return {};

        /*  AN AU's class info is a property list, which JUCE's AU loader takes
            as it is. */
        if (info.format == "AudioUnit")
            return info.state;

        if (info.format != "VST3")
            return {};

        /*  A VST3's two states in the form JUCE's own saves them in - XML with
            each state in JUCE's base64, behind JUCE's binary header - which the
            loader's setStateInformation reads back. A .vstpreset would need the
            class id spelt the way the SDK spells it, which Live's four numbers
            do not settle. */
        juce::XmlElement xml ("VST3PluginState");

        juce::MemoryBlock component (info.state.data(), info.state.size());
        xml.createNewChildElement ("IComponent")->addTextElement (component.toBase64Encoding());

        if (! info.controllerState.empty())
        {
            juce::MemoryBlock controller (info.controllerState.data(), info.controllerState.size());
            xml.createNewChildElement ("IEditController")->addTextElement (controller.toBase64Encoding());
        }

        //  copyXmlToBinary's shape: its magic, the text's length with its nought, the text, a nought.
        const auto text = xml.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()).toStdString();
        std::vector<std::uint8_t> out;

        const auto little = [&out] (std::uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
                out.push_back (static_cast<std::uint8_t> ((value >> shift) & 0xffu));
        };

        little (0x21324356u);
        little (static_cast<std::uint32_t> (text.size() + 1));
        out.insert (out.end(), text.begin(), text.end());
        out.push_back (0);
        return out;
    }

    BuiltShow build (const LiveSet& set, const Walk& walked, const std::string& listName,
                     const std::map<std::string, std::pair<std::string, int>>& media,
                     doc::ShowDocument& document,
                     const std::vector<plugin::KnownPlugin>& knownPlugins)
    {
        BuiltShow built;
        Identities ids;

        const auto said = [&built] (Note::Kind kind, int scene, const std::string& track, const std::string& text)
        {
            built.notes.push_back ({ kind, scene, track, text });
        };

        const auto write = [&] (const std::string& address, const std::string& text, int scene)
        {
            const auto written = document.setAttribute (address, text);

            if (! written.ok)
                said (Note::Kind::dropped, scene, {}, address + " could not be written (" + written.reason + "): "
                                                      + text.substr (0, 80));

            return written.ok;
        };

        //  --- The list --------------------------------------------------------
        const auto listId = ids.of ("list");
        document.createList (listName, listId);

        //  --- The voices: as many as ever sound at once, with room -------------
        {
            std::set<std::string> sounding;
            std::size_t peak = 0;
            int widest = 2;

            for (const auto& step : walked.steps)
            {
                for (const auto& stop : step.stops)
                    sounding.erase (stop.target);

                for (const auto& sound : step.sounds)
                {
                    sounding.insert (sound.key);

                    if (const auto found = media.find (sound.key); found != media.end())
                        widest = std::max (widest, found->second.second);
                }

                peak = std::max (peak, sounding.size());
            }

            write ("/godot/audio/tracks", std::to_string (std::max<std::size_t> (32, peak + 8)), -1);

            if (widest > 2)
                write ("/godot/audio/channelsPerTrack", std::to_string (std::min (widest, 64)), -1);
        }

        //  --- The mixes, in the order of their pairs (QK) -----------------------
        std::set<std::string> sentTo;

        for (const auto& step : walked.steps)
            for (const auto& sound : step.sounds)
            {
                for (const auto& send : sound.sends)
                    sentTo.insert (send.mix);

                if (! sound.directOut.empty())
                    sentTo.insert (sound.directOut);
            }

        /*  AND WHAT AN AUX SENDS INTO, for every aux something reaches - round
            again until nothing new is reached (§93). */
        for (auto grew = true; grew;)
        {
            grew = false;

            for (const auto& mix : walked.mixes)
                if (mix.aux && sentTo.count (mix.key) != 0)
                    for (const auto& send : mix.sends)
                        grew = sentTo.insert (send.mix).second || grew;
        }

        std::vector<const Mix*> mixes;

        for (const auto& mix : walked.mixes)
            if ((mix.pair >= 0 || mix.aux) && sentTo.count (mix.key) != 0)
                mixes.push_back (&mix);

        //  By their first channel of the interface: a mono one's is its channel, a pair's twice its number.
        const auto firstChannel = [] (const Mix* mix) { return mix->pair < 0 ? -1 : mix->mono ? mix->pair : 2 * mix->pair; };

        std::stable_sort (mixes.begin(), mixes.end(), [&firstChannel] (const Mix* a, const Mix* b)
        {
            return firstChannel (a) < firstChannel (b);
        });

        std::map<std::string, std::string> busOf;
        std::vector<std::string> patch;

        /*  WHERE A MIX OR AN AUX'S OUTPUT LANDS: the channels Live patched it
            to - one bus a channel, the walk having shared them out (§121, BDT). */
        const auto patchPair = [&] (const Mix& mix)
        {
            if (mix.mono)
            {
                patch.push_back (std::to_string (mix.pair));
                return;
            }

            patch.push_back (std::to_string (2 * mix.pair));
            patch.push_back (std::to_string (2 * mix.pair + 1));
        };

        std::vector<std::pair<const Mix*, std::string>> auxes;

        for (const auto* mix : mixes)
        {
            /*  A DIRECT OUT (namespace draft §121, BDS): a bus of that kind on
                the track's channels, named after the track, which its cues name. */
            if (mix->direct)
            {
                const auto busId = ids.of ("direct:" + mix->key);
                const auto made = document.createBus ("direct", mix->mono ? 1 : 2, -1, busId);

                if (! made.ok)
                {
                    said (Note::Kind::dropped, -1, mix->name, "its direct out could not be made (" + made.reason + ")");
                    continue;
                }

                write ("/godot/bus/" + busId + "/name", mix->name, -1);
                busOf[mix->key] = busId;
                patchPair (*mix);
                continue;
            }

            /*  AN AUX (namespace draft §93): its output a direct out of its own
                on its pair, and the channel the sends reach. */
            if (mix->aux)
            {
                std::string outId;

                if (mix->pair >= 0)
                {
                    outId = ids.of ("out:" + mix->key);

                    if (document.createBus ("direct", mix->mono ? 1 : 2, -1, outId).ok)
                    {
                        write ("/godot/bus/" + outId + "/name", mix->name, -1);
                        patchPair (*mix);
                    }
                    else
                    {
                        outId.clear();
                    }
                }

                /*  AS WIDE AS ITS OUTPUT: a mono return a mono aux, so that
                    sent into a stereo mix it can be panned (BDT). */
                const auto auxId = ids.of ("aux:" + mix->key);
                const auto made = document.createRackChannel (mix->mono ? "mono" : "stereo", auxId, "shared");

                if (! made.ok)
                {
                    said (Note::Kind::dropped, -1, mix->name, "its aux could not be made (" + made.reason + ")");
                    continue;
                }

                write ("/godot/slot/" + auxId + "/name", mix->name, -1);
                write ("/godot/slot/" + auxId + "/level", number (std::clamp (mix->levelDb, -120.0, 12.0), 2), -1);

                if (! outId.empty())
                    write ("/godot/slot/" + auxId + "/directOut", outId, -1);

                busOf[mix->key] = auxId;
                auxes.push_back ({ mix, auxId });
                continue;
            }

            const auto busId = ids.of ("mix:" + mix->key);
            const auto made = document.createBus ("mix", mix->mono ? 1 : 2, -1, busId);

            if (! made.ok)
            {
                said (Note::Kind::dropped, -1, mix->name, "its mix could not be made (" + made.reason + ")");
                continue;
            }

            write ("/godot/bus/" + busId + "/name", mix->name, -1);
            busOf[mix->key] = busId;

            //  ON THE CHANNELS LIVE PATCHED IT TO.
            patchPair (*mix);
        }

        /*  EACH AUX'S OWN SOUND (§93): its EQ Eight as its EQ when it fits, its
            plugins found in this machine's scan with the state Live saved, and
            its sends into the other returns. */
        for (const auto& [mix, auxId] : auxes)
        {
            if (! mix->eqs.empty())
            {
                const auto fit = fitEq (*mix->eqs.front());

                for (const auto& [row, value] : fit.rows)
                    write ("/godot/slot/" + auxId + "/" + row, value, -1);

                for (const auto& unfit : fit.unfit)
                    said (Note::Kind::approximated, -1, mix->name, "its EQ Eight has " + unfit);
            }

            for (std::size_t at = 0; at < mix->plugins.size(); ++at)
            {
                const auto& info = *mix->plugins[at]->plugin;
                const auto format = info.format == "AudioUnit" ? std::string ("AU") : info.format;

                if (format != "VST3" && format != "AU")
                {
                    said (Note::Kind::dropped, -1, mix->name, "its " + info.name + " is a VST2 plugin, which Go.dot "
                                                              "does not host: put its VST3 on the aux");
                    continue;
                }

                const auto found = std::find_if (knownPlugins.begin(), knownPlugins.end(), [&] (const plugin::KnownPlugin& known)
                {
                    return known.format == format
                             && juce::String::fromUTF8 (known.name.c_str()).trim()
                                  .equalsIgnoreCase (juce::String::fromUTF8 (info.name.c_str()).trim());
                });

                if (found == knownPlugins.end())
                {
                    said (Note::Kind::dropped, -1, mix->name, "its " + info.name + " (" + format + ") is not in this "
                                                              "machine's scan: scan in Show settings, Plugins, then put it "
                                                              "on the aux");
                    continue;
                }

                const auto pluginId = ids.of ("plugin:" + mix->key + ":" + std::to_string (at));
                const auto made = document.createChannelPlugin (auxId, found->name, found->identifier, found->format,
                                                                found->path, pluginId);

                if (! made.ok)
                    continue;

                if (const auto bytes = presetBytesOf (info); ! bytes.empty())
                {
                    const auto file = "aux-" + pluginId + ".state";
                    built.files.push_back ({ "plugins/" + file, bytes });
                    write ("/godot/plugin/" + pluginId + "/preset", file, -1);
                }
            }

            for (const auto& send : mix->sends)
            {
                const auto target = busOf.find (send.mix);

                if (target == busOf.end())
                    continue;

                const auto sendId = ids.of ("auxsend:" + mix->key + ":" + send.mix);
                const auto made = document.createSend (auxId, target->second, sendId,
                                                       number (std::clamp (send.levelDb, -120.0, 12.0), 2));

                if (made.ok && send.pan.has_value())
                    write ("/godot/send/" + sendId + "/pan", number (*send.pan, 2), -1);
            }
        }

        {
            std::string text;

            for (const auto& channel : patch)
                text += (text.empty() ? "" : " ") + channel;

            if (! text.empty())
                write ("/godot/audio/outputPatch", text, -1);
        }

        //  --- The DCAs (QN) ---------------------------------------------------
        std::map<std::string, std::string> dcaOf;

        for (const auto& [trackId, name] : walked.dcas)
        {
            const auto dcaId = ids.of ("dca:" + trackId);

            if (document.createDca (name, dcaId).ok)
                dcaOf[trackId] = dcaId;
        }

        //  --- The GOs ---------------------------------------------------------
        std::map<std::string, std::string> cueOf;       // walk key to cue id
        std::map<std::string, EqFit> eqOf;              // sound key to its EQ's fit
        std::vector<std::string> inputPatch;            // the live inputs' channels of the interface, in their order (BDU)
        std::map<std::string, std::string> portOf;      // Live's MIDI port by name to the Port declared for it (BDV)

        //  THE GRID'S TEMPO: the set's, the scenes' own as tempo cues (§121, BDZ; §122).
        write ("/godot/sync/bpm", number (set.tempo > 0.0 ? set.tempo : 120.0, 3), -1);
        write ("/godot/sync/beatsPerBar", std::to_string (wholeBeatsPerBar (set.timeSignature)), -1);

        for (const auto& step : walked.steps)
        {
            const auto items = step.sounds.size() + step.fades.size() + step.stops.size() + step.midi.size()
                                 + (step.tempo > 0.0 || step.beatsPerBar > 0 ? 1u : 0u);
            const auto alone = items == 1 && step.sounds.size() == 1;
            const auto numberText = std::to_string (step.number);

            /*  WHERE THE GO STARTS ON THE GRID (BDZ): the quantisation its sounds
                share goes on the group, which places its members together as
                Live's scene does; a sound whose own differs is said. */
            std::string groupQuantise;

            for (const auto& sound : step.sounds)
                if (! sound.quantise.empty())
                {
                    if (groupQuantise.empty())
                        groupQuantise = sound.quantise;
                    else if (groupQuantise != sound.quantise)
                        said (Note::Kind::approximated, step.scene, sound.trackName, "\"" + sound.name + "\" starts on the "
                              "next " + sound.quantise + " in Live where the scene's other sounds start on the next "
                              + groupQuantise + ": the GO starts them together, on the " + groupQuantise);
                }

            std::string parent = listId;

            if (! alone)
            {
                const auto groupId = ids.of ("go:" + step.sceneId);
                doc::ShowDocument::Attributes attributes { { "mode", "timeline" }, { "number", numberText } };

                if (! step.notes.empty())
                    attributes.push_back ({ "notes", step.notes });

                if (! groupQuantise.empty())
                    attributes.push_back ({ "quantise", groupQuantise });

                const auto made = document.createCue (listId, doc::endOfSequence, "group",
                                                      step.name.empty() ? "Scene " + std::to_string (step.scene + 1)
                                                                        : step.name,
                                                      groupId, attributes);

                if (! made.ok)
                {
                    said (Note::Kind::dropped, step.scene, {}, "the GO could not be made (" + made.reason + ")");
                    continue;
                }

                parent = groupId;
            }

            ++built.gos;

            //  --- Its tempo and its bar, first (§121 BDR, BEG; §122 BED) ---------
            if (step.tempo > 0.0 || step.beatsPerBar > 0)
            {
                doc::ShowDocument::Attributes attributes { { "verb", "tempo" } };
                std::string name;

                if (step.tempo > 0.0)
                {
                    attributes.push_back ({ "bpm", number (step.tempo, 3) });
                    name = "Tempo " + osc::formatDouble (std::round (step.tempo * 100.0) / 100.0);
                }

                if (step.beatsPerBar > 0)
                {
                    attributes.push_back ({ "beatsPerBar", std::to_string (step.beatsPerBar) });
                    name += (name.empty() ? "Bar of " : ", bar of ") + std::to_string (step.beatsPerBar);
                }

                const auto made = document.createCue (parent, doc::endOfSequence, "transport", name,
                                                      ids.of ("tempo:" + step.sceneId), attributes);

                if (! made.ok)
                    said (Note::Kind::dropped, step.scene, {}, "the tempo cue could not be made (" + made.reason + ")");
            }

            //  --- Its sounds ----------------------------------------------------
            for (const auto& sound : step.sounds)
            {
                const auto cueId = ids.of (sound.key);
                doc::ShowDocument::Attributes attributes;

                const auto named = media.find (sound.key);
                const auto file = named != media.end() ? named->second.first : sound.file.name;
                const auto channels = sound.live ? (sound.inputStereo ? 2 : 1)
                                    : named != media.end() ? named->second.second : 0;

                /*  A LIVE INPUT IS A MIC CUE (namespace draft §121, BDU): on a
                    named input of the interface, patched to the channel Live
                    listened to, through an exclusive rack channel of its own. */
                if (sound.live)
                {
                    const auto inputId = ids.of ("in:" + sound.key);
                    const auto channelId = ids.of ("chan:" + sound.key);
                    const auto madeInput = document.createInput (sound.inputStereo ? 2 : 1, -1, inputId);
                    const auto madeChannel = document.createRackChannel (sound.inputStereo ? "stereo" : "mono", channelId);

                    if (! madeInput.ok || ! madeChannel.ok)
                    {
                        said (Note::Kind::dropped, step.scene, sound.trackName, "the live input \"" + sound.name
                              + "\" could not be made (" + (madeInput.ok ? madeChannel.reason : madeInput.reason) + ")");
                        continue;
                    }

                    write ("/godot/input/" + inputId + "/name", sound.name, step.scene);
                    write ("/godot/slot/" + channelId + "/name", sound.name, step.scene);

                    if (sound.inputStereo)
                    {
                        inputPatch.push_back (std::to_string (2 * sound.inputChannel));
                        inputPatch.push_back (std::to_string (2 * sound.inputChannel + 1));
                    }
                    else
                    {
                        inputPatch.push_back (std::to_string (sound.inputChannel));
                    }

                    attributes.push_back ({ "input", inputId });
                    attributes.push_back ({ "channel", channelId });
                }
                else
                {
                    attributes.push_back ({ "file", file });

                    if (channels > 0)
                        attributes.push_back ({ "channels", std::to_string (channels) });
                }

                if (std::abs (sound.levelDb) > 0.005)
                    attributes.push_back ({ "level", number (std::clamp (sound.levelDb, -120.0, 12.0), 2) });

                if (sound.preWait > 0.0005)
                    attributes.push_back ({ "preWait", number (sound.preWait, 4) });

                if (std::abs (sound.rate - 1.0) > 0.0005)
                {
                    attributes.push_back ({ "rate", number (std::clamp (sound.rate, 0.0, 20.0), 4) });

                    if (sound.timestretch)
                        attributes.push_back ({ "rateMode", "timestretch" });
                }

                if (! sound.levelLane.empty())
                    attributes.push_back ({ "levelLane", laneText (sound.levelLane) });

                if (const auto dca = dcaOf.find (sound.dcaTrack); ! sound.dcaTrack.empty() && dca != dcaOf.end())
                    attributes.push_back ({ "dca", dca->second });

                /*  ITS DIRECT OUT (§121, BDS) - shared on purpose: in Live every
                    clip of the track, and every track on the pair, sums there. */
                if (const auto direct = busOf.find (sound.directOut); ! sound.directOut.empty() && direct != busOf.end())
                {
                    attributes.push_back ({ "directOut", direct->second });
                    attributes.push_back ({ "sharedOut", "true" });
                }

                if (alone)
                {
                    attributes.push_back ({ "number", numberText });

                    if (! step.notes.empty())
                        attributes.push_back ({ "notes", step.notes });

                    if (! sound.quantise.empty())
                        attributes.push_back ({ "quantise", sound.quantise });
                }

                /*  WHAT LIVE DID TO IT THAT GO.DOT DOES NOT, under any notes it
                    has, so the row says it where the operator reads (QM). */
                if (! sound.effects.empty())
                {
                    std::string line = "Live: ";

                    for (std::size_t at = 0; at < sound.effects.size(); ++at)
                        line += (at == 0 ? "" : "; ") + sound.effects[at];

                    line += " - not imported.";

                    auto row = std::find_if (attributes.begin(), attributes.end(),
                                             [] (const auto& one) { return one.first == "notes"; });

                    if (row == attributes.end())
                        attributes.push_back ({ "notes", line });
                    else
                        row->second += "\n\n" + line;
                }

                //  A mic cue keeps its track's name: the scene is not what it is.
                const auto cueName = alone && ! step.name.empty() && ! sound.live ? step.name : sound.name;
                const auto made = document.createCue (parent, doc::endOfSequence, sound.live ? "mic" : "media", cueName,
                                                      cueId, attributes);

                if (! made.ok)
                {
                    said (Note::Kind::dropped, step.scene, sound.trackName, "the sound \"" + sound.name
                                                                              + "\" could not be made (" + made.reason + ")");
                    continue;
                }

                cueOf[sound.key] = cueId;
                (sound.live ? built.lives : built.sounds) += 1;

                //  --- Its ranges --------------------------------------------------
                const auto fileLength = sound.file.sampleRate > 0.0
                                          ? static_cast<double> (sound.file.frames) / sound.file.sampleRate : 0.0;
                const auto whole = sound.live || (sound.in < 0.0005 && fileLength > 0.0 && std::abs (sound.out - fileLength) < 0.0005);

                /*  A RANGE LIVE PLAYS TO THE FILE'S END RUNS TO THE END (§99): so a
                    file bounced again or replaced still plays to its own. */
                const auto loopToEnd = cue::reachesTheEnd (sound.loopOut, fileLength);

                if (sound.loops && ! sound.live)
                {
                    if (std::abs (sound.in - sound.loopIn) < 0.0005)
                    {
                        const auto range = document.createRange (cueId, sound.loopIn, sound.loopOut, ids.of (sound.key + ":loop"),
                                                                 loopToEnd);

                        if (range.ok)
                            write ("/godot/range/" + range.id + "/loops", "0", step.scene);
                    }
                    else
                    {
                        document.createRange (cueId, sound.in, sound.loopOut, ids.of (sound.key + ":range"), loopToEnd);
                        const auto range = document.createRange (cueId, sound.loopIn, sound.loopOut, ids.of (sound.key + ":loop"),
                                                                 loopToEnd);

                        if (range.ok)
                            write ("/godot/range/" + range.id + "/loops", "0", step.scene);
                    }
                }
                else if (! whole && sound.out > sound.in)
                {
                    document.createRange (cueId, sound.in, sound.out, ids.of (sound.key + ":range"),
                                          cue::reachesTheEnd (sound.out, fileLength));
                }

                //  --- Its EQ (QL) -------------------------------------------------
                if (! sound.eqs.empty())
                {
                    auto fit = fitEq (*sound.eqs.front());

                    for (const auto& [row, value] : fit.rows)
                        write ("/godot/cue/" + cueId + "/" + row, value, step.scene);

                    for (const auto& unfit : fit.unfit)
                        said (Note::Kind::approximated, step.scene, sound.trackName, "its EQ Eight has " + unfit);

                    eqOf[sound.key] = std::move (fit);
                }

                //  --- Its sends ---------------------------------------------------
                for (const auto& send : sound.sends)
                {
                    const auto bus = busOf.find (send.mix);

                    if (bus == busOf.end())
                        continue;

                    const auto sendId = ids.of (sound.key + ":send:" + send.mix);
                    const auto made2 = document.createSend (cueId, bus->second, sendId,
                                                            number (std::clamp (send.levelDb, -120.0, 12.0), 2));

                    if (! made2.ok)
                        continue;

                    if (! send.on)
                        write ("/godot/send/" + sendId + "/on", "false", step.scene);

                    if (! send.lane.empty())
                        write ("/godot/send/" + sendId + "/levelLane", laneText (send.lane), step.scene);

                    /*  ITS PLACE (namespace draft §113, BCG): the track's pan, a
                        pan on a mono file and a balance on a stereo one - into
                        a mono mix the balance leans the fold. A file not found
                        has no channel count, and the pan waits for the day it
                        is; nothing is written at the centre. */
                    if (send.pan.has_value())
                        write ("/godot/send/" + sendId + "/pan", number (*send.pan, 2), step.scene);
                    else if (std::abs (sound.pan) > 0.005 && (channels == 1 || channels == 2))
                        write ("/godot/send/" + sendId + "/" + (channels == 1 ? "pan" : "balance"),
                               number (std::round (sound.pan * 100.0) / 100.0, 2), step.scene);
                }
            }

            //  --- Its fades ------------------------------------------------------
            for (const auto& fade : step.fades)
            {
                const auto target = cueOf.find (fade.target);

                if (target == cueOf.end())
                {
                    said (Note::Kind::dropped, step.scene, {}, "a fade on a sound that was not imported");
                    continue;
                }

                doc::ShowDocument::Attributes attributes { { "target", target->second },
                                                           { "duration", number (fade.duration, 4) },
                                                           { "level", number (std::clamp (fade.levelDb, -120.0, 12.0), 2) } };

                if (fade.points.size() >= 2)
                {
                    std::string points;
                    double last = -1.0;

                    for (const auto& point : fade.points)
                    {
                        const auto t = std::round (std::clamp (point.seconds, 0.0, 1.0) * 1.0e4) / 1.0e4;

                        if (t <= last)
                            continue;

                        last = t;
                        points += (points.empty() ? "" : " ") + osc::formatDouble (t) + " "
                                    + number (std::clamp (point.db, -120.0, 12.0), 2);
                    }

                    /*  A drawing starts at nought and ends at one, whatever the
                        rounding left of its last point. */
                    if (last < 1.0 && ! points.empty())
                        points += " 1 " + number (std::clamp (fade.levelDb, -120.0, 12.0), 2);

                    attributes.push_back ({ "points", points });
                }

                if (fade.stopWhenDone)
                    attributes.push_back ({ "stopWhenDone", "true" });

                /*  A SPEED FADE AT A TEMPO CHANGE (§121, BDR): the rate moved,
                    the level left where it is. */
                if (fade.rate.has_value())
                {
                    attributes.push_back ({ "levelOn", "false" });
                    attributes.push_back ({ "rateOn", "true" });
                    attributes.push_back ({ "rate", number (std::clamp (*fade.rate, 0.0, 20.0), 4) });
                }

                {
                    std::string sends;

                    for (const auto& [mix, db] : fade.sends)
                        if (const auto bus = busOf.find (mix); bus != busOf.end())
                            sends += (sends.empty() ? "" : " ") + bus->second + ":" + number (std::clamp (db, -120.0, 12.0), 2);

                    if (! sends.empty())
                        attributes.push_back ({ "sends", sends });
                }

                /*  THE EQ ROWS THE CARRIER MOVED, where the sound's EQ Eight
                    became the cue's EQ and that band became one of its rows. */
                if (const auto fit = eqOf.find (fade.target); fit != eqOf.end())
                {
                    std::map<std::string, std::string> rows;

                    for (const auto& move : fade.eq)
                    {
                        const auto row = fit->second.bandRow.find (move.band);

                        if (row == fit->second.bandRow.end())
                            continue;

                        if (row->second == "eqHpf" || row->second == "eqLpf")
                        {
                            if (move.what == "Freq")
                                rows[row->second + "Freq"] = number (row->second == "eqHpf"
                                                                       ? std::clamp (move.value, 20.0, 2000.0)
                                                                       : std::clamp (move.value, 1000.0, 20000.0), 2);
                            continue;
                        }

                        rows[row->second + move.what] = move.what == "Freq"
                                                          ? number (std::clamp (move.value, 20.0, 20000.0), 2)
                                                          : number (std::clamp (move.value, -24.0, 24.0), 2);
                    }

                    std::string eq;

                    for (const auto& [row, value] : rows)
                        eq += (eq.empty() ? "" : " ") + row + ":" + value;

                    if (! eq.empty())
                        attributes.push_back ({ "eq", eq });
                }

                const auto made = document.createCue (parent, doc::endOfSequence, "fade", fade.name, ids.of (fade.key),
                                                      attributes);

                if (! made.ok)
                    said (Note::Kind::dropped, step.scene, {}, "the fade \"" + fade.name + "\" could not be made ("
                                                               + made.reason + ")");
            }

            //  --- Its controllers sent out (§121, BDV) ---------------------------
            for (const auto& sent : step.midi)
            {
                auto port = portOf.find (sent.port);

                if (port == portOf.end())
                {
                    const auto made = document.createPort (sent.port, ids.of ("port:" + sent.port));

                    if (! made.ok)
                    {
                        said (Note::Kind::dropped, step.scene, {}, "the MIDI port \"" + sent.port + "\" could not be declared ("
                                                                   + made.reason + ")");
                        continue;
                    }

                    port = portOf.emplace (sent.port, made.id).first;
                }

                doc::ShowDocument::Attributes attributes { { "port", port->second },
                                                           { "type", "controlChange" },
                                                           { "channel", std::to_string (sent.channel) },
                                                           { "data1", std::to_string (sent.controller) },
                                                           { "data2", std::to_string (sent.from) } };

                if (sent.to != sent.from)
                {
                    attributes.push_back ({ "fadeTo", std::to_string (sent.to) });
                    attributes.push_back ({ "fadeTime", number (std::clamp (sent.seconds, 0.0, 3600.0), 3) });
                }

                const auto made = document.createCue (parent, doc::endOfSequence, "midi", sent.name, ids.of (sent.key), attributes);

                if (! made.ok)
                    said (Note::Kind::dropped, step.scene, {}, "the MIDI cue \"" + sent.name + "\" could not be made ("
                                                               + made.reason + ")");
            }

            //  --- Its stops ------------------------------------------------------
            for (const auto& stop : step.stops)
            {
                const auto target = cueOf.find (stop.target);

                if (target == cueOf.end())
                    continue;

                const auto made = document.createCue (parent, doc::endOfSequence, "transport", stop.name, ids.of (stop.key),
                                                      { { "target", target->second }, { "verb", "hard" } });

                if (! made.ok)
                    said (Note::Kind::dropped, step.scene, {}, "the stop \"" + stop.name + "\" could not be made ("
                                                               + made.reason + ")");
            }
        }

        //  THE LIVE INPUTS ON THE CHANNELS LIVE LISTENED TO (BDU).
        if (! inputPatch.empty())
        {
            std::string text;

            for (const auto& channel : inputPatch)
                text += (text.empty() ? "" : " ") + channel;

            write ("/godot/audio/inputPatch", text, -1);
        }

        /*  STANDBY ON THE FIRST GO, where a show made by hand has it once its
            first cue is made: with no standby, GO has nothing to fire. */
        if (const auto first = std::find_if (walked.steps.begin(), walked.steps.end(),
                                             [&cueOf, &ids] (const Step& step)
                                             {
                                                 return ids.given.count ("go:" + step.sceneId) != 0
                                                          || (step.sounds.size() == 1 && cueOf.count (step.sounds.front().key) != 0);
                                             });
              first != walked.steps.end())
        {
            const auto groupKey = "go:" + first->sceneId;
            const auto standby = ids.given.count (groupKey) != 0 ? ids.given.at (groupKey)
                                                                 : cueOf.at (first->sounds.front().key);
            write ("/godot/list/" + listId + "/standby", standby, -1);
        }

        (void) set;
        return built;
    }

    //==========================================================================
    namespace
    {
        /*  THE SOUNDS OF ONE OR SEVERAL SETS, copied into one `media/` once each:
            a source file to the name it has there, and the names taken. */
        using import::MediaBook;

        struct Placed
        {
            std::map<std::string, std::pair<std::string, int>> media;   ///< sound key to name and channels
            std::vector<std::string> missing;
            std::vector<Note> notes;
            bool ok = true;
            std::string error;
        };

        /*  EACH SOUND'S FILE FOUND ON THIS DISK, copied into `mediaFolder`
            unless it is there already, and read for its channels (QR). */
        Placed placeMedia (const Walk& walked, const juce::File& setFolder, const juce::File& mediaFolder,
                           bool copy, MediaBook& book, const std::function<void (const std::string&)>& say)
        {
            Placed placed;

            if (copy && ! mediaFolder.createDirectory())
            {
                placed.ok = false;
                placed.error = "the folder " + mediaFolder.getFullPathName().toStdString() + " could not be made";
                return placed;
            }

            for (const auto& step : walked.steps)
            {
                for (const auto& sound : step.sounds)
                {
                    if (sound.live)     // a mic cue has no file to find (§121, BDU)
                        continue;

                    const auto source = findMedia (sound.file, setFolder);

                    if (source == juce::File())
                    {
                        placed.missing.push_back (sound.file.relativePath.empty() ? sound.file.name
                                                                                 : sound.file.relativePath);
                        placed.notes.push_back ({ Note::Kind::dropped, sound.scene, sound.trackName,
                                                  "the file \"" + sound.file.name + "\" was not found: the cue names "
                                                  "it and plays nothing until it is in the show's media/" });
                        placed.media[sound.key] = { sound.file.name, 0 };
                        continue;
                    }

                    const auto file = import::placeFile (source, mediaFolder, copy, book, say);

                    if (file.copyFailed)
                    {
                        placed.notes.push_back ({ Note::Kind::dropped, sound.scene, sound.trackName,
                                                  "the file \"" + file.name + "\" could not be copied into media/" });
                        placed.missing.push_back (file.name);
                    }

                    placed.media[sound.key] = { file.name, file.channels };
                }
            }

            return placed;
        }

        /*  ONE SHOW WRITTEN INTO A FOLDER: built, validated, saved, and its
            report beside it. */
        ImportOutcome writeShow (const juce::File& setFile, const LiveSet& set, const Walk& walked,
                                 std::vector<Note> notes, const Placed& placed, const juce::File& folder,
                                 const std::string& listName, const std::vector<plugin::KnownPlugin>& knownPlugins)
        {
            ImportOutcome outcome;
            outcome.missingMedia = placed.missing;
            notes.insert (notes.end(), placed.notes.begin(), placed.notes.end());

            doc::ShowDocument document;
            const auto built = build (set, walked, listName, placed.media, document, knownPlugins);
            notes.insert (notes.end(), built.notes.begin(), built.notes.end());

            const auto saved = import::saveShow (document, folder, "set");
            outcome.problems = saved.problems;

            /*  THE FILES THE SHOW NAMES BESIDE IT: an aux's plugin's state (§93). */
            for (const auto& [path, bytes] : built.files)
            {
                const auto file = folder.getChildFile (juce::String::fromUTF8 (path.c_str()));
                file.getParentDirectory().createDirectory();
                file.replaceWithData (bytes.data(), bytes.size());
            }

            if (! saved.ok)
            {
                outcome.error = saved.error;
                return outcome;
            }

            outcome.ok = true;
            outcome.show = folder;
            outcome.gos = built.gos;
            outcome.sounds = built.sounds;
            outcome.lives = built.lives;

            for (const auto& note : notes)
            {
                outcome.approximated += note.kind == Note::Kind::approximated ? 1 : 0;
                outcome.dropped += note.kind == Note::Kind::dropped ? 1 : 0;
            }

            outcome.report = folder.getChildFile ("import-report.md");
            outcome.report.replaceWithText (juce::String::fromUTF8 (reportText (setFile, set, walked, notes, outcome).c_str()),
                                            false, false, "\n");
            return outcome;
        }

        using import::holdsAShow;

        /*  A SET WITH NO ANNOTATIONS AT ALL takes the template's, scene by scene
            (QT): the older Lazzi sets predate Live's annotations, and without this
            every one of their performances would read as "notes removed". */
        bool borrowNotes (LiveSet& set, const LiveSet& from)
        {
            const auto any = std::any_of (set.scenes.begin(), set.scenes.end(),
                                          [] (const Scene& scene) { return ! scene.annotation.empty(); });

            if (any)
                return false;

            bool borrowed = false;

            for (auto& scene : set.scenes)
                for (const auto& theirs : from.scenes)
                    if (theirs.id == scene.id && ! theirs.annotation.empty())
                    {
                        scene.annotation = theirs.annotation;
                        borrowed = true;
                    }

            return borrowed;
        }

        /*  The scenes chosen in one set, found in another by their Live
            identifier. */
        std::set<int> sameScenes (const LiveSet& chosenIn, const std::set<int>& chosen, const LiveSet& in)
        {
            std::set<std::string> ids;

            for (const auto index : chosen)
                if (index >= 0 && static_cast<std::size_t> (index) < chosenIn.scenes.size())
                    ids.insert (chosenIn.scenes[static_cast<std::size_t> (index)].id);

            std::set<int> out;

            for (std::size_t at = 0; at < in.scenes.size(); ++at)
                if (ids.count (in.scenes[at].id) != 0)
                    out.insert (static_cast<int> (at));

            return out;
        }
    }

    //==========================================================================
    ImportOutcome importSet (const juce::File& setFile, const ImportOptions& options)
    {
        ImportOutcome outcome;

        const auto say = [&options] (const std::string& sentence)
        {
            if (options.progress)
                options.progress (sentence);
        };

        say ("reading " + setFile.getFileName().toStdString());
        const auto read = readSet (setFile);

        if (! read.set.has_value())
        {
            outcome.error = read.error;
            return outcome;
        }

        const auto& set = *read.set;

        /*  NEVER OVER SOMEBODY'S SHOW. */
        if (options.into == juce::File())
        {
            outcome.error = "no folder to import into";
            return outcome;
        }

        if (holdsAShow (options.into))
        {
            outcome.error = "the folder " + options.into.getFullPathName().toStdString()
                            + " already holds a show; import into a new one";
            return outcome;
        }

        say ("walking the scenes");
        WalkOptions walkOptions;
        walkOptions.scenes = options.scenes.empty() ? defaultScenes (set) : options.scenes;
        walkOptions.laws = options.laws;
        const auto walked = walk (set, walkOptions);

        MediaBook book;
        const auto placed = placeMedia (walked, setFile.getParentDirectory(), options.into.getChildFile ("media"),
                                        options.copyMedia, book, say);

        if (! placed.ok)
        {
            outcome.error = placed.error;
            return outcome;
        }

        say ("writing the show");
        outcome = writeShow (setFile, set, walked, walked.notes, placed, options.into,
                             setFile.getFileNameWithoutExtension().toStdString(), options.knownPlugins);
        say ("done");
        return outcome;
    }

    //==========================================================================
    std::string sharedPrefix (const std::vector<std::string>& names)
    {
        if (names.size() < 2)
            return {};

        auto prefix = names.front();

        for (const auto& name : names)
        {
            std::size_t same = 0;

            while (same < prefix.size() && same < name.size() && prefix[same] == name[same])
                ++same;

            prefix.resize (same);
        }

        /*  BACK TO A WHOLE WORD: "Lazzi régie " and not "Lazzi régie s" when
            two venues happen to start with the same letter. */
        const auto space = prefix.find_last_of (' ');
        return space == std::string::npos ? std::string {} : prefix.substr (0, space + 1);
    }

    std::string performanceName (const juce::File& set, const std::string& prefix)
    {
        const auto stem = set.getFileNameWithoutExtension().toStdString();
        auto rest = stem.size() > prefix.size() && stem.compare (0, prefix.size(), prefix) == 0
                      ? stem.substr (prefix.size()) : stem;

        /*  A name a folder can take on every system: what Windows refuses, as
            a hyphen. */
        for (auto& character : rest)
            if (std::string_view ("<>:\"/\\|?*").find (character) != std::string_view::npos)
                character = '-';

        const auto date = set.getLastModificationTime().formatted ("%Y-%m-%d").toStdString();
        return date + " " + juce::String::fromUTF8 (rest.c_str()).trim().toStdString();
    }

    TourOutcome importTour (const std::vector<juce::File>& sets, const juce::File& templateSet,
                            const ImportOptions& options)
    {
        TourOutcome tour;
        auto& show = tour.show;

        const auto say = [&options] (const std::string& sentence)
        {
            if (options.progress)
                options.progress (sentence);
        };

        if (sets.empty())
        {
            show.error = "no set to import";
            return tour;
        }

        if (options.into == juce::File())
        {
            show.error = "no folder to import into";
            return tour;
        }

        if (holdsAShow (options.into))
        {
            show.error = "the folder " + options.into.getFullPathName().toStdString()
                         + " already holds a show; import into a new one";
            return tour;
        }

        //  --- Every set read, the template chosen ------------------------------
        std::vector<LiveSet> read;

        for (const auto& file : sets)
        {
            say ("reading " + file.getFileName().toStdString());
            auto result = readSet (file);

            if (! result.set.has_value())
            {
                show.error = file.getFileName().toStdString() + ": " + result.error;
                return tour;
            }

            read.push_back (std::move (*result.set));
        }

        std::size_t chosen = 0;

        for (std::size_t at = 0; at < sets.size(); ++at)
        {
            if (templateSet != juce::File() ? sets[at] == templateSet
                                            : sets[at].getLastModificationTime() > sets[chosen].getLastModificationTime())
                chosen = at;
        }

        const auto& model = read[chosen];
        const auto scenes = options.scenes.empty() ? defaultScenes (model) : options.scenes;

        std::vector<std::string> stems;

        for (const auto& file : sets)
            stems.push_back (file.getFileNameWithoutExtension().toStdString());

        const auto prefix = sharedPrefix (stems);
        const auto showName = options.into.getFileName().toStdString();

        MediaBook book;
        const auto mediaFolder = options.into.getChildFile ("media");

        /*  ONE SET'S SHOW: its scenes found by identifier, its notes borrowed
            where it has none, its sounds placed in the show's media/. */
        const auto one = [&] (std::size_t index, const juce::File& folder, const std::string& listName)
        {
            auto set = read[index];
            const auto borrowed = index != chosen && borrowNotes (set, model);

            WalkOptions walkOptions;
            walkOptions.scenes = index == chosen ? scenes : sameScenes (model, scenes, set);
            walkOptions.laws = options.laws;

            say ("walking " + sets[index].getFileName().toStdString());
            const auto walked = walk (set, walkOptions);
            auto notes = walked.notes;

            if (borrowed)
                notes.push_back ({ Note::Kind::info, -1, {}, "the set has no annotations of its own: each GO's notes "
                                                             "are the template's, scene by scene (namespace draft §29, QT)" });

            const auto placed = placeMedia (walked, sets[index].getParentDirectory(), mediaFolder, options.copyMedia,
                                            book, say);

            if (! placed.ok)
            {
                ImportOutcome failed;
                failed.error = placed.error;
                return failed;
            }

            return writeShow (sets[index], set, walked, notes, placed, folder, listName, options.knownPlugins);
        };

        //  --- The template, in the show folder ---------------------------------
        say ("writing the template from " + sets[chosen].getFileName().toStdString());
        show = one (chosen, options.into, showName);

        if (! show.ok)
            return tour;

        //  --- A performance per set, folded inside it ---------------------------
        std::set<std::string> taken;

        for (std::size_t at = 0; at < sets.size(); ++at)
        {
            auto name = performanceName (sets[at], prefix);

            for (int again = 2; taken.count (name) != 0; ++again)
                name = performanceName (sets[at], prefix) + " " + std::to_string (again);

            taken.insert (name);

            const auto folder = options.into.getChildFile (juce::String::fromUTF8 (name.c_str()));
            say ("writing the performance " + name);
            /*  THE SHOW'S LIST NAME, not the venue's: the list is the piece's, and a
                name that differed in every performance would be one more change
                "Update the show's template..." listed for each of them. */
            tour.performances.push_back (one (at, folder, showName));
        }

        say ("done");
        return tour;
    }
}
