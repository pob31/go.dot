/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*  The media cue and its destinations, in the document and in the tree.

    Two author decisions of 2026-09-05 are asserted here rather than described
    somewhere nobody opens.

    ONE ELEMENT PER CUE KIND. A media cue is a <Media>, so `kind` stays derived
    from the element and read-only, and the grammar can refuse a `file`
    attribute on a cue that plays nothing. The alternative - every cue a <Cue>
    with a stored kind - would have made every cue carry every kind's
    attributes and left the grammar with nothing to refuse.

    A DESTINATION IS AN OBJECT. PRD §3.9b says a cue's destinations are a list
    rather than a choice, so <Route> repeats; it is identified rather than
    positional so that changing one destination's gains is a write to one node,
    and because an index is a position - deleting the first route would
    silently re-point a client holding the second.

    `Route/@gains` is also the first list-typed attribute in the tree, so the
    cases that cover it are covering `d*` at the same time: one value per
    element, a TYPE that grows to match, and nothing at all when it does not
    parse.

    A serialisation surface, so every case runs under fr_FR as well as C - a
    gains list is the worst possible thing to hand to a locale that writes a
    decimal comma, since it is numbers separated by spaces.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /*  A show with one list, built through the ordinary command path so that
        what is tested is what a client would get rather than a tree assembled
        by hand. */
    struct Rig
    {
        Rig()
        {
            doc::registerDocumentCommands (engine.commands(), document);

            listId = document.createList ("Main").id;
        }

        std::string createMedia (const std::string& name)
        {
            return document.createCue (listId, 0, "media", name).id;
        }

        std::shared_ptr<const tree::TreeSnapshot> publish()
        {
            tree::EngineState state;
            return parameters.publish (0, state);
        }

        Engine engine;
        doc::ShowDocument document;
        tree::TouchTable touches;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };

        std::string listId;
    };

    const tree::Node* nodeAt (const tree::TreeSnapshot& snapshot, const std::string& address)
    {
        return snapshot.find (address);
    }
}

//==============================================================================
TEST_CASE ("media cue: a media cue is its own element, and says so")
{
    Rig rig;

    const auto id = rig.createMedia ("Thunder");
    REQUIRE_FALSE (id.empty());

    /*  The element, not a stored word. This is the whole of what the author's
        decision buys: a client cannot turn a memo into a media cue by writing
        to `kind`, because there is nothing there to write. */
    const auto cue = rig.document.findById (id);
    REQUIRE (cue.isValid());
    CHECK (cue.getType().toString() == "Media");

    const auto snapshot = rig.publish();
    REQUIRE (snapshot != nullptr);

    const auto* kind = nodeAt (*snapshot, "/godot/cue/" + id + "/kind");
    REQUIRE (kind != nullptr);
    REQUIRE (kind->soleValue().has_value());
    CHECK (kind->soleValue()->getString() == "media");
    CHECK (kind->access == tree::Access::read);
}

TEST_CASE ("media cue: it is addressed as a cue, and carries a cue's rows as well as its own")
{
    /*  A media cue has a number, a name and a pre-wait like any other, and it
        lives at /godot/cue/<id> - so a client holding an identifier never has
        to know which kind it got. That is the same arrangement a Group has. */
    Rig rig;

    const auto id = rig.createMedia ("Thunder");
    const auto snapshot = rig.publish();
    const auto base = "/godot/cue/" + id;

    for (const auto* row : { "name", "number", "preWait", "postWait", "enabled" })
    {
        INFO ("the cue row " << row);
        CHECK (nodeAt (*snapshot, base + "/" + row) != nullptr);
    }

    for (const auto* row : { "file", "level", "startOffset" })
    {
        INFO ("the media row " << row);
        CHECK (nodeAt (*snapshot, base + "/" + row) != nullptr);
    }
}

TEST_CASE ("media cue: a memo cue carries no media rows at all")
{
    /*  The other half of the decision, and the half that would be impossible
        with a stored kind: a cue that plays nothing has no `file` to be wrong
        about, in the document or in the tree. */
    Rig rig;

    const auto id = rig.document.createCue (rig.listId, 0, "memo", "House to half").id;
    REQUIRE_FALSE (id.empty());

    const auto snapshot = rig.publish();
    const auto base = "/godot/cue/" + id;

    REQUIRE (nodeAt (*snapshot, base + "/name") != nullptr);

    for (const auto* row : { "file", "level", "startOffset" })
    {
        INFO ("the media row " << row << " must not be on a memo cue");
        CHECK (nodeAt (*snapshot, base + "/" + row) == nullptr);
    }
}

//==============================================================================
TEST_CASE ("route: a destination has an address of its own, not a position")
{
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    const auto first = rig.document.createRoute (cueId, "J3MT5XYA");
    const auto second = rig.document.createRoute (cueId, "K4NV6ZB1");

    REQUIRE (first.ok);
    REQUIRE (second.ok);
    CHECK (first.id != second.id);

    const auto snapshot = rig.publish();

    for (const auto& id : { first.id, second.id })
    {
        INFO ("route " << id);
        CHECK (nodeAt (*snapshot, "/godot/route/" + id + "/bus") != nullptr);
        CHECK (nodeAt (*snapshot, "/godot/route/" + id + "/gains") != nullptr);
    }

    /*  And it is NOT a nested cue. The tree's recursion takes any identified
        child, so without a case for it a route would have been published at
        /godot/cue/<route id> and carried a cue's whole row set. */
    CHECK (nodeAt (*snapshot, "/godot/cue/" + first.id + "/name") == nullptr);

    const auto* bus = nodeAt (*snapshot, "/godot/route/" + first.id + "/bus");
    REQUIRE (bus->soleValue().has_value());
    CHECK (bus->soleValue()->getString() == "J3MT5XYA");
}

TEST_CASE ("route: only a cue that plays something can have somewhere to play it")
{
    Rig rig;

    const auto memo = rig.document.createCue (rig.listId, 0, "memo", "House to half").id;

    /*  Refused here rather than left to the grammar, so the client is told
        which of its two identifiers was wrong and told it when it asked. */
    const auto onMemo = rig.document.createRoute (memo, "J3MT5XYA");
    CHECK_FALSE (onMemo.ok);
    CHECK (onMemo.reason == reason::typeMismatch);

    const auto onNothing = rig.document.createRoute ("NOSUCHID", "J3MT5XYA");
    CHECK_FALSE (onNothing.ok);
    CHECK (onNothing.reason == reason::unknownId);
}

TEST_CASE ("route: route.create is a command, like every other gesture")
{
    /*  PRD §4.11: every gesture-reachable action exists as a named command.
        Adding a destination is a gesture. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");

    REQUIRE (rig.engine.submit ("cli", "route.create",
                                { osc::Value::string (cueId),
                                  osc::Value::string ("J3MT5XYA") }));

    const auto outcome = rig.engine.processTick (0);
    CHECK (outcome.applied == 1);

    const auto cue = rig.document.findById (cueId);
    REQUIRE (cue.getNumChildren() == 1);
    CHECK (cue.getChild (0).getType().toString() == "Route");
}

//==============================================================================
TEST_CASE ("gains: a list publishes one value per element, and a type to match")
{
    /*  The first list-typed attribute in the tree. OSCQuery's TYPE is per node
        and its VALUE is an array, so four coefficients are "dddd" and four
        numbers - which is what an OSC client receives as four arguments, with
        no encoding of Go.dot's own to undo. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    const auto route = rig.document.createRoute (cueId, "J3MT5XYA");
    REQUIRE (route.ok);

    /*  The row says it is a list; everything below is what that means once it
        reaches a client. */
    const doc::AttributeRow* gainsRow = nullptr;

    for (const auto* row : doc::Schema::rowsForOwner ("route"))
        if (row->name == "gains")
            gainsRow = row;

    REQUIRE (gainsRow != nullptr);
    CHECK (gainsRow->isList);

    auto node = rig.document.findById (route.id);
    node.setProperty (juce::Identifier ("gains"), "1 0 0 0.5", nullptr);

    const auto snapshot = rig.publish();
    const auto* published = nodeAt (*snapshot, "/godot/route/" + route.id + "/gains");

    REQUIRE (published != nullptr);
    CHECK (published->typeTags == "dddd");
    REQUIRE (published->values.size() == 4u);

    CHECK (published->values[0] == osc::Value::float64 (1.0));
    CHECK (published->values[1] == osc::Value::float64 (0.0));
    CHECK (published->values[2] == osc::Value::float64 (0.0));
    CHECK (published->values[3] == osc::Value::float64 (0.5));

    /*  And it is not a single value wearing a list's clothes: asking for "the
        value" of four gains has no answer, and soleValue says so rather than
        handing back the first one. */
    CHECK_FALSE (published->soleValue().has_value());
}

TEST_CASE ("gains: an empty list is zero values, not one empty one")
{
    /*  A cue routed nowhere yet is an ordinary state for a show being written,
        and the grammar accepts it. The node exists and carries nothing. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    const auto route = rig.document.createRoute (cueId, "J3MT5XYA");

    const auto snapshot = rig.publish();
    const auto* published = nodeAt (*snapshot, "/godot/route/" + route.id + "/gains");

    REQUIRE (published != nullptr);
    CHECK (published->values.empty());
    CHECK (published->typeTags.empty());
}

TEST_CASE ("gains: a list that fails anywhere publishes nothing, not the part that parsed")
{
    /*  Half a routing matrix is not a smaller routing matrix, it is a different
        one. A client handed three of four gains has been told something untrue
        about where a cue goes, and it has no way to know. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    const auto route = rig.document.createRoute (cueId, "J3MT5XYA");

    auto node = rig.document.findById (route.id);
    node.setProperty (juce::Identifier ("gains"), "1 0 banana 0.5", nullptr);

    const auto snapshot = rig.publish();
    const auto* published = nodeAt (*snapshot, "/godot/route/" + route.id + "/gains");

    REQUIRE (published != nullptr);
    CHECK (published->values.empty());
}

//==============================================================================
TEST_CASE ("media cue: a show with media and routes round-trips byte for byte")
{
    /*  The locale case in disguise, and the reason this file runs twice. A
        gains list is numbers separated by spaces, which is the worst thing to
        hand to a locale that writes a decimal comma: one `1,5` in the middle of
        `1 0 0 1.5` and the list silently becomes a different length. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    auto cue = rig.document.findById (cueId);
    cue.setProperty (juce::Identifier ("file"), "thunder.wav", nullptr);
    cue.setProperty (juce::Identifier ("level"), -3.5, nullptr);
    cue.setProperty (juce::Identifier ("startOffset"), 0.25, nullptr);

    const auto route = rig.document.createRoute (cueId, "J3MT5XYA");
    auto node = rig.document.findById (route.id);
    node.setProperty (juce::Identifier ("gains"), "1 0 0 1.5", nullptr);

    const auto written = doc::CanonicalXml::write (rig.document);

    INFO (written);
    CHECK (written.find ("<Media") != std::string::npos);
    CHECK (written.find ("<Route") != std::string::npos);
    CHECK (written.find ("gains=\"1 0 0 1.5\"") != std::string::npos);

    doc::ShowDocument reloaded;
    const auto result = doc::CanonicalXml::read (written, reloaded);

    for (const auto& problem : result.problems)
        INFO (problem);
    REQUIRE (result.ok);

    CHECK (doc::CanonicalXml::write (reloaded) == written);
}

//==============================================================================
/*  Raw string literals, so the XML below is the XML - no escaped quotes and no
    line continuations between a reader and what it is reading. */
namespace
{
    doc::ReadResult readShow (const std::string& text, doc::ShowDocument& into)
    {
        return doc::CanonicalXml::read (text, into);
    }
}

TEST_CASE ("media cue: the reader refuses media attributes on a cue that plays nothing")
{
    /*  The negative half of "one element per kind", and the thing a stored kind
        could never have given us. `file` is a media row, a <Cue> carries only
        cue rows, so a memo cue with a file is not a cue with a harmless extra -
        it is a document that does not describe a show.

        The generated RELAX NG refuses it too; the positive case is committed as
        tests/fixtures/documents/media-cue.xml and every build runs lxml over
        it, which is the outside opinion. This is our own reader giving the same
        answer, and it is the half that produces a message somebody can act on. */
    const std::string memoWithMedia = R"(<Show>
  <Lists>
    <List id="7WBV41P3" name="Sound">
      <Cue id="D9FH2JKA" file="thunder.wav" name="House to half"/>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="0"/>
</Show>
)";

    doc::ShowDocument document;
    const auto result = readShow (memoWithMedia, document);

    for (const auto& problem : result.problems)
        INFO (problem);

    CHECK_FALSE (result.ok);
    REQUIRE_FALSE (result.problems.empty());
    CHECK (result.problems.front().find ("file") != std::string::npos);
}

TEST_CASE ("media cue: a gains list is canonicalised, so one show is one file")
{
    /*  `1.50` and `1.5` are the same coefficient, and a document that could
        spell them two ways would round-trip to different bytes depending on who
        typed it. Every element goes through the same formatter a scalar goes
        through, and the run comes back single-spaced. */
    const std::string spelt = R"(<Show>
  <Lists>
    <List id="7WBV41P3" name="Sound">
      <Media id="JMS7SB5T" name="Thunder">
        <Route id="Z04EH7PH" bus="J3MT5XYA" gains="1.50   0    0.250"/>
      </Media>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="0"/>
</Show>
)";

    doc::ShowDocument document;
    const auto result = readShow (spelt, document);

    for (const auto& problem : result.problems)
        INFO (problem);

    REQUIRE (result.ok);

    const auto written = doc::CanonicalXml::write (document);

    INFO (written);
    CHECK (written.find (R"(gains="1.5 0 0.25")") != std::string::npos);
}

TEST_CASE ("media cue: one bad coefficient refuses the whole document, and says which")
{
    const std::string bad = R"(<Show>
  <Lists>
    <List id="7WBV41P3" name="Sound">
      <Media id="JMS7SB5T" name="Thunder">
        <Route id="Z04EH7PH" bus="J3MT5XYA" gains="1 0 banana 1"/>
      </Media>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="0"/>
</Show>
)";

    doc::ShowDocument document;
    const auto result = readShow (bad, document);

    CHECK_FALSE (result.ok);
    REQUIRE_FALSE (result.problems.empty());

    /*  And it says WHICH one. A routing matrix is long, and "gains is wrong" is
        not something anybody can act on at 2 a.m. */
    INFO (result.problems.front());
    CHECK (result.problems.front().find ("element 2") != std::string::npos);
}

//==============================================================================
/*  THE SHOW'S MEDIA, ONE RECORD PER FILE - PR 5.6, namespace draft §14.12.

    Two halves that must not behave alike. The DURATIONS are read once, when
    the show is opened, and handed by address to a cache that asks on every
    publish whether that address is the one it last built with. The HASH and
    the PYRAMID arrive later, from a thread of their own, through a snapshot
    swapped whole. The first cases are the durations half's law; the one after
    them is that law pinned where it bites - the slot analysis's rebuild count -
    so that a later pull request which returns the map by value, or reaches it
    through a swapped pointer, is told so by a red build rather than by a tick
    thread that has quietly started rebuilding the slot walk fifty times a
    second; and the last is the snapshot's.

    Media files are written here as SILENCE: every question these cases ask
    is about a length, and a length is a header.
*/
namespace
{
    /*  A folder of its own per case, removed afterwards, so that two cases -
        or two locale passes of one - never read each other's files. */
    struct ScratchFolder
    {
        ScratchFolder()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-media-test-" + juce::Uuid().toDashedString()))
        {
        }

        ~ScratchFolder() { folder.deleteRecursively(); }

        ScratchFolder (const ScratchFolder&) = delete;
        ScratchFolder& operator= (const ScratchFolder&) = delete;

        juce::File folder;
    };

    /*  `frames` of mono silence at `rate`, so its length is `frames / rate`
        seconds exactly - rates and frame counts below are chosen to make that
        a number a double holds without rounding. */
    bool writeSilence (const juce::File& file, int rate, int frames)
    {
        if (file.getParentDirectory().createDirectory().failed())
            return false;

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };

        if (stream == nullptr)
            return false;

        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (rate))
                                                .withNumChannels (1)
                                                .withBitsPerSample (16));

        if (writer == nullptr)
            return false;

        juce::AudioBuffer<float> buffer { 1, frames };
        buffer.clear();

        return writer->writeFromAudioSampleBuffer (buffer, 0, frames);
    }

    /*  BYTE FOR BYTE, and meant literally: two doubles that print alike and
        differ in the last bit are two different durations, and a comparison
        with a tolerance would call them one. */
    bool sameBytes (const std::map<std::string, double>& left,
                    const std::map<std::string, double>& right)
    {
        return left.size() == right.size()
            && std::equal (left.begin(), left.end(), right.begin(),
                           [] (const auto& one, const auto& other)
                           {
                               return one.first == other.first
                                   && std::memcmp (&one.second, &other.second, sizeof (double)) == 0;
                           });
    }

    /*  A double's bit pattern, as an integer: comparable exactly without a
        floating-point equality, and printable without a locale. */
    std::uint64_t bitsOf (double value)
    {
        std::uint64_t bits = 0;
        static_assert (sizeof (bits) == sizeof (value));
        std::memcpy (&bits, &value, sizeof (bits));
        return bits;
    }

    /*  EVERY SHAPE A SHOW'S `file` CAN TAKE, in one bundle: a file two cues
        share, a path with a folder in it and reached only through a group, a
        file named and missing, a file present and not audio, and a media cue
        that names nothing yet. */
    struct MediaShow
    {
        MediaShow()
        {
            const auto list = document.createList ("Sound");
            built = list.ok;

            built = built && writeSilence (media.getChildFile ("thunder.wav"), 8000, 12000);
            built = built && writeSilence (media.getChildFile ("sub").getChildFile ("rain.wav"),
                                           8000, 4000);
            built = built && media.getChildFile ("notes.txt").replaceWithText ("this is not a sound");

            add (list.id, 0, "Thunder", "thunder.wav");
            add (list.id, 1, "Thunder again", "thunder.wav");     // two cues, one file, one read
            add (list.id, 2, "Gone", "absent.wav");               // named, and not there
            add (list.id, 3, "Notes", "notes.txt");               // there, and not a sound
            add (list.id, 4, "Unset", nullptr);                   // a media cue naming nothing yet

            built = built && document.createCue (list.id, 5, "memo", "House to half").ok;

            const auto scene = document.createCue (list.id, 6, "group", "Scene");
            built = built && scene.ok;
            add (scene.id, 0, "Rain", "sub/rain.wav");            // only inside a group
        }

        void add (const std::string& parent, int index, const char* cueName, const char* file)
        {
            const auto made = document.createCue (parent, index, "media", cueName);
            built = built && made.ok;

            if (file != nullptr)
                built = built && document.setAttribute ("/godot/cue/" + made.id + "/file", file).ok;
        }

        std::string mediaFolder() const { return media.getFullPathName().toStdString(); }

        ScratchFolder scratch;
        juce::File media { scratch.folder.getChildFile ("media") };
        doc::ShowDocument document;
        bool built = false;
    };
}

//==============================================================================
TEST_CASE ("media info: the durations are the ones mediaDurations read, byte for byte")
{
    /*  PR 5.6 moved the map into an object, and that must move no number any
        consumer reads: the tree's `duration` rows, the solver, the slot walk
        and the log's `media` lines all read this map, and a replay of a log
        written before the move compares against those lines. So the object is
        built ON `mediaDurations` rather than beside it, and this compares the
        two - and then pins what they both say, so that agreeing with each other
        is not all they are asked to do. */
    MediaShow show;
    REQUIRE (show.built);

    const audio::MediaInfo info { show.document, show.mediaFolder() };

    CHECK (sameBytes (info.durations(), audio::mediaDurations (show.document, show.mediaFolder())));

    REQUIRE (info.durations().size() == 4u);
    CHECK (info.durations().at ("thunder.wav") == doctest::Approx (1.5));
    CHECK (info.durations().at ("sub/rain.wav") == doctest::Approx (0.5));
    CHECK (info.durations().at ("absent.wav") == doctest::Approx (0.0));
    CHECK (info.durations().at ("notes.txt") == doctest::Approx (0.0));
    CHECK (info.durations().count ("") == 0u);

    /*  NO MEDIA FOLDER - `wfg tree` on a show that is not a bundle - takes a
        `file` as given. An absolute path, because a relative one would be read
        against whatever directory the suite happened to start in. */
    doc::ShowDocument loose;
    const auto looseList = loose.createList ("Loose");
    REQUIRE (looseList.ok);
    const auto looseCue = loose.createCue (looseList.id, 0, "media", "Thunder");
    REQUIRE (looseCue.ok);

    const auto absolute = show.media.getChildFile ("thunder.wav").getFullPathName().toStdString();
    REQUIRE (loose.setAttribute ("/godot/cue/" + looseCue.id + "/file", absolute).ok);

    const audio::MediaInfo unbundled { loose, std::string() };

    CHECK (sameBytes (unbundled.durations(), audio::mediaDurations (loose, std::string())));
    REQUIRE (unbundled.durations().size() == 1u);
    CHECK (unbundled.durations().at (absolute) == doctest::Approx (1.5));

    /*  And a show that names no media at all is an empty table and an empty
        snapshot, not a failure. */
    doc::ShowDocument silent;
    REQUIRE (silent.createList ("Nothing").ok);

    audio::MediaInfo nothing { silent, show.mediaFolder() };
    CHECK (nothing.durations().empty());
    CHECK (nothing.snapshot()->empty());

    /*  A file imported into that show afterwards still gets a record - the
        analyser is queued for it - and the frozen table still names nothing. */
    audio::MediaRecord imported;
    imported.seconds = 2.5;
    nothing.publish ("thunder.wav", imported);

    CHECK (nothing.snapshot()->count ("thunder.wav") == 1u);
    CHECK (nothing.durations().empty());
}

TEST_CASE ("media info: the files a show names, in the order it names them, each once")
{
    /*  PR 5.7's analyser works through this list front to back, so cue 1's
        sound has its colours before cue 90's - and the durations walk is
        built on it, so the two cannot disagree about which files a show has.
        A file two cues share is one entry, at the first cue; a media cue that
        names nothing is no entry; a file inside a group is where the group
        puts it. */
    MediaShow show;
    REQUIRE (show.built);

    CHECK (audio::mediaFilesNamedBy (show.document)
           == std::vector<std::string> { "thunder.wav", "absent.wav", "notes.txt", "sub/rain.wav" });

    /*  And a `file` is resolved in one place: under the media folder, or as
        given when there is none. */
    CHECK (audio::resolveMediaPath (show.mediaFolder(), "sub/rain.wav")
           == show.media.getChildFile ("sub").getChildFile ("rain.wav").getFullPathName().toStdString());
    CHECK (audio::resolveMediaPath (std::string(), "/somewhere/rain.wav") == "/somewhere/rain.wav");
}

TEST_CASE ("media info: durations is one address for the object's whole life, and no publish writes to it")
{
    /*  The durations half is FROZEN (§14.12). A consumer holds its address, so
        the address must not move - which is why the object can be neither
        copied nor moved, asserted here at compile time - and a cache compares
        that address, so the numbers under it must not change either: a
        publish is the one thing in this class that writes, and it writes only
        the other half. */
    static_assert (! std::is_copy_constructible_v<audio::MediaInfo>);
    static_assert (! std::is_move_constructible_v<audio::MediaInfo>);
    static_assert (! std::is_copy_assignable_v<audio::MediaInfo>);
    static_assert (! std::is_move_assignable_v<audio::MediaInfo>);

    MediaShow show;
    REQUIRE (show.built);

    audio::MediaInfo info { show.document, show.mediaFolder() };

    const auto* const address = &info.durations();
    const auto asLoaded = info.durations();

    /*  THE FIRST SNAPSHOT is every file, with its seconds and nothing else. */
    const auto unanalysed = info.snapshot();
    REQUIRE (unanalysed != nullptr);
    REQUIRE (unanalysed->size() == asLoaded.size());

    for (const auto& [file, record] : *unanalysed)
    {
        REQUIRE (asLoaded.count (file) == 1u);
        CHECK (bitsOf (record.seconds) == bitsOf (asLoaded.at (file)));
        CHECK (record.contentHash.empty());
        CHECK (record.pyramid.get() == nullptr);
    }

    /*  A PUBLISHER THAT DISAGREES about how long a file is, every time. */
    constexpr int passes = 32;

    for (int pass = 0; pass < passes; ++pass)
    {
        for (const auto& [file, seconds] : asLoaded)
        {
            audio::MediaRecord analysed;
            analysed.seconds = seconds + 100.0;
            analysed.contentHash = file + "#" + std::to_string (pass);

            info.publish (file, analysed);
            CHECK (&info.durations() == address);
        }
    }

    CHECK (sameBytes (info.durations(), asLoaded));

    /*  And the snapshot carries the FROZEN seconds, not the publisher's: one
        file has one length, whichever half is asked. */
    const auto analysedRecords = info.snapshot();
    REQUIRE (analysedRecords->size() == asLoaded.size());

    for (const auto& [file, record] : *analysedRecords)
    {
        CHECK (bitsOf (record.seconds) == bitsOf (asLoaded.at (file)));
        CHECK (record.contentHash == file + "#" + std::to_string (passes - 1));
    }

    /*  A snapshot taken before all of that is unchanged by it. */
    for (const auto& [file, record] : *unanalysed)
    {
        INFO (file);
        CHECK (record.contentHash.empty());
    }

    /*  A FILE IMPORTED AFTER THE OPEN reaches the snapshot and NEVER the
        durations. It has no frozen length, so its record keeps the seconds its
        publisher read; and the table the slot analysis caches by address does
        not grow by one entry, or by one byte, for it - which is the law. */
    audio::MediaRecord imported;
    imported.seconds = 7.25;
    imported.contentHash = "a file this show did not name at open";

    info.publish ("imported.wav", imported);

    const auto afterImport = info.snapshot();
    REQUIRE (afterImport->count ("imported.wav") == 1u);
    CHECK (bitsOf (afterImport->at ("imported.wav").seconds) == bitsOf (7.25));
    CHECK (info.durations().count ("imported.wav") == 0u);
    CHECK (&info.durations() == address);
    CHECK (sameBytes (info.durations(), asLoaded));
}

TEST_CASE ("media info: a late publish does not rebuild the slot analysis, which reads the durations by address")
{
    /*  THE LAW, PINNED WHERE IT BITES. `ParameterTree::publish` asks the slot
        analysis first thing, every publish, and the analysis skips its rebuild
        only when the document's revision AND the durations map's ADDRESS are
        the ones it last built with (`SlotAnalysis::ensureBuilt`). A
        `durations()` that returned by value, or a map reached through a
        pointer a publish swapped, would fail that test on every tick: about 77
        ms of a Debug build's slot walk, fifty times a second, on the tick
        thread (§14.12). What is asserted is a count, for M18's reason - a count
        of zero is exact where a wall clock on a shared runner is not. */
    Rig rig;
    ScratchFolder scratch;

    const auto media = scratch.folder.getChildFile ("media");
    REQUIRE (writeSilence (media.getChildFile ("thunder.wav"), 8000, 12000));

    const auto cueId = rig.createMedia ("Thunder");
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + cueId + "/file", "thunder.wav").ok);

    audio::MediaInfo info { rig.document, media.getFullPathName().toStdString() };
    rig.parameters.setMediaDurations (&info.durations());

    rig.publish();

    const auto baseline = rig.parameters.analysisRebuilds();
    REQUIRE (baseline > 0u);

    /*  Nothing changed: nothing rebuilt. */
    rig.publish();
    CHECK (rig.parameters.analysisRebuilds() == baseline);

    /*  THE LATE HALF ARRIVES - what PR 5.7's analyser will do from its own
        thread - and the tree publishes again, several times, as the tick
        thread would. */
    audio::MediaRecord analysed;
    analysed.contentHash = "0123456789abcdef";
    info.publish ("thunder.wav", analysed);
    REQUIRE (info.snapshot()->at ("thunder.wav").contentHash == "0123456789abcdef");

    std::shared_ptr<const tree::TreeSnapshot> afterwards;

    for (int n = 0; n < 5; ++n)
        afterwards = rig.publish();

    CHECK (rig.parameters.analysisRebuilds() == baseline);

    /*  And the tree still reads the length through the same address. */
    REQUIRE (afterwards != nullptr);
    const auto* duration = nodeAt (*afterwards, "/godot/cue/" + cueId + "/duration");
    REQUIRE (duration != nullptr);
    const auto value = duration->soleValue();
    REQUIRE (value.has_value());
    CHECK (value->asDouble() == doctest::Approx (1.5));

    /*  THE COUNT COULD HAVE MOVED, which is what makes the two above worth
        reading: the same numbers at ANOTHER address are, to the cache, another
        show's media, and it rebuilds - once, and not again. This is exactly
        what a by-value accessor would have done on every publish. */
    const auto elsewhere = info.durations();
    rig.parameters.setMediaDurations (&elsewhere);

    rig.publish();
    rig.publish();
    CHECK (rig.parameters.analysisRebuilds() == baseline + 1u);

    /*  DETACHED BEFORE THE SCOPE ENDS. `info` and `elsewhere` are built from
        the rig's own document, so they cannot be declared before it, and
        without this the tree would hold a pointer into maps destroyed first.
        Nothing reads it on the way out today; a later destructor that walked
        the show would, and this line is cheaper than finding out. */
    rig.parameters.setMediaDurations (nullptr);
}

//==============================================================================
namespace
{
    /*  Which pass of the publisher below wrote this hash: nought for a record
        not yet analysed, and -1 for anything that is not one of its hashes. */
    int passOf (const std::string& hash)
    {
        if (hash.empty())
            return 0;

        int pass = 0;
        std::size_t at = 0;

        for (; at < hash.size() && hash[at] >= '0' && hash[at] <= '9'; ++at)
            pass = pass * 10 + (hash[at] - '0');

        return at > 0 && at < hash.size() && hash[at] == ' ' ? pass : -1;
    }
}

TEST_CASE ("media info: a record published on one thread is seen whole on another")
{
    /*  PR 5.7's analyser publishes from its own thread and the tick thread
        reads - §14.5's timbre lookup, fifty times a second. What the reader
        must never see is HALF a publish: a record whose fields disagree with
        each other, a map with some other generation's record in it, or a
        snapshot that changes after it was handed over.

        So every record the publisher writes is derived from itself - its hash
        spells the pass, the file and the bit pattern of the file's length - and
        the publisher walks the files in one fixed order, pass after pass. A
        whole snapshot is then a PREFIX of that walk: along the files, the pass
        never rises, and never falls by more than one. A torn one is not. */
    ScratchFolder scratch;
    const auto media = scratch.folder.getChildFile ("media");

    doc::ShowDocument document;
    const auto list = document.createList ("Takes");
    REQUIRE (list.ok);

    constexpr int takes = 4;

    for (int n = 0; n < takes; ++n)
    {
        const auto file = "take" + std::to_string (n) + ".wav";
        REQUIRE (writeSilence (media.getChildFile (juce::String (file)), 8000, 2000 * (n + 1)));

        const auto made = document.createCue (list.id, n, "media", "Take " + std::to_string (n));
        REQUIRE (made.ok);
        REQUIRE (document.setAttribute ("/godot/cue/" + made.id + "/file", file).ok);
    }

    audio::MediaInfo info { document, media.getFullPathName().toStdString() };
    REQUIRE (info.durations().size() == static_cast<std::size_t> (takes));

    /*  The walk's fixed order is the map's. */
    std::vector<std::string> files;

    for (const auto& entry : info.durations())
        files.push_back (entry.first);

    const auto hashFor = [&info] (int generation, const std::string& named)
    {
        return std::to_string (generation) + " " + named + " "
             + std::to_string (bitsOf (info.durations().at (named)));
    };

    constexpr int passes = 400;

    std::atomic<bool> finished { false };
    std::atomic<bool> readerStarted { false };

    /*  NOTHING IN THIS THREAD ASSERTS, and nothing between its start and its
        join may REQUIRE: a REQUIRE throws, and a std::thread destroyed
        unjoined terminates the suite rather than failing a case. What goes
        wrong is counted, and checked after the join.

        AND IT DOES NOT START UNTIL THE READER HAS, which is what makes this a
        test of two threads rather than of one. Without the handshake a main
        thread descheduled just after the start could let every publish land
        during its first look, see one finished map, and fail `looks > 1` on a
        correct build - the shape of every flake this project has paid for. */
    std::thread analyser ([&info, &files, &hashFor, &finished, &readerStarted]
    {
        while (! readerStarted.load())
            std::this_thread::yield();

        for (int generation = 1; generation <= passes; ++generation)
        {
            for (const auto& target : files)
            {
                audio::MediaRecord analysed;
                analysed.seconds = -1.0;        // wrong on purpose: publish must replace it
                analysed.contentHash = hashFor (generation, target);

                info.publish (target, analysed);
            }
        }

        finished.store (true);
    });

    struct Held
    {
        std::shared_ptr<const audio::MediaRecords> records;
        std::vector<std::string> hashesWhenTaken;
    };

    std::vector<Held> held;
    std::size_t looks = 0;
    int torn = 0;
    int unordered = 0;
    int regressed = 0;
    int latestSeen = 0;

    /*  At least two looks whatever the scheduler does: the first releases the
        publisher, and the loop does not end before a second, so `looks > 1`
        below is a claim about the code and never about the runner. */
    while (! finished.load() || looks < 2)
    {
        const auto seen = info.snapshot();
        ++looks;

        if (looks == 1)
            readerStarted.store (true);

        if (seen == nullptr || seen->size() != files.size())
        {
            ++torn;
            continue;
        }

        int previous = passes + 1;
        int newest = -1;
        int oldest = -1;

        for (const auto& named : files)
        {
            const auto found = seen->find (named);

            if (found == seen->end())
            {
                ++torn;
                continue;
            }

            const auto& record = found->second;
            const auto generation = passOf (record.contentHash);

            if (generation < 0
                  || bitsOf (record.seconds) != bitsOf (info.durations().at (named))
                  || (generation > 0 && record.contentHash != hashFor (generation, named)))
                ++torn;

            if (generation > previous)
                ++unordered;

            if (newest < 0)
                newest = generation;

            previous = generation;
            oldest = generation;
        }

        if (newest - oldest > 1)
            ++unordered;

        /*  And time does not run backwards between two looks. */
        if (newest < latestSeen)
            ++regressed;

        latestSeen = std::max (latestSeen, newest);

        if (looks % 64 == 1 && held.size() < 256)
        {
            Held kept;
            kept.records = seen;

            /*  `find` and not `at`: nothing in this loop may throw while the
                publisher is still running, for the reason above. */
            for (const auto& named : files)
            {
                const auto found = seen->find (named);
                kept.hashesWhenTaken.push_back (found != seen->end() ? found->second.contentHash
                                                                     : std::string());
            }

            held.push_back (std::move (kept));
        }
    }

    analyser.join();

    CHECK (torn == 0);
    CHECK (unordered == 0);
    CHECK (regressed == 0);
    CHECK (looks > 1u);

    /*  EVERY SNAPSHOT HELD while the publisher ran is what it was when it was
        taken: a publish builds a new map and never edits one a reader has. */
    REQUIRE_FALSE (held.empty());

    for (const auto& kept : held)
        for (std::size_t n = 0; n < files.size(); ++n)
            CHECK (kept.records->at (files[n]).contentHash == kept.hashesWhenTaken[n]);

    /*  And the last publish is seen, whole. */
    const auto last = info.snapshot();

    for (const auto& named : files)
    {
        CHECK (last->at (named).contentHash == hashFor (passes, named));
        CHECK (bitsOf (last->at (named).seconds) == bitsOf (info.durations().at (named)));
    }
}

//==============================================================================
namespace
{
    /*  A runner with NO AUDIO SIDE, which is what `wfg replay` is and what
        `wfg serve` without `--hosted` is. Built through the ordinary command
        path, the way GoTests' rig is, so what is asserted is what a replayed
        `cue.fire` does. */
    struct ReplayRig
    {
        ReplayRig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setPlayer (nullptr);
            runner.setSamplesPerTick (960);          // 48 kHz at 50 Hz

            listId = document.createList ("Sound").id;
            mediaId = document.createCue (listId, 0, "media", "Thunder").id;
            memoId = document.createCue (listId, 1, "memo", "House to half").id;

            named = document.setAttribute ("/godot/cue/" + mediaId + "/file",
                                           "storm/thunder.wav").ok;
        }

        /** Fires one cue, and ticks once with the Runner observing first. */
        void fire (const std::string& cueId)
        {
            REQUIRE (engine.submit ("cli", "cue.fire", { osc::Value::string (cueId) }));
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
        }

        const cue::Run* runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return &run;

            return nullptr;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        cue::Runner runner { document, runs, runIds, focus };

        std::string listId, mediaId, memoId;
        std::int64_t tick = 0;
        bool named = false;
    };
}

TEST_CASE ("run media: a run knows what it plays with no audio side at all")
{
    /*  §14.5. `Run::media` is copied in `armMedia` ABOVE the null-player
        return, beside the slot claim, because which file a cue names is a
        fact about the document and not about the audio side. Copied below
        that return it would be empty on every replay - and the case it exists
        for, a run still sounding after an undo took its cue away, is one a
        replay must reproduce. */
    ReplayRig rig;
    REQUIRE (rig.named);

    rig.fire (rig.mediaId);

    const auto* played = rig.runOf (rig.mediaId);
    REQUIRE (played != nullptr);
    CHECK (played->track == -1);                    // no voice: there is no audio
    CHECK (played->media == "storm/thunder.wav");   // and still the file, as the document writes it

    /*  THE RUN'S OWN COPY. Editing the cue afterwards changes the next run and
        not this one - §4.10's rule for everything a run instantiates. */
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/file", "rain.wav").ok);
    rig.runner.beforeTick (rig.engine, rig.tick);
    rig.engine.processTick (rig.tick++);

    played = rig.runOf (rig.mediaId);
    REQUIRE (played != nullptr);
    CHECK (played->media == "storm/thunder.wav");

    /*  And a cue that plays nothing has nothing to say. */
    rig.fire (rig.memoId);

    const auto* memo = rig.runOf (rig.memoId);
    REQUIRE (memo != nullptr);
    CHECK (memo->media.empty());
}

TEST_CASE ("run media: a run armed twice with no audio side keeps the file of its first arm")
{
    /*  A PRE-WAIT ARMS TWICE when there is no audio side: once when the cue is
        entered, and again when the wait elapses and the fire path finds no
        track. With an audio side the first arm reserves a track and the second
        returns at once, so a hosted run keeps the file it was armed with. The
        copy is taken only while the run has none, so this configuration agrees
        with that one - and a file edited during the wait is the NEXT run's.

        THE SECOND ARM IS PROVED TO HAVE HAPPENED, not assumed: the run is seen
        in `waiting` and then seen to leave it, which is the path that arms
        again. Without that, this case would pass whether or not the copy was
        guarded, because a run that was never armed twice never had the chance
        to take the wrong file. */
    ReplayRig rig;
    REQUIRE (rig.named);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/preWait", "0.1").ok);

    rig.fire (rig.mediaId);

    const auto* waiting = rig.runOf (rig.mediaId);
    REQUIRE (waiting != nullptr);
    REQUIRE (waiting->state == std::string (cue::runState::waiting));
    CHECK (waiting->media == "storm/thunder.wav");

    /*  Edited while the run waits. */
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/file", "rain.wav").ok);

    bool left = false;

    for (int n = 0; n < 50 && ! left; ++n)
    {
        rig.runner.beforeTick (rig.engine, rig.tick);
        rig.engine.processTick (rig.tick++);

        const auto* now = rig.runOf (rig.mediaId);
        left = now != nullptr && now->state != std::string (cue::runState::waiting);
    }

    REQUIRE (left);

    const auto* fired = rig.runOf (rig.mediaId);
    REQUIRE (fired != nullptr);
    CHECK (fired->media == "storm/thunder.wav");
}
