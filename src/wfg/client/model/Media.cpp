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

#include <wfg/client/model/Media.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        /*  The last dot, and only when something follows it and something
            precedes it: a file called `.hidden` has no extension to strip and
            one called `thunder.` has nothing after the dot to be one. */
        std::size_t extensionAt (const std::string& name)
        {
            const auto dot = name.find_last_of ('.');

            if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size())
                return std::string::npos;

            return dot;
        }

        /*  ONE CODE POINT OUT OF UTF-8, moving `at` past it. A byte that does
            not start a well-formed sequence is taken as itself, so a name in
            some other encoding is compared byte for byte rather than lost. */
        char32_t nextPoint (std::string_view utf8, std::size_t& at)
        {
            const auto lead = static_cast<unsigned char> (utf8[at]);
            auto width = std::size_t { 1 };
            auto point = static_cast<char32_t> (lead);

            if (lead >= 0xC0 && lead < 0xE0)      { width = 2; point = static_cast<char32_t> (lead & 0x1F); }
            else if (lead >= 0xE0 && lead < 0xF0) { width = 3; point = static_cast<char32_t> (lead & 0x0F); }
            else if (lead >= 0xF0 && lead < 0xF8) { width = 4; point = static_cast<char32_t> (lead & 0x07); }

            if (width == 1 || at + width > utf8.size())
            {
                ++at;
                return static_cast<char32_t> (lead);
            }

            for (std::size_t i = 1; i < width; ++i)
            {
                const auto next = static_cast<unsigned char> (utf8[at + i]);

                if ((next & 0xC0) != 0x80)
                {
                    ++at;
                    return static_cast<char32_t> (lead);
                }

                point = static_cast<char32_t> ((point << 6) | static_cast<char32_t> (next & 0x3F));
            }

            at += width;
            return point;
        }

        /*  A LETTER AS A CASE-BLIND DISK READS IT, through Latin-1 and Latin
            Extended-A: every capital there has its small letter one or
            thirty-two places on, with the few exceptions named. Further than
            that the code point is its own, and the disk is asked as well. */
        char32_t folded (char32_t point)
        {
            if (point >= U'A' && point <= U'Z')
                return static_cast<char32_t> (point + 32u);

            if (point >= 0xC0u && point <= 0xDEu && point != 0xD7u)    // not ×
                return static_cast<char32_t> (point + 32u);

            if (point < 0x100u || point > 0x17Fu)
                return point;

            //  İ ı ĸ ŉ ſ have no partner in the block; Ÿ's is in Latin-1.
            if (point == 0x130u || point == 0x131u || point == 0x138u || point == 0x149u || point == 0x17Fu)
                return point;

            if (point == 0x178u)
                return 0xFFu;

            /*  Pairs, capital first: on an even code point through most of the
                block, on an odd one from Ĺ to ň and from Ź to ž. */
            const auto oddCapitals = (point >= 0x139u && point <= 0x148u) || (point >= 0x179u && point <= 0x17Eu);
            const auto capital = oddCapitals ? (point % 2u == 1u) : (point % 2u == 0u);

            return capital ? static_cast<char32_t> (point + 1u) : point;
        }

        std::u32string foldedName (std::string_view name)
        {
            std::u32string out;
            out.reserve (name.size());

            for (std::size_t at = 0; at < name.size();)
                out.push_back (folded (nextPoint (name, at)));

            return out;
        }

        std::string asText (std::size_t number)
        {
            return std::to_string (number);
        }
    }

    std::string cueNameFor (const std::string& fileName)
    {
        const auto dot = extensionAt (fileName);

        return dot == std::string::npos ? fileName : fileName.substr (0, dot);
    }

    std::string mediaNameFor (const std::string& fileName)
    {
        /*  THE NAME, NEVER THE PATH. `media/@file` is relative to the bundle's
            `media/` folder because a show travels between machines and an
            absolute path is a fact about the machine it was authored on - the
            parameter table says so in as many words. So whatever was dropped,
            what the cue carries is what the file is called. */
        const auto slash = fileName.find_last_of ("/\\");

        return slash == std::string::npos ? fileName : fileName.substr (slash + 1);
    }

    std::string createdAt (const std::string& orderText, int index)
    {
        if (index < 0)
            return {};

        const auto members = words (orderText);

        if (members.empty())
            return {};

        /*  AN INDEX PAST THE END MEANS THE END, which is the document's own
            rule for a create: `min (index, length)`. So a drop at the foot of
            a list finds the cue it just made rather than nothing. */
        const auto at = static_cast<std::size_t> (index);

        return at < members.size() ? members[at] : members.back();
    }

    bool madeByImport (const Import& job, const std::string& kind,
                       const std::string& name, const std::string& file)
    {
        return kind == "media" && name == job.cueName && file.empty();
    }

    bool outOfPatience (const Import& job, std::int64_t tickNow)
    {
        const auto waited = tickNow - job.askedTick;

        return waited < 0 || waited >= importPatienceTicks;
    }

    //==========================================================================
    bool sameFileName (std::string_view a, std::string_view b)
    {
        return foldedName (a) == foldedName (b);
    }

    std::string nameAmong (const std::vector<std::string>& present, std::string_view wanted)
    {
        const auto key = foldedName (wanted);

        for (const auto& name : present)
            if (foldedName (name) == key)
                return name;

        return {};
    }

    std::string freeName (const std::string& wanted, const std::function<bool (const std::string&)>& taken)
    {
        if (! taken (wanted))
            return wanted;

        const auto dot = extensionAt (wanted);
        const auto stem = dot == std::string::npos ? wanted : wanted.substr (0, dot);
        const auto extension = dot == std::string::npos ? std::string {} : wanted.substr (dot);

        for (std::size_t number = 2; number < 10000; ++number)
        {
            auto candidate = stem + " " + asText (number) + extension;

            if (! taken (candidate))
                return candidate;
        }

        return {};
    }

    std::string freeName (const std::string& wanted, const std::vector<std::string>& present)
    {
        return freeName (wanted, [&present] (const std::string& name) { return ! nameAmong (present, name).empty(); });
    }

    //==========================================================================
    Found verdictFor (const Arrival& arrival, const std::function<bool()>& sameBytes)
    {
        if (! arrival.exists)
            return Found::missing;

        if (! arrival.readable)
            return Found::unreadable;

        /*  Before the name: a file picked out of the show's own folder meets
            itself there, and is that file. */
        if (arrival.inShow)
            return Found::inShow;

        if (arrival.meets.empty())
            return Found::free;

        if (arrival.size != arrival.meetsSize)
            return Found::other;

        return sameBytes() ? Found::same : Found::other;
    }

    ClashWords clashWords (const std::string& picked, const std::string& met,
                           const std::vector<std::string>& playedBy, const std::string& keptAs)
    {
        ClashWords out;
        out.title = "Another " + picked + " is in the show";
        out.message = "The show already has a file called " + met + ", and it is not this one.";

        /*  WHO ELSE HEARS IT, since a replace is not this cue's alone: three
            names and a count past that, which is a sentence and not a list. */
        if (! playedBy.empty())
        {
            const auto quoted = [] (const std::string& name) { return "\"" + name + "\""; };
            const auto shown = std::min<std::size_t> (playedBy.size(), 3);
            std::string who;

            for (std::size_t i = 0; i < shown; ++i)
            {
                if (i > 0)
                    who += (i + 1 == shown && playedBy.size() <= 3) ? " and " : ", ";

                who += quoted (playedBy[i]);
            }

            if (playedBy.size() > 3)
                who += " and " + asText (playedBy.size() - 3) + " more";

            const auto one = playedBy.size() == 1;
            out.message += "\n\n" + who + (one ? " plays" : " play") + " it, so replacing it changes "
                           + (one ? "that cue" : "those cues") + " too.";
        }

        if (! keptAs.empty())
            out.message += "\n\nKeep both puts this one beside it as " + keptAs + ".";

        return out;
    }

    std::vector<std::string> cuesPlaying (const tree::TreeSnapshot& snapshot, const std::string& mediaName)
    {
        constexpr std::string_view cues = "/godot/cue/";
        std::vector<std::string> names;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (cues, 0) != 0)
                continue;

            const auto rest = std::string_view (node->address).substr (cues.size());
            const auto slash = rest.find ('/');

            if (slash == std::string_view::npos || rest.substr (slash + 1) != "file")
                continue;

            const auto file = text (node);

            if (file.empty() || ! sameFileName (file, mediaName))
                continue;

            const auto name = text (snapshot, std::string (cues) + std::string (rest.substr (0, slash)) + "/name");
            names.push_back (name.empty() ? std::string ("an unnamed cue") : name);
        }

        return names;
    }

    std::string orderIn (const tree::TreeSnapshot& snapshot, const std::string& container)
    {
        const auto inList = text (snapshot, "/godot/list/" + container + "/order");

        return inList.empty() ? text (snapshot, "/godot/cue/" + container + "/order") : inList;
    }

    //==========================================================================
    std::string notFoundWords (const std::string& name)
    {
        return name + " could not be found";
    }

    std::string unreadableWords (const std::string& name)
    {
        return name + " could not be read";
    }

    std::string copyFailedWords (const std::string& name, const std::string& why)
    {
        return name + " could not be copied into the show" + (why.empty() ? std::string {} : ": " + why);
    }

    std::string noFolderWords (const std::string& name)
    {
        return name + " has nowhere to go: the show has no folder yet";
    }

    std::string refusedWords (const std::string& mediaName)
    {
        return mediaName + " is in the show, but the cue for it was refused";
    }

    //==========================================================================
    void MediaImports::add (const std::string& parent, int index, const std::string& orderText,
                            const std::vector<std::string>& sources)
    {
        if (sources.empty())
            return;

        Batch batch;
        batch.serial = ++serials;
        batch.parent = parent;

        /*  THE NUMBER TURNED INTO A MEMBER: the one the hand let go after, or
            the top, or the end - which is what the drop meant, and what a long
            copy cannot move. */
        const auto members = words (orderText);
        const auto length = static_cast<int> (members.size());

        batch.atEnd = index < 0 || index >= length;
        batch.index = batch.atEnd ? length : index;

        if (! batch.atEnd && index > 0)
            batch.after = members[static_cast<std::size_t> (index - 1)];

        for (const auto& source : sources)
        {
            Arriving file;
            file.source = source;
            file.name = mediaNameFor (source);
            batch.files.push_back (std::move (file));
        }

        batches.push_back (std::move (batch));
    }

    void MediaImports::link (const std::string& cueId, const std::string& source)
    {
        Batch batch;
        batch.serial = ++serials;
        batch.linkTo = cueId;

        Arriving file;
        file.source = source;
        file.name = mediaNameFor (source);
        batch.files.push_back (std::move (file));

        batches.push_back (std::move (batch));
    }

    MediaImports::Batch* MediaImports::batchOf (int serial)
    {
        for (auto& batch : batches)
            if (batch.serial == serial)
                return &batch;

        return nullptr;
    }

    MediaImports::Arriving* MediaImports::fileAt (const Where& where)
    {
        auto* batch = batchOf (where.serial);

        return batch != nullptr && where.file < batch->files.size() ? &batch->files[where.file] : nullptr;
    }

    std::optional<MediaJob> MediaImports::nextJob()
    {
        if (handed.has_value())
            return std::nullopt;

        //  Every look before any copy (SI), anywhere in the queue.
        for (auto& batch : batches)
            for (std::size_t i = 0; i < batch.files.size(); ++i)
                if (auto& file = batch.files[i]; file.stage == Stage::waiting)
                {
                    file.stage = Stage::looking;
                    handed = Where { batch.serial, i };

                    MediaJob job;
                    job.source = file.source;
                    return job;
                }

        //  Then the copies, in the order picked.
        for (auto& batch : batches)
            for (std::size_t i = 0; i < batch.files.size(); ++i)
                if (auto& file = batch.files[i]; file.stage == Stage::toCopy)
                {
                    file.stage = Stage::copying;
                    handed = Where { batch.serial, i };

                    MediaJob job;
                    job.copy = true;
                    job.source = file.source;
                    job.answer = file.answer;
                    job.met = file.met;
                    return job;
                }

        return std::nullopt;
    }

    void MediaImports::fail (Arriving& file, std::string sentence)
    {
        file.stage = Stage::failed;
        file.said = std::move (sentence);
        unsaid.push_back (file.said);
    }

    void MediaImports::settle (Arriving& file, Clash given)
    {
        switch (given)
        {
            case Clash::useTheShows:
                file.stage = Stage::landed;
                file.mediaName = file.met;
                file.wrote = false;
                break;

            case Clash::replace:
            case Clash::keepBoth:
                file.stage = Stage::toCopy;
                file.answer = given;
                break;

            case Clash::ask:
                file.stage = Stage::asking;
                break;
        }
    }

    void MediaImports::worked (const MediaWork& work)
    {
        if (! handed.has_value())
            return;

        auto* batch = batchOf (handed->serial);
        auto* file = fileAt (*handed);
        handed.reset();

        if (batch == nullptr || file == nullptr)
            return;

        switch (work.found)
        {
            case Found::missing:    fail (*file, notFoundWords (file->name)); break;
            case Found::unreadable: fail (*file, unreadableWords (file->name)); break;
            case Found::noFolder:   fail (*file, noFolderWords (file->name)); break;
            case Found::failed:     fail (*file, copyFailedWords (file->name, work.why)); break;

            case Found::inShow:
            case Found::same:
                file->stage = Stage::landed;
                file->mediaName = work.name;
                file->wrote = false;
                break;

            case Found::copied:
                file->stage = Stage::landed;
                file->mediaName = work.name;
                file->wrote = true;
                break;

            case Found::free:
                file->stage = Stage::toCopy;
                file->answer = Clash::ask;
                break;

            /*  ANOTHER FILE OF ITS NAME: asked about, unless this import has
                already been answered for the rest of its files. */
            case Found::other:
                file->met = work.name;
                settle (*file, batch->standing);
                break;
        }
    }

    std::optional<MediaImports::Asked> MediaImports::asking() const
    {
        for (const auto& batch : batches)
            for (const auto& file : batch.files)
            {
                if (file.stage != Stage::asking)
                    continue;

                Asked asked;
                asked.picked = file.name;
                asked.met = file.met;

                for (const auto& another : batch.files)
                    if (&another != &file && (another.stage == Stage::waiting || another.stage == Stage::looking
                                               || another.stage == Stage::asking))
                        asked.more = true;

                return asked;
            }

        return std::nullopt;
    }

    void MediaImports::answer (Clash given, bool forTheRest)
    {
        for (auto& batch : batches)
            for (auto& file : batch.files)
            {
                if (file.stage != Stage::asking)
                    continue;

                settle (file, given);

                if (forTheRest && given != Clash::ask)
                {
                    batch.standing = given;

                    for (auto& another : batch.files)
                        if (another.stage == Stage::asking)
                            settle (another, given);
                }

                return;
            }
    }

    int MediaImports::indexFor (const Batch& batch, const tree::TreeSnapshot& snapshot)
    {
        const auto members = words (orderIn (snapshot, batch.parent));
        const auto length = static_cast<int> (members.size());

        if (batch.atEnd)
            return length;

        if (batch.after.empty())
            return 0;

        const auto found = std::find (members.begin(), members.end(), batch.after);

        /*  THE MEMBER IT FOLLOWED HAS GONE, deleted or moved out while the
            files were copying: the position the hand dropped at, as near as
            the container still has one. */
        if (found == members.end())
            return std::min (batch.index, length);

        return static_cast<int> (found - members.begin()) + 1;
    }

    MediaImports::Ended MediaImports::endOf (const Batch& batch)
    {
        Ended ended;
        std::size_t reused = 0;
        std::vector<const Arriving*> missed;

        for (const auto& file : batch.files)
        {
            if (file.stage == Stage::done)
            {
                ++ended.made;

                if (! file.wrote)
                    ++reused;
            }
            else
            {
                missed.push_back (&file);
            }
        }

        const auto total = batch.files.size();

        if (! batch.linkTo.empty())
        {
            ended.sentence = ended.made > 0 ? batch.files.front().mediaName + " is on the cue"
                                            : batch.files.front().said;
            return ended;
        }

        if (missed.empty() && total == 1)
        {
            const auto& only = batch.files.front();
            ended.sentence = "imported " + only.name;

            if (only.mediaName != only.name)
                ended.sentence += " as " + only.mediaName;

            if (reused > 0)
                ended.sentence += " (already in the show)";

            return ended;
        }

        if (missed.empty())
        {
            ended.sentence = "imported " + asText (total) + " files";

            if (reused == total)
                ended.sentence += " (all already in the show)";
            else if (reused > 0)
                ended.sentence += " (" + asText (reused) + " already in the show)";

            return ended;
        }

        /*  AND WHAT WENT WRONG IS SAID AGAIN at the end, where it stays: the
            sentence said when it happened was gone under the next file's
            progress before anybody could read it. */
        if (total == 1)
        {
            ended.sentence = missed.front()->said;
            return ended;
        }

        ended.sentence = "imported " + asText (static_cast<std::size_t> (ended.made)) + " of " + asText (total)
                       + " files - " + missed.front()->said;

        if (missed.size() > 1)
            ended.sentence += " (and " + asText (missed.size() - 1) + " more)";

        return ended;
    }

    MediaImports::Steps MediaImports::follow (const tree::TreeSnapshot& snapshot, std::uint64_t revision)
    {
        Steps steps;
        steps.said = std::move (unsaid);
        unsaid.clear();

        /*  THE CUE ASKED FOR: found and given its file, or given up on once
            the engine has had its chance (SM) - never sooner, however many
            passes this window has run meanwhile. */
        if (inFlight.has_value() && creating.has_value())
        {
            auto* batch = batchOf (creating->serial);
            auto* file = fileAt (*creating);

            const auto id = revision > inFlight->askedAt
                              ? createdAt (orderIn (snapshot, inFlight->parent), inFlight->index)
                              : std::string {};

            if (batch == nullptr || file == nullptr)
            {
                inFlight.reset();
                creating.reset();
            }
            else if (! id.empty()
                       && madeByImport (*inFlight,
                                        text (snapshot, "/godot/cue/" + id + "/kind"),
                                        text (snapshot, "/godot/cue/" + id + "/name"),
                                        text (snapshot, "/godot/cue/" + id + "/file")))
            {
                steps.namings.push_back ({ id, inFlight->mediaName });
                file->stage = Stage::done;
                batch->after = id;
                batch->atEnd = false;
                inFlight.reset();
                creating.reset();
            }
            else if (outOfPatience (*inFlight, snapshot.tick()))
            {
                file->stage = Stage::failed;
                file->said = refusedWords (inFlight->mediaName);
                steps.said.push_back (file->said);
                inFlight.reset();
                creating.reset();
            }
        }

        /*  THEN, IN THE ORDER PICKED, the next cue to ask for and every import
            that has finished. Only the first import's next file is looked at:
            a later file, or a later import, waits for it, which is what keeps
            the cues in the order the files were picked. */
        while (! inFlight.has_value() && ! batches.empty())
        {
            auto& batch = batches.front();
            const auto next = std::find_if (batch.files.begin(), batch.files.end(), [] (const Arriving& file)
                                            { return file.stage != Stage::done && file.stage != Stage::failed; });

            if (next == batch.files.end())
            {
                steps.ended.push_back (endOf (batch));
                batches.pop_front();
                continue;
            }

            if (next->stage != Stage::landed)
                break;

            if (! batch.linkTo.empty())
            {
                steps.namings.push_back ({ batch.linkTo, next->mediaName });
                next->stage = Stage::done;
                continue;
            }

            Import job;
            job.parent = batch.parent;
            job.index = indexFor (batch, snapshot);
            job.cueName = cueNameFor (next->mediaName);
            job.mediaName = next->mediaName;
            job.askedAt = revision;
            job.askedTick = snapshot.tick();

            next->stage = Stage::creating;
            creating = Where { batch.serial, static_cast<std::size_t> (next - batch.files.begin()) };
            inFlight = job;
            steps.create = job;
        }

        return steps;
    }

    std::string MediaImports::progress() const
    {
        if (! handed.has_value())
            return {};

        for (const auto& batch : batches)
        {
            if (batch.serial != handed->serial || handed->file >= batch.files.size())
                continue;

            const auto& file = batch.files[handed->file];

            if (file.stage != Stage::copying)
                return {};

            return "Copying " + asText (handed->file + 1) + " of " + asText (batch.files.size()) + ": " + file.name;
        }

        return {};
    }
}
