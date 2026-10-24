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
    The canvases this show declares and the video outputs that show them
    (Phase 8a, namespace draft 35), in the author's words: "Canvas (surfaces in
    QLab lingo) render to Outputs with their mapping."

    A canvas is the show's: a name and a size. An output is the show's too -
    its name, the canvas it shows, the display it opens on - and beside that,
    what the machine found tonight: whether a display is behind it, why not,
    and how its frames are going. Read off the tree and never worked out here.
*/

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct CanvasRow
    {
        std::string id;
        std::string name;
        int width = 1920;
        int height = 1080;

        /*  ITS LEVEL AND ITS DCA (namespace draft §38, WT): how much of it
            reaches its outputs, in percent, and the DCA that rides it - by
            identifier, empty for none. */
        double level = 100.0;
        std::string dca;
        double curve = 0.0;         ///< its DCA's curve, % (namespace draft §109, BBK)

        std::string label() const;   // the name, else the id
        std::string levelWord() const;   // "100 %" or "62.5 %", to the tenth
        std::string curveWord() const;   // "0 %", "+20 %", "-7.5 %", to the tenth
        std::string sizeWord() const;   // "1920 × 1080"
    };

    struct VideoOutputRow
    {
        std::string id;
        std::string name;
        std::string canvas;
        std::string display;
        bool enabled = true;
        bool bound = false;
        std::string problem;
        bool testPattern = false;
        bool grid = false;          ///< its alignment grid shown, tonight's (§68, ANX)
        std::int64_t framesPresented = 0;
        std::int64_t framesLate = 0;
        int zones = 0;              ///< how many zones lie over its own canvas (namespace draft 40)

        /*  WHAT IT DOES WITH ITS CANVAS (namespace draft §44, YA): shows it on
            a display, or sends it - ndi, spout, syphon - under a name, at a
            rate. */
        std::string kind = "display";
        std::string sendName;
        double frameRate = 60.0;

        /*  DOWN A DECKLINK CARD'S CABLE (namespace draft §71): which card -
            its identifier, and its name when it was picked - at which format,
            keying how, carrying which buses; and how it goes tonight. */
        std::string card;
        std::string cardName;
        std::string videoMode = "1080p50";
        std::string key = "off";
        std::vector<std::string> sound;
        double latencyMs = 0.0;
        double skewPpm = 0.0;
        std::string reference = "none";
        std::int64_t soundLate = 0;
        std::int64_t soundSent = 0;

        /*  Sent to other programs - NDI, Spout, Syphon - and sent down a card's
            cable: neither is a display, and only the first has a send name. */
        bool sends() const { return ! kind.empty() && kind != "display" && kind != "card"; }
        bool toCard() const { return kind == "card"; }
        std::string label() const;
    };

    /*  A VIDEO INPUT (namespace draft §44, YB): a picture another program
        sends, the show's - its name, kind and sender - and what arrives
        tonight, the machine's. */
    struct VideoInputRow
    {
        std::string id;
        std::string name;
        std::string kind = "ndi";
        std::string sender;
        bool enabled = true;
        bool alpha = false;             ///< the program sends a meaningful alpha (61, AKF)
        std::string videoMode = "auto"; ///< a capture input's format, or Automatic (§71 APM, §72)
        std::string format;             ///< the format arriving at a card input, tonight's
        bool connected = false;
        int width = 0;
        int height = 0;
        double frameRate = 0.0;
        std::string problem;

        std::string label() const;
    };

    /*  A VIDEO INSERT (§44, YE): a cue's picture to another program and back. */
    struct VideoInsertRow
    {
        std::string id;
        std::string name;
        std::string kind = "spout";
        std::string sendName;
        std::string returnSender;
        bool connected = false;
        double frameRate = 0.0;
        double returnAge = 0.0;
        std::string problem;

        std::string label() const;
    };

    /*  A PICTURE ANOTHER PROGRAM OFFERS NOW (§44, YB), as
        /godot/videoInput/available lists it. */
    struct OfferedSender
    {
        std::string kind;
        std::string name;
    };

    std::vector<VideoInputRow> readVideoInputs (const tree::TreeSnapshot&);
    std::vector<VideoInsertRow> readVideoInserts (const tree::TreeSnapshot&);
    std::vector<OfferedSender> readOfferedSenders (const tree::TreeSnapshot&);

    /*  THE KINDS THIS SYSTEM SENDS AND TAKES IN: NDI everywhere, Spout on
        Windows, Syphon on macOS - and each one's name on screen. */
    std::vector<std::string> pictureKindsHere();
    std::string pictureKindWord (const std::string& kind);

    /*  THE DECKLINK CARDS THIS MACHINE HAS (namespace draft §71), as
        `/godot/videoOutput/cards` says them: each card's identifier, name,
        what it can do and the formats it sends and takes in; or why there are
        none - Desktop Video not installed or too old. */
    struct CardRow
    {
        std::string id;
        std::string name;
        bool output = false;
        bool input = false;
        bool internalKey = false;
        bool externalKey = false;
        bool tenBit = false;
        int soundChannels = 0;
        std::vector<std::string> sends;
        std::vector<std::string> takes;
    };

    struct CardsFound
    {
        std::vector<CardRow> cards;
        std::string problem;

        const CardRow* card (const std::string& id) const;
    };

    CardsFound cardsFrom (const std::string& text);
    CardsFound readCards (const tree::TreeSnapshot&);

    /*  WHAT BRINGS A PICTURE IN ON THIS MACHINE (namespace draft §72, APR), as
        `/godot/videoInput/devices` says it: each card's input and each camera -
        its identifier, name, maker and the formats it takes in, a camera's once
        it has been opened. What a Video capture input picks from. */
    struct CaptureRow
    {
        std::string id;
        std::string name;
        std::string maker;
        std::vector<std::string> formats;

        /*  Its name with its maker where the name does not say it:
            "DeckLink Duo 2 (1)", "Logitech C920 - camera". */
        std::string shown() const;
    };

    struct CapturesFound
    {
        std::vector<CaptureRow> devices;

        const CaptureRow* device (const std::string& id) const;
    };

    CapturesFound capturesFrom (const std::string& text);
    CapturesFound readCaptures (const tree::TreeSnapshot&);

    /*  THE KINDS AN INPUT CAN BE HERE (APN, APR): this system's, and Video
        capture where something brings a picture in - a card's input or a
        camera. An insert's stay this system's. */
    std::vector<std::string> inputKindsHere (const CapturesFound& captures);

    /*  WHAT A VIDEO CAPTURE INPUT TAKES IN, in a row's words: the device by
        name and maker - or that it is not on this machine - and its format
        unless Automatic. */
    std::string captureSummary (const VideoInputRow& input, const CapturesFound& captures);

    /*  WHAT A CARD OUTPUT SENDS, in a row's words: the card, its format, its
        key, the buses its sound carries - named as `buses` names them. */
    struct OutputRow;
    std::string cardSummary (const VideoOutputRow& output, const CardsFound& cards, const std::vector<OutputRow>& buses);

    /*  A MENU OF THE SHOW'S VIDEO INPUTS AND OF ITS INSERTS, "(none)" first,
        identifier and label apart, as the canvases' menu is. */
    std::vector<std::pair<std::string, std::string>> videoInputChoices (const std::vector<VideoInputRow>&);
    std::vector<std::pair<std::string, std::string>> videoInsertChoices (const std::vector<VideoInsertRow>&);

    /*  A NEW INSERT OF A KIND (namespace draft §47, AAK): "Spout insert 1",
        numbered by kind, the first number free; and the two names it is given
        when it is made - sent as "Go.dot - Spout insert 1 (send)", expected
        back as "... (return)" - written once, never moved by a rename. */
    struct NewInsert
    {
        std::string name;
        std::string kind;
        std::string sendName;
        std::string returnSender;
    };

    NewInsert newInsertOf (const std::vector<VideoInsertRow>& inserts, const std::string& kind);

    /*  WHAT GO.DOT ITSELF SENDS UNDER (§47, AAK): every sending output's name
        and every insert's - what "comes back from" never offers, since an
        insert taking back its own send is a loop. */
    std::vector<std::string> ownSendNames (const std::vector<VideoOutputRow>& outputs,
                                           const std::vector<VideoInsertRow>& inserts);

    /** The name an insert's picture is sent under, the show's or the default. */
    std::string sentAs (const VideoInsertRow& insert);

    /** And an output's, while it sends. */
    std::string sentAs (const VideoOutputRow& output);

    /** Every canvas, in /godot/canvas/order. */
    std::vector<CanvasRow> readCanvases (const tree::TreeSnapshot&);

    /** Every video output, in /godot/videoOutput/order. */
    std::vector<VideoOutputRow> readVideoOutputs (const tree::TreeSnapshot&);

    //==============================================================================
    /*  A WARP AS THE WINDOW HOLDS IT (namespace draft 40): its grid of control
        points - the four corners, and the extra splits between them - each
        where it lands on the display, x right and y down in 0..1, row by row
        from the top-left. The output's own mesh and every zone's are one. */
    struct WarpPoints
    {
        int columns = 2;
        int rows = 2;
        std::vector<double> x, y;

        static WarpPoints whole (int columns = 2, int rows = 2);
        bool isWhole() const;
        std::size_t index (int column, int row) const { return static_cast<std::size_t> (row * columns + column); }
    };

    /*  The warp at `base` ("/godot/videoOutput/<id>/" or "/godot/zone/<id>/"):
        its `meshColumns`, `meshRows` and `mesh`; the whole output when the
        mesh is empty or the wrong size for its grid, as the renderer reads it. */
    WarpPoints readWarp (const tree::TreeSnapshot&, const std::string& base);

    /** The mesh row's text: "x y x y ...", four decimals, '.' always. */
    std::string warpText (const WarpPoints&);

    /*  THE SAME SHAPE ON ANOTHER GRID: a split added or taken away keeps where
        the picture lands, each new point read off the warp as it is drawn
        (Mapping.h's curve through the points), so adding splits never moves
        anything until one is dragged. */
    WarpPoints regridded (const WarpPoints&, int columns, int rows);

    /*  A ZONE ON AN OUTPUT (namespace draft 40, WY): a further canvas through
        a warp of its own, by its blend and its opacity, bottom first. */
    struct ZoneRow
    {
        std::string id;
        std::string name;
        std::string canvas;
        std::string blend = "normal";
        double opacity = 100.0;
        WarpPoints warp;
    };

    /** An output's zones, in /godot/videoOutput/<id>/zones order. */
    std::vector<ZoneRow> readZones (const tree::TreeSnapshot&, const std::string& outputId);

    /*  EVERY WARP OF THE SHOW (namespace draft §47.12), for the warp editor's
        "Copy to...": each output's own and each of its zones, bottom first,
        output by output in the show's order - where its rows live, which
        output it is on, and what the editor's list calls it. */
    struct WarpTarget
    {
        std::string base;           ///< "/godot/videoOutput/<id>/" or "/godot/zone/<id>/"
        std::string outputId;
        std::string outputLabel;
        std::string label;
    };

    std::vector<WarpTarget> warpTargets (const tree::TreeSnapshot&);

    /*  A WARP'S SHAPE PUT ON ANOTHER (the author's pick, 2026-10-09: "the shape
        only"): its grid and its points, the writes of one `node.setMany` - the
        target keeps its canvas, blend and opacity. */
    std::vector<std::pair<std::string, std::string>> warpCopyWrites (const WarpPoints& warp, const std::string& targetBase);

    /** The displays this machine has, by the names the system gives them. */
    std::vector<std::string> readDisplays (const tree::TreeSnapshot&);

    /*  THE DISPLAYS A MENU OFFERS (author, 2026-10-15: "Can the screen
        detection ... happen in the Show Settings panel when clicking on the
        drop down menu again"): the renderer's, in its order, then the ones
        this machine has that it has not named - the renderer looks only while
        it runs, so a screen plugged in before an output is switched on is
        still offered. Each name once. */
    std::vector<std::string> mergedDisplays (const std::vector<std::string>& published,
                                             const std::vector<std::string>& here);

    /*  A cue's or an output's canvas as a menu: "(none)" first, which shows
        nothing, then every canvas by name with its size. The key is the
        identifier, so a canvas renamed leaves every cue on it where it was. */
    std::vector<std::pair<std::string, std::string>> canvasChoices (const std::vector<CanvasRow>&);

    /*  WHETHER A FILE IS A PICTURE the renderer reads (namespace draft 36): by
        its extension, the ones JUCE's image readers take - PNG, JPEG, GIF.
        What makes a dropped file a picture cue rather than a sound. */
    bool isPictureFile (const std::string& name);

    /*  AND A MOVIE (namespace draft 37): a QuickTime `.mov`, HAP inside - or,
        since 37.5 (WF), any movie FFmpeg reads, played as a preview until it
        is converted: .mp4, .m4v, .mkv, .avi, .mxf, .webm, .mpg, .mpeg. */
    bool isMovieFile (const std::string& name);

    /*  A MOVIE BEING CONVERTED TO HAP (namespace draft 37.6, F.3), as
        /godot/videoOutput/conversions says it: the file as the show names it,
        waiting, converting, done, failed or cancelled, how far in %, and what
        went wrong. */
    struct ConversionRow
    {
        std::string file;
        std::string state;
        int percent = 0;
        std::string problem;

        bool running() const  { return state == "waiting" || state == "converting"; }
    };

    std::vector<ConversionRow> readConversions (const tree::TreeSnapshot&);

    /*  Where FFmpeg was found on the engine's machine, or empty. */
    std::string ffmpegPath (const tree::TreeSnapshot&);

    /*  FFMPEG BEING DOWNLOADED on first use (namespace draft 37.5, WN), as
        /godot/videoOutput/ffmpegInstall says it; `state` empty when nobody
        asked. */
    struct FfmpegInstallRow
    {
        std::string state;
        int percent = 0;
        std::string problem;
        std::string source;

        bool running() const  { return state == "downloading" || state == "unpacking" || state == "checking"; }
    };

    FfmpegInstallRow readFfmpegInstall (const tree::TreeSnapshot&);

    /*  The sentence its change is worth: each tenth of the way, done, failed. */
    std::string installNews (const FfmpegInstallRow& before, const FfmpegInstallRow& now);

    /*  THE ONE SENTENCE A CONVERSION'S CHANGE IS WORTH, for the transport's
        line: started, how far (in tens of %), done, failed and why, or
        cancelled. Empty when nothing worth saying changed. */
    std::string conversionNews (const ConversionRow* before, const ConversionRow& now);

    /*  A picture or a movie: what a video cue shows from a file. */
    inline bool isVisualFile (const std::string& name)  { return isPictureFile (name) || isMovieFile (name); }

    /*  The wildcard a file chooser offers for a video cue's file. */
    const char* pictureWildcard();
}
