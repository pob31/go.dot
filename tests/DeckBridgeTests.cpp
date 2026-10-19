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
    A STREAM DECK AND THE SHOW, WITH NO DECK (namespace draft §95).

    Which key does what on every model; what a deck's hands become - commands
    from the origin `surface:<id>`, the tablet's Cart tab's and the D700's -
    and what it is asked to show, from a snapshot written by hand. A fake host
    stands where the deck's thread would, so every case runs on a machine with
    no deck plugged in, which every CI runner is. With `WFG_SNAPSHOT_DIR` set,
    one case paints every model's keys to PNG files there, for the author to
    look at.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/command/Event.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/surface/DeckBridge.h>
#include <wfg/engine/surface/DeckLayout.h>
#include <wfg/engine/surface/DeckPainter.h>
#include <wfg/engine/surface/FaderCurve.h>
#include <wfg/engine/surface/SurfacePages.h>
#include <wfg/engine/surface/SurfaceProfile.h>
#include <wfg/engine/surface/SurfaceTable.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;
using surface::DeckRole;

namespace
{
    surface::DeckSpec specOf (std::string_view word)
    {
        const auto spec = surface::deckSpecFor (word);
        REQUIRE (spec.has_value());
        return *spec;
    }

    std::vector<DeckRole> rolesOf (const surface::DeckLayout& layout)
    {
        std::vector<DeckRole> out;

        for (const auto& key : layout.keys)
            out.push_back (key.role);

        return out;
    }

    /*  A DECK OF ITS OWN MAKING: plugged in, opened at once when wanted, and
        a notebook of what it was asked to show. */
    struct FakeHost final : surface::DeckHost
    {
        std::vector<Plugged> decks;
        std::vector<std::string> wanted;
        std::vector<surface::DeckEvent> events;
        std::map<int, surface::KeyPicture> keys;
        std::map<int, surface::ZonePicture> zones;
        std::optional<surface::ScreenPicture> shown;
        std::optional<int> bright;
        int keyWrites = 0;
        bool app = false;
        bool opens = true;

        void plug (const std::string& serial, std::string_view word)
        {
            Plugged deck;
            deck.serial = serial;
            deck.spec = specOf (word);
            decks.push_back (deck);
        }

        void unplug (const std::string& serial)
        {
            const auto was = std::find_if (decks.begin(), decks.end(),
                                           [&serial] (const Plugged& deck) { return deck.serial == serial; });

            if (was == decks.end())
                return;

            if (was->open)
            {
                surface::DeckEvent event;
                event.kind = surface::DeckEvent::Kind::closed;
                event.serial = serial;
                events.push_back (event);
            }

            decks.erase (was);
        }

        void keysDown (const std::string& serial, std::set<int> down)
        {
            surface::DeckEvent event;
            event.serial = serial;
            event.input.kind = surface::DeckInput::Kind::keys;

            for (const auto key : down)
                event.input.keys[static_cast<std::size_t> (key)] = true;

            events.push_back (event);
        }

        void input (const std::string& serial, surface::DeckInput in)
        {
            surface::DeckEvent event;
            event.serial = serial;
            event.input = in;
            events.push_back (event);
        }

        std::vector<Plugged> plugged() const override { return decks; }

        void want (const std::vector<std::string>& serials) override
        {
            wanted = serials;

            for (auto& deck : decks)
            {
                const auto asked = std::find (serials.begin(), serials.end(), deck.serial) != serials.end();

                if (asked && ! deck.open && opens)
                {
                    deck.open = true;
                    surface::DeckEvent event;
                    event.kind = surface::DeckEvent::Kind::opened;
                    event.serial = deck.serial;
                    events.push_back (event);
                }
                else if (asked && ! deck.open)
                {
                    deck.busy = true;
                }
                else if (! asked && deck.open)
                {
                    deck.open = false;
                    surface::DeckEvent event;
                    event.kind = surface::DeckEvent::Kind::closed;
                    event.serial = deck.serial;
                    events.push_back (event);
                }
            }
        }

        std::vector<surface::DeckEvent> drain() override { return std::exchange (events, {}); }

        void key (const std::string&, int index, const surface::KeyPicture& picture) override
        {
            keys[index] = picture;
            ++keyWrites;
        }

        void zone (const std::string&, int index, const surface::ZonePicture& picture) override { zones[index] = picture; }
        void screen (const std::string&, const surface::ScreenPicture& picture) override { shown = picture; }
        void brightness (const std::string&, int percent) override { bright = percent; }
        bool appRunning() const override { return app; }
        void closeApp() override { app = false; }
    };

    /*  THE TREE, AS SurfaceBridgeTests WRITES ONE BY HAND. */
    struct FakeTree
    {
        void text (const std::string& address, const std::string& value) { put (address, osc::Value::string (value), 's'); }
        void number (const std::string& address, double value) { put (address, osc::Value::float64 (value), 'd'); }
        void flag (const std::string& address, bool value) { put (address, osc::Value::boolean (value), value ? 'T' : 'F'); }

        void eqOf (const std::string& cue)
        {
            const auto base = "/godot/cue/" + cue + "/";

            for (const auto* row : doc::Schema::rowsForOwner ("sound"))
            {
                const std::string name { row->name };

                if (name.rfind ("eq", 0) != 0)
                    continue;

                if (row->type == doc::ValueType::boolean)
                    flag (base + name, row->defaultText == "true");
                else if (row->type == doc::ValueType::string)
                    text (base + name, std::string (row->defaultText));
                else
                    number (base + name, osc::parseDouble (row->defaultText).value_or (0.0));
            }
        }

        std::shared_ptr<const tree::TreeSnapshot> publish (std::int64_t at) const
        {
            auto nodes = std::make_shared<std::vector<tree::Node>> (leaves);
            std::sort (nodes->begin(), nodes->end(),
                       [] (const tree::Node& a, const tree::Node& b) { return a.address < b.address; });

            return std::make_shared<const tree::TreeSnapshot> (
                at, nodes, std::make_shared<const std::vector<tree::Node>>(), std::vector<tree::Node> {});
        }

        void put (const std::string& address, const osc::Value& value, char tag)
        {
            for (auto& existing : leaves)
                if (existing.address == address)
                {
                    existing.values = { value };
                    return;
                }

            tree::Node leaf;
            leaf.address = address;
            leaf.kind = tree::Kind::state;
            leaf.access = tree::Access::read;
            leaf.typeTags = std::string (1, tag);
            leaf.values = { value };
            leaves.push_back (std::move (leaf));
        }

        std::vector<tree::Node> leaves;
    };

    /*  A SHOW WITH A CART: a sampler bank of two clips and one OSC cue, and a
        show list whose standby is cue 1. A deck, declared and ticked. */
    struct Desk
    {
        FakeHost host;
        FakeTree fake;
        surface::SurfaceTable table;
        surface::DeckBridge bridge { host, table };
        std::vector<Event> submitted;
        std::int64_t tick = 100;
        surface::DeckSurfaceSpec spec;

        Desk (std::string_view word = "mk2", bool showKeys = false)
        {
            host.plug ("AL12K1A00001", word);

            fake.text ("/godot/list/order", "LST00001 CRT00001");
            fake.text ("/godot/list/focus", "LST00001");
            fake.text ("/godot/list/LST00001/kind", "show");
            fake.text ("/godot/list/LST00001/order", "CUE00001 GRP00001 CUE00002");
            fake.text ("/godot/list/LST00001/standby", "CUE00001");
            fake.text ("/godot/list/CRT00001/kind", "cart");
            fake.text ("/godot/list/CRT00001/name", "Cart");
            fake.text ("/godot/list/CRT00001/order", "BNK00001 OSC00001");
            fake.text ("/godot/list/doh", "");
            fake.text ("/godot/cue/BNK00001/mode", "sampler");
            fake.text ("/godot/cue/BNK00001/kind", "group");
            fake.text ("/godot/cue/BNK00001/order", "CLP00001 CLP00002");
            fake.text ("/godot/cue/CLP00001/number", "101");
            fake.text ("/godot/cue/CLP00001/name", "Rain");
            fake.text ("/godot/cue/CLP00001/colour", "#3060c0");
            fake.text ("/godot/cue/CLP00002/number", "102");
            fake.text ("/godot/cue/CLP00002/name", "");
            fake.text ("/godot/cue/CLP00002/autoName", "thunder.wav");
            fake.text ("/godot/cue/OSC00001/number", "103");
            fake.text ("/godot/cue/OSC00001/name", "Backup tracker");
            fake.text ("/godot/cue/OSC00001/kind", "osc");
            fake.text ("/godot/cue/CUE00001/number", "1");
            fake.text ("/godot/cue/CUE00001/name", "Preshow");
            fake.text ("/godot/cue/CUE00001/kind", "media");
            fake.text ("/godot/cue/GRP00001/kind", "group");
            fake.text ("/godot/cue/GRP00001/order", "CUE00003");
            fake.text ("/godot/cue/CUE00003/number", "2");
            fake.text ("/godot/cue/CUE00003/name", "Storm");
            fake.text ("/godot/cue/CUE00003/kind", "media");
            fake.text ("/godot/cue/CUE00002/number", "3");
            fake.text ("/godot/cue/CUE00002/name", "Mic");
            fake.text ("/godot/cue/CUE00002/kind", "mic");
            fake.text ("/godot/run/order", "");
            fake.text ("/godot/surface/aim", "");
            fake.text ("/godot/audio/mixes", "");

            spec.id = "SURF0001";
            spec.showKeys = showKeys;
            bridge.declare ({ spec });
            step();
        }

        void declare()
        {
            bridge.declare ({ spec });
        }

        /*  A tick: the hands, then the tree, then what the deck is shown. */
        void step()
        {
            ++tick;
            bridge.beforeTick ([this] (Event event) { submitted.push_back (std::move (event)); return true; }, tick);
            bridge.afterTick (fake.publish (tick), tick);
        }

        std::vector<std::string> commands() const
        {
            std::vector<std::string> out;

            for (const auto& event : submitted)
            {
                auto line = event.command;

                for (const auto& arg : event.args)
                    line += " " + (arg.isString() ? arg.getString() : std::to_string (arg.asDouble()));

                out.push_back (line);
            }

            return out;
        }
    };
}

//==============================================================================
TEST_CASE ("stream deck layout: a cart from the top left, the show's keys at the end of the last row")
{
    const auto mk2 = specOf ("mk2");

    const auto three = surface::deckLayoutFor (mk2, false, 3, 0);
    CHECK (three.pages == 1);
    CHECK (three.keys.size() == 15);
    CHECK (three.keys[0].role == DeckRole::pad);
    CHECK (three.keys[2].pad == 2);
    CHECK (three.keys[3].role == DeckRole::blank);
    CHECK (three.keys[14].role == DeckRole::blank);

    const auto shown = rolesOf (surface::deckLayoutFor (mk2, true, 3, 0));
    CHECK (std::vector<DeckRole> (shown.begin() + 10, shown.end())
           == std::vector<DeckRole> { DeckRole::standbyUp, DeckRole::standbyDown, DeckRole::doh, DeckRole::esc,
                                      DeckRole::go });

    //  Twenty pads and the show's keys: nine a page, the tenth key turns it.
    const auto long_ = surface::deckLayoutFor (mk2, true, 20, 1);
    CHECK (long_.pages == 3);
    CHECK (long_.page == 1);
    CHECK (long_.keys[0].pad == 9);
    CHECK (long_.keys[8].pad == 17);
    CHECK (long_.keys[9].role == DeckRole::cartPage);
    CHECK (surface::deckLayoutFor (mk2, true, 20, 3).page == 0);
    CHECK (surface::deckLayoutFor (mk2, true, 20, -1).page == 2);

    //  The last page has what is left.
    const auto last = surface::deckLayoutFor (mk2, true, 20, 2);
    CHECK (last.keys[0].pad == 18);
    CHECK (last.keys[1].pad == 19);
    CHECK (last.keys[2].role == DeckRole::blank);

    //  Smaller decks, fewer show keys; the Pedal's three are all the show's.
    CHECK (surface::deckShowKeys (specOf ("mini")) == std::vector<DeckRole> { DeckRole::esc, DeckRole::go });
    CHECK (surface::deckShowKeys (specOf ("neo")).size() == 3);
    CHECK (surface::deckShowKeys (specOf ("xl")).size() == 5);
    CHECK (rolesOf (surface::deckLayoutFor (specOf ("pedal"), true, 4, 0))
           == std::vector<DeckRole> { DeckRole::esc, DeckRole::go, DeckRole::doh });
    CHECK (rolesOf (surface::deckLayoutFor (specOf ("pedal"), false, 3, 0))
           == std::vector<DeckRole> { DeckRole::pad, DeckRole::pad, DeckRole::pad });
    CHECK (rolesOf (surface::deckLayoutFor (specOf ("pedal"), false, 4, 0))
           == std::vector<DeckRole> { DeckRole::pad, DeckRole::pad, DeckRole::cartPage });

    //  The Neo turns its pages with its touch keys: no key given up.
    const auto neo = surface::deckLayoutFor (specOf ("neo"), false, 12, 1);
    CHECK (neo.pages == 2);
    CHECK (neo.keys[0].pad == 8);
    CHECK (neo.keys[3].pad == 11);
    CHECK (neo.keys[7].role == DeckRole::blank);

    //  The Stream Deck+: pages on the top row, the cue's arrows below.
    CHECK (surface::deckShowKeys (specOf ("plus")).empty());
    CHECK (rolesOf (surface::plusLayout (false))
           == std::vector<DeckRole> { DeckRole::pageSend, DeckRole::pageEq, DeckRole::pageFx, DeckRole::pageLoop,
                                      DeckRole::aimPrevious, DeckRole::aimNext, DeckRole::controlsPrevious,
                                      DeckRole::controlsNext });
    CHECK (surface::plusLayout (true).keys[7].role == DeckRole::go);
}

TEST_CASE ("stream deck bridge: a deck plugged in is found, opened and drawn; its status says why not otherwise")
{
    Desk desk;

    CHECK (desk.host.wanted == std::vector<std::string> { "AL12K1A00001" });
    const auto status = desk.table.statusOf ("SURF0001");
    CHECK (status.connected);
    CHECK (status.serial == "AL12K1A00001");
    CHECK (status.model == "mk2");
    CHECK (status.problem.empty());

    //  The cart's pads: the bank's members, then the OSC cue.
    REQUIRE (desk.host.keys.count (0) == 1);
    CHECK (desk.host.keys[0].top == "101");
    CHECK (desk.host.keys[0].label == "Rain");
    CHECK (desk.host.keys[0].background == surface::DeckColour { 0x30, 0x60, 0xc0 });
    CHECK (desk.host.keys[1].label == "thunder.wav");
    CHECK (desk.host.keys[2].label == "Backup tracker");
    CHECK (desk.host.keys[3] == surface::KeyPicture {});
    CHECK (desk.host.bright == 70);

    //  Its keys, published for a client: what each does, then its words.
    const auto keys = desk.table.keysOf ("SURF0001");
    CHECK (keys.rfind ("cue CLP00001\t101\tRain\t\t#3060c0\t", 0) == 0);

    //  Nothing changed, nothing sent again.
    const auto writes = desk.host.keyWrites;
    desk.step();
    CHECK (desk.host.keyWrites == writes);

    //  A run of the first clip playing lights its key and says so.
    desk.fake.text ("/godot/run/order", "RUN00001");
    desk.fake.text ("/godot/run/RUN00001/cue", "CLP00001");
    desk.fake.text ("/godot/run/RUN00001/state", "playing");
    desk.step();
    CHECK (desk.host.keys[0].lit);
    CHECK (desk.host.keys[0].bottom == "playing");
    CHECK (desk.host.keyWrites == writes + 1);

    //  Unplugged: let go of, and the status says so.
    desk.host.unplug ("AL12K1A00001");
    desk.step();
    CHECK_FALSE (desk.table.statusOf ("SURF0001").connected);
    CHECK (desk.table.statusOf ("SURF0001").problem == "no Stream Deck is plugged in");

    //  A deck named that is not here; one switched off.
    desk.spec.device = "AL12K1A09999";
    desk.declare();
    desk.step();
    CHECK (desk.table.statusOf ("SURF0001").problem == "the Stream Deck AL12K1A09999 is not plugged in");

    desk.spec.enabled = false;
    desk.declare();
    desk.step();
    CHECK (desk.table.statusOf ("SURF0001").problem == "it is switched off in the show");
    CHECK (desk.host.wanted.empty());
}

TEST_CASE ("stream deck bridge: a deck Elgato's app holds says so")
{
    Desk desk;
    desk.host.unplug ("AL12K1A00001");
    desk.step();

    desk.host.opens = false;
    desk.host.app = true;
    desk.host.plug ("AL12K1A00002", "xl");
    desk.step();
    desk.step();

    const auto status = desk.table.statusOf ("SURF0001");
    CHECK_FALSE (status.connected);
    CHECK (status.model == "xl");
    CHECK (status.problem == "Elgato's Stream Deck app holds it: close the app to use it here");
}

TEST_CASE ("stream deck bridge: a pad is pressed and let go of as the Cart tab's, from the surface's origin")
{
    Desk desk;

    desk.host.keysDown ("AL12K1A00001", { 1 });
    desk.step();
    desk.host.keysDown ("AL12K1A00001", {});
    desk.step();

    CHECK (desk.commands() == std::vector<std::string> { "cue.press CLP00002", "cue.release CLP00002" });
    CHECK (desk.submitted.front().origin == "surface:SURF0001");

    //  A key held while the deck goes away is let go of.
    desk.submitted.clear();
    desk.host.keysDown ("AL12K1A00001", { 2 });
    desk.step();
    desk.host.unplug ("AL12K1A00001");
    desk.step();
    CHECK (desk.commands() == std::vector<std::string> { "cue.press OSC00001", "cue.release OSC00001" });

    //  A blank key does nothing.
    desk.submitted.clear();
    desk.host.plug ("AL12K1A00001", "mk2");
    desk.step();
    desk.host.keysDown ("AL12K1A00001", { 7 });
    desk.step();
    CHECK (desk.submitted.empty());
}

TEST_CASE ("stream deck bridge: the show's keys are GO, Esc and again, Doh! and the standby's arrows")
{
    Desk desk ("mk2", true);

    CHECK (desk.host.keys[14].label == "GO");
    CHECK (desk.host.keys[14].bottom == "1 Preshow");
    CHECK (desk.host.keys[12].label == "Doh!");
    CHECK (desk.host.keys[12].dim);

    for (const auto key : { 14, 13, 13, 12, 10, 11 })
    {
        desk.host.keysDown ("AL12K1A00001", { key });
        desk.step();
        desk.host.keysDown ("AL12K1A00001", {});
        desk.step();
    }

    CHECK (desk.commands() == std::vector<std::string> { "go", "run.stopAll", "run.killAll", "go.doh",
                                                         "standby.previous", "standby.next" });

    //  Esc again after the window is Esc again.
    for (std::int64_t n = 0; n <= surface::doubleStopTicks; ++n)
        desk.step();

    desk.submitted.clear();
    desk.host.keysDown ("AL12K1A00001", { 13 });
    desk.step();
    desk.host.keysDown ("AL12K1A00001", {});

    for (std::int64_t n = 0; n <= surface::doubleStopTicks; ++n)
        desk.step();

    desk.host.keysDown ("AL12K1A00001", { 13 });
    desk.step();
    CHECK (desk.commands() == std::vector<std::string> { "run.stopAll", "run.stopAll" });
}

TEST_CASE ("stream deck bridge: a long cart turns its pages on its last free key")
{
    Desk desk ("mini");

    //  Seven pads on a Mini: five a page and a page key.
    desk.fake.text ("/godot/list/CRT00001/order", "C0000001 C0000002 C0000003 C0000004 C0000005 C0000006 C0000007");

    for (int n = 1; n <= 7; ++n)
        desk.fake.text ("/godot/cue/C000000" + std::to_string (n) + "/name", "Pad " + std::to_string (n));

    desk.step();
    CHECK (desk.host.keys[5].label == "More");
    CHECK (desk.host.keys[5].top == "1/2");
    CHECK (desk.host.keys[0].label == "Pad 1");

    desk.host.keysDown ("AL12K1A00001", { 5 });
    desk.step();
    desk.step();
    CHECK (desk.host.keys[0].label == "Pad 6");
    CHECK (desk.host.keys[1].label == "Pad 7");
    CHECK (desk.host.keys[2] == surface::KeyPicture {});
    CHECK (desk.host.keys[5].top == "2/2");
    CHECK (desk.submitted.empty());
}

TEST_CASE ("stream deck bridge: the Stream Deck+'s dials turn the aimed cue's EQ as a D700's rotaries do")
{
    Desk desk ("plus");
    desk.fake.eqOf ("CUE00001");
    desk.step();

    //  Nothing aimed: the strip says so.
    CHECK (desk.host.zones[0].label == "No cue");
    CHECK (desk.table.pageOf ("SURF0001").word == "send");

    //  The EQ page on the aimed cue.
    desk.fake.text ("/godot/surface/aim", "CUE00001");
    desk.host.keysDown ("AL12K1A00001", { 1 });
    desk.step();
    desk.host.keysDown ("AL12K1A00001", {});
    desk.step();

    CHECK (desk.table.pageOf ("SURF0001").word == "eq");
    CHECK (desk.table.pageOf ("SURF0001").count == 4);
    CHECK (desk.host.keys[1].lit);
    CHECK (desk.host.zones[0].label == "HP freq");
    CHECK (desk.host.zones[0].title.find ("EQ 1/4 - 1 Preshow") == 0);

    //  A turn: the high-pass's frequency, by the page's own law.
    surface::DeckInput turn;
    turn.kind = surface::DeckInput::Kind::dialTurns;
    turn.detents = { 4, 0, 0, 0 };
    desk.host.input ("AL12K1A00001", turn);
    desk.step();

    REQUIRE (desk.submitted.size() == 1);
    CHECK (desk.submitted[0].command == "node.set");
    CHECK (desk.submitted[0].args[0].getString() == "/godot/cue/CUE00001/eqHpfFreq");

    const auto from = 80.0;     // the table's default
    CHECK (desk.submitted[0].args[1].asDouble()
           == doctest::Approx (surface::turned (surface::Law::frequency, from, 4, 20.0, 2000.0,
                                                surface::FaderLaw::generic)));
    CHECK (desk.table.pageOf ("SURF0001").edited == "/godot/cue/CUE00001/eqHpfFreq");

    //  A click - down and up with no turn - switches the high-pass in.
    desk.submitted.clear();
    surface::DeckInput press;
    press.kind = surface::DeckInput::Kind::dialPresses;
    press.dialsDown = { true, false, false, false };
    desk.host.input ("AL12K1A00001", press);
    press.dialsDown = { false, false, false, false };
    desk.host.input ("AL12K1A00001", press);
    desk.step();

    REQUIRE (desk.submitted.size() == 1);
    CHECK (desk.submitted[0].args[0].getString() == "/godot/cue/CUE00001/eqHpf");

    //  Held and turned, it only turns.
    desk.submitted.clear();
    press.dialsDown = { true, false, false, false };
    desk.host.input ("AL12K1A00001", press);
    turn.detents = { -1, 0, 0, 0 };
    desk.host.input ("AL12K1A00001", turn);
    press.dialsDown = { false, false, false, false };
    desk.host.input ("AL12K1A00001", press);
    desk.step();
    REQUIRE (desk.submitted.size() == 1);
    CHECK (desk.submitted[0].args[0].getString() == "/godot/cue/CUE00001/eqHpfFreq");

    //  A swipe to the left brings the next four.
    surface::DeckInput swipe;
    swipe.kind = surface::DeckInput::Kind::swipe;
    swipe.x = 600;
    swipe.toX = 300;
    desk.host.input ("AL12K1A00001", swipe);
    desk.step();
    desk.step();
    CHECK (desk.table.pageOf ("SURF0001").index == 1);
    CHECK (desk.host.zones[0].label == "B1 Q");
}

TEST_CASE ("stream deck bridge: the Send page makes a send a turn up asks for, and the arrows aim the next cue")
{
    Desk desk ("plus");
    desk.fake.text ("/godot/audio/mixes", "BUS00001 BUS00002");
    desk.fake.text ("/godot/bus/BUS00001/name", "Reverb");
    desk.fake.text ("/godot/surface/aim", "CUE00001");
    desk.fake.text ("/godot/cue/CUE00001/sends", "");
    desk.step();

    CHECK (desk.host.zones[0].label == "Reverb");
    CHECK (desk.host.zones[0].value == "no send");
    CHECK (desk.host.zones[1].label == "BUS00002");

    surface::DeckInput turn;
    turn.kind = surface::DeckInput::Kind::dialTurns;
    turn.detents = { 3, 0, 0, 0 };
    desk.host.input ("AL12K1A00001", turn);
    desk.step();

    REQUIRE (desk.submitted.size() == 1);
    CHECK (desk.submitted[0].command == "send.create");
    CHECK (desk.submitted[0].args[0].getString() == "CUE00001");
    CHECK (desk.submitted[0].args[1].getString() == "BUS00001");

    //  The next cue the list reads: into the group, then the mic cue; none past it.
    desk.submitted.clear();

    for (const auto& aimed : { "CUE00001", "CUE00003", "CUE00002" })
    {
        desk.fake.text ("/godot/surface/aim", aimed);
        desk.step();
        desk.host.keysDown ("AL12K1A00001", { 5 });
        desk.step();
        desk.host.keysDown ("AL12K1A00001", {});
        desk.step();
    }

    CHECK (desk.commands() == std::vector<std::string> { "surface.aim CUE00003", "surface.aim CUE00002" });
    CHECK (desk.host.keys[5].dim);
}

TEST_CASE ("stream deck bridge: the Neo's screen says what is next and its touch keys turn the cart")
{
    Desk desk ("neo");

    REQUIRE (desk.host.shown.has_value());
    CHECK (desk.host.shown->first == "Next: 1 Preshow");
    CHECK (desk.host.shown->second == "Cart");

    desk.fake.text ("/godot/list/CRT00001/order", "C0000001 C0000002 C0000003 C0000004 C0000005 C0000006 C0000007 "
                                                  "C0000008 C0000009");
    desk.fake.text ("/godot/cue/C0000009/name", "Ninth");
    desk.step();
    CHECK (desk.host.shown->second == "Cart 1/2");

    desk.host.keysDown ("AL12K1A00001", { 9 });     // the second touch key
    desk.step();
    desk.step();
    CHECK (desk.host.keys[0].label == "Ninth");
    CHECK (desk.host.shown->second == "Cart 2/2");
}

TEST_CASE ("stream deck painter: every model's keys drawn the deck's way")
{
    surface::KeyPicture pad;
    pad.top = "101";
    pad.label = "Rain on the roof";
    pad.bottom = "playing";
    pad.background = { 0x30, 0x60, 0xc0 };
    pad.lit = true;
    pad.progress = 400;

    const auto drawn = surface::painter::key (pad, 72);
    CHECK (drawn.width == 72);
    CHECK (drawn.height == 72);
    REQUIRE (drawn.rgb.size() == 72 * 72 * 3);

    //  A lit key's frame is white; its ground is the colour, darkened.
    CHECK (drawn.rgb[0] >= 240);
    CHECK (drawn.rgb[2] >= 240);
    const auto middle = static_cast<std::size_t> ((36 * 72 + 4) * 3);
    CHECK (drawn.rgb[middle + 2] > drawn.rgb[middle]);

    for (const auto& spec : surface::deckModels())
    {
        if (spec.image == surface::DeckImage::none)
            continue;

        const auto bytes = surface::painter::keyBytes (spec, surface::painter::key (pad, spec.keyPixels));
        REQUIRE (bytes.size() > 100);

        if (spec.image == surface::DeckImage::bmp)
            CHECK (bytes[0] == 'B');
        else
            CHECK ((bytes[0] == 0xff && bytes[1] == 0xd8));
    }

    surface::ZonePicture zone;
    zone.label = "B2 gain";
    zone.value = "+3.5 dB";
    zone.fill = 0.6;
    zone.fromCentre = true;
    zone.colour = { 0xff, 0xeb, 0x3b };
    const auto strip = surface::painter::stripBytes (specOf ("plus"), surface::painter::zone (zone, 200, 100));
    CHECK ((strip.size() > 100 && strip[0] == 0xff && strip[1] == 0xd8));

    /*  THE LOOK, for the author: every model's keys with the show's keys
        on, a cart of a few pads, and the Stream Deck+'s strip, as PNGs. */
    const auto folder = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}).toStdString();

    if (folder.empty())
        return;

    for (const auto word : { "mk2", "xl", "mini", "neo", "plus" })
    {
        Desk desk (word, true);
        desk.fake.text ("/godot/list/CRT00001/order", "BNK00001 OSC00001");
        desk.fake.text ("/godot/run/order", "RUN00001");
        desk.fake.text ("/godot/run/RUN00001/cue", "CLP00001");
        desk.fake.text ("/godot/run/RUN00001/state", "playing");

        if (std::string (word) == "plus")
        {
            desk.fake.eqOf ("CUE00001");
            desk.fake.text ("/godot/surface/aim", "CUE00001");
            desk.host.keysDown ("AL12K1A00001", { 1 });
        }

        desk.step();
        desk.step();

        const auto spec = specOf (word);
        const auto size = spec.keyPixels;
        const auto gap = size / 6;
        surface::DeckPixels sheet;
        sheet.width = std::max (spec.columns * (size + gap) + gap, spec.stripWidth + 2 * gap);
        sheet.height = spec.rows * (size + gap) + gap + (spec.stripHeight > 0 ? spec.stripHeight + gap : 0);
        sheet.rgb.assign (static_cast<std::size_t> (sheet.width * sheet.height * 3), 40);

        const auto paste = [&sheet] (const surface::DeckPixels& part, int left, int top)
        {
            for (int y = 0; y < part.height; ++y)
                for (int x = 0; x < part.width; ++x)
                    for (int c = 0; c < 3; ++c)
                        sheet.rgb[static_cast<std::size_t> (((top + y) * sheet.width + left + x) * 3 + c)]
                            = part.rgb[static_cast<std::size_t> ((y * part.width + x) * 3 + c)];
        };

        for (int key = 0; key < spec.keys; ++key)
            paste (surface::painter::key (desk.host.keys[key], size),
                   gap + (key % spec.columns) * (size + gap), gap + (key / spec.columns) * (size + gap));

        const auto stripTop = gap + spec.rows * (size + gap);

        if (spec.model == surface::DeckModel::plus)
            for (int part = 0; part < 4; ++part)
                paste (surface::painter::zone (desk.host.zones[part], 200, 100), gap + part * 200, stripTop);

        if (spec.model == surface::DeckModel::neo && desk.host.shown.has_value())
            paste (surface::painter::screen (*desk.host.shown, spec.stripWidth, spec.stripHeight), gap, stripTop);

        CHECK (surface::painter::writePng (sheet, folder + "/streamdeck-" + word + ".png"));
    }
}
