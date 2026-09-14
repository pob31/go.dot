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
    How long a media file is, asked once when the show is opened.

    WHY THIS EXISTS AT ALL. Nothing in Go.dot has ever known how long a cue is.
    §3.13's solver cannot work without it - "is this cue still playing at time
    T" is a question about a duration - and the running pane has nothing to draw
    a progress bar against. `/godot/cue/<id>/duration` is the node; this is
    where the number comes from.

    WHY IT IS NOT ASKED OF TRACKTION, which is what the namespace draft first
    said. `te::AudioFile` needs a `te::Engine&`, and there is none at the moment
    a show is read: `wfg tree` and `wfg validate` build no audio at all, and
    `wfg serve` brings the engine up three hundred lines after the document is
    loaded and the first tree snapshot is published. Standing an engine up to
    ask a file's length would also set flush-to-zero on the calling thread for
    the rest of the process, which is a thing this project has already been bitten
    by once and scopes deliberately everywhere else.

    So it is JUCE's own format manager, with the basic formats registered: WAV,
    AIFF, FLAC, Ogg Vorbis, and MP3 only where the build enables it. Tracktion
    reads through the same JUCE readers, so the two agree about every format the
    engine can actually play.

    A FILE THAT WILL NOT READ IS NOUGHT, and that is the answer rather than an
    error: a missing file has failed the ARM and never the load since Phase 2 -
    "a show with one missing sound is still a show somebody has to run tonight" -
    and the same rule holds for one whose header cannot be parsed. The solver
    reads a nought as *I do not know how long this is* and says so in its
    confused list rather than guessing.

    No JUCE type in the signature: the tree and the document read this and
    neither of them names an audio library.

    TWO HALVES SINCE PR 5.6, and they are not allowed to behave alike (namespace
    draft §14.12). The DURATIONS are frozen at load and read by address, fifty
    times a second, by a cache that cannot tell a changed number from an
    unchanged one. The HASH and the PYRAMID arrive late, from a thread of their
    own, and are read through a snapshot that is swapped whole. One record per
    file keeps the two from becoming two tables that disagree; the rule below
    about who may write which half keeps the cache honest.
*/

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::audio
{
    /*  WHAT A FILE SOUNDS LIKE, at every zoom - defined in `Timbre.h` by PR 5.7,
        and only named here (§14.12: window 2048, hop 1024, then halvings down
        to 64 frames). A `shared_ptr` to an incomplete type may be declared,
        copied, moved and destroyed anywhere, empty or not: its deleter is
        type-erased and captured once, where the pointer is first made from a
        complete object - which happens in the analyser, `MediaAnalyser.cpp`,
        the one place that makes one. What needs the complete type is only
        MAKING one, and dereferencing it; a reader that does either includes
        `Timbre.h`. It is a STRUCT there and here: a class/struct mismatch
        between a declaration and its definition warns on MSVC (C4099) and on
        Clang (-Wmismatched-tags). GCC, which the strict job runs, leaves that
        warning off by default, so the build would not catch it - which is why
        the rule is written here rather than trusted to CI. */
    struct TimbrePyramid;

    /*  EVERYTHING GO.DOT KNOWS ABOUT ONE MEDIA FILE, keyed elsewhere by the
        bundle-relative path the document writes.

        Path is the key because path is what a cue names; the content hash is a
        FIELD because it arrives late and because the cache it keys
        (`media/.timbre/<sha256>.tpy`) is a different question - "have these
        bytes been analysed" - from the one a cue asks. A second map from path to
        hash would be a second thing to keep in step, and by §13.4's rule the one
        that is wrong is always the copy.

        `seconds` is the length read at load and never anything else: it is the
        same number `MediaInfo::durations()` holds, restated so that a reader of
        a snapshot has the whole record without asking two objects. An empty
        `contentHash` and a null `pyramid` mean NOT ANALYSED YET, which the tree
        publishes as nothing at all rather than as grey (§14.12, since PR 5.8:
        `run/<id>/timbre` and `cue/<id>/hash` both read empty until the pyramid
        is here). */
    struct MediaRecord
    {
        double seconds = 0.0;
        std::string contentHash;
        std::shared_ptr<const TimbrePyramid> pyramid;
    };

    /*  One record per file the show named when it was opened, by that path. */
    using MediaRecords = std::map<std::string, MediaRecord>;

    /*  Seconds, or 0.0 when the file is absent, unreadable, or in a format this
        build has no reader for. Never throws. */
    double mediaDurationSeconds (const std::string& absolutePath);

    /*  WHERE A `file` THE DOCUMENT NAMES IS, on this machine: relative to the
        bundle's `media/` folder, or taken as given when there is no folder to
        be relative to - the way the runner resolves it, so that the duration
        published, the colours drawn and the file played are one file. The
        durations walk and the analyser (PR 5.7) both ask this, and nothing
        else spells it. */
    std::string resolveMediaPath (const std::string& mediaFolder, const std::string& named);

    /*  EVERY DISTINCT `file` A MEDIA CUE IN THIS SHOW NAMES, in the order the
        show first names it, and never an empty one. Reads the document and
        nothing else - no file is opened - so the tick thread may ask it after
        a show edit, to hand the analyser a file somebody has just imported.

        IN DOCUMENT ORDER rather than sorted, because the analyser works
        through it front to back: cue 1's sound gets its colours before cue
        90's, which is the order an operator will reach them in. */
    std::vector<std::string> mediaFilesNamedBy (const doc::ShowDocument& document);

    /*  Every distinct `file` a media cue in this show names, mapped to its
        length in seconds.

        KEYED BY THE PATH THE DOCUMENT WRITES, bundle-relative, rather than by
        cue identifier: two cues naming one file are one read, and a cue whose
        `file` is edited to another file the show already uses gets the right
        answer rather than the previous one's. A `file` edited to something the
        walk never saw reads nought until the show is reloaded, which is the
        same shape as §3.25's graph: what the show declared when it was opened
        is what this session runs on.

        `mediaFolder` is the bundle's `media/` directory; a cue names its file
        relative to it. Reads each file once, on the thread that opens the
        show.

        KEPT AS A FUNCTION, with `MediaInfo` below as its one caller in the
        engine, so that there is exactly one reading of how long a show's media
        is and not two that could drift: the object is built on this, and a test
        compares the two. */
    std::map<std::string, double> mediaDurations (const doc::ShowDocument& document,
                                                  const std::string& mediaFolder);

    //==============================================================================
    /*  THE SHOW'S MEDIA, one record per file, owned by the verb that opened it.

        THE DURATIONS HALF IS FROZEN, and that is law rather than an aside.
        `durations()` is handed by ADDRESS to the parameter tree, the runner,
        the solver and the show walk, and `SlotAnalysis::ensureBuilt` - which
        `ParameterTree::publish` calls first thing, every publish - skips its
        rebuild only when the revision AND that address are the ones it last
        built with. Both halves of that test are sharp:

          - an address that moved, or a map reached through a pointer somebody
            swapped, fails the test on every publish and rebuilds the slot walk
            fifty times a second on the tick thread;
          - numbers that changed UNDER the same address pass the test, and every
            slot overlap is then computed from stale seconds, silently.

        So the map is a const member, filled once by `mediaDurations` in the
        constructor, and nothing writes to it afterwards - not `publish`, not the
        analyser PR 5.7 starts, not a corrected length a full read might find.
        The seconds are what they were when the show was opened, which is
        also what a replay of this session will be told (the log's `media`
        lines are written from this map).

        THE LATE HALF IS A SNAPSHOT: an immutable map swapped whole under a
        short mutex, the shape `ParameterTree::publish` and `snapshot()` use. A
        reader holding one keeps it, unchanged, for as long as it likes; a
        publish never edits a map anybody can see.

        `wfg replay` does NOT build one of these. It hands the runner a plain map
        filled from the log's own `media` lines, because a replay must use the
        lengths that were true when the log was written - and a `MediaInfo`
        there would mean hashing a bundle the replay was told not to trust. */
    class MediaInfo
    {
    public:
        /*  Reads every distinct file's length, on the calling thread - the one
            that opens the show, before the first publish and before any socket
            is open. That cost is Phase 4's and unchanged by this class. */
        MediaInfo (const doc::ShowDocument& document, const std::string& mediaFolder);

        /*  NEITHER COPYABLE NOR MOVABLE, and this is what enforces the law above
            rather than a matter of style. Every consumer holds `&durations()`;
            a copy would hand out a second address for the same show and a move
            would leave the first one pointing into a husk. With both deleted,
            the only way to get a durations map is to ask the one object that
            owns it, and its address is fixed for as long as that object lives. */
        MediaInfo (const MediaInfo&) = delete;
        MediaInfo (MediaInfo&&) = delete;
        MediaInfo& operator= (const MediaInfo&) = delete;
        MediaInfo& operator= (MediaInfo&&) = delete;

        ~MediaInfo() = default;

        /*  Any thread. The SAME reference on every call, into a map that never
            changes after construction - hand its address to anything that
            caches on one. */
        const std::map<std::string, double>& durations() const noexcept { return frozenDurations; }

        /*  Any thread. The most recently published records - never nullptr,
            and never changed after it is returned. Before anything is
            published, every file the show named, with its seconds, an empty
            hash and a null pyramid. */
        std::shared_ptr<const MediaRecords> snapshot() const;

        /*  The analyser thread calls this (`MediaAnalyser`, PR 5.7), once for
            each file it has a pyramid for; so do tests.

            Builds a new map from the current one plus this record, outside the
            reader's lock, and swaps it in under that lock. Publishers are
            serialised among themselves by a second mutex, so two of them cannot
            each copy the same map and lose one another's record - a reader never
            waits for that one.

            A PATH THE SHOW NAMED AT OPEN has its `seconds` REPLACED by the
            frozen one, so the two halves cannot disagree about a file they both
            know - the analyser's business there is the hash and the pyramid.

            A PATH IT DID NOT is published as given, and `durations()` never
            learns of it. The plan queues the analyser for "any file a
            media/file edit introduces", which is a file somebody imported
            mid-session: it has no frozen length, because the durations were
            read once at open and must never change (the law above), and it
            still deserves its colours now rather than at the next open. So the
            snapshot may name a file `durations()` does not; the reverse can
            never happen, and `durations()` never grows. *Corrected in PR 5.6's
            review (2026-09-14):* the first build refused such a path, which
            would have left an imported file grey until the show was reopened.
            */
        void publish (const std::string& path, MediaRecord record);

    private:
        /*  Declared FIRST, and const: it is filled by the constructor's
            initialiser list and by nothing, ever, afterwards. */
        const std::map<std::string, double> frozenDurations;

        /*  A plain mutex rather than the RtSnapshot spin lock, for
            `ParameterTree::publishMutex`'s reason: no reader is the audio
            thread, and a pointer copy is the whole of what anyone holds it for.
            The lipogram (PRD §4.2) is about the audio thread and not these. */
        mutable std::mutex swapMutex;
        std::shared_ptr<const MediaRecords> published;

        /*  Held by a publisher for the whole of a copy-and-swap, and never by a
            reader. */
        std::mutex publisherMutex;
    };
}
