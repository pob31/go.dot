/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */

/*
    WHAT IS MISSING, SAID WHERE IT IS USED (namespace draft §64), tested on
    snapshots made by hand: every state the engine publishes on an object is
    one node here, so each rule is checked without a port, a projector or a
    plugin in the room.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/client/model/Problems.h>
#include <wfg/client/model/SettingsPages.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    namespace model = client::model;
    namespace page = model::settingsPages;

    using Value = osc::Value;

    //  A snapshot holding exactly these nodes, as the runtime half: sorted, one value each.
    tree::TreeSnapshot snapshotOf (std::map<std::string, Value> rows)
    {
        std::vector<tree::Node> nodes;

        for (auto& [address, value] : rows)
        {
            tree::Node node;
            node.address = address;
            node.values = { std::move (value) };
            nodes.push_back (std::move (node));
        }

        return tree::TreeSnapshot (0, nullptr, nullptr, std::move (nodes));
    }

    Value word (const char* text) { return Value::string (text); }
    Value yes() { return Value::boolean (true); }
    Value no() { return Value::boolean (false); }

    model::Row cueRow (std::string id, std::string kind)
    {
        model::Row row;
        row.rowKind = model::RowKind::cue;
        row.id = std::move (id);
        row.kind = std::move (kind);
        row.isGroup = row.kind == "group";
        return row;
    }

    std::vector<std::string> pagesOf (const std::vector<model::PageProblems>& pages)
    {
        std::vector<std::string> out;

        for (const auto& entry : pages)
            out.push_back (entry.page);

        return out;
    }

    bool anySays (const std::vector<model::Problem>& problems, const std::string& words)
    {
        return std::any_of (problems.begin(), problems.end(),
                            [&words] (const model::Problem& problem) { return problem.sentence.find (words) != std::string::npos; });
    }
}

TEST_CASE ("problems: what is missing is said on the page that puts it right, in the window's order (§64)")
{
    /*  The author, 2026-10-15: "Add warnings in the main window for each type
        of error. Clicking on one of these icons opens the show settings to
        the relevant tab" - and, chosen, "Missing or failed": absent or broken
        is a problem, waiting is not. */
    const auto snapshot = snapshotOf ({
        //  A show with a sound in it, and no interface running.
        { "/godot/cue/M1/kind", word ("media") },
        { "/godot/audio/status", word ("stopped") },

        { "/godot/input/IN1/name", word ("Voix") },
        { "/godot/input/IN1/problem", word ("no input interface is open.") },

        { "/godot/plugin/order", word ("PL1") },
        { "/godot/plugin/PL1/name", word ("Reverb") },
        { "/godot/plugin/PL1/state", word ("missing") },
        { "/godot/plugin/PL1/problem", word ("the identifier is not in the scan") },

        { "/godot/videoOutput/renderer", word ("running") },
        { "/godot/videoOutput/order", word ("VO1") },
        { "/godot/videoOutput/VO1/name", word ("Front") },
        { "/godot/videoOutput/VO1/enabled", yes() },
        { "/godot/videoOutput/VO1/bound", no() },
        { "/godot/videoOutput/VO1/problem", word ("no such display on this machine") },

        { "/godot/mount/DV1/name", word ("Desk") },
        { "/godot/mount/DV1/prefix", word ("/desk") },
        { "/godot/mount/DV1/problem", word ("no usable port") },

        { "/godot/port/PT1/name", word ("Lights") },
        { "/godot/port/PT1/bound", no() },
        { "/godot/port/PT1/problem", word ("no such device on this machine") },

        { "/godot/serial/SR1/name", word ("Arduino") },
        { "/godot/serial/SR1/state", word ("retrying") },
        { "/godot/serial/SR1/problem", word ("in use by another program") },

        { "/godot/pair/alarm", word ("Two mains!") },
    });

    const auto index = model::indexObjects (snapshot);
    CHECK (index.needsSound);

    const auto problems = model::machineProblems (snapshot, index);
    const auto pages = model::byPage (problems);

    CHECK (pagesOf (pages) == std::vector<std::string> { page::redundancy, page::audioInterface, page::inputs,
                                                         page::plugins, page::video, page::network, page::midi,
                                                         page::serial });

    //  Each says what, the object's own name, and where it is put right - the engine's full stop dropped.
    CHECK (anySays (problems, "No audio interface is running: sound and mic cues play nothing - Show settings, Audio interface."));
    CHECK (anySays (problems, "Input Voix: no input interface is open - Show settings, Inputs."));
    CHECK (anySays (problems, "Plugin Reverb is missing: the identifier is not in the scan - Show settings, Plugins."));
    CHECK (anySays (problems, "Output Front: no such display on this machine - Show settings, Video."));
    CHECK (anySays (problems, "Device Desk: no usable port - Show settings, Network."));
    CHECK (anySays (problems, "MIDI port Lights: no such device on this machine - Show settings, MIDI."));
    CHECK (anySays (problems, "Serial port Arduino: in use by another program - Show settings, Serial."));
    CHECK (anySays (problems, "Two mains! - Show settings, Redundancy."));

    //  The icons the warnings wear.
    CHECK (model::iconForPage (page::audioInterface) == model::Icon::sound);
    CHECK (model::iconForPage (page::midi) == model::Icon::midi);
    CHECK (model::iconForPage (page::plugins) == model::Icon::plugin);
    CHECK (model::iconForPage ("") == model::Icon::missing);
}

TEST_CASE ("problems: waiting is not a problem, and nothing is said of what nobody needs (§64)")
{
    //  Each object in its transient state: a renderer starting, a port opening, a plugin loading, audio being applied.
    const auto snapshot = snapshotOf ({
        { "/godot/cue/M1/kind", word ("media") },
        { "/godot/audio/status", word ("stopped") },
        { "/godot/audio/settingsStatus", word ("applying") },

        { "/godot/plugin/order", word ("PL1") },
        { "/godot/plugin/PL1/state", word ("loading") },

        { "/godot/videoOutput/renderer", word ("starting") },
        { "/godot/videoOutput/order", word ("VO1") },
        { "/godot/videoOutput/VO1/enabled", yes() },
        { "/godot/videoOutput/VO1/problem", word ("the renderer not running") },

        { "/godot/serial/SR1/state", word ("opening") },
        { "/godot/serial/SR1/problem", word ("not yet") },

        { "/godot/mount/DV1/link", word ("opening") },
        { "/godot/mount/DV1/linkProblem", word ("nothing answered yet") },
    });

    CHECK (model::machineProblems (snapshot, model::indexObjects (snapshot)).empty());

    //  A show with no sound and no interface switched on needs no audio.
    const auto quiet = snapshotOf ({ { "/godot/cue/M1/kind", word ("memo") }, { "/godot/audio/status", word ("stopped") } });
    CHECK (model::machineProblems (quiet, model::indexObjects (quiet)).empty());

    //  And an output switched off has no screen to miss.
    const auto off = snapshotOf ({
        { "/godot/videoOutput/renderer", word ("running") },
        { "/godot/videoOutput/order", word ("VO1") },
        { "/godot/videoOutput/VO1/enabled", no() },
        { "/godot/videoOutput/VO1/problem", word ("no such display on this machine") },
    });
    CHECK (model::machineProblems (off, model::indexObjects (off)).empty());
}

TEST_CASE ("problems: a cue that needs a missing thing is marked, and a shut group carries its members' (§64)")
{
    /*  The author, 2026-10-15: "when a cue is missing something (screen is not
        connected for a video output, audio interface for a audio/mic cue,
        missing plugin, video insert etc...) mark the cues in the cue list." */
    const auto snapshot = snapshotOf ({
        { "/godot/audio/status", word ("running") },

        //  A sound through an FX whose plugin is missing.
        { "/godot/cue/SND1/kind", word ("media") },
        { "/godot/cue/SND1/fx", word ("FX1") },
        { "/godot/fx/FX1/plugin", word ("PL1") },
        { "/godot/plugin/order", word ("PL1") },
        { "/godot/plugin/PL1/name", word ("Reverb") },
        { "/godot/plugin/PL1/state", word ("failed") },

        //  A MIDI cue on a port with no device.
        { "/godot/cue/MID1/kind", word ("midi") },
        { "/godot/cue/MID1/port", word ("PT1") },
        { "/godot/port/PT1/name", word ("Lights") },
        { "/godot/port/PT1/bound", no() },
        { "/godot/port/PT1/problem", word ("no such device on this machine") },

        //  A picture on a canvas shown by an output whose screen is not there.
        { "/godot/cue/PIC1/kind", word ("video") },
        { "/godot/cue/PIC1/canvas", word ("CV1") },
        { "/godot/videoOutput/renderer", word ("running") },
        { "/godot/videoOutput/order", word ("VO1") },
        { "/godot/videoOutput/VO1/name", word ("Front") },
        { "/godot/videoOutput/VO1/canvas", word ("CV1") },
        { "/godot/videoOutput/VO1/enabled", yes() },
        { "/godot/videoOutput/VO1/problem", word ("no such display on this machine") },

        //  A memo, which needs nothing.
        { "/godot/cue/MEM1/kind", word ("memo") },

        //  A shut group: its one member is no row, and is a MIDI cue on the same port.
        { "/godot/cue/GRP1/kind", word ("group") },
        { "/godot/cue/GRP1/order", word ("HID1") },
        { "/godot/cue/HID1/kind", word ("midi") },
        { "/godot/cue/HID1/port", word ("PT1") },
    });

    const std::vector<model::Row> rows { cueRow ("SND1", "media"), cueRow ("MID1", "midi"), cueRow ("PIC1", "video"),
                                         cueRow ("MEM1", "memo"), cueRow ("GRP1", "group") };

    const auto index = model::indexObjects (snapshot);
    const auto needs = model::needsOf (snapshot, rows, index);
    const auto marks = model::cueProblems (needs, model::machineProblems (snapshot, index));

    REQUIRE (marks.count ("SND1") == 1);
    CHECK (marks.at ("SND1").icon == model::Icon::warning);
    CHECK (marks.at ("SND1").text == "plugin failed");
    CHECK (marks.at ("SND1").meaning.find ("Plugin Reverb failed") != std::string::npos);

    REQUIRE (marks.count ("MID1") == 1);
    CHECK (marks.at ("MID1").text == "no port");
    CHECK (marks.at ("MID1").meaning.find ("- Show settings, MIDI.") != std::string::npos);

    REQUIRE (marks.count ("PIC1") == 1);
    CHECK (marks.at ("PIC1").text == "no screen");

    CHECK (marks.count ("MEM1") == 0);

    //  The shut group says what is inside it, and the hidden member's own mark waits for its row.
    REQUIRE (marks.count ("GRP1") == 1);
    CHECK (marks.at ("GRP1").text == "inside");
    CHECK (marks.at ("GRP1").meaning.find ("A cue inside needs what is missing") != std::string::npos);
    CHECK (needs.hiddenIn.at ("GRP1") == std::vector<std::string> { "HID1" });

    //  Opened, the member is a row of its own and the group carries nothing.
    auto open = rows;
    open.push_back (cueRow ("HID1", "midi"));
    const auto openMarks = model::cueProblems (model::needsOf (snapshot, open, index),
                                               model::machineProblems (snapshot, index));
    CHECK (openMarks.count ("GRP1") == 0);
    CHECK (openMarks.count ("HID1") == 1);

    //  A cue switched off needs nothing tonight.
    auto off = rows;
    off[1].enabled = false;
    CHECK (model::cueProblems (model::needsOf (snapshot, off, index),
                               model::machineProblems (snapshot, index)).count ("MID1") == 0);
}

TEST_CASE ("problems: a file looked for and not there marks its cue, and a problem wins over ready (§64)")
{
    const auto snapshot = snapshotOf ({
        { "/godot/audio/status", word ("running") },
        { "/godot/cue/SND1/kind", word ("media") },
        { "/godot/cue/SND1/file", word ("thunder.wav") },
        { "/godot/cue/SND1/missing", yes() },
        { "/godot/cue/SND2/kind", word ("media") },
        { "/godot/cue/SND2/file", word ("rain.wav") },
        { "/godot/cue/SND2/missing", no() },
    });

    const std::vector<model::Row> rows { cueRow ("SND1", "media"), cueRow ("SND2", "media") };
    const auto needs = model::needsOf (snapshot, rows, model::indexObjects (snapshot));
    const auto files = model::fileProblems (snapshot, needs);

    REQUIRE (files.size() == 1u);
    CHECK (files.front().page.empty());
    CHECK (files.front().key == "file:SND1");
    CHECK (files.front().sentence.find ("thunder.wav") != std::string::npos);

    //  The warning for files names the cues, for the button that reveals them.
    const auto pages = model::byPage (files);
    REQUIRE (pages.size() == 1u);
    CHECK (pages.front().page.empty());
    CHECK (pages.front().icon == model::Icon::missing);
    CHECK (pages.front().cues == std::vector<std::string> { "SND1" });

    const auto marks = model::cueProblems (needs, files);
    REQUIRE (marks.count ("SND1") == 1);
    CHECK (marks.at ("SND1").text == "missing");

    //  Ready is untrue of a cue whose file is missing: the problem takes the cell.
    std::map<std::string, model::Mark> readiness {
        { "SND1", { model::Icon::ready, {}, "Ready" } },
        { "SND2", { model::Icon::ready, {}, "Ready" } },
    };

    const auto cells = model::withProblems (readiness, marks);
    CHECK (cells.at ("SND1").icon == model::Icon::warning);
    CHECK (cells.at ("SND2").icon == model::Icon::ready);
}
