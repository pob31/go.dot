/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/OutputLayout.h>
#include <wfg/engine/document/ShowDocument.h>

#include <string>
#include <vector>

using namespace wfg;

namespace
{
    std::vector<doc::BusShape> layout (std::initializer_list<std::pair<const char*, int>> buses)
    {
        std::vector<doc::BusShape> out;
        auto next = 0;

        for (const auto& [id, width] : buses)
        {
            out.push_back ({ id, width, next });
            next += width;
        }

        return out;
    }

    doc::LayoutEdit added (int width, int index)
    {
        doc::LayoutEdit edit;
        edit.kind = doc::LayoutEdit::Kind::create;
        edit.width = width;
        edit.index = index;
        return edit;
    }

    doc::LayoutEdit dropped (std::string id)
    {
        doc::LayoutEdit edit;
        edit.kind = doc::LayoutEdit::Kind::remove;
        edit.id = std::move (id);
        return edit;
    }

    doc::LayoutEdit movedTo (std::string id, int index)
    {
        doc::LayoutEdit edit;
        edit.kind = doc::LayoutEdit::Kind::move;
        edit.id = std::move (id);
        edit.index = index;
        return edit;
    }

    doc::LayoutEdit widened (std::string id, int width)
    {
        doc::LayoutEdit edit;
        edit.kind = doc::LayoutEdit::Kind::resize;
        edit.id = std::move (id);
        edit.width = width;
        return edit;
    }

    std::string channels (const doc::OutputLayout& result)
    {
        std::string out;

        for (const auto& bus : result.buses)
        {
            if (! out.empty()) out += ' ';
            out += (bus.id.empty() ? std::string ("new") : bus.id)
                     + ":" + std::to_string (bus.firstChannel)
                     + "+" + std::to_string (bus.width);
        }

        return out;
    }
}

//==============================================================================
TEST_CASE ("output layout: a fresh show's patch follows the list and is never written")
{
    const auto before = layout ({ { "a", 1 }, { "b", 2 } });

    /*  The whole point of the fresh regime: a stereo mix inserted at the top
        moves everything below it, and the patch stays empty so the interface
        follows. Nobody has heard the rig yet, so nothing is being re-patched -
        the designer is still arranging it. */
    const auto grown = doc::applyLayoutEdit (before, {}, false, added (2, 0));
    REQUIRE (grown.problem.empty());
    CHECK (channels (grown) == "new:0+2 a:2+1 b:3+2");
    CHECK (grown.outputPatch.empty());
    CHECK_FALSE (grown.patchChanged);

    for (const auto& edit : { dropped ("a"), movedTo ("b", 0), widened ("a", 2) })
    {
        const auto result = doc::applyLayoutEdit (before, {}, false, edit);
        REQUIRE (result.problem.empty());
        CHECK (result.outputPatch.empty());
        CHECK_FALSE (result.patchChanged);
    }
}

TEST_CASE ("output layout: every edit repacks the channels behind it")
{
    const auto before = layout ({ { "a", 2 }, { "b", 1 }, { "c", 2 } });

    CHECK (channels (doc::applyLayoutEdit (before, {}, false, added (1, 1))) == "a:0+2 new:2+1 b:3+1 c:4+2");
    CHECK (channels (doc::applyLayoutEdit (before, {}, false, added (2, -1))) == "a:0+2 b:2+1 c:3+2 new:5+2");
    CHECK (channels (doc::applyLayoutEdit (before, {}, false, dropped ("a"))) == "b:0+1 c:1+2");
    CHECK (channels (doc::applyLayoutEdit (before, {}, false, widened ("a", 1))) == "a:0+1 b:1+1 c:2+2");
    CHECK (channels (doc::applyLayoutEdit (before, {}, false, widened ("b", 2))) == "a:0+2 b:2+2 c:4+2");

    /*  A move's index is a position in the list AS IT STANDS, the moved output
        still counted, which is `juce::ValueTree::moveChild`'s rule and
        therefore `ShowDocument::move`'s. One convention, or a row dragged in
        the output list would land somewhere else than the same drag in the cue
        list. */
    CHECK (channels (doc::applyLayoutEdit (before, {}, false, movedTo ("a", 2))) == "b:0+1 c:1+2 a:3+2");
    CHECK (channels (doc::applyLayoutEdit (before, {}, false, movedTo ("c", 0))) == "c:0+2 a:2+2 b:4+1");
    CHECK (channels (doc::applyLayoutEdit (before, {}, false, movedTo ("b", 99))) == "a:0+2 c:2+2 b:4+1");
}

TEST_CASE ("output layout: a settled show keeps every output on the channels it is plugged into")
{
    const auto before = layout ({ { "a", 2 }, { "b", 1 }, { "c", 2 } });

    /*  Settled by the flag, with the patch still empty: the identity it has
        been running on is materialised first, and only then repacked. */
    const auto moved = doc::applyLayoutEdit (before, {}, true, movedTo ("c", 0));
    REQUIRE (moved.problem.empty());
    CHECK (channels (moved) == "c:0+2 a:2+2 b:4+1");
    CHECK (moved.patchChanged);

    /*  c was on interface 3 and 4 and stays there; a was on 0 and 1 and stays
        there. The LIST has changed and the RIG has not, which is the whole of
        what settling means. */
    CHECK (moved.outputPatch == std::vector<int> { 3, 4, 0, 1, 2 });

    const auto gone = doc::applyLayoutEdit (before, {}, true, dropped ("a"));
    REQUIRE (gone.problem.empty());
    CHECK (gone.outputPatch == std::vector<int> { 2, 3, 4 });

    /*  A new output takes the next interface channels past everything in use -
        WFS-DIY's "diagonal-continue" - rather than the lowest free one. */
    const auto grown = doc::applyLayoutEdit (before, {}, true, added (2, 0));
    REQUIRE (grown.problem.empty());
    CHECK (channels (grown) == "new:0+2 a:2+2 b:4+1 c:5+2");
    CHECK (grown.outputPatch == std::vector<int> { 5, 6, 0, 1, 2, 3, 4 });
}

TEST_CASE ("output layout: widening finds a channel and narrowing gives one back")
{
    const auto before = layout ({ { "a", 1 }, { "b", 2 } });

    const auto wider = doc::applyLayoutEdit (before, {}, true, widened ("a", 2));
    REQUIRE (wider.problem.empty());
    CHECK (channels (wider) == "a:0+2 b:2+2");

    /*  a keeps interface 0 and finds a right past everything used; b keeps 1
        and 2. Mono to stereo must not move what was already plugged in. */
    CHECK (wider.outputPatch == std::vector<int> { 0, 3, 1, 2 });

    const auto narrower = doc::applyLayoutEdit (before, {}, true, widened ("b", 1));
    REQUIRE (narrower.problem.empty());
    CHECK (channels (narrower) == "a:0+1 b:1+1");
    CHECK (narrower.outputPatch == std::vector<int> { 0, 1 });
}

TEST_CASE ("output layout: a hand-written layout is a rig, and is preserved rather than repacked")
{
    /*  `tests/fixtures/bundles/slots`, which is why this case exists: a
        twelve-wide processor send at channel 8 and a foldback at 0, out of
        document order with a hole between them. Repacking that silently would
        move a processor feed to channels nobody wired. */
    std::vector<doc::BusShape> slots { { "foldback", 2, 0 }, { "wfs", 12, 8 } };

    CHECK_FALSE (doc::isPacked (slots));

    /*  Unpacked counts as settled even with the flag false and no patch: the
        channels move out of `firstChannel`, where a packed list can no longer
        say them, and into the patch, which is where they belong. */
    const auto result = doc::applyLayoutEdit (slots, {}, false, added (2, -1));
    REQUIRE (result.problem.empty());
    CHECK (channels (result) == "foldback:0+2 wfs:2+12 new:14+2");
    REQUIRE (result.outputPatch.size() == 16);
    CHECK (result.outputPatch[0] == 0);
    CHECK (result.outputPatch[1] == 1);
    CHECK (result.outputPatch[2] == 8);     // the WFS send still starts at interface 8
    CHECK (result.outputPatch[13] == 19);   // and still ends at 19
    CHECK (result.outputPatch[14] == 20);   // the new output takes what is past it
    CHECK (result.outputPatch[15] == 21);

    /*  The mutation that catches a lost materialise step: without it the WFS
        send would come out on interface 2, which is the failure this whole
        branch exists to prevent. */
    CHECK (result.outputPatch[2] != 2);
}

TEST_CASE ("output layout: a disconnected channel stays disconnected, and rows nobody feeds are kept")
{
    const auto before = layout ({ { "a", 2 }, { "b", 2 } });

    /*  -1 is somebody saying "this output goes nowhere", which an edit to the
        list must not quietly undo. The tail - patch rows past the outputs the
        show has - is a row count the designer declared in the matrix, and is
        carried through too. */
    const std::vector<int> patch { 5, -1, 6, 7, 20, 21 };

    const auto moved = doc::applyLayoutEdit (before, patch, true, movedTo ("b", 0));
    REQUIRE (moved.problem.empty());
    CHECK (moved.outputPatch == std::vector<int> { 6, 7, 5, -1, 20, 21 });

    const auto grown = doc::applyLayoutEdit (before, patch, true, added (1, 0));
    REQUIRE (grown.problem.empty());
    CHECK (grown.outputPatch == std::vector<int> { 22, 5, -1, 6, 7, 20, 21 });
}

TEST_CASE ("output layout: it refuses what it cannot do, and changes nothing when it does")
{
    const auto before = layout ({ { "a", 2 } });

    CHECK_FALSE (doc::applyLayoutEdit (before, {}, false, added (0, -1)).problem.empty());
    CHECK_FALSE (doc::applyLayoutEdit (before, {}, false, widened ("a", -1)).problem.empty());
    CHECK_FALSE (doc::applyLayoutEdit (before, {}, false, dropped ("nobody")).problem.empty());
    CHECK_FALSE (doc::applyLayoutEdit (before, {}, false, movedTo ("nobody", 0)).problem.empty());
    CHECK_FALSE (doc::applyLayoutEdit (before, {}, false, added (audio::maximumPatchChannels, -1))
                   .problem.empty());

    const auto refused = doc::applyLayoutEdit (before, {}, false, dropped ("nobody"));
    CHECK (refused.buses.empty());
    CHECK (refused.outputPatch.empty());
}

TEST_CASE ("output layout: applying the same edit twice lands in the same place")
{
    const auto before = layout ({ { "a", 2 }, { "b", 1 } });
    const std::vector<int> patch { 4, 5, 6 };

    const auto once = doc::applyLayoutEdit (before, patch, true, movedTo ("b", 0));
    const auto twice = doc::applyLayoutEdit (once.buses, once.outputPatch, true, movedTo ("b", 0));

    CHECK (channels (once) == channels (twice));
    CHECK (once.outputPatch == twice.outputPatch);
    CHECK_FALSE (twice.patchChanged);
}

//==============================================================================
TEST_CASE ("bus commands: the four of them keep the channels packed and the list in order")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"/></Lists><Mounts/>"
        "<Audio tracks=\"2\"/></Show>", document).ok);

    document.beginTransaction ("bus.create", 0, "test", {});
    const auto first = document.createBus ("direct", 1);
    REQUIRE (first.ok);
    const auto second = document.createBus ("mix", 2);
    REQUIRE (second.ok);
    const auto third = document.createBus ("direct", 1);
    REQUIRE (third.ok);

    CHECK (document.getAttribute ("/godot/bus/" + first.id + "/firstChannel") == "0");
    CHECK (document.getAttribute ("/godot/bus/" + second.id + "/firstChannel") == "1");
    CHECK (document.getAttribute ("/godot/bus/" + third.id + "/firstChannel") == "3");

    /*  Named on arrival, and counted per kind: an output list of rows reading
        "Bus" would be no list at all. */
    CHECK (document.getAttribute ("/godot/bus/" + first.id + "/name") == "Direct 1");
    CHECK (document.getAttribute ("/godot/bus/" + second.id + "/name") == "Mix 1");
    CHECK (document.getAttribute ("/godot/bus/" + third.id + "/name") == "Direct 2");

    /*  Nothing has played and nobody has patched, so the interface is still
        following the list and the patch is not written at all. */
    CHECK (document.getAttribute ("/godot/audio/outputPatch") == "");

    REQUIRE (document.moveBus (third.id, 0).ok);
    CHECK (document.getAttribute ("/godot/bus/" + third.id + "/firstChannel") == "0");
    CHECK (document.getAttribute ("/godot/bus/" + first.id + "/firstChannel") == "1");
    CHECK (document.getAttribute ("/godot/bus/" + second.id + "/firstChannel") == "2");

    /*  The document's own order follows, so the file reads the way the list
        does and the next reader of either finds the same show. */
    const auto audio = document.root().getChildWithName ("Audio");
    CHECK (audio.getChild (0)["id"].toString().toStdString() == third.id);
    CHECK (audio.getChild (1)["id"].toString().toStdString() == first.id);

    REQUIRE (document.resizeBus (first.id, 2).ok);
    CHECK (document.getAttribute ("/godot/bus/" + second.id + "/firstChannel") == "3");

    REQUIRE (document.removeBus (first.id).ok);
    CHECK (document.getAttribute ("/godot/bus/" + second.id + "/firstChannel") == "1");
    CHECK_FALSE (document.findById (first.id).isValid());
}

TEST_CASE ("bus commands: deleting an output takes its routes with it, in one undo step")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\">"
        "<Media id=\"B3N8R5TW\" file=\"a.wav\"><Route id=\"F7HR8TVD\" bus=\"J3MT5XYA\" gains=\"1 0 0 1\"/>"
        "<Route id=\"P9XKC2WR\" bus=\"K4NV6ZB1\" gains=\"1 0 0 1\"/></Media></List></Lists>"
        "<Mounts><Mount id=\"M1000001\" namespace=\"namespaces/wfs.json\" port=\"9000\" prefix=\"/wfs\">"
        "<Slot id=\"S1000001\" address=\"/wfs/input/1\" bus=\"J3MT5XYA\"/></Mount></Mounts>"
        "<Audio tracks=\"2\"><Bus id=\"J3MT5XYA\" name=\"Main\" width=\"2\"/>"
        "<Bus id=\"K4NV6ZB1\" firstChannel=\"2\" name=\"Foldback\" width=\"2\"/></Audio></Show>",
        document).ok);

    const auto before = doc::CanonicalXml::write (document);

    document.beginTransaction ("bus.delete", 0, "test", {});
    REQUIRE (document.removeBus ("J3MT5XYA").ok);

    /*  The route that named it is gone rather than left dangling: a dangling
        destination is a run that fails `bad-route` at GO, months after the
        delete that caused it. */
    CHECK_FALSE (document.findById ("F7HR8TVD").isValid());
    CHECK (document.findById ("P9XKC2WR").isValid());

    /*  A processor input keeps its address and its width and loses the bus it
        fed from - there is an address and a width somebody typed in there. */
    CHECK (document.findById ("S1000001").isValid());
    CHECK (document.getAttribute ("/godot/slot/S1000001/bus") == "");

    CHECK (document.getAttribute ("/godot/bus/K4NV6ZB1/firstChannel") == "0");

    /*  ONE STEP, which is what makes the delete safe to offer: the bus, its
        routes and every repacked channel come back together. */
    REQUIRE (document.undo (doc::UndoDomain::document).has_value());
    CHECK (doc::CanonicalXml::write (document) == before);
}

TEST_CASE ("bus commands: a settled show keeps its channels when the list is edited")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"/></Lists><Mounts/>"
        "<Audio tracks=\"2\"><Bus id=\"J3MT5XYA\" name=\"Main\" width=\"2\"/>"
        "<Bus id=\"K4NV6ZB1\" firstChannel=\"2\" name=\"Foldback\" width=\"2\"/></Audio></Show>",
        document).ok);

    document.beginTransaction ("node.set", 0, "test", {});
    REQUIRE (document.setAttribute ("/godot/audio/patchSettled", "true").ok);

    REQUIRE (document.createBus ("mix", 2, 0).ok);

    /*  The two outputs that existed keep interface 0-1 and 2-3; the new one
        takes 4-5 although it now sits first in the list. */
    CHECK (document.getAttribute ("/godot/audio/outputPatch") == "4 5 0 1 2 3");
    CHECK (document.getAttribute ("/godot/bus/J3MT5XYA/firstChannel") == "2");

    /*  And the flag is state, not show: it belongs to this rig on this night
        rather than to the show, so it lands in state.xml and leaves the saved
        document alone. */
    CHECK (doc::CanonicalXml::write (document).find ("patchSettled") == std::string::npos);
}

TEST_CASE ("a new show arrives able to play something")
{
    /*  THE FAULT THIS PINS. A document built by the constructor declares
        `tracks` nought and has no outputs, which is right for a scratch
        document and was what File - New wrote to disk: a show that looked
        complete, armed every dropped file against a polyphony ceiling of
        nought, and ended every GO `no-track`. No command could set the count,
        because the row was read-only.  */
    doc::ShowDocument empty;
    CHECK (empty.getAttribute ("/godot/audio/tracks") == "0");

    doc::ShowDocument fresh;
    fresh.beginTransaction ("new", 0, "test", {});
    REQUIRE (fresh.startNewShow().ok);

    //  Somewhere to put cues.
    const auto lists = fresh.root().getChildWithName ("Lists");
    REQUIRE (lists.getNumChildren() == 1);

    //  Room for some of them to sound at once: thirty-two (author, 2026-09-25).
    CHECK (fresh.getAttribute ("/godot/audio/tracks") == "32");

    /*  AND SOMEWHERE FOR THE SOUND TO GO. Both halves or neither: the engine
        refuses to start a show that has tracks and no bus ("there is nowhere
        for them to go"), so tracks alone would be worse than nothing. */
    const auto audio = fresh.root().getChildWithName ("Audio");
    auto buses = 0;

    for (const auto& child : audio)
        if (child.hasType ("Bus"))
        {
            ++buses;
            const auto id = child["id"].toString().toStdString();
            CHECK (fresh.getAttribute ("/godot/bus/" + id + "/width") == "2");
            CHECK (fresh.getAttribute ("/godot/bus/" + id + "/firstChannel") == "0");
            CHECK (fresh.getAttribute ("/godot/bus/" + id + "/kind") == "direct");
            CHECK (fresh.getAttribute ("/godot/bus/" + id + "/name") == "Main L/R");
        }

    CHECK (buses == 1);

    //  And it is still following its list: nothing has been heard yet.
    CHECK (fresh.getAttribute ("/godot/audio/outputPatch") == "");
}

TEST_CASE ("how many cues can sound at once is a decision, so a client can state it")
{
    /*  It was `access=r` with no command behind it, which is a decision nobody
        could take. The graph is built from it when the audio settings are
        applied, so a write lands at the next apply - the same moment a changed
        buffer size lands - and never under a running show. */
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"/></Lists><Mounts/>"
        "<Audio tracks=\"2\"><Bus id=\"J3MT5XYA\" name=\"Main\" width=\"2\"/></Audio></Show>",
        document).ok);

    CHECK (document.setAttribute ("/godot/audio/tracks", "16").ok);
    CHECK (document.getAttribute ("/godot/audio/tracks") == "16");

    //  Nought is legal and means a show with no audio, as it always did.
    CHECK (document.setAttribute ("/godot/audio/tracks", "0").ok);

    //  And it is a show decision, so the lock refuses it like any other.
    REQUIRE (document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (document.setAttribute ("/godot/audio/tracks", "4").reason == reason::locked);
}

TEST_CASE ("bus commands: firstChannel and width refuse every client that is not a layout command")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"/></Lists><Mounts/>"
        "<Audio tracks=\"2\"><Bus id=\"J3MT5XYA\" name=\"Main\" width=\"2\"/></Audio></Show>",
        document).ok);

    /*  The reason the four commands exist: a client that could write these
        could leave two outputs summing into one interface channel, and nobody
        would hear it until the night. */
    CHECK (document.setAttribute ("/godot/bus/J3MT5XYA/firstChannel", "8").reason == reason::readOnly);
    CHECK (document.setAttribute ("/godot/bus/J3MT5XYA/width", "1").reason == reason::readOnly);
    CHECK (document.setAttribute ("/godot/bus/J3MT5XYA/name", "Renamed").ok);
    CHECK (document.setAttribute ("/godot/bus/J3MT5XYA/kind", "mix").ok);
}
