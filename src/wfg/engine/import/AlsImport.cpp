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

        /*  ROUNDED BEFORE THEY ARE WRITTEN: a tenth of a millisecond and a
            hundredth of a decibel, through the engine's own formatter, so the
            file reads and no locale moves a digit. */
        std::string number (double value, int decimals)
        {
            const auto scale = std::pow (10.0, decimals);
            return osc::formatDouble (std::round (value * scale) / scale);
        }

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

        std::uint64_t fnv1a (const std::string& text)
        {
            std::uint64_t hash = 14695981039346656037ull;

            for (const auto character : text)
            {
                hash ^= static_cast<unsigned char> (character);
                hash *= 1099511628211ull;
            }

            return hash;
        }

        /*  IDENTIFIERS, ONE PER KEY, NEVER TWO ALIKE: a key that hashes onto
            one already given takes the next of its own line, so the answer is
            still the same every time the same set is imported. */
        struct Identities
        {
            std::map<std::string, std::string> given;
            std::set<std::string> taken;

            std::string of (const std::string& key)
            {
                if (const auto found = given.find (key); found != given.end())
                    return found->second;

                auto id = idFor (key);

                for (int again = 2; taken.count (id) != 0; ++again)
                    id = idFor (key + "#" + std::to_string (again));

                taken.insert (id);
                given[key] = id;
                return id;
            }
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

        int channelsOf (const juce::File& file)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();

            const std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) };
            return reader != nullptr ? static_cast<int> (reader->numChannels) : 0;
        }

        /*  A FILE NAME WITH ITS ACCENTS FOLDED AWAY, for comparing two
            spellings of one name. macOS writes a name DECOMPOSED - an "e" and a
            combining acute - and Live keeps it so in the set; a copy made on
            another system has it COMPOSED, one "é". The Lazzi sets name five of
            their files the first way and the disk holds them the second. So a
            combining mark is dropped, and a composed Latin letter (U+00C0 to
            U+017F) becomes the letter it is built on, by the table below -
            Unicode's own decompositions, generated, not typed. */
        juce::String folded (const juce::String& name)
        {
            static constexpr juce::juce_wchar latin[] = {
                0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0xC6, 0x43, 0x45, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49,
                0xD0, 0x4E, 0x4F, 0x4F, 0x4F, 0x4F, 0x4F, 0xD7, 0xD8, 0x55, 0x55, 0x55, 0x55, 0x59, 0xDE, 0xDF,
                0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0xE6, 0x63, 0x65, 0x65, 0x65, 0x65, 0x69, 0x69, 0x69, 0x69,
                0xF0, 0x6E, 0x6F, 0x6F, 0x6F, 0x6F, 0x6F, 0xF7, 0xF8, 0x75, 0x75, 0x75, 0x75, 0x79, 0xFE, 0x79,
                0x41, 0x61, 0x41, 0x61, 0x41, 0x61, 0x43, 0x63, 0x43, 0x63, 0x43, 0x63, 0x43, 0x63, 0x44, 0x64,
                0x110, 0x111, 0x45, 0x65, 0x45, 0x65, 0x45, 0x65, 0x45, 0x65, 0x45, 0x65, 0x47, 0x67, 0x47, 0x67,
                0x47, 0x67, 0x47, 0x67, 0x48, 0x68, 0x126, 0x127, 0x49, 0x69, 0x49, 0x69, 0x49, 0x69, 0x49, 0x69,
                0x49, 0x131, 0x132, 0x133, 0x4A, 0x6A, 0x4B, 0x6B, 0x138, 0x4C, 0x6C, 0x4C, 0x6C, 0x4C, 0x6C, 0x13F,
                0x140, 0x141, 0x142, 0x4E, 0x6E, 0x4E, 0x6E, 0x4E, 0x6E, 0x149, 0x14A, 0x14B, 0x4F, 0x6F, 0x4F, 0x6F,
                0x4F, 0x6F, 0x152, 0x153, 0x52, 0x72, 0x52, 0x72, 0x52, 0x72, 0x53, 0x73, 0x53, 0x73, 0x53, 0x73,
                0x53, 0x73, 0x54, 0x74, 0x54, 0x74, 0x166, 0x167, 0x55, 0x75, 0x55, 0x75, 0x55, 0x75, 0x55, 0x75,
                0x55, 0x75, 0x55, 0x75, 0x57, 0x77, 0x59, 0x79, 0x59, 0x5A, 0x7A, 0x5A, 0x7A, 0x5A, 0x7A, 0x17F,
            };

            juce::String out;

            for (auto character = name.getCharPointer(); ! character.isEmpty(); ++character)
            {
                const auto code = *character;

                if (code >= 0x300 && code <= 0x36F)
                    continue;

                out += code >= 0xC0 && code <= 0x17F ? latin[code - 0xC0] : code;
            }

            return out;
        }

        /*  THE NAMES A `:` BECOMES on a copy that left the Mac. */
        std::vector<std::string> spellings (const std::string& name)
        {
            std::vector<std::string> out { name };

            if (name.find (':') == std::string::npos)
                return out;

            for (const auto instead : { '/', '_', ' ' })
            {
                auto spelled = name;
                std::replace (spelled.begin(), spelled.end(), ':', instead);
                out.push_back (spelled);
            }

            /*  And dropped, with the space after it, which is what the Lazzi
                copy did: "19_CARGO: S.Berger" became "19_CARGO S.Berger". */
            auto dropped = name;
            dropped.erase (std::remove (dropped.begin(), dropped.end(), ':'), dropped.end());
            out.push_back (dropped);
            return out;
        }

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
            out << "- " << outcome.gos << " GO(s), " << outcome.sounds << " sound(s).\n";

            int fades = 0, stops = 0, lanes = 0, sendLanes = 0;

            for (const auto& step : walked.steps)
            {
                fades += static_cast<int> (step.fades.size());
                stops += static_cast<int> (step.stops.size());

                for (const auto& sound : step.sounds)
                {
                    lanes += sound.levelLane.empty() ? 0 : 1;

                    for (const auto& send : sound.sends)
                        sendLanes += send.lane.empty() ? 0 : 1;
                }
            }

            out << "- " << fades << " fade(s) from carrier clips launched alone, " << stops << " stop(s).\n";
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
                    for (const auto& send : sound.sends)
                        sentTo.insert (send.mix);

            for (const auto& mix : walked.mixes)
            {
                if (mix.pair < 0 || sentTo.count (mix.key) == 0)
                    continue;

                out << "- **" << mix.name << "**: " << (mix.mono ? "output " + std::to_string (mix.pair + 1)
                                                                 : "outputs " + std::to_string (2 * mix.pair + 1) + "/"
                                                                     + std::to_string (2 * mix.pair + 2))
                    << (mix.parked ? " - **parked**: its effects are Live's, its sends are written switched off" : "")
                    << "\n";
            }

            out << "\n## The hands\n\nWhat a controller moved in Live, and what in Go.dot does the same. Nothing about "
                   "the controller is written into the show (PRD §4.9).\n\n";
            out << "| Control | In Live | In Go.dot |\n|---|---|---|\n";

            for (const auto& hand : walked.hands)
                out << "| " << hand.control << " | " << hand.moved << " | " << hand.goDot << " |\n";

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
        return doc::Id::encode (fnv1a ("wfg-als:" + key) & 0xFFFFFFFFFFull);
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
        /*  BY ITS RELATIVE PATH, a folder at a time, so a `:` in one part is
            tried as its spellings without the drive-letter reading Windows
            would give the whole string. */
        if (! reference.relativePath.empty())
        {
            juce::StringArray parts;
            parts.addTokens (juce::String::fromUTF8 (reference.relativePath.c_str()), "/", {});
            parts.removeEmptyStrings();

            std::vector<juce::File> here { setFolder };

            for (const auto& part : parts)
            {
                std::vector<juce::File> next;

                for (const auto& folder : here)
                    for (const auto& spelled : spellings (part.toStdString()))
                        next.push_back (folder.getChildFile (juce::String::fromUTF8 (spelled.c_str())));

                here = std::move (next);
            }

            for (const auto& candidate : here)
                if (candidate.existsAsFile())
                    return candidate;
        }

        if (! reference.absolutePath.empty())
            if (const juce::File absolute { juce::String::fromUTF8 (reference.absolutePath.c_str()) };
                  juce::File::isAbsolutePath (absolute.getFullPathName()) && absolute.existsAsFile())
                return absolute;

        /*  BY NAME AND SIZE, under the set's folder - the project a set sits in
            holds its samples, and a size is what tells two takes of one name
            apart - each name compared with its accents folded, so a name Live
            kept decomposed finds the file a copy composed. */
        std::set<juce::String> wanted;

        for (const auto& spelled : spellings (reference.name))
            wanted.insert (folded (juce::String::fromUTF8 (spelled.c_str())));

        for (const auto& entry : juce::RangedDirectoryIterator (setFolder, true, "*", juce::File::findFiles))
        {
            const auto file = entry.getFile();

            if (file.getFullPathName().contains ("Backup") || file.getFileExtension() == ".asd")
                continue;

            if (wanted.count (folded (file.getFileName())) == 0)
                continue;

            if (reference.size <= 0 || file.getSize() == reference.size)
                return file;
        }

        return {};
    }

    //==========================================================================
    BuiltShow build (const LiveSet& set, const Walk& walked, const std::string& listName,
                     const std::map<std::string, std::pair<std::string, int>>& media,
                     doc::ShowDocument& document)
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
                for (const auto& send : sound.sends)
                    sentTo.insert (send.mix);

        std::vector<const Mix*> mixes;

        for (const auto& mix : walked.mixes)
            if (mix.pair >= 0 && sentTo.count (mix.key) != 0)
                mixes.push_back (&mix);

        std::stable_sort (mixes.begin(), mixes.end(), [] (const Mix* a, const Mix* b)
        {
            return a->parked != b->parked ? b->parked : a->pair < b->pair;
        });

        std::map<std::string, std::string> busOf;
        std::vector<std::string> patch;
        std::set<int> patchedPairs;

        for (const auto* mix : mixes)
        {
            const auto busId = ids.of ("mix:" + mix->key);
            const auto made = document.createBus ("mix", mix->mono ? 1 : 2, -1, busId);

            if (! made.ok)
            {
                said (Note::Kind::dropped, -1, mix->name, "its mix could not be made (" + made.reason + ")");
                continue;
            }

            write ("/godot/bus/" + busId + "/name", mix->name, -1);
            busOf[mix->key] = busId;

            /*  ON THE CHANNELS LIVE PATCHED IT TO - and a parked return on none,
                since nothing it would carry is heard. Two mixes Live put on one
                pair sum there, which Go.dot allows and says so. */
            if (mix->parked)
            {
                for (int side = 0; side < (mix->mono ? 1 : 2); ++side)
                    patch.push_back ("-1");
            }
            else if (mix->mono)
            {
                patch.push_back (std::to_string (mix->pair));
            }
            else
            {
                if (! patchedPairs.insert (mix->pair).second)
                    said (Note::Kind::info, -1, mix->name, "shares its outputs with another mix, as in Live: the two sum");

                patch.push_back (std::to_string (2 * mix->pair));
                patch.push_back (std::to_string (2 * mix->pair + 1));
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

        for (const auto& step : walked.steps)
        {
            const auto items = step.sounds.size() + step.fades.size() + step.stops.size();
            const auto alone = items == 1 && step.sounds.size() == 1;
            const auto numberText = std::to_string (step.number);

            std::string parent = listId;

            if (! alone)
            {
                const auto groupId = ids.of ("go:" + step.sceneId);
                doc::ShowDocument::Attributes attributes { { "mode", "timeline" }, { "number", numberText } };

                if (! step.notes.empty())
                    attributes.push_back ({ "notes", step.notes });

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

            //  --- Its sounds ----------------------------------------------------
            for (const auto& sound : step.sounds)
            {
                const auto cueId = ids.of (sound.key);
                doc::ShowDocument::Attributes attributes;

                const auto named = media.find (sound.key);
                const auto file = named != media.end() ? named->second.first : sound.file.name;
                const auto channels = named != media.end() ? named->second.second : 0;

                attributes.push_back ({ "file", file });

                if (channels > 0)
                    attributes.push_back ({ "channels", std::to_string (channels) });

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

                if (alone)
                {
                    attributes.push_back ({ "number", numberText });

                    if (! step.notes.empty())
                        attributes.push_back ({ "notes", step.notes });
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

                const auto cueName = alone && ! step.name.empty() ? step.name : sound.name;
                const auto made = document.createCue (parent, doc::endOfSequence, "media", cueName, cueId, attributes);

                if (! made.ok)
                {
                    said (Note::Kind::dropped, step.scene, sound.trackName, "the sound \"" + sound.name
                                                                              + "\" could not be made (" + made.reason + ")");
                    continue;
                }

                cueOf[sound.key] = cueId;
                ++built.sounds;

                //  --- Its ranges --------------------------------------------------
                const auto fileLength = sound.file.sampleRate > 0.0
                                          ? static_cast<double> (sound.file.frames) / sound.file.sampleRate : 0.0;
                const auto whole = sound.in < 0.0005 && fileLength > 0.0 && std::abs (sound.out - fileLength) < 0.0005;

                if (sound.loops)
                {
                    if (std::abs (sound.in - sound.loopIn) < 0.0005)
                    {
                        const auto range = document.createRange (cueId, sound.loopIn, sound.loopOut, ids.of (sound.key + ":loop"));

                        if (range.ok)
                            write ("/godot/range/" + range.id + "/loops", "0", step.scene);
                    }
                    else
                    {
                        document.createRange (cueId, sound.in, sound.loopOut, ids.of (sound.key + ":range"));
                        const auto range = document.createRange (cueId, sound.loopIn, sound.loopOut, ids.of (sound.key + ":loop"));

                        if (range.ok)
                            write ("/godot/range/" + range.id + "/loops", "0", step.scene);
                    }
                }
                else if (! whole && sound.out > sound.in)
                {
                    document.createRange (cueId, sound.in, sound.out, ids.of (sound.key + ":range"));
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
        struct MediaBook
        {
            std::map<juce::String, std::string> copied;
            std::set<std::string> namesUsed;
        };

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

                    const auto key = source.getFullPathName();

                    if (const auto done = book.copied.find (key); done != book.copied.end())
                    {
                        const auto there = mediaFolder.getChildFile (juce::String::fromUTF8 (done->second.c_str()));
                        placed.media[sound.key] = { done->second, channelsOf (there.existsAsFile() ? there : source) };
                        continue;
                    }

                    /*  ITS OWN NAME, unless another file already took it. */
                    auto name = source.getFileName().toStdString();

                    for (int again = 2; book.namesUsed.count (name) != 0; ++again)
                        name = source.getFileNameWithoutExtension().toStdString() + " (" + std::to_string (again) + ")"
                                 + source.getFileExtension().toStdString();

                    book.namesUsed.insert (name);
                    book.copied[key] = name;

                    const auto destination = mediaFolder.getChildFile (juce::String::fromUTF8 (name.c_str()));

                    if (copy && ! (destination.existsAsFile() && destination.getSize() == source.getSize()))
                    {
                        if (say)
                            say ("copying " + name);

                        if (! source.copyFileTo (destination))
                        {
                            placed.notes.push_back ({ Note::Kind::dropped, sound.scene, sound.trackName,
                                                      "the file \"" + name + "\" could not be copied into media/" });
                            placed.missing.push_back (name);
                        }
                    }

                    placed.media[sound.key] = { name, channelsOf (copy ? destination : source) };
                }
            }

            return placed;
        }

        /*  ONE SHOW WRITTEN INTO A FOLDER: built, validated, saved, and its
            report beside it. */
        ImportOutcome writeShow (const juce::File& setFile, const LiveSet& set, const Walk& walked,
                                 std::vector<Note> notes, const Placed& placed, const juce::File& folder,
                                 const std::string& listName)
        {
            ImportOutcome outcome;
            outcome.missingMedia = placed.missing;
            notes.insert (notes.end(), placed.notes.begin(), placed.notes.end());

            doc::ShowDocument document;
            const auto built = build (set, walked, listName, placed.media, document);
            notes.insert (notes.end(), built.notes.begin(), built.notes.end());

            outcome.problems = document.validate();

            if (! outcome.problems.empty())
            {
                outcome.error = "the show built from the set did not validate: " + outcome.problems.front();
                return outcome;
            }

            const auto saved = doc::Bundle::save (folder, document);

            if (! saved.ok)
            {
                outcome.error = "the show could not be saved"
                                + (saved.problems.empty() ? std::string {} : ": " + saved.problems.front());
                return outcome;
            }

            outcome.ok = true;
            outcome.show = folder;
            outcome.gos = built.gos;
            outcome.sounds = built.sounds;

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

        bool holdsAShow (const juce::File& folder)
        {
            return doc::Bundle::manifestFile (folder).existsAsFile() || doc::Bundle::showFile (folder).existsAsFile();
        }

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
                             setFile.getFileNameWithoutExtension().toStdString());
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

            return writeShow (sets[index], set, walked, notes, placed, folder, listName);
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
