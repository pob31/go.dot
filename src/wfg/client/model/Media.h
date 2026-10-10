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
    Bringing media into a show: the part of it that is not file handling.

    WHY THIS EXISTS AT ALL, and it is the reason this client is compiled rather
    than served. A browser is never told a dropped file's path - it is given
    the name and the bytes and nothing else, deliberately and by design - so a
    web client cannot express *use the file where it sits*. That is decision Y
    (§14.16), and it is the one capability the page cannot be given later: not
    a hole the engine must open for a second client, but something the FIRST
    client could never have.

    WHAT AN IMPORT IS, in three parts, and only the last of them is the show's:

      1. the bytes arrive in `<bundle>/media/` - a fact about a disk, like the
         timbre cache the analyser writes with no command and no record
      2. a cue is created, which IS a decision and goes through `cue.create`
      3. the cue names the file, which is the decision that matters (§4.10),
         and goes through `node.set` like any other value

    §14.16 argues 1 explicitly: copying media into the bundle is not a change
    to the show, so the client may do it itself and then send one ordinary
    command for the cue to name it. What it may never do is reach past the
    door for 2 and 3.

    AND THE CLIENT DOES NOT DRAW IDENTIFIERS. `cue.create` takes an optional
    id, and that argument is for REPLAY - the engine draws one, the log records
    the call with it, and a replay supplies it rather than drawing again. There
    is exactly one entropy consumer in this project and a window is not going
    to become the second, so a create is followed by finding what it made.

    std only, like the rest of model/: the file handling is the window's, and
    everything here can be asserted without one.

    AND SINCE THE BUG ROUND OF 2026-10-05 (namespace draft §30, S7) the bytes
    arrive off the window's thread, a name already in `media/` is a question
    rather than a refusal, and each way an import can fail says which it was.
    The decisions are here (`MediaImports`); the reading and copying is
    ui/MediaCopier's.
*/

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  What to call a cue made from a file. The extension goes and nothing
        else does: a name is the operator's, and a client that tidied
        underscores or stripped track numbers would be guessing at what
        somebody meant to call their own material. */
    std::string cueNameFor (const std::string& fileName);

    /** The name a cue's `file` attribute carries: relative to the bundle's `media/`. */
    std::string mediaNameFor (const std::string& fileName);

    /*  WHICH CUE A CREATE JUST MADE. `cue.create` answers on the tick thread
        and a client sees the result only in the next published tree, so an
        import that must then name a file has to find what it asked for. The
        index it gave is a MEMBER position and `order` lists members, so the
        cue at that position is the one - which is deterministic, and needs no
        diffing of before and after.

        Empty when the order cannot answer, which a caller must treat as "do
        not write anything": attaching a file to the wrong cue is worse than
        attaching it to none. */
    std::string createdAt (const std::string& orderText, int index);

    /*  A PENDING IMPORT: a file copied in, a cue asked for, and the naming
        still to do. Held by the window between one pass and the next. */
    struct Import
    {
        std::string parent;      ///< the list or group the cue was created in
        int index = 0;           ///< the member position it was asked for
        std::string cueName;     ///< what the create was told to call it
        std::string mediaName;   ///< what the cue's `file` should say
        std::uint64_t askedAt = 0;   ///< the revision when the create was sent
        std::int64_t askedTick = 0;  ///< and the engine's tick then, which is what patience counts (S7)

        /*  THE CUE TEMPLATE IT IS BORN FROM (namespace draft §38), or empty:
            `cue.createFrom` rather than `cue.create`. */
        std::string cueTemplate;
    };

    /*  WHETHER THE CUE STANDING AT THE ASKED-FOR POSITION IS THE ONE THE
        CREATE MADE, which has to be asked because the position alone does not
        answer it: the show has other clients, and somebody inserting a cue
        from the page in the same two hundred milliseconds would put a
        stranger exactly where this import is looking.

        Three things must agree - the name the create was given, a media cue,
        and no file yet - and a stranger passing all three is a media cue
        somebody called the same thing and left empty, where naming it is what
        was wanted anyway. Attaching a file to the wrong cue is worse than
        attaching it to none, so a caller writes nothing when this is false. */
    bool madeByImport (const Import& job, const std::string& kind,
                       const std::string& name, const std::string& file);

    /*  HOW LONG AN IMPORT WAITS FOR ITS CUE, counted in the ENGINE'S ticks
        and not in this window's passes (namespace draft §30, S7, SM).

        A create is applied on the first tick after it is sent, so a tree
        published some ticks later either holds the cue or holds the refusal.
        Counted in passes, as it was, an engine that fell behind - a tick
        thread busy for a couple of seconds - let the window run the count
        down without the engine having looked at the create at all, and the
        file was left in `media/` with "the cue for it was refused" said about
        a cue nobody had refused. The engine's own clock does not move while
        it is not working, so a stalled engine is waited for and a refusal is
        not: five seconds of its time at fifty ticks a second.

        Not the show's revision either: a refused create moves no revision,
        so a lone refusal would have been waited on until somebody else edited
        something. */
    inline constexpr std::int64_t importPatienceTicks = 250;

    /*  Whether a create has had its chance: true once the engine has run
        `importPatienceTicks` past the tick it was asked at, and true if its
        clock went backwards - a started-over clock is not one to wait for,
        and a wait that cannot end would hold every import behind it. */
    bool outOfPatience (const Import& job, std::int64_t tickNow);

    /*  How many passes a create from the new-cue row, a grouping and a move
        into a section yet to be made wait for what they asked for. They still
        count passes; an import counts the engine's ticks (above). */
    inline constexpr int importPatience = 50;   // two seconds at 25 Hz

    //==========================================================================
    /*  A NAME ALREADY IN `media/` (namespace draft §30, S7). The author's
        report - "I had issues importing some media files (wav) at first" -
        was most likely a file whose name a session that was never saved had
        left in the show's folder, refused as "could not copy". What is decided here is
        what to do instead; the reading and copying is the window's.

        WHETHER TWO NAMES ARE ONE FILE is the question a case-blind disk asks:
        Windows and macOS keep "Thunder.WAV" and "thunder.wav" as one file, and
        a show travels between machines, so the answer is the case-blind one
        everywhere - a show made on Linux with both would break on the Mac.
        Letters are folded through Latin-1 and Latin Extended-A, which is every
        letter of French and of most European names ("Chœur" and "CHŒUR" are
        one); past that a name is compared as written, and the window asks the
        disk as well, which folds whatever it folds. */
    bool sameFileName (std::string_view a, std::string_view b);

    /** The name among `present` that `wanted` would land on, or empty when none would. */
    std::string nameAmong (const std::vector<std::string>& present, std::string_view wanted);

    /*  THE FIRST NAME NOT TAKEN: `wanted` itself when it is free, else the
        first of "X 2.wav", "X 3.wav"... - the number before the extension,
        the last dot's, as `cueNameFor` reads one, so "Take.01.wav" becomes
        "Take.01 2.wav" and "Thunder" becomes "Thunder 2". Empty only when
        ten thousand are taken. */
    std::string freeName (const std::string& wanted, const std::function<bool (const std::string&)>& taken);

    /** The same, with a name taken when `present` holds it, case-blind. */
    std::string freeName (const std::string& wanted, const std::vector<std::string>& present);

    //==========================================================================
    /*  WHAT ONE PICKED FILE MET, as the window's worker found it, and what it
        did with it. A look answers with one of the first six. A copy answers
        `copied` or `failed` - or what a look would have said, when the name
        it was to take was taken in between (two files of one name in one
        drop) - and `noFolder` is the window's, for a show with nowhere to
        copy to. */
    enum class Found
    {
        missing,      ///< it is not where it was picked
        unreadable,   ///< it is there and cannot be opened
        inShow,       ///< it IS one of the show's own files, picked out of `media/`
        free,         ///< nothing in `media/` has its name
        same,         ///< a file there has its name and its bytes: that one is used
        other,        ///< a file there has its name and other bytes: somebody is asked
        copied,       ///< the bytes are in `media/`
        failed,       ///< they could not be put there
        noFolder      ///< the show has no folder to put them in
    };

    /*  What the worker read about a file and the one of its name, before
        deciding anything. */
    struct Arrival
    {
        bool exists = false;
        bool readable = false;
        bool inShow = false;          ///< the picked file is the one in `media/`
        std::string meets;            ///< the name in `media/` it would land on; empty when free
        std::uint64_t size = 0;
        std::uint64_t meetsSize = 0;
    };

    /*  THE SAME FILE OR ANOTHER. The sizes first, and the bytes only when the
        sizes agree: `sameBytes` reads both files, which is a second of disk
        for a long sound and nothing a different size needs. A file that is
        byte for byte the one already in the show is that one, used silently,
        because nothing would be lost and there is nothing to ask. */
    Found verdictFor (const Arrival& arrival, const std::function<bool()>& sameBytes);

    /*  THE THREE ANSWERS when another file has the name: use the show's,
        write over it, or keep both, the new one under the first free number.
        The words on the buttons are the stage brief's, put to the author with
        S7 rather than chosen by them. `ask` is no answer yet. */
    enum class Clash { ask, useTheShows, replace, keepBoth };

    /*  ONE FILE FOR THE WORKER: look at it, or copy it in. A copy is under
        its own name (`ask`, the name being free when it was looked at), over
        `met` (`replace`), or under the first free number (`keepBoth`). */
    struct MediaJob
    {
        bool copy = false;
        std::string source;           ///< the picked file, its whole path
        Clash answer = Clash::ask;
        std::string met;              ///< what a replace writes over
    };

    /*  AND WHAT IT DID, said back on the message thread. */
    struct MediaWork
    {
        Found found = Found::failed;
        std::string name;             ///< the name in `media/`: the one met, or the one written
        std::string why;              ///< a failed copy: the system's own words
    };

    /*  THE QUESTION, in words. `playedBy` are the cues that name the file
        already there - replacing it changes them too, and the question says
        so; `keptAs` is the name Keep both would give this one. */
    struct ClashWords
    {
        std::string title;
        std::string message;
    };

    ClashWords clashWords (const std::string& picked, const std::string& met,
                           const std::vector<std::string>& playedBy, const std::string& keptAs);

    /** The names of the cues whose `file` is `mediaName`, case-blind, in address order. */
    std::vector<std::string> cuesPlaying (const tree::TreeSnapshot& snapshot, const std::string& mediaName);

    /** A container's members as the tree lists them, whether it is a list or a group. */
    std::string orderIn (const tree::TreeSnapshot& snapshot, const std::string& container);

    //==========================================================================
    /*  EVERY IMPORT UNDER WAY, in the order the hands asked (namespace draft
        §30, S7). The window's worker reads and copies; this decides, in the
        order the files were picked, and is what the tests hold.

        THE WORKER IS GIVEN ONE FILE AT A TIME, and every picked file is LOOKED
        AT before any is copied (SI): a look is a name and a size, and the
        bytes only when a size agrees, so every question about a name already
        in the show comes within moments of the drop - never three minutes into
        a copy, when a hand may be on GO and a box would take the key.

        THE CUES ARE ASKED FOR ONE AT A TIME, each when its file has landed and
        the cue before it has been seen in the tree, and placed AFTER that one
        (SN). A position fixed at the drop would have been a number a long
        copy gave somebody time to move: a cue inserted above, from either
        client, and every file after it would have found a stranger where its
        cue should be. The first goes after the member the hand let go after,
        and a drop at the end stays at the end. A second import waits behind
        the first, cue for cue, so its files land after the first one's
        whatever order the copies finish in.

        A FILE DROPPED ON A MEDIA CUE (`link`) takes the same road with no
        create: the same look, the same question, the same copy off the
        window's thread, and the cue named at the end. */
    class MediaImports
    {
    public:
        /*  Files dropped, or chosen with `+ media`, into `parent` at member
            `index` (-1, or past the end: the end). `orderText` is the
            container's order as the window drew it, which turns the number
            into the member the cues follow. */
        void add (const std::string& parent, int index, const std::string& orderText,
                  const std::vector<std::string>& sources, const std::string& cueTemplate = {});

        /** One file for a media cue that is already there. */
        void link (const std::string& cueId, const std::string& source);

        bool idle() const noexcept { return batches.empty(); }

        /*  THE WORKER'S NEXT FILE, when it has none: the first one waiting to
            be looked at, anywhere in the queue, then the first waiting to be
            copied. Marked as handed out; `worked` is its answer. */
        std::optional<MediaJob> nextJob();
        void worked (const MediaWork& work);

        /*  THE QUESTION WAITING, the first in the order picked. `more` when
            the same import has another file still to be looked at or asked
            about, which is when "the same for the others" is worth offering. */
        struct Asked
        {
            std::string picked, met;
            bool more = false;
        };

        std::optional<Asked> asking() const;

        /*  The answer to `asking()`, and with `forTheRest`, the answer for
            every other file of the same import that meets a name. */
        void answer (Clash given, bool forTheRest);

        struct Naming
        {
            std::string cueId, mediaName;
        };

        struct Ended
        {
            std::string sentence;
            int made = 0;             ///< cues that now play a file, for the window's own warnings
        };

        /*  ONE PASS. What the tree now says about the cue asked for, and what
            to send: the namings (`file`), at most one create - which the
            window MUST send in this pass, since it is recorded as asked - the
            failures said since the last pass, and the imports that finished. */
        struct Steps
        {
            std::vector<Naming> namings;
            std::optional<Import> create;
            std::vector<std::string> said;
            std::vector<Ended> ended;
        };

        Steps follow (const tree::TreeSnapshot& snapshot, std::uint64_t revision);

        /** "Copying 3 of 8: Thunder.wav" while a copy is under way, else empty. */
        std::string progress() const;

    private:
        enum class Stage { waiting, looking, asking, toCopy, copying, landed, creating, done, failed };

        struct Arriving
        {
            std::string source, name;
            Stage stage = Stage::waiting;
            Clash answer = Clash::ask;
            std::string met;          ///< the name in `media/` it met
            std::string mediaName;    ///< what its cue plays, once landed
            bool wrote = false;       ///< landed by a copy rather than by using a file already there
            std::string said;         ///< failed: why, in words
        };

        struct Batch
        {
            int serial = 0;
            std::string parent;       ///< where the cues go; empty for a link
            std::string linkTo;       ///< a link: the cue that takes the file
            std::string after;        ///< the member the next cue follows; empty: the top
            bool atEnd = false;       ///< the next cue goes at the end
            int index = 0;            ///< the position dropped at, should `after` go
            Clash standing = Clash::ask;
            std::vector<Arriving> files;
            std::string cueTemplate;  ///< each cue born from it (namespace draft §38), or empty
        };

        struct Where
        {
            int serial = 0;
            std::size_t file = 0;
        };

        Batch* batchOf (int serial);
        Arriving* fileAt (const Where& where);
        void settle (Arriving& file, Clash given);
        void fail (Arriving& file, std::string sentence);
        static int indexFor (const Batch& batch, const tree::TreeSnapshot& snapshot);
        static Ended endOf (const Batch& batch);

        std::deque<Batch> batches;
        int serials = 0;
        std::optional<Where> handed;      ///< the file the worker has
        std::optional<Where> creating;    ///< the file whose cue is asked for
        std::optional<Import> inFlight;   ///< and the create itself
        std::vector<std::string> unsaid;  ///< failures waiting for the next `follow`
    };

    /*  THE WORDS FOR EACH WAY A FILE IS NOT IMPORTED, one per cause (the
        brief's item 2): before this, all four read "could not copy X into the
        show". */
    std::string notFoundWords (const std::string& name);
    std::string unreadableWords (const std::string& name);
    std::string copyFailedWords (const std::string& name, const std::string& why);
    std::string noFolderWords (const std::string& name);
    std::string refusedWords (const std::string& mediaName);

    /*  THE ANALYSIS CACHE'S LAST SWEEP (namespace draft §52), as
        `/godot/engine/mediaCacheSweep` says it; `number` nought before any. */
    struct CacheSweepRow
    {
        int number = 0;
        std::string state;          ///< sweeping, done or skipped
        bool asked = false;         ///< media.cleanCache, not the one at launch
        int removed = 0;
        std::int64_t bytes = 0;
        std::string problem;
    };

    CacheSweepRow readCacheSweep (const tree::TreeSnapshot&);

    /*  THE SENTENCE A SWEEP SOMEBODY ASKED FOR IS WORTH, once as it starts and
        once as it ends - what it removed and freed, or why it stopped. The one
        at launch is the machine's housekeeping and says nothing. */
    std::string cacheSweepNews (const CacheSweepRow& before, const CacheSweepRow& now);
}
