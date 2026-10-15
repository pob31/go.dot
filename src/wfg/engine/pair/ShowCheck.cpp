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

#include <wfg/engine/pair/ShowCheck.h>

#include <wfg/engine/document/ShowDocument.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string_view>
#include <utility>

namespace wfg::pair
{
    namespace
    {
        osc::Value text (std::string value)     { return osc::Value::string (std::move (value)); }
        osc::Value count (std::size_t value)    { return osc::Value::int32 (static_cast<std::int32_t> (value)); }

        /*  A TEXT CUT INTO PARTS OF WHOLE LINES no longer than `partBytes` - one
            line longer goes alone - as a position is cut (RD.8): no part ends
            inside a line, and the parts run together are the text. */
        std::vector<std::string_view> wholeLineParts (std::string_view all, std::size_t partBytes)
        {
            std::vector<std::string_view> parts;
            std::size_t from = 0;

            while (from < all.size())
            {
                auto upTo = from;

                while (upTo < all.size())
                {
                    const auto newline = all.find ('\n', upTo);
                    const auto lineEnd = newline == std::string_view::npos ? all.size() : newline + 1;

                    if (upTo > from && lineEnd - from > partBytes)
                        break;

                    upTo = lineEnd;
                }

                parts.push_back (all.substr (from, upTo - from));
                from = upTo;
            }

            if (parts.empty())
                parts.push_back (all);

            return parts;
        }

        //======================================================================
        /*  A FEW NAMES, AS A SENTENCE SAYS THEM: all of them up to three, then
            the first three and how many more. */
        std::string named (const std::vector<std::string>& names)
        {
            constexpr std::size_t most = 3;
            std::string out;

            for (std::size_t n = 0; n < names.size() && n < most; ++n)
            {
                if (n > 0)
                    out += ", ";

                out += names[n];
            }

            if (names.size() > most)
                out += " and " + std::to_string (names.size() - most) + " more";

            return out;
        }

        std::string some (std::size_t n, const std::string& one, const std::string& many)
        {
            return std::to_string (n) + " " + (n == 1 ? one : many);
        }

        std::string keyOfPlace (const std::string& place)
        {
            const auto slash = place.rfind ('/');
            return slash == std::string::npos ? place : place.substr (slash + 1);
        }

        std::vector<std::string> placesIn (const std::string& order)
        {
            std::vector<std::string> places;
            std::size_t at = 0;

            while (at < order.size())
            {
                const auto space = order.find (' ', at);
                const auto end = space == std::string::npos ? order.size() : space;

                if (end > at)
                    places.push_back (order.substr (at, end - at));

                at = end + 1;
            }

            return places;
        }

        bool isCueElement (const std::string& element)
        {
            return doc::ShowDocument::ownerForElement (element) == "cue";
        }

        /*  WHETHER ONE FILE IS THE SAME FILE ON BOTH MACHINES (ShowCheck.h):
            its bytes when both analysers have read them, its size and when it
            was written - within two seconds - when either has not. */
        bool sameFile (const doc::MediaEntry& a, const doc::MediaEntry& b)
        {
            if (! a.hash.empty() && ! b.hash.empty())
                return a.hash == b.hash;

            return a.size == b.size && std::llabs (a.modified - b.modified) <= 2000;
        }

        std::string fileWord (const std::vector<const doc::MediaEntry*>& files, bool plural)
        {
            std::set<std::string> kinds;

            for (const auto* file : files)
                kinds.insert (file->kind);

            const auto kind = kinds.size() == 1 ? *kinds.begin() : std::string ("file");
            return plural ? kind + "s" : kind;
        }

        std::string filesSaid (const std::vector<const doc::MediaEntry*>& files, const std::string& what)
        {
            std::vector<std::string> names;

            for (const auto* file : files)
                names.push_back (file->path);

            return std::to_string (files.size()) + " " + fileWord (files, files.size() != 1) + " " + what + ": "
                 + named (names);
        }
    }

    //==========================================================================
    namespace wire
    {
        std::vector<std::uint8_t> showAskBytes (std::int32_t what)
        {
            return encode (showAsk, { osc::Value::int32 (what) });
        }
    }

    std::shared_ptr<const ShowState> stateOf (std::shared_ptr<const doc::ShowFingerprint> print)
    {
        auto state = std::make_shared<ShowState>();

        if (print == nullptr)
            return state;

        const auto& fingerprint = print->fingerprint;

        /*  THE ANSWER TO "THE SHOW", in the order a follower can use it: the
            text in parts, each description, and the units last - a follower
            that has the units has everything sent before them. */
        const auto parts = wholeLineParts (print->decided, wire::showTextPartBytes);

        for (std::size_t n = 0; n < parts.size(); ++n)
            state->show.push_back (wire::encode (wire::showText, { text (fingerprint), count (n), count (parts.size()),
                                                                   text (std::string (parts[n])) }));

        for (const auto& description : print->descriptions)
        {
            osc::Blob bytes;
            bytes.bytes.assign (description.bytes.begin(), description.bytes.end());
            state->show.push_back (wire::encode (wire::showFile, { text (fingerprint), text (description.name),
                                                                   osc::Value::blob (std::move (bytes)) }));
        }

        {
            std::vector<osc::Value> args;
            args.reserve (2 + 5 * print->units.size());
            args.push_back (text (fingerprint));
            args.push_back (count (print->units.size()));

            for (const auto& unit : print->units)
            {
                args.push_back (text (unit.key));
                args.push_back (text (unit.element));
                args.push_back (text (unit.hash));
                args.push_back (text (unit.order));
                args.push_back (text (unit.label));
            }

            state->show.push_back (wire::encode (wire::showUnits, std::move (args)));
        }

        {
            std::vector<osc::Value> args;
            args.reserve (2 + 6 * print->media.size());
            args.push_back (text (print->mediaDigest));
            args.push_back (count (print->media.size()));

            for (const auto& entry : print->media)
            {
                args.push_back (text (entry.path));
                args.push_back (text (entry.kind));
                args.push_back (osc::Value::int32 (entry.present ? 1 : 0));
                args.push_back (osc::Value::int64 (entry.size));
                args.push_back (osc::Value::int64 (entry.modified));
                args.push_back (text (entry.hash));
            }

            state->media = wire::encode (wire::showMedia, std::move (args));
        }

        state->print = std::move (print);
        return state;
    }

    //==========================================================================
    Fingerprinter::Fingerprinter (juce::File bundleToRead, std::string mediaFolderToRead, Analysed analysedToRead,
                                  std::chrono::milliseconds every)
        : bundle (std::move (bundleToRead)),
          mediaFolder (std::move (mediaFolderToRead)),
          analysed (std::move (analysedToRead)),
          lookAgainEvery (every)
    {
    }

    Fingerprinter::~Fingerprinter()
    {
        stop();
    }

    bool Fingerprinter::start()
    {
        const std::lock_guard<std::mutex> held { guard };

        if (thread.joinable())
            return false;

        stopping = false;
        thread = std::thread ([this] { run(); });
        return true;
    }

    void Fingerprinter::stop()
    {
        {
            const std::lock_guard<std::mutex> held { guard };
            stopping = true;
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();
    }

    void Fingerprinter::offer (juce::ValueTree copy, std::uint64_t revision)
    {
        {
            const std::lock_guard<std::mutex> held { guard };
            waiting = std::move (copy);
            waitingRevision = revision;
        }

        wake.notify_one();
    }

    bool Fingerprinter::idle() const
    {
        const std::lock_guard<std::mutex> held { guard };
        return ! waiting.isValid() && ! busy;
    }

    std::shared_ptr<const ShowState> Fingerprinter::latest() const
    {
        const std::lock_guard<std::mutex> held { guard };
        return current;
    }

    std::shared_ptr<const ShowState> Fingerprinter::make (const juce::ValueTree& show, std::uint64_t revision) const
    {
        const auto records = analysed ? analysed() : std::shared_ptr<const audio::MediaRecords> {};

        return stateOf (std::make_shared<const doc::ShowFingerprint> (
            doc::fingerprintShow (show, revision, bundle, mediaFolder, records.get())));
    }

    std::shared_ptr<const ShowState> Fingerprinter::lookAgain (const ShowState& now) const
    {
        if (now.print == nullptr || ! lastShow.isValid())
            return {};

        const auto records = analysed ? analysed() : std::shared_ptr<const audio::MediaRecords> {};
        auto descriptions = doc::descriptionsIn (bundle);
        auto media = doc::mediaListOf (lastShow, mediaFolder, records.get());

        if (descriptions == now.print->descriptions && media == now.print->media)
            return {};

        /*  THE SAME SHOW, ITS SURROUNDINGS MOVED: the text and its units are
            the tree's and stand; a description changed is a new fingerprint,
            and the media list a new digest. */
        auto print = std::make_shared<doc::ShowFingerprint> (*now.print);

        if (descriptions != print->descriptions)
        {
            print->fingerprint = doc::fingerprintOf (print->decided, descriptions);
            print->units.erase (std::remove_if (print->units.begin(), print->units.end(),
                                                [] (const doc::ShowUnit& unit) { return unit.element == "Description"; }),
                                print->units.end());

            for (auto& unit : doc::showUnitsOf (juce::ValueTree(), descriptions))
                print->units.push_back (std::move (unit));

            print->descriptions = std::move (descriptions);
        }

        print->mediaDigest = doc::mediaDigestOf (media);
        print->media = std::move (media);
        return stateOf (std::move (print));
    }

    void Fingerprinter::publish (std::shared_ptr<const ShowState> state)
    {
        current = std::move (state);
        publishedCount.fetch_add (1, std::memory_order_release);
    }

    void Fingerprinter::run()
    {
        std::unique_lock<std::mutex> held { guard };

        while (! stopping)
        {
            if (! waiting.isValid())
            {
                wake.wait_for (held, lookAgainEvery, [this] { return stopping || waiting.isValid(); });

                if (stopping)
                    break;

                /*  NOTHING OFFERED FOR A WHILE: the media and the descriptions
                    looked at again - a sound removed or replaced, a hash the
                    analyser has learned since - outside the lock. */
                if (! waiting.isValid())
                {
                    const auto now = current;
                    held.unlock();
                    auto moved = now != nullptr ? lookAgain (*now) : std::shared_ptr<const ShowState> {};
                    held.lock();

                    if (moved != nullptr && ! waiting.isValid())
                        publish (std::move (moved));

                    continue;
                }
            }

            auto show = waiting;
            waiting = juce::ValueTree();
            const auto revision = waitingRevision;
            busy = true;
            held.unlock();

            auto made = make (show, revision);

            /*  The tree kept for looking again, and the one before it let go
                of here, on this thread - never the tick's. */
            lastShow = std::move (show);

            held.lock();
            busy = false;
            publish (std::move (made));
        }
    }

    //==========================================================================
    std::string showDifferenceOf (const std::vector<doc::ShowUnit>& theirs, const std::vector<doc::ShowUnit>& ours)
    {
        std::map<std::string, const doc::ShowUnit*> mine, there;

        for (const auto& unit : ours)
            mine.emplace (unit.key, &unit);

        for (const auto& unit : theirs)
            there.emplace (unit.key, &unit);

        std::vector<std::string> cuesChanged, cuesAdded, cuesRemoved, settings, descriptions, moved;
        std::set<std::string> comeOrGone;

        //  The leader's in its order - root first, then the show as it reads - then this machine's it has not.
        for (const auto& unit : theirs)
        {
            const auto found = mine.find (unit.key);
            const auto description = unit.element == "Description";
            const auto cue = isCueElement (unit.element);

            if (found == mine.end())
            {
                comeOrGone.insert (unit.key);
                (description ? descriptions : cue ? cuesAdded : settings).push_back (unit.label);
                continue;
            }

            if (found->second->hash != unit.hash || found->second->element != unit.element)
                (description ? descriptions : cue ? cuesChanged : settings).push_back (unit.label);
        }

        for (const auto& unit : ours)
        {
            if (there.count (unit.key) != 0)
                continue;

            comeOrGone.insert (unit.key);
            (unit.element == "Description" ? descriptions
                                           : isCueElement (unit.element) ? cuesRemoved : settings).push_back (unit.label);
        }

        /*  MOVED: a unit on both machines whose units inside it come in
            another order once those only one machine has are set aside -
            which an addition or a removal alone never is. */
        for (const auto& unit : theirs)
        {
            const auto found = mine.find (unit.key);

            if (found == mine.end() || found->second->order == unit.order)
                continue;

            auto a = placesIn (unit.order), b = placesIn (found->second->order);
            const auto settled = [&comeOrGone] (const std::string& place) { return comeOrGone.count (keyOfPlace (place)) != 0; };
            a.erase (std::remove_if (a.begin(), a.end(), settled), a.end());
            b.erase (std::remove_if (b.begin(), b.end(), settled), b.end());

            if (a != b)
                moved.push_back (unit.label);
        }

        std::vector<std::string> parts;

        //  THE CUES FIRST: one named in a sentence of its own, more counted and named.
        const auto cueKinds = (cuesChanged.empty() ? 0 : 1) + (cuesAdded.empty() ? 0 : 1) + (cuesRemoved.empty() ? 0 : 1);

        if (cueKinds == 1 && cuesChanged.size() + cuesAdded.size() + cuesRemoved.size() == 1)
        {
            parts.push_back ("cue " + (! cuesChanged.empty() ? cuesChanged.front() + " changed"
                                     : ! cuesAdded.empty()  ? cuesAdded.front() + " added"
                                                            : cuesRemoved.front() + " removed"));
        }
        else if (cueKinds > 0)
        {
            std::string said;
            const auto say = [&said] (const std::vector<std::string>& labels, const std::string& what, bool first)
            {
                if (labels.empty())
                    return;

                if (! said.empty())
                    said += ", ";

                said += (first ? some (labels.size(), "cue", "cues") : std::to_string (labels.size())) + " " + what
                      + " (" + named (labels) + ")";
            };

            say (cuesChanged, "changed", true);
            say (cuesAdded, "added", said.empty());
            say (cuesRemoved, "removed", said.empty());
            parts.push_back (said);
        }

        if (! moved.empty())
            parts.push_back ("cues moved in " + named (moved));

        if (settings.size() == 1)
            parts.push_back (settings.front() + " changed");
        else if (! settings.empty())
            parts.push_back (some (settings.size(), "setting", "settings") + " changed (" + named (settings) + ")");

        if (descriptions.size() == 1)
            parts.push_back ("the device description " + descriptions.front() + " differs");
        else if (! descriptions.empty())
            parts.push_back (std::to_string (descriptions.size()) + " device descriptions differ (" + named (descriptions)
                             + ")");

        if (parts.empty())
            return {};

        std::string sentence = "the show differs: ";

        for (std::size_t n = 0; n < parts.size(); ++n)
            sentence += (n > 0 ? ", " : "") + parts[n];

        return sentence;
    }

    std::string mediaDifferenceOf (const std::vector<doc::MediaEntry>& theirs, const std::vector<doc::MediaEntry>& ours,
                                   const std::string& there)
    {
        std::map<std::string, const doc::MediaEntry*> mine;

        for (const auto& entry : ours)
            mine.emplace (entry.path, &entry);

        std::vector<const doc::MediaEntry*> missingHere, missingThere, differing;

        /*  ONLY WHAT BOTH SHOWS NAME: a file one show names and the other does
            not is the show's difference, said with the show's. */
        for (const auto& entry : theirs)
        {
            const auto found = mine.find (entry.path);

            if (found == mine.end())
                continue;

            const auto& own = *found->second;

            if (entry.present && ! own.present)
                missingHere.push_back (&entry);
            else if (! entry.present && own.present)
                missingThere.push_back (&own);
            else if (entry.present && own.present && ! sameFile (entry, own))
                differing.push_back (&entry);
        }

        std::vector<std::string> parts;

        if (! missingHere.empty())
            parts.push_back (filesSaid (missingHere, "missing here"));

        if (! missingThere.empty())
            parts.push_back (filesSaid (missingThere, "missing on " + there));

        if (! differing.empty())
            parts.push_back (filesSaid (differing, differing.size() == 1 ? "differs" : "differ"));

        std::string sentence;

        for (std::size_t n = 0; n < parts.size(); ++n)
            sentence += (n > 0 ? "; " : "") + parts[n];

        return sentence;
    }

    //==========================================================================
    void ShowCheck::received (const std::string& channel, const osc::Packet& message)
    {
        const std::lock_guard<std::mutex> locked { lock };
        inbox.push_back ({ channel, message });
    }

    void ShowCheck::drain()
    {
        {
            const std::lock_guard<std::mutex> locked { lock };
            items.swap (inbox);
        }

        for (const auto& item : items)
            take (item);

        items.clear();
    }

    void ShowCheck::take (const Item& item)
    {
        const auto& message = item.message;
        const auto& a = message.args;

        if (message.isBundle())
            return;

        if (message.address == wire::showAsk)
        {
            if (a.size() == 1 && a[0].isInt32() && (a[0].getInt32() & (wire::askShow | wire::askMedia)) != 0)
                questions[item.channel] |= a[0].getInt32() & (wire::askShow | wire::askMedia);

            return;
        }

        if (message.address == wire::showText)
        {
            if (a.size() != 4 || ! a[0].isString() || ! a[1].isInt32() || ! a[2].isInt32() || ! a[3].isString()
                  || a[2].getInt32() <= 0 || a[1].getInt32() < 0 || a[1].getInt32() >= a[2].getInt32())
                return;

            //  A new text - another fingerprint, or the first part again - starts afresh.
            if (a[0].getString() != incoming.fingerprint || a[1].getInt32() == 0)
            {
                incoming = {};
                incoming.fingerprint = a[0].getString();
                parts.assign (static_cast<std::size_t> (a[2].getInt32()), std::string());
                partsGot.assign (parts.size(), false);
            }

            const auto at = static_cast<std::size_t> (a[1].getInt32());

            if (at < parts.size())
            {
                parts[at] = a[3].getString();
                partsGot[at] = true;
            }

            return;
        }

        if (message.address == wire::showFile)
        {
            if (a.size() == 3 && a[0].isString() && a[1].isString() && a[2].isBlob()
                  && a[0].getString() == incoming.fingerprint)
            {
                const auto& bytes = a[2].getBlob().bytes;
                incoming.descriptions.push_back ({ a[1].getString(), std::string (bytes.begin(), bytes.end()) });
            }

            return;
        }

        if (message.address == wire::showUnits)
        {
            if (a.size() < 2 || ! a[0].isString() || ! a[1].isInt32() || a[1].getInt32() < 0
                  || a.size() != 2 + 5 * static_cast<std::size_t> (a[1].getInt32()))
                return;

            std::vector<doc::ShowUnit> units;
            units.reserve (static_cast<std::size_t> (a[1].getInt32()));

            for (std::size_t at = 2; at + 4 < a.size(); at += 5)
            {
                for (std::size_t n = 0; n < 5; ++n)
                    if (! a[at + n].isString())
                        return;

                units.push_back ({ a[at].getString(), a[at + 1].getString(), a[at + 2].getString(),
                                   a[at + 3].getString(), a[at + 4].getString() });
            }

            unitsOf = a[0].getString();
            theirUnits = std::move (units);
            heardNew = true;

            /*  THE UNITS COME LAST: the text and the descriptions before them,
                whole, are the leader's show - what `pair.adoptShow` takes. */
            if (incoming.fingerprint == unitsOf && ! partsGot.empty()
                  && std::all_of (partsGot.begin(), partsGot.end(), [] (bool got) { return got; }))
            {
                for (const auto& part : parts)
                    incoming.text += part;

                std::sort (incoming.descriptions.begin(), incoming.descriptions.end(),
                           [] (const doc::Description& x, const doc::Description& y) { return x.name < y.name; });

                whole = std::move (incoming);
                incoming = {};
                parts.clear();
                partsGot.clear();
            }

            return;
        }

        if (message.address == wire::showMedia)
        {
            if (a.size() < 2 || ! a[0].isString() || ! a[1].isInt32() || a[1].getInt32() < 0
                  || a.size() != 2 + 6 * static_cast<std::size_t> (a[1].getInt32()))
                return;

            std::vector<doc::MediaEntry> media;
            media.reserve (static_cast<std::size_t> (a[1].getInt32()));

            for (std::size_t at = 2; at + 5 < a.size(); at += 6)
            {
                if (! a[at].isString() || ! a[at + 1].isString() || ! a[at + 2].isInt32() || ! a[at + 3].isInt64()
                      || ! a[at + 4].isInt64() || ! a[at + 5].isString())
                    return;

                doc::MediaEntry entry;
                entry.path = a[at].getString();
                entry.kind = a[at + 1].getString();
                entry.present = a[at + 2].getInt32() != 0;
                entry.size = a[at + 3].getInt64();
                entry.modified = a[at + 4].getInt64();
                entry.hash = a[at + 5].getString();
                media.push_back (std::move (entry));
            }

            mediaOf = a[0].getString();
            theirMedia = std::move (media);
            heardNew = true;
        }
    }

    void ShowCheck::answer (const ShowState* state, bool current, const Send& send)
    {
        if (questions.empty() || state == nullptr || state->print == nullptr || ! current || ! send)
            return;

        for (const auto& [channel, what] : questions)
        {
            if ((what & wire::askShow) != 0)
                for (const auto& message : state->show)
                    if (! message.empty())
                        send (channel, message);

            if ((what & wire::askMedia) != 0 && ! state->media.empty())
                send (channel, state->media);
        }

        questions.clear();
    }

    void ShowCheck::heard (std::int64_t leaderTick, std::string fingerprint, std::string mediaDigest)
    {
        //  A few seconds of them at most, as `InStep` holds its own.
        if (held.size() >= 256)
            held.pop_front();

        held.push_back ({ leaderTick, std::move (fingerprint), std::move (mediaDigest) });
    }

    void ShowCheck::afterTick (std::int64_t localTick, std::int64_t offset, bool comparable, const Own& own,
                               const std::string& there, LinkClock::time_point)
    {
        if (! comparable)
            held.clear();

        const auto* mine = own.state != nullptr ? own.state->print.get() : nullptr;

        while (! held.empty())
        {
            const auto slot = held.front().tick + offset;

            if (slot > localTick)
                break;

            auto said = std::move (held.front());
            held.pop_front();

            /*  ITS SLOT PASSED - a summary that came late reads a moment this
                machine has left - OR ONE SIDE IS NOT OF ITS SHOW YET: the
                leader still working its fingerprint out (an empty one), or this
                machine; nothing is said until both are. */
            if (slot < localTick || said.fingerprint.empty() || mine == nullptr || ! own.current)
                continue;

            //  THE SHOW: twice in a row before it is said to differ, as a summary's runs are.
            leaderPrint = said.fingerprint;

            if (said.fingerprint == mine->fingerprint)
            {
                showMisses = 0;
                showSame = Verdict::same;
            }
            else if (++showMisses >= 2)
            {
                showSame = Verdict::differs;
            }

            /*  THE MEDIA: the same digest is the same media; another, twice,
                is decided by the lists themselves (below), whose comparison
                knows a copy that kept its date from a file that changed. */
            leaderDigest = said.mediaDigest;

            if (said.mediaDigest == mine->mediaDigest)
            {
                mediaMisses = 0;
                mediaSame = Verdict::same;
                mediaWords.clear();
            }
            else
            {
                ++mediaMisses;
            }
        }

        if (mine != nullptr)
            judge (own, there);
    }

    void ShowCheck::judge (const Own& own, const std::string& there)
    {
        const auto& mine = *own.state->print;

        if (showSame == Verdict::differs)
        {
            if (unitsOf == leaderPrint && (heardNew || showWords.empty() || judgedOwnPrint != mine.fingerprint))
            {
                showWords = showDifferenceOf (theirUnits, mine.units);

                //  Two fingerprints apart with every unit alike: nothing this build can name.
                if (showWords.empty())
                    showWords = "the show differs";
            }
        }
        else
        {
            showWords.clear();
        }

        if (mediaMisses >= 2 && mediaOf == leaderDigest && mediaOf != mine.mediaDigest
              && (heardNew || judgedOwnDigest != mine.mediaDigest || mediaSame == Verdict::unknown))
        {
            mediaWords = mediaDifferenceOf (theirMedia, mine.media, there);
            mediaSame = mediaWords.empty() ? Verdict::same : Verdict::differs;
        }

        heardNew = false;
        judgedOwnPrint = mine.fingerprint;
        judgedOwnDigest = mine.mediaDigest;

        sentence = showSame == Verdict::differs ? showWords : std::string();

        if (mediaSame == Verdict::differs && ! mediaWords.empty())
            sentence += (sentence.empty() ? "" : "; ") + mediaWords;
    }

    std::vector<std::uint8_t> ShowCheck::question (LinkClock::time_point now)
    {
        std::int32_t what = 0;
        constexpr auto again = std::chrono::seconds (2);

        /*  THE SHOW, when it differs and what the leader last said has not come
            - its units, or the text beside them; asked again after two seconds
            unanswered (a leader still working its show out, a path that went). */
        if (showSame == Verdict::differs && ! leaderPrint.empty()
              && (unitsOf != leaderPrint || package() == nullptr)
              && (askedPrint != leaderPrint || now - askedPrintAt >= again))
        {
            what |= wire::askShow;
            askedPrint = leaderPrint;
            askedPrintAt = now;
        }

        if (mediaMisses >= 2 && ! leaderDigest.empty() && mediaOf != leaderDigest
              && (askedDigest != leaderDigest || now - askedDigestAt >= again))
        {
            what |= wire::askMedia;
            askedDigest = leaderDigest;
            askedDigestAt = now;
        }

        if (what == 0)
            return {};

        ++askedCount;
        return wire::showAskBytes (what);
    }

    const ShowCheck::Package* ShowCheck::package() const noexcept
    {
        return whole.has_value() && ! leaderPrint.empty() && whole->fingerprint == leaderPrint ? &*whole : nullptr;
    }

    void ShowCheck::adopted()
    {
        whole.reset();
        showSame = Verdict::unknown;
        showMisses = 0;
        showWords.clear();
        sentence = mediaSame == Verdict::differs ? mediaWords : std::string();
    }

    void ShowCheck::reset()
    {
        held.clear();
        showSame = mediaSame = Verdict::unknown;
        showMisses = mediaMisses = 0;
        leaderPrint.clear();
        leaderDigest.clear();
        showWords.clear();
        mediaWords.clear();
        sentence.clear();
        unitsOf.clear();
        theirUnits.clear();
        mediaOf.clear();
        theirMedia.clear();
        heardNew = false;
        judgedOwnPrint.clear();
        judgedOwnDigest.clear();
        incoming = {};
        parts.clear();
        partsGot.clear();
        whole.reset();
        askedPrint.clear();
        askedDigest.clear();
    }

    //==========================================================================
    FollowerVerdict verdictOf (const InStep& inStep, const ShowCheck& show, bool showCurrent, std::string seat)
    {
        FollowerVerdict said;
        said.seat = std::move (seat);

        /*  IN STEP, as `pair/inStep` and the alarm read it here: a summary
            matched; or one said what differs (or a record came out otherwise,
            RD.6); or neither yet - a follow just begun, nothing compared. */
        if (inStep.inStep())
        {
            said.inStep = true;
        }
        else if (! inStep.differs().empty())
        {
            said.inStep = false;
            said.differs = wire::clipped (inStep.differs());
        }

        /*  THE SAME SHOW, as `pair/showMatch` and the alarm read it here - and
            NOT KNOWN while this machine's fingerprint is a moment behind its
            own show, after an edit: the last comparison is of a show it no
            longer has. */
        if (showCurrent)
        {
            if (show.matches())
            {
                said.showMatch = true;
            }
            else if (! show.differs().empty())
            {
                said.showMatch = false;
                said.showDiffers = wire::clipped (show.differs());
            }
        }

        return said;
    }
}
