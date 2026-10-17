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
    A QLAB WORKSPACE, READ (namespace draft §46, QL.4): the first of the
    importer's three stages, as `AlsReader` is for a Live set.

    A `.qlab4` or `.qlab5` is a keyed archive holding the workspace's name,
    version and settings, and - as data - a second keyed archive holding every
    cue (§46.1). This reads both into plain facts IN QLAB'S OWN TERMS: a level
    is a linear gain where QLab stores one, a fade says whether it is absolute
    as QLab says it, a continue mode is the number QLab wrote. Nothing is
    interpreted - what a playlist means, where a level lands, which device a
    message goes to is the walk's (§46.2, ZS-ZX).

    QLAB 4 AND 5 (ZR), whose differences are §46.5's table - the network
    destinations, the message's key, a file's relative path, the type of a
    build number - are read into the same facts here, so nothing after the
    reader asks which QLab wrote them. Any other version is refused in words,
    naming it.

    WHAT IS STORED, NEVER WHAT IS DERIVED: QLab 5 reports a playlist's children
    as auto-continuing, but stores what somebody set (extraction §9.3). This
    reads the file, so `continueMode` is the decision.
*/

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wfg::import::qlab
{
    /** One stored cell of a level matrix: row nought the cue's main and output faders, a row per input below it. */
    struct Level
    {
        int row = 0;
        int column = 0;
        double gain = 1.0;          ///< linear, as QLab stores it; nought is silence
    };

    /** One faded cell of a fade, its start and end as linear gains (a relative fade's end is the offset). */
    struct FadeLevel
    {
        int row = 0;
        int column = 0;
        double start = 0.0;
        double end = 0.0;
    };

    /*  A FADE'S SHAPE (QLab's curve tab): 1 the S-curve, QLab's default; 2
        parametric, `parameter` its intensity; 3 linear; and custom shapes,
        whose breakpoints are `points` - time and completion, both nought to
        one. */
    struct Shape
    {
        int type = 1;
        double parameter = 1.0;
        std::vector<std::pair<double, double>> points;
    };

    struct Slice
    {
        double time = 0.0;          ///< where the slice ENDS, in the file's seconds
        int playCount = 1;
        bool infinite = false;
    };

    /*  A COLOUR AS COCOA ARCHIVES IT (namespace draft §62.5): red, green, blue
        and alpha, nought to one, from whichever colour space it was written in
        - RGB, a grey, CMYK, or components with a colour space of their own.
        `set` false where there was none. */
    struct Colour
    {
        double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
        bool set = false;
    };

    /*  ONE RUN OF A TEXT CUE'S WORDS, ITS STYLE AS COCOA KEEPS IT (§62.5): the
        attributes of an `NSAttributedString`, over `length` UTF-16 units from
        `start`, as Cocoa counts. Nothing interpreted: an alignment is the
        number archived, a stroke's width a % of the size, negative when the
        letters are filled as well. */
    struct TextRun
    {
        std::size_t start = 0;
        std::size_t length = 0;
        std::string font;                   ///< the PostScript name, "HelveticaNeue-Bold"
        double size = 0.0;                  ///< in points
        Colour colour;
        int alignment = -1;                 ///< NSTextAlignment as archived; -1 when not said
        double lineHeightMultiple = 0.0;    ///< nought when not said
        double lineSpacing = 0.0;
        double strokeWidth = 0.0;
        Colour strokeColour;
        bool shadow = false;
        double shadowBlur = 0.0, shadowX = 0.0, shadowY = 0.0;
        Colour shadowColour;
        Colour background;
        int underline = 0;
        int strikethrough = 0;
        double kern = 0.0;
    };

    /*  WHERE A PICTURE GOES ON ITS STAGE, IN QLAB'S TERMS (§62.5): pixels from
        the stage's middle, a scale for each axis, an origin, a turn kept as a
        quaternion, the opacity nought to one, a layer, a blend mode by number;
        whether it fills the stage and how. A fade's targets are read the same. */
    struct Placement
    {
        double translationX = 0.0, translationY = 0.0;
        double scaleX = 1.0, scaleY = 1.0;
        double originX = 0.0, originY = 0.0;
        std::optional<std::array<double, 4>> quaternion;    ///< x, y, z, w; none where QLab wrote nothing
        bool quaternionUnread = false;      ///< one was written, in a shape this does not read
        double rotation = 0.0;              ///< a fade's turn, in degrees, about what `rotationType` says
        double opacity = 1.0;
        double layer = 1000.0;
        int blendMode = 0;
        bool fullScreen = true;
        int fillStyle = 0;
        bool preserveAspectRatio = true;
        double cropLeft = 0.0, cropRight = 0.0, cropTop = 0.0, cropBottom = 0.0;
    };

    /*  A FILE AS THE WORKSPACE NAMES IT: the absolute path it had where it was
        saved, and a relative path - from the workspace's folder in QLab 5, from
        the folder above it in QLab 4 (§46.5). */
    struct FileRef
    {
        std::string absolutePath;
        std::string relativePath;
        bool relativeToParent = false;

        std::string name() const;
    };

    /*  ONE CUE, AS ITS CLASS SAYS. `type` is the archived class without its
        "Cue" - "Group", "Audio", "OSC", "Fade", "Memo"... - since that is what
        a workspace says and the walk asks. Every field below is read where the
        class has it and left at its default where it does not. */
    struct Cue
    {
        std::string type;
        std::string id;             ///< QLab's UUID, kept across saves
        std::string name;           ///< as typed, empty where nobody typed one
        std::string number;
        std::string notes;          ///< the words of QLab's rich text
        bool armed = true;
        double preWait = 0.0;
        double postWait = 0.0;
        double duration = 0.0;
        int continueMode = 0;       ///< 0 none, 1 auto-continue, 2 auto-follow - as stored
        std::string target;         ///< the targeted cue's UUID

        //  Triggers QLab had - read for the report, not imported (§46.2, ZY).
        bool hotkeyTrigger = false;
        bool midiTrigger = false;
        bool wallClockTrigger = false;
        bool timecodeTrigger = false;

        //  A group, and a cue list or cart (which are groups of mode 0 and 5).
        int groupMode = -1;         ///< 0 list, 1 start first and enter, 2 start first, 3 timeline, 4 random, 5 cart, 6 playlist
        bool playlistLoop = false;
        bool playlistShuffle = false;
        bool playlistCrossfade = false;
        double playlistCrossfadeDuration = 0.0;   ///< seconds; nought when the archive carries none (§69, AOI)
        std::vector<Cue> children;

        //  An audio cue.
        FileRef file;
        std::string audioPatch;     ///< QLab 5's patch UUID, or QLab 4's patch number as text
        std::vector<Level> levels;
        double rate = 1.0;
        bool pitchFollowsRate = true;   ///< QLab's `doPitchShift`: varispeed
        double startTime = 0.0;
        double endTime = 0.0;
        double fileDuration = 0.0;  ///< as QLab last saw it, nought when not said
        int playCount = 1;
        bool infiniteLoop = false;
        std::vector<Slice> slices;
        Slice lastSlice;

        //  A fade.
        bool absolute = true;
        bool stopTargetWhenDone = false;
        std::vector<FadeLevel> fadeLevels;
        bool fadesRate = false;
        Shape shape;                ///< the rising shape; QLab draws the falling one from the same type

        //  A network cue.
        std::string message;        ///< QLab's text: the address, and each argument after a space
        std::string networkPatch;   ///< QLab 5's patch UUID, or QLab 4's patch number as text
        int messageType = 2;        ///< QLab 4: 1 a QLab command, 2 OSC, 3 UDP; QLab 5 says it by the patch
        int networkFadeType = 0;    ///< 0 none, 1 a value over time (`#v#`), 2 a path (`#x#` `#y#`)
        double fadeFrom = 0.0;
        double fadeTo = 0.0;
        bool fadeFloats = true;

        //  A mic cue.
        std::string inputPatch;
        int inputChannel = 0;       ///< the first, from nought
        int inputChannels = 0;

        //  A script cue, kept as text and never run (PRD §3.20).
        std::string source;

        /*  A VIDEO, TEXT OR CAMERA CUE (namespace draft §62): the stage it is
            on - QLab 5's viewport UUID, QLab 4's surface number as text - and
            where it goes there; a video cue's file and timing are an audio
            cue's fields above, its sound's levels and patch those of the audio
            cue QLab keeps inside it. QLab 5's last look at a screen it drew on
            directly, where it names no stage. */
        std::string stage;
        Placement placement;
        bool hasEffects = false;
        bool holdLastFrame = false;
        std::string lastSeenScreen;
        int lastSeenWidth = 0;
        int lastSeenHeight = 0;

        //  A text cue: its words, their runs, its fixed width (nought: none), the fonts QLab last saw.
        std::string words;
        std::vector<TextRun> runs;
        double fixedWidth = 0.0;
        std::vector<std::string> lastSeenFontNames;

        //  A fade of a picture (its targets in `placement`), and how its geometry and turn are meant.
        bool fadesOpacity = false;
        bool fadesTranslation = false;
        bool fadesScale = false;
        bool fadesRotation = false;
        bool fadesEffects = false;
        int geoMode = 0;
        int rotationType = 0;
    };

    /*  A STAGE OR A SURFACE (§62.5): what QLab 5 calls a stage and QLab 4 a
        surface - a picture's frame of so many pixels, drawn to outputs. `id`
        is what a cue names: QLab 5's viewport UUID, QLab 4's surface number. */
    struct Stage
    {
        std::string id;
        std::string name;
        int width = 0;
        int height = 0;
        std::vector<std::string> outputs;   ///< by name: QLab 5's routes, QLab 4's displays
        bool blended = false;               ///< an edge blend set
        bool masked = false;                ///< a mask image set
    };

    struct AudioPatch
    {
        std::string id;             ///< a UUID in QLab 5, the patch's number as text in QLab 4
        std::string name;
        int outputs = 0;            ///< QLab 5's cue outputs; nought where the workspace does not say
        std::map<int, std::string> outputNames;    ///< by cue output, from one
    };

    struct NetworkPatch
    {
        std::string id;             ///< a UUID in QLab 5, the patch's number as text in QLab 4
        std::string name;
        std::string kind;           ///< "osc" for an OSC message patch, else what QLab called it
        std::string host;
        int port = 0;
        bool tcp = false;
    };

    struct Workspace
    {
        int major = 0;              ///< 4 or 5
        std::string version;        ///< "5.6.3"
        std::string build;
        std::string name;
        std::vector<Cue> lists;     ///< cue lists and carts, in the workspace's order
        std::vector<AudioPatch> audioPatches;
        std::vector<NetworkPatch> networkPatches;
        double minVolume = -60.0;   ///< dB: at or below it is silence

        std::vector<Stage> stages;
        std::string defaultStage;   ///< QLab 4's default surface; QLab 5's screen when it has no stages

        std::vector<std::string> problems;  ///< what was read past rather than refused
    };

    struct ReadResult
    {
        std::optional<Workspace> workspace;
        std::string error;          ///< why nothing was read
    };

    ReadResult readWorkspace (const juce::File&);
    ReadResult readWorkspaceBytes (const std::uint8_t* bytes, std::size_t size);

    /** How many cues a list holds, itself not counted, every depth. */
    int countCues (const Cue&);
}
