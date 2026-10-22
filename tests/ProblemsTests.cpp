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

TEST_CASE ("problems: a cue or an output past what the audio was built with is said, and where Apply puts it right")
{
    /*  PRD §3.25: the graph's slots and output width are fixed when it is
        built - the show opened, Apply while stopped, Load now. An edit since
        that goes past them is refused or silent at GO, so it is said before. */
    std::map<std::string, Value> rows {
        { "/godot/cue/M1/kind", word ("media") },
        { "/godot/cue/M1/number", word ("12") },
        { "/godot/cue/M1/name", word ("Rain") },
        { "/godot/cue/M2/kind", word ("media") },
        { "/godot/audio/status", word ("running") },
        { "/godot/audio/hardwareOutputs", word ("8") },
        { "/godot/audio/outputs", word ("2") },
        { "/godot/audio/slots", word ("12") },
        { "/godot/bus/B1/firstChannel", word ("0") },
        { "/godot/bus/B1/width", word ("2") },
        { "/godot/bus/B2/firstChannel", word ("2") },
        { "/godot/bus/B2/width", word ("2") },
    };

    //  Thirteen slices on M1, past the twelve slots; nine on M2, inside them.
    for (auto at = 0; at < 13; ++at)
        rows.emplace ("/godot/range/RA" + std::to_string (at) + "/cue", word ("M1"));

    for (auto at = 0; at < 9; ++at)
        rows.emplace ("/godot/range/RB" + std::to_string (at) + "/cue", word ("M2"));

    const auto snapshot = snapshotOf (rows);
    const auto index = model::indexObjects (snapshot);
    const auto problems = model::machineProblems (snapshot, index);

    CHECK (anySays (problems, "Cue 12 Rain has 13 slices and the audio was built with room for 12 on one cue: GO plays"
                              " nothing of it until Apply while stopped - Show settings, Audio interface."));
    CHECK (! anySays (problems, "has 9 slices"));
    CHECK (anySays (problems, "The show's outputs take 4 channels and the audio was built with 2: the last are not heard"
                              " until Apply while stopped - Show settings, Audio interface."));

    //  The cue is marked on its row by the same words.
    const auto needs = model::needsOf (snapshot, { cueRow ("M1", "media"), cueRow ("M2", "media") }, index);
    const auto marks = model::cueProblems (needs, problems);
    REQUIRE (marks.count ("M1") == 1);
    CHECK (marks.at ("M1").text == "too many slices");
    CHECK (marks.count ("M2") == 0);

    //  Built again with room for both, nothing is said.
    rows["/godot/audio/slots"] = word ("17");
    rows["/godot/audio/outputs"] = word ("4");
    const auto rebuilt = snapshotOf (rows);
    CHECK (model::machineProblems (rebuilt, model::indexObjects (rebuilt)).empty());
}

//==============================================================================
/*  WHAT A CUE'S RANGES DO NOT FIT, said on its row (namespace draft §99, AXT):
    a range that begins at or past the end plays nothing; one that only ends
    past it by more than a millisecond plays to the end, and says it was
    written longer - never for a sound locked to its movie; a range to the end,
    or one a hair past it, says nothing. */
TEST_CASE ("problems: a range beginning past what the cue plays marks it, one ending past it says it plays to the end (§99)")
{
    const auto range = [] (const char* id, const char* cue, double in, double out, int index, bool toEnd = false)
    {
        const auto base = std::string ("/godot/range/") + id + "/";
        return std::vector<std::pair<std::string, Value>> {
            { base + "cue", word (cue) }, { base + "in", Value::float64 (in) }, { base + "out", Value::float64 (out) },
            { base + "index", Value::int32 (index) }, { base + "toEnd", toEnd ? yes() : no() } };
    };

    std::map<std::string, Value> rows {
        { "/godot/audio/status", word ("running") },
    };

    const auto sound = [&rows] (const char* id, const char* file, double seconds, const char* lockedTo = "")
    {
        const auto base = std::string ("/godot/cue/") + id + "/";
        rows[base + "kind"] = word ("media");
        rows[base + "file"] = word (file);
        rows[base + "missing"] = no();
        rows[base + "duration"] = Value::float64 (seconds);
        rows[base + "lockedTo"] = word (lockedTo);
    };

    sound ("OUT1", "late.wav", 10.0);                 // its second range begins past the end
    sound ("OVR1", "faith (sound).wav", 162.2465);    // 0.42 s past
    sound ("HAIR", "nadia (edit) 2.wav", 338.8245);   // 1.2 microseconds past
    sound ("END1", "bed.wav", 20.0);                  // to the end, its number stale
    sound ("LCK1", "clip (sound).wav", 9.5, "MOV1");  // locked: shorter than its movie by nature

    for (const auto& list : { range ("R1", "OUT1", 0.0, 5.0, 0), range ("R2", "OUT1", 12.0, 14.0, 1),
                              range ("R3", "OVR1", 0.0, 162.6625, 0),
                              range ("R4", "HAIR", 0.0, 338.82450124688285, 0),
                              range ("R5", "END1", 0.0, 25.0, 0, true),
                              range ("R6", "LCK1", 0.0, 10.0, 0) })
        for (const auto& [address, value] : list)
            rows[address] = value;

    const auto snapshot = snapshotOf (rows);
    const std::vector<model::Row> shown { cueRow ("OUT1", "media"), cueRow ("OVR1", "media"), cueRow ("HAIR", "media"),
                                          cueRow ("END1", "media"), cueRow ("LCK1", "media") };
    const auto needs = model::needsOf (snapshot, shown, model::indexObjects (snapshot));
    const auto marks = model::cueProblems (needs, model::fileProblems (snapshot, needs));

    REQUIRE (marks.count ("OUT1") == 1);
    CHECK (marks.at ("OUT1").text == "outside the file");
    CHECK (marks.at ("OUT1").meaning.find ("range 2 begins at") != std::string::npos);

    REQUIRE (marks.count ("OVR1") == 1);
    CHECK (marks.at ("OVR1").text == "past the end");
    CHECK (marks.at ("OVR1").meaning.find ("it plays to the end") != std::string::npos);

    CHECK (marks.count ("HAIR") == 0);
    CHECK (marks.count ("END1") == 0);
    CHECK (marks.count ("LCK1") == 0);
}

TEST_CASE ("problems: every cue of the show whose file is missing, in any list, for Find missing files in a folder (§100)")
{
    const auto snapshot = snapshotOf ({
        { "/godot/cue/SND1/kind", word ("media") },
        { "/godot/cue/SND1/file", word ("thunder.wav") },
        { "/godot/cue/SND1/missing", yes() },
        { "/godot/cue/SND2/kind", word ("media") },
        { "/godot/cue/SND2/file", word ("rain.wav") },
        { "/godot/cue/SND2/missing", no() },
        { "/godot/cue/MOV1/kind", word ("video") },
        { "/godot/cue/MOV1/file", word ("sub/clip.mov") },
        { "/godot/cue/MOV1/missing", yes() },
        { "/godot/cue/EMPTY/kind", word ("media") },
        { "/godot/cue/EMPTY/missing", yes() },
    });

    const auto missing = model::missingFilesOf (snapshot);

    REQUIRE (missing.size() == 2u);
    CHECK (missing[0] == std::pair<std::string, std::string> { "MOV1", "sub/clip.mov" });
    CHECK (missing[1] == std::pair<std::string, std::string> { "SND1", "thunder.wav" });
}

TEST_CASE ("problems: this computer held back by its own system is said first, and no cue wears it (§108)")
{
    /*  The author, 2026-10-22: "Is there a way to make sure the CPU on the
        mac stays on full power...?" - what no app can switch off is said
        where the show is run: Low Power Mode, and a Mac slowed for heat. */
    const auto snapshot = snapshotOf ({
        { "/godot/engine/lowPower", yes() },
        { "/godot/engine/thermal", word ("serious") },
        { "/godot/cue/M1/kind", word ("media") },
        { "/godot/audio/status", word ("stopped") },
    });

    const auto index = model::indexObjects (snapshot);
    const auto problems = model::machineProblems (snapshot, index);
    const auto pages = model::byPage (problems);

    REQUIRE (pages.size() == 2);
    CHECK (pages[0].page == page::thisComputer);
    CHECK (pages[0].icon == model::Icon::warning);
    CHECK (pages[0].sentences.size() == 2);
    CHECK (pages[1].page == page::audioInterface);

    CHECK (anySays (problems, "This computer is in Low Power Mode: its processor is capped, and a show heavy with plugins or video can fall behind - System Settings, Battery."));
    CHECK (anySays (problems, "This computer is too hot and has slowed its processor: let it cool, or move it out of the heat - nothing in Go.dot puts this right."));

    //  Every cue's at once, so none wears it: the sound cue is marked for the interface alone.
    const auto needs = model::needsOf (snapshot, { cueRow ("M1", "media") }, index);
    const auto marks = model::cueProblems (needs, problems);
    REQUIRE (marks.count ("M1") == 1);
    CHECK (marks.at ("M1").text == "no audio");

    //  Fair is the system's "slightly warm, nothing done": not a problem. Critical says so.
    const auto fair = snapshotOf ({ { "/godot/engine/thermal", word ("fair") } });
    CHECK (model::machineProblems (fair, model::indexObjects (fair)).empty());

    const auto critical = snapshotOf ({ { "/godot/engine/thermal", word ("critical") } });
    CHECK (anySays (model::machineProblems (critical, model::indexObjects (critical)),
                    "This computer is too hot and has slowed its processor hard: let it cool, or move it out of the heat - nothing in Go.dot puts this right."));
}

TEST_CASE ("problems: a Windows laptop held back says which setting caps it, and that it runs on its battery")
{
    /*  Namespace draft §108, the Windows half (2026-10-22): the engine names
        the setting, so the sentence says what to flip and where it lives;
        running on the battery is its own warning. All on "This computer",
        and no cue wears any of it. */
    const auto snapshot = snapshotOf ({
        { "/godot/engine/lowPower", yes() },
        { "/godot/engine/lowPowerBy", word ("batterySaver") },
        { "/godot/engine/onBattery", yes() },
        { "/godot/engine/thermal", word ("nominal") },
    });

    const auto index = model::indexObjects (snapshot);
    const auto problems = model::machineProblems (snapshot, index);
    const auto pages = model::byPage (problems);

    REQUIRE (pages.size() == 1);
    CHECK (pages[0].page == page::thisComputer);
    CHECK (pages[0].icon == model::Icon::warning);
    CHECK (pages[0].sentences.size() == 2);

    CHECK (anySays (problems, "This computer is in Battery saver: its processor is capped, and a show heavy with plugins or video can fall behind - Settings, Power & battery."));
    CHECK (anySays (problems, "This computer is running on its battery: it slows itself as the charge falls, and turns Battery saver on by itself - plug it in."));

    //  Each of Windows' other reasons names its own setting; a reason not known falls back to the Mac's words.
    const auto saying = [] (const char* by)
    {
        const auto one = snapshotOf ({ { "/godot/engine/lowPower", yes() }, { "/godot/engine/lowPowerBy", word (by) } });
        const auto found = model::machineProblems (one, model::indexObjects (one));
        REQUIRE (found.size() == 1);
        return found[0].sentence;
    };

    CHECK (saying ("bestPowerEfficiency") == "This computer's power mode is Best power efficiency: its processor is held back, and a show heavy with plugins or video can fall behind - Settings, Power & battery.");
    CHECK (saying ("powerSaverPlan") == "This computer is on the Power saver plan: its processor is capped, and a show heavy with plugins or video can fall behind - Control Panel, Power Options.");
    CHECK (saying ("processorLimited") == "This computer's processor is limited below its full speed by its power plan or firmware, and a show heavy with plugins or video can fall behind - Control Panel, Power Options, the plan's processor power management.");
    CHECK (saying ("lowPowerMode") == "This computer is in Low Power Mode: its processor is capped, and a show heavy with plugins or video can fall behind - System Settings, Battery.");
    CHECK (saying ("none") == saying ("lowPowerMode"));

    //  On the mains with nothing capping it: nothing said.
    const auto fine = snapshotOf ({ { "/godot/engine/lowPower", no() }, { "/godot/engine/onBattery", no() } });
    CHECK (model::machineProblems (fine, model::indexObjects (fine)).empty());
}
