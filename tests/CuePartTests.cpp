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

/*
    Part of a cue, copied and pasted (namespace draft §38): the fragment holds
    that part and nothing else; a paste REPLACES the part whole on every cue
    named (WO) - rows back to their defaults, sends matched by bus with their
    lanes kept (WV), the effects chain swapped in order, the playlist replaced;
    every refusal is asked before the first write; and the whole paste is one
    step of undo whose record a replay follows to the same names.

    A serialisation surface, so every case here runs under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/CueParts.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>

#include <juce_data_structures/juce_data_structures.h>

#include <string>
#include <vector>

using namespace wfg;

namespace
{
    /*  Three sounds set three ways, a mic, and a movie with its sound locked
        to it. A: EQ, a send into the reverb, a chain of two, a Range. B: other
        EQ, a send into the reverb with a lane and one into the delay, the
        verb alone. C: nothing at all. Hand-written, as every fixture of the
        document's is. */
    const char* const show =
        "<Show>\n"
        "  <Lists>\n"
        "    <List id=\"CP000001\" name=\"Main\">\n"
        "      <Media eqB1Gain=\"6\" eqHpf=\"true\" file=\"a.wav\" id=\"CP000002\" name=\"A\">\n"
        "        <Send bus=\"CP000090\" id=\"CP000010\" level=\"-6\"/>\n"
        "        <Fx id=\"CP000011\" plugin=\"CP000080\" values=\"0:0.25\"/>\n"
        "        <Fx enabled=\"false\" id=\"CP000012\" plugin=\"CP000081\"/>\n"
        "        <Range id=\"CP000013\" in=\"1\" loops=\"2\" name=\"Verse\" out=\"4\"/>\n"
        "      </Media>\n"
        "      <Media eqB2Gain=\"-3\" file=\"b.wav\" id=\"CP000003\" name=\"B\">\n"
        "        <Send bus=\"CP000090\" id=\"CP000020\" levelLane=\"0 0 4 -12\" on=\"false\"/>\n"
        "        <Send bus=\"CP000091\" id=\"CP000021\" level=\"-10\"/>\n"
        "        <Fx id=\"CP000022\" plugin=\"CP000081\"/>\n"
        "      </Media>\n"
        "      <Media file=\"c.wav\" id=\"CP000004\" name=\"C\"/>\n"
        "      <Mic channel=\"CP000031\" id=\"CP000005\" input=\"CP000030\" name=\"Voice\"/>\n"
        "      <Video file=\"m.mov\" id=\"CP000006\" name=\"Movie\" source=\"movie\"/>\n"
        "      <Media file=\"m.wav\" id=\"CP000007\" lockedTo=\"CP000006\" name=\"Movie sound\"/>\n"
        "    </List>\n"
        "  </Lists>\n"
        "  <Mounts/>\n"
        "  <Audio tracks=\"4\">\n"
        "    <Bus id=\"CP000090\" kind=\"mix\" name=\"Reverb\" width=\"2\"/>\n"
        "    <Bus id=\"CP000091\" kind=\"mix\" name=\"Delay\" width=\"2\"/>\n"
        "    <Inputs>\n"
        "      <Input id=\"CP000030\" name=\"Voix\"/>\n"
        "    </Inputs>\n"
        "    <Plugins>\n"
        "      <Plugin format=\"VST3\" id=\"CP000080\" identifier=\"VST3-gain\" name=\"Gain\" path=\"C:/gain.vst3\"/>\n"
        "      <Plugin format=\"VST3\" id=\"CP000081\" identifier=\"VST3-verb\" name=\"Verb\" path=\"C:/verb.vst3\"/>\n"
        "    </Plugins>\n"
        "    <Rack>\n"
        "      <Channel class=\"monoToStereo\" id=\"CP000031\" name=\"Vox 1\"/>\n"
        "    </Rack>\n"
        "  </Audio>\n"
        "</Show>\n";

    void open (doc::ShowDocument& document)
    {
        const auto result = doc::CanonicalXml::read (show, document);

        for (const auto& problem : result.problems)
            INFO (problem);

        REQUIRE (result.ok);
    }

    std::string at (const doc::ShowDocument& document, const std::string& address)
    {
        return document.getAttribute (address).value_or ("<none>");
    }

    /*  The children of one kind under a cue, as identifiers in document order. */
    std::vector<juce::ValueTree> childrenOf (const doc::ShowDocument& document, const std::string& cueId,
                                             const char* type)
    {
        std::vector<juce::ValueTree> out;

        for (const auto& child : document.findById (cueId))
            if (child.hasType (type))
                out.push_back (child);

        return out;
    }

    std::string idOf (const juce::ValueTree& node)
    {
        return node.getProperty ("id").toString().toStdString();
    }

    /*  The engine wired as `serve` wires it, for the cases about the record
        and the step: UndoTests' rig, the part of it these need. */
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            runner.setSamplesPerTick (960);

            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });
            open (document);
        }

        Engine::TickResult apply (const std::string& command, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit (std::string (origin::cli), command, std::move (args)));
            return engine.processTick (tick++);
        }

        std::string lastAppliedArg (std::size_t index)
        {
            const auto parsed = LogFile::parse (engine.log().contents());
            REQUIRE (! parsed.records.empty());
            const auto& last = parsed.records.back();
            REQUIRE (last.kind == LogRecord::Kind::applied);
            REQUIRE (last.args.size() > index);
            return last.args[index].getString();
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        cue::Runner runner { document, runs, runIds, focus };
        std::int64_t tick = 1;
    };

    osc::Value text (const std::string& s) { return osc::Value::string (s); }
}

//==============================================================================
TEST_CASE ("cue parts: the words, and which kinds of cue each part fits")
{
    using doc::parts::Part;

    CHECK (doc::parts::partsForWords ("eq sends").value() == std::vector<Part> { Part::eq, Part::sends });
    CHECK_FALSE (doc::parts::partsForWords ("eq eq").has_value());
    CHECK_FALSE (doc::parts::partsForWords ("eq loudness").has_value());
    CHECK_FALSE (doc::parts::partsForWords ("").has_value());
    CHECK (doc::parts::wordsFor ({ Part::time, Part::fx }) == "time fx");

    CHECK (doc::parts::fits (Part::eq, "Mic"));
    CHECK_FALSE (doc::parts::fits (Part::eq, "Video"));
    CHECK (doc::parts::fits (Part::time, "Video"));
    CHECK_FALSE (doc::parts::fits (Part::time, "Mic"));
    CHECK (doc::parts::fits (Part::picture, "Video"));

    /*  The 23 EQ rows, every one a row the schema gives a sound. */
    CHECK (doc::parts::rowsOf (Part::eq).size() == 23);

    for (const auto row : doc::parts::rowsOf (Part::eq))
        CHECK (doc::Schema::instance().attribute ("Media", row) != nullptr);

    /*  Every row of every part is one some kind it fits carries, and writable:
        a misspelt row would be passed over in silence where it is used. */
    for (const auto part : { Part::eq, Part::sends, Part::fx, Part::time, Part::speed,
                             Part::mix, Part::play, Part::picture })
    {
        for (const auto row : doc::parts::rowsOf (part))
        {
            INFO (doc::parts::wordFor (part) << "/" << row);
            bool carried = false;

            for (const auto* element : { "Media", "Mic", "Video" })
                if (doc::parts::fits (part, element))
                    if (const auto* attribute = doc::Schema::instance().attribute (element, row))
                        carried = carried || attribute->access() == doc::Access::readWrite;

            CHECK (carried);
        }

        const auto child = doc::parts::childOf (part);

        for (const auto row : doc::parts::childRowsOf (part))
        {
            INFO (child << "/" << row);
            CHECK (doc::Schema::instance().attribute (child, row) != nullptr);
        }
    }

    /*  WP: a template carries everything but what is bound to the file. */
    CHECK (doc::parts::templatePartsFor ("Media").size() == 6);
    CHECK (doc::parts::templatePartsFor ("Video").size() == 3);
    CHECK (doc::parts::templatePartsFor ("Mic").empty());
}

TEST_CASE ("cue parts: a copied part is that part of the cue and nothing else")
{
    doc::ShowDocument document;
    open (document);

    const auto eq = document.partFragmentOf ("eq", "CP000002");
    CHECK (eq.rfind ("<Fragment part=\"eq\">", 0) == 0);
    CHECK (eq.find ("eqB1Gain=\"6\"") != std::string::npos);
    CHECK (eq.find ("eqHpf=\"true\"") != std::string::npos);
    CHECK (eq.find ("file=") == std::string::npos);
    CHECK (eq.find ("name=") == std::string::npos);
    CHECK (eq.find ("<Send") == std::string::npos);

    /*  A send's lane stays behind (WV): it is drawn in its file's seconds. */
    const auto sends = document.partFragmentOf ("sends", "CP000003");
    CHECK (sends.find ("<Send") != std::string::npos);
    CHECK (sends.find ("on=\"false\"") != std::string::npos);
    CHECK (sends.find ("levelLane") == std::string::npos);
    CHECK (sends.find ("<Fx") == std::string::npos);

    const auto both = document.partFragmentOf ("time fx", "CP000002");
    CHECK (both.find ("<Range") != std::string::npos);
    CHECK (both.find ("<Fx") != std::string::npos);
    CHECK (both.find ("<Send") == std::string::npos);

    /*  A part the cue has not got, a word that is no part, a cue not there. */
    CHECK (document.partFragmentOf ("eq", "CP000006").empty());
    CHECK (document.partFragmentOf ("loudness", "CP000002").empty());
    CHECK (document.partFragmentOf ("eq", "CP0000ZZ").empty());

    /*  Read back as it was written. */
    const auto read = doc::CanonicalXml::readPartFragment (eq);
    REQUIRE (read.ok);
    CHECK (read.parts == "eq");
    CHECK (read.node.hasType ("Media"));

    CHECK_FALSE (doc::CanonicalXml::readPartFragment ("<Fragment part=\"eq\"><Video id=\"CP000006\"/></Fragment>").ok);
    CHECK_FALSE (doc::CanonicalXml::readPartFragment ("<Fragment><Media id=\"CP000006\"/></Fragment>").ok);
    CHECK_FALSE (doc::CanonicalXml::readPartFragment ("<Fragment part=\"eq\"></Fragment>").ok);

    /*  Kept apart from the clipboard of cues. */
    REQUIRE (document.copyPartToClipboard ("eq", "CP000002").ok);
    CHECK (document.partClipboardText() == eq);
    CHECK (document.clipboardText().empty());
    CHECK (document.copyPartToClipboard ("eq", "CP000006").reason == std::string (reason::typeMismatch));
    CHECK (document.copyPartToClipboard ("eq", "CP0000ZZ").reason == std::string (reason::unknownId));
    CHECK (document.copyPartToClipboard ("eq", "CP000002").ok);
}

TEST_CASE ("cue parts: an EQ pasted replaces the target's EQ whole, on every cue named")
{
    doc::ShowDocument document;
    open (document);

    const auto fragment = document.partFragmentOf ("eq", "CP000002");
    const auto pasted = document.pastePart (fragment, { "CP000003", "CP000004" }, {});
    REQUIRE (pasted.ok);
    CHECK (pasted.id.empty());

    for (const auto* cue : { "CP000003", "CP000004" })
    {
        INFO (cue);
        const std::string base = std::string ("/godot/cue/") + cue + "/";
        CHECK (at (document, base + "eqB1Gain") == "6");
        CHECK (at (document, base + "eqHpf") == "true");

        /*  What the source left at its default is the default again. */
        CHECK (at (document, base + "eqB2Gain") == "0");
    }

    /*  The rest of the cue is its own. */
    CHECK (childrenOf (document, "CP000003", "Send").size() == 2);
    CHECK (at (document, "/godot/cue/CP000003/file") == "b.wav");
    CHECK (document.validate().empty());
}

TEST_CASE ("cue parts: sends pasted match by bus, keep the target's lane, and take away the rest")
{
    doc::ShowDocument document;
    open (document);

    const auto fragment = document.partFragmentOf ("sends", "CP000002");
    REQUIRE (document.pastePart (fragment, { "CP000003" }, {}).ok);

    const auto sends = childrenOf (document, "CP000003", "Send");
    REQUIRE (sends.size() == 1);
    CHECK (idOf (sends[0]) == "CP000020");
    CHECK (at (document, "/godot/send/CP000020/level") == "-6");
    CHECK (at (document, "/godot/send/CP000020/on") == "true");
    CHECK (at (document, "/godot/send/CP000020/levelLane") == "0 0 4 -12");
    CHECK_FALSE (document.findById ("CP000021").isValid());

    /*  Onto a cue that sends nowhere: made, and named in the answer. */
    const auto made = document.pastePart (fragment, { "CP000004" }, {});
    REQUIRE (made.ok);
    const auto fresh = childrenOf (document, "CP000004", "Send");
    REQUIRE (fresh.size() == 1);
    CHECK (made.id == idOf (fresh[0]));
    CHECK (at (document, "/godot/send/" + made.id + "/level") == "-6");

    /*  A mic takes a media cue's sends: a send is a level at a mix. */
    REQUIRE (document.pastePart (fragment, { "CP000005" }, {}).ok);
    CHECK (childrenOf (document, "CP000005", "Send").size() == 1);

    /*  A mix the show has not got is passed over. */
    const std::string elsewhere = "<Fragment part=\"sends\"><Media id=\"CP000002\">"
                                  "<Send bus=\"CP0000ZZ\" id=\"CP000010\" level=\"-3\"/></Media></Fragment>";
    REQUIRE (document.pastePart (elsewhere, { "CP000004" }, {}).ok);
    CHECK (childrenOf (document, "CP000004", "Send").empty());
    CHECK (document.validate().empty());
}

TEST_CASE ("cue parts: an effects chain pasted is swapped whole, in the source's order, and only between kinds that match")
{
    doc::ShowDocument document;
    open (document);

    const auto fragment = document.partFragmentOf ("fx", "CP000002");
    const auto pasted = document.pastePart (fragment, { "CP000003" }, {});
    REQUIRE (pasted.ok);

    const auto chain = childrenOf (document, "CP000003", "Fx");
    REQUIRE (chain.size() == 2);
    CHECK (chain[0].getProperty ("plugin").toString() == "CP000080");
    CHECK (chain[1].getProperty ("plugin").toString() == "CP000081");
    CHECK (at (document, "/godot/fx/" + idOf (chain[0]) + "/values") == "0:0.25");
    CHECK (at (document, "/godot/fx/" + idOf (chain[1]) + "/enabled") == "false");
    CHECK_FALSE (document.findById ("CP000022").isValid());
    CHECK (pasted.id == idOf (chain[0]) + " " + idOf (chain[1]));

    /*  WU: a media cue's chain is entries of the show's set; a mic's is its
        rack channel's. */
    CHECK (document.pastePart (fragment, { "CP000005" }, {}).reason == std::string (reason::typeMismatch));
    CHECK (childrenOf (document, "CP000005", "Fx").empty());
    CHECK (document.validate().empty());
}

TEST_CASE ("cue parts: time and loops pasted replace the playlist, and a movie takes its locked sound along")
{
    doc::ShowDocument document;
    open (document);

    REQUIRE (document.setAttribute ("/godot/cue/CP000002/rate", "0.5").ok);
    const auto fragment = document.partFragmentOf ("time", "CP000002");

    REQUIRE (document.pastePart (fragment, { "CP000004" }, {}).ok);
    const auto ranges = childrenOf (document, "CP000004", "Range");
    REQUIRE (ranges.size() == 1);
    CHECK (at (document, "/godot/range/" + idOf (ranges[0]) + "/name") == "Verse");
    CHECK (at (document, "/godot/range/" + idOf (ranges[0]) + "/loops") == "2");
    CHECK (at (document, "/godot/range/" + idOf (ranges[0]) + "/out") == "4");
    CHECK (at (document, "/godot/cue/CP000004/rate") == "0.5");

    /*  A sound locked to its movie takes its time from the movie, and is
        refused alone. */
    CHECK (document.pastePart (fragment, { "CP000007" }, {}).reason == std::string (reason::lockedToMovie));
    CHECK (childrenOf (document, "CP000007", "Range").empty());

    /*  The movie and its sound together: the movie takes the paste, and the
        sound the movie. */
    REQUIRE (document.pastePart (fragment, { "CP000007", "CP000006" }, {}).ok);
    CHECK (childrenOf (document, "CP000006", "Range").size() == 1);
    CHECK (childrenOf (document, "CP000007", "Range").size() == 1);
    CHECK (at (document, "/godot/cue/CP000006/rate") == "0.5");
    CHECK (at (document, "/godot/cue/CP000007/rate") == "0.5");
    CHECK (document.validate().empty());
}

TEST_CASE ("cue parts: every refusal comes before the first write, and a part is never pasted as a cue")
{
    doc::ShowDocument document;
    open (document);

    const auto eq = document.partFragmentOf ("eq", "CP000002");

    /*  The first target fits, the second is not there: neither is touched. */
    CHECK (document.pastePart (eq, { "CP000003", "CP0000ZZ" }, {}).reason == std::string (reason::unknownId));
    CHECK (at (document, "/godot/cue/CP000003/eqB2Gain") == "-3");

    CHECK (document.pastePart (eq, { "CP000003", "CP000006" }, {}).reason == std::string (reason::typeMismatch));
    CHECK (at (document, "/godot/cue/CP000003/eqB2Gain") == "-3");

    CHECK (document.pastePart ("hello", { "CP000003" }, {}).reason == std::string (reason::badValue));
    CHECK (document.pastePart (eq, {}, {}).reason == std::string (reason::badValue));

    /*  A fragment of cues is not part of one, and the other way round. */
    CHECK (document.paste ("CP000001", 0, eq, {}).reason == std::string (reason::badValue));
    CHECK (document.pastePart (document.fragmentOf ({ "CP000002" }), { "CP000003" }, {}).reason
             == std::string (reason::badValue));

    /*  A locked show refuses it. */
    REQUIRE (document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (document.pastePart (eq, { "CP000003" }, {}).reason == std::string (reason::locked));
}

TEST_CASE ("cue parts: a paste onto several cues is one step of undo, and its record replays to the same names")
{
    Rig rig;

    REQUIRE (rig.apply ("cue.copyPart", { text ("sends fx"), text ("CP000002") }).applied == 1);
    const auto fragment = rig.document.partClipboardText();
    REQUIRE (! fragment.empty());

    REQUIRE (rig.apply ("cue.pastePart", { text (fragment), text ("CP000003 CP000004") }).applied == 1);
    CHECK (rig.document.history (doc::UndoDomain::document).getUndoDescription() == "cue.pastePart");

    const auto recorded = rig.lastAppliedArg (2);
    CHECK (! recorded.empty());

    const auto afterB = childrenOf (rig.document, "CP000003", "Fx").size();
    const auto afterC = childrenOf (rig.document, "CP000004", "Fx").size();
    CHECK (afterB == 2);
    CHECK (afterC == 2);
    CHECK (childrenOf (rig.document, "CP000004", "Send").size() == 1);

    /*  ONE STEP takes the whole paste back, from both cues. */
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (childrenOf (rig.document, "CP000003", "Fx").size() == 1);
    CHECK (childrenOf (rig.document, "CP000003", "Send").size() == 2);
    CHECK (childrenOf (rig.document, "CP000004", "Fx").empty());
    CHECK (childrenOf (rig.document, "CP000004", "Send").empty());
    CHECK (rig.document.findById ("CP000021").isValid());

    /*  A REPLAY handed the recorded names makes the same objects. */
    doc::ShowDocument replayed;
    open (replayed);
    std::vector<std::string> ids;

    for (const auto& word : juce::StringArray::fromTokens (juce::String (recorded), " ", ""))
        ids.push_back (word.toStdString());

    const auto again = replayed.pastePart (fragment, { "CP000003", "CP000004" }, ids);
    REQUIRE (again.ok);
    CHECK (again.id == recorded);
}
