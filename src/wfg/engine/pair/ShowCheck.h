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
    WHETHER THE TWO MACHINES PLAY THE SAME SHOW (namespace draft §58, RD.10;
    the author's AFU, "Compare, warn, offer the text").

    Each machine of a pair works out its show's fingerprint and its media
    list's digest (document/ShowFingerprint.h). A machine that FOLLOWS lays
    its leader's against its own, says in one sentence what differs, and holds
    the leader's show text ready for `pair.adoptShow`, which takes it. Three
    pieces, for three threads.

    `Fingerprinter` is the worker. The tick thread hands it a COPY of the show
    tree when the show revision has moved - the shape of `DocumentWriter`, the
    autosave's writer: the tick thread takes what only it may take, and a thread
    of its own does the work that would cost the tick. What differs is how
    little the tick takes. The autosave's snapshot is the canonical text
    itself, made on the tick (1.4 ms in Release on the 500-cue show, M23); a
    fingerprint wants that text too, and in the Debug build it is several ticks
    of work, so the tick takes a deep copy of the tree (`ValueTree::createCopy`,
    a fraction of the text's cost) and the worker writes, hashes and cuts it.
    One copy at a time: a copy is handed over only while the worker is idle, and
    only once the show has been quiet for a few ticks or a second has passed,
    so a fader dragged across the show's rows costs a copy a second at most.
    The worker also looks at the media and the descriptions on its own every
    couple of seconds - a file removed, a hash the analyser has just learned -
    and says so when either moved. What it makes is published whole, as a
    `ShowState` behind a `shared_ptr`, with the answers to the other machine's
    questions already made into messages, so the tick that answers them only
    queues bytes.

    `ShowCheck` is the tick thread's - the questions and answers of the link,
    and the comparison. The leader's fingerprint and digest come in its
    summaries (pair/Summary.h) and are held to their slot as the summaries are
    (`InStep`), and compared there with this machine's own when both are of the
    show each machine has at that moment: same records applied, same show. Two
    in a row that differ, and the follower asks the leader for what it needs to
    say where - the leader's units, its show text and its descriptions for a
    show that differs, its media list for media that differs - down the path
    relays take. The answers say which fingerprint or digest they describe, and
    the sentence is made from them and this machine's own.

    THE WORDS. `pair/showDiffers` is one sentence: "the show differs: cue 4
    Rain changed, 1 added: 7 Bells", "1 sound missing here: tone.wav", "1 sound
    differs: rain.wav", joined by semicolons when there is more than one thing
    to say. `pair/showMatch` is true once the fingerprints agree and the media
    is the same; false until the first comparison after a follow begins, while
    anything differs, and on a machine that does not follow - as `pair/inStep`
    is.

    A SOUND THE SAME ON BOTH MACHINES is one whose content hashes agree when
    both analysers have one; when either has not, one of the same size written
    within two seconds of the other - a copy that kept its date, and a file
    system that keeps it to the even second (FAT). A copy that did not keep its
    date reads as differing until both analysers have hashed it; a movie or a
    still, which the analyser never hashes, by its size and its date alone.
*/

#include <wfg/engine/document/ShowFingerprint.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/pair/PeerLink.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wfg::pair
{
    namespace wire
    {
        /*  WHAT `/godot/pair/showAsk i:what` ASKS FOR: the show - its units,
            its text, its descriptions - the media list, or both. */
        inline constexpr std::int32_t askShow = 1;
        inline constexpr std::int32_t askMedia = 2;

        std::vector<std::uint8_t> showAskBytes (std::int32_t what);

        /*  HOW LONG ONE PART OF A SHOW'S TEXT MAY BE, in bytes: whole lines,
            one longer going alone, as a position's parts are (RD.8). */
        inline constexpr std::size_t showTextPartBytes = 16 * 1024;
    }

    //==========================================================================
    /*  ONE MACHINE'S SHOW AS ITS WORKER LAST MADE IT: the fingerprint and all
        it was made from, and the answers to the other machine's questions,
        already messages - the text in parts, each description and the units
        last (`show`), and the media list (`media`). Never changed once
        published. */
    struct ShowState
    {
        std::shared_ptr<const doc::ShowFingerprint> print;
        std::vector<std::vector<std::uint8_t>> show;
        std::vector<std::uint8_t> media;
    };

    /** The answers made from a fingerprint: what `Fingerprinter` publishes. */
    std::shared_ptr<const ShowState> stateOf (std::shared_ptr<const doc::ShowFingerprint> print);

    //==========================================================================
    class Fingerprinter
    {
    public:
        /*  Where the show's descriptions are read from (its bundle) and its
            files are found (its `media/`), and how the analyser's records are
            read - any thread, a snapshot (`audio::MediaInfo::snapshot`). */
        using Analysed = std::function<std::shared_ptr<const audio::MediaRecords>()>;

        Fingerprinter (juce::File bundle, std::string mediaFolder, Analysed analysed,
                       std::chrono::milliseconds lookAgainEvery = std::chrono::milliseconds (2000));
        ~Fingerprinter();

        Fingerprinter (const Fingerprinter&) = delete;
        Fingerprinter& operator= (const Fingerprinter&) = delete;

        bool start();
        void stop();

        /*  TICK THREAD: a copy of the show, at this show revision - a tree
            nobody else will ever write. Taken only while `idle`; one offered
            while the worker is busy replaces the one waiting. */
        void offer (juce::ValueTree copy, std::uint64_t revision);

        /** Nothing waiting and nothing in hand. Any thread. */
        bool idle() const;

        /*  THE LATEST ShowState, null before the first; and a count that moves
            with every one published, so the tick thread takes the lock only
            when there is something new. Any thread. */
        std::shared_ptr<const ShowState> latest() const;
        std::uint64_t published() const noexcept { return publishedCount.load (std::memory_order_acquire); }

        /*  What the worker does with one offer, on the calling thread: a test's
            way in, and the measurement's. */
        std::shared_ptr<const ShowState> make (const juce::ValueTree& show, std::uint64_t revision) const;

    private:
        void run();
        void publish (std::shared_ptr<const ShowState> state);

        //  Media and descriptions looked at again; a new state when either moved, null when neither did.
        std::shared_ptr<const ShowState> lookAgain (const ShowState& now) const;

        const juce::File bundle;
        const std::string mediaFolder;
        const Analysed analysed;
        const std::chrono::milliseconds lookAgainEvery;

        mutable std::mutex guard;
        std::condition_variable wake;
        juce::ValueTree waiting;                // under `guard`
        std::uint64_t waitingRevision = 0;      // under `guard`
        bool busy = false;                      // under `guard`
        bool stopping = false;                  // under `guard`
        std::shared_ptr<const ShowState> current;   // under `guard`
        std::atomic<std::uint64_t> publishedCount { 0 };
        std::thread thread;

        //  The worker's own: the last tree it made a state of, for looking again.
        juce::ValueTree lastShow;
    };

    //==========================================================================
    /*  WHAT DIFFERS, IN WORDS (above): the show's units laid side by side -
        "the show differs: cue 4 Rain changed", or empty when they agree - and
        the media lists, `there` naming the other machine ("the main") for a
        file only it has. */
    std::string showDifferenceOf (const std::vector<doc::ShowUnit>& theirs, const std::vector<doc::ShowUnit>& ours);
    std::string mediaDifferenceOf (const std::vector<doc::MediaEntry>& theirs, const std::vector<doc::MediaEntry>& ours,
                                   const std::string& there);

    //==========================================================================
    class ShowCheck
    {
    public:
        /*  THE OTHER MACHINE'S SHOW, WHOLE: the text a backup takes and the
            descriptions beside it, and the fingerprint they are. */
        struct Package
        {
            std::string fingerprint;
            std::string text;
            std::vector<doc::Description> descriptions;
        };

        /** What this machine's own show is now, as the comparison needs it. */
        struct Own
        {
            const ShowState* state = nullptr;   // the worker's latest, null before the first
            bool current = false;               // made from the show this machine has now
        };

        //======================================================================
        //  Any thread: the server's, a dialled path's.

        /** A question or an answer of RD.10's, by the channel that brought it. */
        void received (const std::string& channel, const osc::Packet& message);

        //======================================================================
        //  The tick thread's.

        /** Reads what arrived. */
        void drain();

        /*  THE LEADER'S HALF: every question waiting is answered from `state`,
            down the channel it came by - once `current` says it is of the show
            this machine has now, so an answer is never of a show a summary has
            already left behind; until then, and with none yet, they wait. */
        using Send = std::function<bool (const std::string& channel, const std::vector<std::uint8_t>& bytes)>;
        void answer (const ShowState* state, bool current, const Send& send);

        /** Questions waiting for an answer. */
        std::size_t asked() const noexcept { return questions.size(); }

        /*  THE FOLLOWER'S HALF. A summary's show, as it arrived; the
            comparison after local tick `localTick` has run, of every one whose
            slot - its tick plus `offset` - it is (`comparable` false drops
            them all, as `InStep` does); and the question to ask the leader now,
            if there is one - empty when there is none. `there` names the leader
            in the sentence. */
        void heard (std::int64_t leaderTick, std::string fingerprint, std::string mediaDigest);
        void afterTick (std::int64_t localTick, std::int64_t offset, bool comparable, const Own& own,
                        const std::string& there, LinkClock::time_point now);
        std::vector<std::uint8_t> question (LinkClock::time_point now);

        bool matches() const noexcept                { return showSame == Verdict::same && mediaSame == Verdict::same; }
        const std::string& differs() const noexcept  { return sentence; }

        /** Whether the SHOW differs, which taking the leader's would mend; media does not travel. */
        bool showDiffers() const noexcept            { return showSame == Verdict::differs; }

        /*  THE LEADER'S SHOW, WHOLE, when it is the one the leader last said in
            a summary; null otherwise - none fetched (the two were the same, or
            it is on its way), or the leader's has moved since. */
        const Package* package() const noexcept;

        /** The leader's show was taken: nothing is known of this one's until it is compared again. */
        void adopted();

        /** A follow that starts or stops: nothing known. */
        void reset();

        /*  The leader's units and media list as they last came, and what has
            been asked; for a test. */
        std::size_t unitsHeld() const noexcept { return theirUnits.size(); }
        std::int64_t questionsAsked() const noexcept { return askedCount; }

    private:
        enum class Verdict { unknown, same, differs };

        struct Said
        {
            std::int64_t tick = 0;
            std::string fingerprint, mediaDigest;
        };

        struct Item
        {
            std::string channel;
            osc::Packet message;
        };

        void take (const Item& item);
        void judge (const Own& own, const std::string& there);

        //  The tick thread's.
        std::vector<Item> items;
        std::map<std::string, std::int32_t> questions;      // channel -> what, waiting to be answered
        std::deque<Said> held;

        Verdict showSame = Verdict::unknown, mediaSame = Verdict::unknown;
        int showMisses = 0, mediaMisses = 0;
        std::string leaderPrint, leaderDigest;              // the leader's, as last compared
        std::string showWords, mediaWords, sentence;

        //  What came back, and what was asked.
        std::string unitsOf;                                // the fingerprint `theirUnits` describe
        std::vector<doc::ShowUnit> theirUnits;
        std::string mediaOf;                                // the digest `theirMedia` describe
        std::vector<doc::MediaEntry> theirMedia;
        bool heardNew = false;                              // an answer arrived: say again
        std::string judgedOwnPrint, judgedOwnDigest;        // this machine's, when the sentence was made

        Package incoming;                                   // a text and descriptions arriving
        std::vector<std::string> parts;                     // its text, part by part
        std::vector<bool> partsGot;
        std::optional<Package> whole;

        std::string askedPrint, askedDigest;
        LinkClock::time_point askedPrintAt {}, askedDigestAt {};
        std::int64_t askedCount = 0;

        //  Both.
        mutable std::mutex lock;
        std::vector<Item> inbox;
    };
}
