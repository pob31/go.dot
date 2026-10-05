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
    AN ABLETON LIVE SET, READ INTO PLAIN FACTS (namespace draft §29.1).

    The first of the importer's three stages, and the only one that knows how
    Live spells anything. It interprets NOTHING: what a scene does to a track,
    what a silent clip on a monitor-In track means, which return is a speaker -
    all of that is `AlsWalk`'s. What is here is what the file says, in Live's own
    units: times in BEATS, a volume as the linear gain Live stores (1 is 0 dB),
    a send the same, a pan from -1 to 1, a modulation from -1 to 1. Keeping the
    reader dumb is what lets the walk be argued about, and tested, on facts.

    WHAT IS READ. Session view: the scenes in order, every track with its
    routing, monitoring, mixer and devices, every clip slot, and every clip's
    file, markers, warp and envelopes - each envelope's target resolved to
    "this track, this parameter". The MIDI controller mappings (`KeyMidi`), for
    the report's hands table. Arrangement view is counted, not read (§29.4).

    WHICH LIVE. 10, 11 and 12 differ in a handful of spellings the reader
    absorbs: the scenes are `Scenes/Scene` with a `Name` from 11 and
    `SceneNames/Scene` with a `Value` in 10; a file's relative path is one
    string from 11 and a list of folders in 10; the master is `MasterTrack`
    until 12 and `MainTrack` after. Anything else it does not recognise is a
    `problem`, never a guess.

    WHAT IT DOES NOT DO: open audio files, resolve a path on this disk, or
    touch a document. Vendor-light: JUCE for gzip, XML and the file, nothing
    else.
*/

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace juce { class File; class XmlElement; }

namespace wfg::import::als
{
    /*  WHAT AN ENVELOPE OR A MAPPING MOVES. `send` names a return by its
        place in the set's return list; `device` names a device by its place
        on its track's chain and one of its parameters by its path inside it
        (`DryWet`, `Bands.3/ParameterA/Freq`); `modulation` is a clip's own
        relative envelope on the track's volume (`VolumeModulationTarget`),
        and `otherModulation` one on anything else a clip may modulate. */
    enum class Parameter { volume, pan, send, device, modulation, otherModulation, unknown };

    struct Target
    {
        std::string trackId;        ///< the Live track's `Id`; a return's carries its own
        Parameter what = Parameter::unknown;
        int sendIndex = -1;         ///< with `send`: which return, by its place in the set
        int deviceIndex = -1;       ///< with `device`: which device on the chain
        std::string deviceKind;     ///< with `device`: Live's element, `Eq8`, `Reverb`...
        std::string path;           ///< with `device` or `otherModulation`: the parameter

        /*  A MODULATION envelope's target: relative to where the parameter
            stands rather than a value of its own. Every Live parameter has an
            automation target and a modulation target, each with its `Id`. */
        bool relative = false;
    };

    /*  ONE POINT OF AN ENVELOPE, in the CLIP's beats. A switch's (a device's
        `On`) is 1 or 0, an enum's (an EQ band's mode) its index. Live writes a
        first point far before the clip - at minus two years of beats - which
        is the envelope's value before its first real point; it is kept as
        read, and the walk decides what it means. */
    struct EnvelopePoint
    {
        double beats = 0.0;
        double value = 0.0;
    };

    struct Envelope
    {
        Target target;
        std::string pointeeId;      ///< what the clip named, kept for a report when `target` is unknown
        bool switches = false;      ///< a BoolEvent or EnumEvent envelope, not a FloatEvent one
        std::vector<EnvelopePoint> points;
    };

    /** One warp marker: a second of the sample and the beat it is placed on. */
    struct WarpMarker
    {
        double seconds = 0.0;
        double beats = 0.0;
    };

    struct FileReference
    {
        std::string relativePath;   ///< as Live wrote it, folders joined with '/'
        std::string absolutePath;   ///< the machine it was saved on's, empty in Live 10
        std::string name;           ///< the file's own name
        std::int64_t size = 0;      ///< `OriginalFileSize`, nought when not said
        std::int64_t crc = 0;       ///< `OriginalCrc`, nought when not said
        double sampleRate = 0.0;    ///< `DefaultSampleRate`
        std::int64_t frames = 0;    ///< `DefaultDuration`, in the file's samples
    };

    struct Clip
    {
        std::string name;
        int colour = -1;            ///< Live's palette index
        FileReference file;

        /*  THE MARKERS, in beats, as Live keeps them: with the loop off, the
            clip plays from `loopStart` to `loopEnd`; with it on, from
            `loopStart + startRelative` to `loopEnd` and then round the loop.
            `currentStart`/`currentEnd` are the clip's own extent. */
        double loopStart = 0.0;
        double loopEnd = 0.0;
        double startRelative = 0.0;
        bool loopOn = false;
        double currentStart = 0.0;
        double currentEnd = 0.0;

        double gain = 1.0;          ///< `SampleVolume`, linear
        bool warped = true;
        int warpMode = 0;           ///< Live's index: 0 Beats, 1 Tones, 2 Texture, 3 Re-Pitch, 4 Complex, 6 Complex Pro
        std::vector<WarpMarker> warp;
        int pitchCoarse = 0;        ///< semitones
        double pitchFine = 0.0;     ///< cents

        bool deactivated = false;   ///< `Disabled`: a clip switched off in Live
        bool followAction = false;  ///< a follow action switched on - reported, not imported
        bool legato = false;
        int launchMode = 0;
        int quantisation = 0;

        std::vector<Envelope> envelopes;
    };

    /*  ONE CLIP SLOT: a clip, or an empty slot that STOPS its track when its
        scene is launched (`hasStop`), or one that leaves it running. */
    struct Slot
    {
        std::optional<Clip> clip;
        bool hasStop = true;
    };

    /*  ONE BAND OF AN EQ EIGHT, its first parameter set (`ParameterA`): Live's
        mode index - 0 low cut 48, 1 low cut 12, 2 low shelf, 3 bell, 4 notch,
        5 high shelf, 6 high cut 12, 7 high cut 48 - its frequency in Hz, its
        gain in dB and its Q. */
    struct EqBand
    {
        bool on = false;
        int mode = 3;
        double frequency = 1000.0;
        double gain = 0.0;
        double q = 0.7071;
    };

    struct Device
    {
        std::string kind;           ///< Live's element: `Eq8`, `Reverb`, `PluginDevice`...
        std::string name;           ///< a plugin's own name, or the device's user name
        bool on = true;
        std::map<std::string, double> values;   ///< every automatable number by its path, as saved
        std::vector<EqBand> bands;  ///< an EQ Eight's eight, empty for anything else
    };

    /*  WHERE A TRACK'S SOUND GOES OR COMES FROM, in Live's words: `None`, a
        track (`Track.27` - its Id), the master, a group, or the interface
        (`External/S0` - the first stereo pair, `M17` the eighteenth mono
        output). `target` is the string as written; the rest is it parsed. */
    struct Routing
    {
        std::string target;
        enum class Kind { none, track, master, group, external, other } kind = Kind::other;
        std::string trackId;        ///< with `track`
        bool stereo = false;        ///< with `external`: a pair, not one channel
        int channel = -1;           ///< with `external`: the pair's or the channel's index from nought
    };

    struct Track
    {
        std::string id;
        std::string name;
        enum class Kind { audio, midi, group, returnTrack, master } kind = Kind::audio;
        std::string groupId;        ///< the group track it sits in, empty at the top

        /** Live's monitoring: In plays the input and silences the track's clips. */
        enum class Monitoring { in, automatic, off } monitoring = Monitoring::automatic;

        Routing input;
        Routing output;

        double volume = 1.0;        ///< linear, 1 is 0 dB
        double pan = 0.0;           ///< -1 to 1
        std::vector<double> sends;  ///< linear, one per return in the set's order
        bool active = true;         ///< the track activator (`Speaker`)

        std::vector<Device> devices;
        std::vector<Slot> slots;    ///< one per scene
    };

    struct Scene
    {
        std::string id;
        std::string name;
        std::string annotation;
        int colour = -1;
        bool tempoEnabled = false;
        double tempo = 0.0;
        bool followAction = false;
    };

    /*  A CONTROL A HAND MOVED: a MIDI channel (one-based, as a desk says it), a
        controller or a note, and what it was mapped to. */
    struct Mapping
    {
        int channel = 0;
        int number = 0;
        bool note = false;
        Target target;
    };

    struct LiveSet
    {
        std::string creator;        ///< "Ableton Live 11.2.6"
        double tempo = 120.0;
        std::vector<Scene> scenes;
        std::vector<Track> tracks;  ///< audio, MIDI and group tracks, in Live's order
        std::vector<Track> returns; ///< in Live's order, which is the order of every track's sends
        std::optional<Track> master;
        std::vector<bool> sendsPre; ///< per return: whether its sends are taken before the fader

        std::vector<Mapping> mappings;
        std::optional<Mapping> fireScene, sceneUp, sceneDown;

        int arrangementClips = 0;   ///< counted for the report, never read (§29.4)

        /*  What could not be read, in words that name the element - a set
            that was read with some of these is still a set; the walk and the
            report decide what they cost. */
        std::vector<std::string> problems;
    };

    struct ReadResult
    {
        std::optional<LiveSet> set;
        std::string error;          ///< why there is no set at all: not gzip, not XML, not Live
    };

    /*  THE SET IN A FILE: gzipped as Live writes it, or plain XML, which is how
        a hand-made fixture is kept readable in a repository. */
    ReadResult readSet (const juce::File& file);

    /** The same, from the XML text itself. */
    ReadResult readSetXml (const std::string& xml);

    /*  WHERE IN THE SAMPLE A BEAT OF THE CLIP IS, in seconds: Live's warp map,
        straight between markers and carried on past either end by the nearest
        segment's slope - which is how Live plays a clip whose markers stop
        short. Unwarped, or with fewer than two markers, a beat is half a
        second at the set's tempo of 120, so the map is `beats / (tempo / 60)`
        from the first marker. */
    double sampleSecondsAt (const Clip&, double beats, double tempo);

    /*  The warp's beats per second of the sample over its whole length - two
        at 120 BPM for a sample that plays at its own rate - and whether the
        map is one straight line (every segment within a part in a thousand of
        that slope). Nought for a clip with fewer than two markers. */
    double beatsPerSecond (const Clip&);
    bool warpIsStraight (const Clip&);
}
