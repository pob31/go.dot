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

#include <wfg/engine/import/AlsReader.h>

#include <wfg/engine/osc/OscValue.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::import::als
{
    namespace
    {
        //======================================================================
        //  Reading Live's XML. Live writes every value as an element with a
        //  `Value` attribute - `<Name><EffectiveName Value="1"/></Name>` - so a
        //  path of element names and the attribute at its end is the whole
        //  vocabulary, spelled once here.

        const juce::XmlElement* at (const juce::XmlElement* from, std::string_view path)
        {
            auto* here = from;

            while (here != nullptr && ! path.empty())
            {
                const auto slash = path.find ('/');
                const auto name = path.substr (0, slash);

                here = here->getChildByName (juce::String (std::string (name)));
                path = slash == std::string_view::npos ? std::string_view {} : path.substr (slash + 1);
            }

            return here;
        }

        std::string text (const juce::XmlElement* from, std::string_view path, const char* attribute = "Value")
        {
            const auto* element = at (from, path);
            return element != nullptr ? element->getStringAttribute (attribute).toStdString() : std::string {};
        }

        bool has (const juce::XmlElement* from, std::string_view path)
        {
            return at (from, path) != nullptr;
        }

        /*  NUMBERS THROUGH THE ENGINE'S OWN PARSER, which no locale moves: a
            French machine reads "0.5" as a half, as Live wrote it. */
        double number (const juce::XmlElement* from, std::string_view path, double fallback = 0.0,
                       const char* attribute = "Value")
        {
            const auto written = text (from, path, attribute);

            if (written == "true")  return 1.0;
            if (written == "false") return 0.0;

            return osc::parseDouble (written).value_or (fallback);
        }

        double numberAttribute (const juce::XmlElement& element, const char* attribute, double fallback = 0.0)
        {
            return osc::parseDouble (element.getStringAttribute (attribute).toStdString()).value_or (fallback);
        }

        bool flag (const juce::XmlElement* from, std::string_view path, bool fallback = false)
        {
            const auto written = text (from, path);
            return written.empty() ? fallback : written == "true";
        }

        int integer (const juce::XmlElement* from, std::string_view path, int fallback = 0)
        {
            return static_cast<int> (std::lround (number (from, path, static_cast<double> (fallback))));
        }

        std::int64_t whole (const juce::XmlElement* from, std::string_view path)
        {
            return static_cast<std::int64_t> (std::llround (number (from, path, 0.0)));
        }

        //======================================================================
        //  Routing, as Live spells it: "AudioOut/None", "AudioOut/Track.28/TrackIn",
        //  "AudioIn/Track.27/TrackOut", "AudioOut/Master", "AudioOut/GroupTrack",
        //  "AudioOut/External/S0", "AudioIn/External/M17".

        Routing routingOf (const std::string& target)
        {
            Routing out;
            out.target = target;

            const auto slash = target.find ('/');
            const auto rest = slash == std::string::npos ? std::string {} : target.substr (slash + 1);

            if (rest.empty() || rest == "None")
            {
                out.kind = Routing::Kind::none;
            }
            else if (rest.rfind ("Track.", 0) == 0)
            {
                out.kind = Routing::Kind::track;
                const auto end = rest.find ('/', 6);
                out.trackId = rest.substr (6, end == std::string::npos ? std::string::npos : end - 6);
            }
            else if (rest == "Master" || rest == "Main")
            {
                out.kind = Routing::Kind::master;
            }
            else if (rest == "GroupTrack")
            {
                out.kind = Routing::Kind::group;
            }
            else if (rest.rfind ("External/", 0) == 0 && rest.size() > 10)
            {
                out.kind = Routing::Kind::external;
                out.stereo = rest[9] == 'S';
                out.channel = static_cast<int> (osc::parseDouble (rest.substr (10)).value_or (-1.0));
            }

            return out;
        }

        //======================================================================
        //  THE TARGET TABLE: every automatable parameter in the set by the `Id`
        //  of its `AutomationTarget` (or of a clip's modulation target), so a
        //  clip envelope's `PointeeId` and a mapping's parameter both find what
        //  they move.

        using Targets = std::map<std::string, Target>;

        /*  Every element under `node` that carries an AutomationTarget, by its
            path from `node` - a device's parameters, an EQ band's - each
            handed to `found` with that path. An `AutomationTarget` inside a
            parameter is the parameter's; one deeper is a nested parameter's. */
        void eachParameter (const juce::XmlElement& node, const std::string& path,
                            const std::function<void (const juce::XmlElement&, const std::string&)>& found)
        {
            for (auto* child : node.getChildIterator())
            {
                const auto name = child->getTagName().toStdString();

                if (name == "AutomationTarget" || name == "ModulationTarget" || name == "MidiControllerRange"
                      || name == "KeyMidi" || name == "LomId" || name == "ArrangerAutomation")
                    continue;

                const auto here = path.empty() ? name : path + "/" + name;

                if (child->getChildByName ("AutomationTarget") != nullptr)
                    found (*child, here);

                eachParameter (*child, here, found);
            }
        }

        std::string targetIdOf (const juce::XmlElement& parameter, const char* which = "AutomationTarget")
        {
            const auto* automation = parameter.getChildByName (which);
            return automation != nullptr ? automation->getStringAttribute ("Id").toStdString() : std::string {};
        }

        /*  A parameter's two targets into the table: its automation's, and its
            modulation's marked relative. */
        void enter (Targets& targets, const juce::XmlElement& parameter, const Target& target)
        {
            if (const auto id = targetIdOf (parameter); ! id.empty())
                targets[id] = target;

            if (const auto id = targetIdOf (parameter, "ModulationTarget"); ! id.empty())
            {
                auto relative = target;
                relative.relative = true;
                targets[id] = relative;
            }
        }

        //======================================================================
        //  Mappings: a `KeyMidi` beside a parameter's automation target.

        std::optional<Mapping> mappingIn (const juce::XmlElement* keyMidi)
        {
            if (keyMidi == nullptr)
                return std::nullopt;

            const auto channel = integer (keyMidi, "Channel", -1);
            const auto controller = integer (keyMidi, "NoteOrController", -1);

            if (channel < 0 || controller < 0)
                return std::nullopt;

            Mapping out;
            out.channel = channel + 1;
            out.number = controller;
            out.note = flag (keyMidi, "IsNote");
            return out;
        }

        //======================================================================

        struct Reader
        {
            LiveSet set;
            Targets targets;

            /*  The mixer of a track: its volume, pan and sends, and what any
                hand was mapped to. */
            void readMixer (const juce::XmlElement* mixer, Track& track)
            {
                if (mixer == nullptr)
                {
                    set.problems.push_back ("track \"" + track.name + "\" has no mixer");
                    return;
                }

                track.volume = number (mixer, "Volume/Manual", 1.0);
                track.pan = number (mixer, "Pan/Manual", 0.0);
                track.active = flag (mixer, "Speaker/Manual", true);

                const auto note = [this, &track] (const juce::XmlElement* parameter, Target target)
                {
                    if (parameter == nullptr)
                        return;

                    target.trackId = track.id;
                    enter (targets, *parameter, target);

                    if (auto mapping = mappingIn (parameter->getChildByName ("KeyMidi")))
                    {
                        mapping->target = target;
                        set.mappings.push_back (*mapping);
                    }
                };

                const auto of = [] (Parameter what)
                {
                    Target target;
                    target.what = what;
                    return target;
                };

                note (at (mixer, "Volume"), of (Parameter::volume));
                note (at (mixer, "Pan"), of (Parameter::pan));

                if (const auto* sends = at (mixer, "Sends"))
                {
                    int index = 0;

                    for (auto* holder : sends->getChildWithTagNameIterator ("TrackSendHolder"))
                    {
                        const auto* send = holder->getChildByName ("Send");
                        track.sends.push_back (number (send, "Manual", 0.0));

                        auto target = of (Parameter::send);
                        target.sendIndex = index++;
                        note (send, target);
                    }
                }
            }

            void readDevices (const juce::XmlElement* devices, Track& track)
            {
                if (devices == nullptr)
                    return;

                int index = 0;

                for (auto* element : devices->getChildIterator())
                {
                    Device device;
                    device.kind = element->getTagName().toStdString();
                    device.on = flag (element, "On/Manual", true);

                    device.name = text (element, "PluginDesc/VstPluginInfo/PlugName");

                    if (device.name.empty())
                        device.name = text (element, "PluginDesc/Vst3PluginInfo/Name");
                    if (device.name.empty())
                        device.name = text (element, "PluginDesc/AuPluginInfo/Name");
                    if (device.name.empty())
                        device.name = text (element, "UserName");

                    const auto deviceIndex = index++;

                    eachParameter (*element, {}, [&] (const juce::XmlElement& parameter, const std::string& path)
                    {
                        if (const auto* manual = parameter.getChildByName ("Manual"))
                        {
                            const auto written = manual->getStringAttribute ("Value").toStdString();
                            device.values[path] = written == "true" ? 1.0
                                                : written == "false" ? 0.0
                                                : osc::parseDouble (written).value_or (0.0);
                        }

                        Target target;
                        target.trackId = track.id;
                        target.what = Parameter::device;
                        target.deviceIndex = deviceIndex;
                        target.deviceKind = device.kind;
                        target.path = path;
                        enter (targets, parameter, target);

                        if (auto mapping = mappingIn (parameter.getChildByName ("KeyMidi")))
                        {
                            mapping->target = target;
                            set.mappings.push_back (*mapping);
                        }
                    });

                    if (device.kind == "Eq8")
                    {
                        for (int band = 0; band < 8; ++band)
                        {
                            const auto* a = at (element, "Bands." + std::to_string (band) + "/ParameterA");

                            EqBand eq;
                            eq.on = flag (a, "IsOn/Manual");
                            eq.mode = integer (a, "Mode/Manual", 3);
                            eq.frequency = number (a, "Freq/Manual", 1000.0);
                            eq.gain = number (a, "Gain/Manual", 0.0);
                            eq.q = number (a, "Q/Manual", 0.7071);
                            device.bands.push_back (eq);
                        }
                    }

                    track.devices.push_back (std::move (device));
                }
            }

            /*  The clip's own modulation targets, which sit on the track's
                sequencer rather than on a parameter: the volume's, and the
                rest by name. */
            void readModulationTargets (const juce::XmlElement* sequencer, const Track& track)
            {
                if (sequencer == nullptr)
                    return;

                for (auto* child : sequencer->getChildIterator())
                {
                    const auto name = child->getTagName().toStdString();
                    constexpr std::string_view suffix = "ModulationTarget";

                    if (name.size() <= suffix.size() || name.compare (name.size() - suffix.size(), suffix.size(), suffix) != 0)
                        continue;

                    Target target;
                    target.trackId = track.id;
                    target.what = name == "VolumeModulationTarget" ? Parameter::modulation : Parameter::otherModulation;
                    target.path = name.substr (0, name.size() - suffix.size());
                    target.relative = true;

                    if (const auto id = child->getStringAttribute ("Id").toStdString(); ! id.empty())
                        targets[id] = target;
                }
            }

            FileReference readFile (const juce::XmlElement* sampleRef)
            {
                FileReference out;
                const auto* fileRef = at (sampleRef, "FileRef");

                if (fileRef == nullptr)
                    return out;

                out.name = text (fileRef, "Name");
                out.absolutePath = text (fileRef, "Path");

                /*  LIVE 11 AND AFTER: one string. LIVE 10: a folder per element,
                    and the file's name beside them. */
                if (const auto* relative = at (fileRef, "RelativePath"))
                {
                    out.relativePath = relative->getStringAttribute ("Value").toStdString();

                    if (out.relativePath.empty())
                    {
                        for (auto* folder : relative->getChildWithTagNameIterator ("RelativePathElement"))
                            out.relativePath += folder->getStringAttribute ("Dir").toStdString() + "/";

                        out.relativePath += out.name;
                    }
                }

                if (out.name.empty())
                {
                    const auto& from = ! out.relativePath.empty() ? out.relativePath : out.absolutePath;
                    const auto slash = from.find_last_of ('/');
                    out.name = slash == std::string::npos ? from : from.substr (slash + 1);
                }

                out.size = whole (fileRef, "OriginalFileSize");
                out.crc = whole (fileRef, "OriginalCrc");
                out.sampleRate = number (sampleRef, "DefaultSampleRate", 0.0);
                out.frames = whole (sampleRef, "DefaultDuration");
                return out;
            }

            Clip readClip (const juce::XmlElement& element)
            {
                Clip clip;
                clip.name = text (&element, "Name");
                clip.colour = has (&element, "Color") ? integer (&element, "Color", -1)
                                                      : integer (&element, "ColorIndex", -1);
                clip.file = readFile (at (&element, "SampleRef"));

                clip.loopStart = number (&element, "Loop/LoopStart");
                clip.loopEnd = number (&element, "Loop/LoopEnd");
                clip.startRelative = number (&element, "Loop/StartRelative");
                clip.loopOn = flag (&element, "Loop/LoopOn");
                clip.currentStart = number (&element, "CurrentStart");
                clip.currentEnd = number (&element, "CurrentEnd");

                clip.gain = number (&element, "SampleVolume", 1.0);
                clip.warped = flag (&element, "IsWarped", true);
                clip.warpMode = integer (&element, "WarpMode", 0);
                clip.pitchCoarse = integer (&element, "PitchCoarse", 0);
                clip.pitchFine = number (&element, "PitchFine", 0.0);

                if (const auto* markers = at (&element, "WarpMarkers"))
                    for (auto* marker : markers->getChildWithTagNameIterator ("WarpMarker"))
                        clip.warp.push_back ({ numberAttribute (*marker, "SecTime"),
                                               numberAttribute (*marker, "BeatTime") });

                clip.deactivated = flag (&element, "Disabled");
                clip.legato = flag (&element, "Legato");
                clip.launchMode = integer (&element, "LaunchMode", 0);
                clip.quantisation = integer (&element, "LaunchQuantisation", 0);

                /*  LIVE 11 SAYS WHETHER A FOLLOW ACTION IS ON; Live 10 has an
                    action other than "none" for it. */
                if (has (&element, "FollowAction/FollowActionEnabled"))
                    clip.followAction = flag (&element, "FollowAction/FollowActionEnabled");
                else
                    clip.followAction = integer (&element, "FollowAction/FollowActionA", 0) != 0
                                          || integer (&element, "FollowAction/FollowActionB", 0) != 0;

                if (const auto* envelopes = at (&element, "Envelopes/Envelopes"))
                {
                    for (auto* one : envelopes->getChildWithTagNameIterator ("ClipEnvelope"))
                    {
                        Envelope envelope;
                        envelope.pointeeId = text (one, "EnvelopeTarget/PointeeId");

                        if (const auto found = targets.find (envelope.pointeeId); found != targets.end())
                            envelope.target = found->second;

                        if (const auto* events = at (one, "Automation/Events"))
                        {
                            for (auto* event : events->getChildIterator())
                            {
                                const auto kind = event->getTagName();
                                envelope.switches = kind == "BoolEvent" || kind == "EnumEvent";

                                const auto written = event->getStringAttribute ("Value").toStdString();
                                const auto value = written == "true" ? 1.0
                                                 : written == "false" ? 0.0
                                                 : osc::parseDouble (written).value_or (0.0);

                                envelope.points.push_back ({ numberAttribute (*event, "Time"), value });
                            }
                        }

                        clip.envelopes.push_back (std::move (envelope));
                    }
                }

                return clip;
            }

            /*  A track: what it is, where it sends its sound, its mixer and
                devices, and - in a second pass, once every target is known -
                its clip slots. */
            Track readTrack (const juce::XmlElement& element, Track::Kind kind)
            {
                Track track;
                track.id = element.getStringAttribute ("Id").toStdString();
                track.kind = kind;
                track.name = text (&element, "Name/EffectiveName");

                if (track.name.empty())
                    track.name = text (&element, "Name/UserName");

                if (const auto group = text (&element, "TrackGroupId"); ! group.empty() && group != "-1")
                    track.groupId = group;

                const auto* chain = at (&element, "DeviceChain");

                switch (integer (chain, "MainSequencer/MonitoringEnum", 1))
                {
                    case 0:  track.monitoring = Track::Monitoring::in; break;
                    case 2:  track.monitoring = Track::Monitoring::off; break;
                    default: track.monitoring = Track::Monitoring::automatic; break;
                }

                track.input = routingOf (text (chain, "AudioInputRouting/Target"));
                track.output = routingOf (text (chain, "AudioOutputRouting/Target"));

                readMixer (at (chain, "Mixer"), track);
                readDevices (at (chain, "DeviceChain/Devices"), track);
                readModulationTargets (at (chain, "MainSequencer"), track);

                return track;
            }

            void readSlots (const juce::XmlElement& element, Track& track)
            {
                const auto* list = at (&element, "DeviceChain/MainSequencer/ClipSlotList");

                if (list == nullptr)
                    return;

                for (auto* slot : list->getChildWithTagNameIterator ("ClipSlot"))
                {
                    Slot out;
                    out.hasStop = flag (slot, "HasStop", true);

                    if (const auto* audio = at (slot, "ClipSlot/Value/AudioClip"))
                        out.clip = readClip (*audio);
                    else if (at (slot, "ClipSlot/Value/MidiClip") != nullptr)
                        set.problems.push_back ("track \"" + track.name + "\" holds a MIDI clip, which is not read");

                    track.slots.push_back (std::move (out));
                }

                if (const auto* events = at (&element, "DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events"))
                    set.arrangementClips += events->getNumChildElements();
            }

            void readScenes (const juce::XmlElement& liveSet)
            {
                /*  LIVE 11 AND AFTER: `Scenes`, the name an element of its own. */
                if (const auto* scenes = at (&liveSet, "Scenes"))
                {
                    for (auto* element : scenes->getChildWithTagNameIterator ("Scene"))
                    {
                        Scene scene;
                        scene.id = element->getStringAttribute ("Id").toStdString();
                        scene.name = text (element, "Name");
                        scene.annotation = text (element, "Annotation");
                        scene.colour = integer (element, "Color", -1);
                        scene.tempoEnabled = flag (element, "IsTempoEnabled");
                        scene.tempo = number (element, "Tempo", 0.0);
                        scene.followAction = flag (element, "FollowAction/FollowActionEnabled");
                        set.scenes.push_back (std::move (scene));
                    }

                    return;
                }

                /*  LIVE 10: `SceneNames`, the name the scene's own value. */
                if (const auto* names = at (&liveSet, "SceneNames"))
                {
                    for (auto* element : names->getChildWithTagNameIterator ("Scene"))
                    {
                        Scene scene;
                        scene.id = element->getStringAttribute ("Id").toStdString();
                        scene.name = element->getStringAttribute ("Value").toStdString();
                        scene.annotation = text (element, "Annotation");
                        scene.colour = integer (element, "ColorIndex", -1);
                        set.scenes.push_back (std::move (scene));
                    }

                    return;
                }

                set.problems.push_back ("the set has no scenes");
            }

            std::optional<Mapping> globalKey (const juce::XmlElement& liveSet, const char* name)
            {
                std::function<const juce::XmlElement* (const juce::XmlElement&)> find;

                find = [&find, name] (const juce::XmlElement& from) -> const juce::XmlElement*
                {
                    for (auto* child : from.getChildIterator())
                    {
                        if (child->hasTagName (name))
                            return child;

                        if (const auto* deeper = find (*child))
                            return deeper;
                    }

                    return nullptr;
                };

                const auto* key = find (liveSet);

                if (key == nullptr)
                    return std::nullopt;

                const auto channel = integer (key, "Channel", -1);
                const auto controller = integer (key, "NoteOrController", -1);

                if (channel < 0 || controller < 0)
                    return std::nullopt;

                Mapping out;
                out.channel = channel + 1;
                out.number = controller;
                out.note = flag (key, "IsNote");
                return out;
            }

            void read (const juce::XmlElement& root)
            {
                set.creator = root.getStringAttribute ("Creator").toStdString();

                const auto* liveSet = root.getChildByName ("LiveSet");

                if (liveSet == nullptr)
                {
                    set.problems.push_back ("the file has no LiveSet");
                    return;
                }

                const auto* master = at (liveSet, "MasterTrack");

                if (master == nullptr)
                    master = at (liveSet, "MainTrack");   // Live 12

                /*  EVERY TRACK FIRST, slots after: a clip envelope names its
                    parameter by an identifier that may be on another track - a
                    carrier's envelope moves its own track, but a mapping may
                    not - so the table is whole before any clip is read. */
                std::vector<std::pair<const juce::XmlElement*, Track>> pending;

                if (const auto* tracks = at (liveSet, "Tracks"))
                {
                    for (auto* element : tracks->getChildIterator())
                    {
                        const auto tag = element->getTagName();
                        const auto kind = tag == "AudioTrack"  ? Track::Kind::audio
                                        : tag == "MidiTrack"   ? Track::Kind::midi
                                        : tag == "GroupTrack"  ? Track::Kind::group
                                        : tag == "ReturnTrack" ? Track::Kind::returnTrack
                                                               : Track::Kind::audio;

                        if (tag != "AudioTrack" && tag != "MidiTrack" && tag != "GroupTrack" && tag != "ReturnTrack")
                        {
                            set.problems.push_back ("a track of a kind not read: <" + tag.toStdString() + ">");
                            continue;
                        }

                        pending.emplace_back (element, readTrack (*element, kind));
                    }
                }

                if (master != nullptr)
                {
                    set.master = readTrack (*master, Track::Kind::master);
                    set.tempo = number (master, "DeviceChain/Mixer/Tempo/Manual", 120.0);
                }

                for (auto& [element, track] : pending)
                {
                    readSlots (*element, track);

                    if (track.kind == Track::Kind::returnTrack)
                        set.returns.push_back (std::move (track));
                    else
                        set.tracks.push_back (std::move (track));
                }

                if (const auto* pre = at (liveSet, "SendsPre"))
                    for (auto* one : pre->getChildIterator())
                        set.sendsPre.push_back (one->getStringAttribute ("Value") == "true");

                readScenes (*liveSet);

                set.fireScene = globalKey (*liveSet, "KeyMidiFireSelectedScene");
                set.sceneUp = globalKey (*liveSet, "KeyMidiSceneUp");
                set.sceneDown = globalKey (*liveSet, "KeyMidiSceneDown");
            }
        };
    }

    //==========================================================================
    ReadResult readSetXml (const std::string& xml)
    {
        ReadResult out;

        const auto root = juce::XmlDocument::parse (juce::String::fromUTF8 (xml.data(), static_cast<int> (xml.size())));

        if (root == nullptr)
        {
            out.error = "the set is not XML";
            return out;
        }

        if (! root->hasTagName ("Ableton"))
        {
            out.error = "the file is XML but not an Ableton Live set: its root is <"
                        + root->getTagName().toStdString() + ">";
            return out;
        }

        Reader reader;
        reader.read (*root);
        out.set = std::move (reader.set);
        return out;
    }

    ReadResult readSet (const juce::File& file)
    {
        juce::MemoryBlock bytes;

        if (! file.loadFileAsData (bytes))
            return { std::nullopt, "the set could not be read from " + file.getFullPathName().toStdString() };

        /*  GZIP AS LIVE WRITES IT, recognised by its two magic bytes; anything
            else is taken for the XML itself. */
        const auto* data = static_cast<const unsigned char*> (bytes.getData());

        if (bytes.getSize() >= 2 && data[0] == 0x1f && data[1] == 0x8b)
        {
            juce::MemoryInputStream compressed (bytes, false);
            juce::GZIPDecompressorInputStream gzip (&compressed, false, juce::GZIPDecompressorInputStream::gzipFormat);

            juce::MemoryBlock xml;
            juce::MemoryOutputStream sink (xml, false);
            sink.writeFromInputStream (gzip, -1);
            sink.flush();

            if (xml.getSize() == 0)
                return { std::nullopt, "the set is gzip but did not decompress" };

            return readSetXml (std::string (static_cast<const char*> (xml.getData()), xml.getSize()));
        }

        return readSetXml (std::string (static_cast<const char*> (bytes.getData()), bytes.getSize()));
    }

    //==========================================================================
    double sampleSecondsAt (const Clip& clip, double beats, double tempo)
    {
        const auto beatsPerSecondAtTempo = (tempo > 0.0 ? tempo : 120.0) / 60.0;

        if (clip.warp.size() < 2)
        {
            const auto originBeats = clip.warp.empty() ? 0.0 : clip.warp.front().beats;
            const auto originSeconds = clip.warp.empty() ? 0.0 : clip.warp.front().seconds;
            return originSeconds + (beats - originBeats) / beatsPerSecondAtTempo;
        }

        /*  THE SEGMENT THE BEAT IS IN - or the nearest end's, carried on. */
        std::size_t segment = 0;

        while (segment + 2 < clip.warp.size() && beats > clip.warp[segment + 1].beats)
            ++segment;

        const auto& a = clip.warp[segment];
        const auto& b = clip.warp[segment + 1];
        const auto spanBeats = b.beats - a.beats;

        if (! (std::abs (spanBeats) > 0.0))
            return a.seconds;

        return a.seconds + (beats - a.beats) * (b.seconds - a.seconds) / spanBeats;
    }

    double beatsPerSecond (const Clip& clip)
    {
        if (clip.warp.size() < 2)
            return 0.0;

        const auto& first = clip.warp.front();
        const auto& last = clip.warp.back();
        const auto seconds = last.seconds - first.seconds;

        return seconds > 0.0 ? (last.beats - first.beats) / seconds : 0.0;
    }

    bool warpIsStraight (const Clip& clip)
    {
        const auto overall = beatsPerSecond (clip);

        if (! (overall > 0.0))
            return true;

        for (std::size_t at = 1; at < clip.warp.size(); ++at)
        {
            const auto seconds = clip.warp[at].seconds - clip.warp[at - 1].seconds;

            if (! (seconds > 0.0))
                continue;

            const auto slope = (clip.warp[at].beats - clip.warp[at - 1].beats) / seconds;

            /*  A thousandth, and the reason it is that loose: Live writes its
                markers' seconds to a few microseconds, so two markers a
                thirty-second of a beat apart - the pair every unwarped-looking
                clip carries - already differ from the whole in the fourth
                figure. */
            if (std::abs (slope - overall) > overall * 1.0e-3 && seconds > 0.1)
                return false;
        }

        return true;
    }
}
