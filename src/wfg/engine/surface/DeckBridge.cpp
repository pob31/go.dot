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

#include <wfg/engine/surface/DeckBridge.h>

#include <wfg/engine/audio/EqColours.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/surface/DeckLayout.h>
#include <wfg/engine/surface/FaderCurve.h>
#include <wfg/engine/surface/SurfacePages.h>
#include <wfg/engine/surface/SurfaceProfile.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::surface
{
    namespace
    {
        using Glyph = KeyPicture::Glyph;

        constexpr int zones = 4;                    // the Stream Deck+'s dials, and its strip's parts
        constexpr int swipeAtLeast = 40;            // pixels along the strip before a touch is a swipe

        //======================================================================
        //  The tree, read where it lies (SurfaceBridge's way, and its reason).
        const std::string& noText()
        {
            static const std::string none;
            return none;
        }

        const osc::Value* soleAt (const tree::TreeSnapshot* snapshot, const std::string& address)
        {
            if (snapshot == nullptr || address.empty())
                return nullptr;

            const auto* node = snapshot->find (address);

            if (node == nullptr || node->values.size() != 1)
                return nullptr;

            return &node->values.front();
        }

        const std::string& textAt (const tree::TreeSnapshot* snapshot, const std::string& address)
        {
            const auto* value = soleAt (snapshot, address);
            return value != nullptr && value->isString() ? value->getString() : noText();
        }

        std::optional<double> numberAt (const tree::TreeSnapshot* snapshot, const std::string& address)
        {
            const auto* value = soleAt (snapshot, address);

            if (value == nullptr || ! value->isNumber() || value->isNonFinite())
                return std::nullopt;

            return value->asDouble();
        }

        bool flagAt (const tree::TreeSnapshot* snapshot, const std::string& address)
        {
            const auto* value = soleAt (snapshot, address);
            return value != nullptr && value->isBool() && value->getBool();
        }

        std::vector<std::string> wordsOf (std::string_view text)
        {
            std::vector<std::string> out;
            std::size_t at = 0;

            while (at < text.size())
            {
                const auto start = text.find_first_not_of (' ', at);

                if (start == std::string_view::npos)
                    break;

                const auto end = text.find (' ', start);
                out.emplace_back (text.substr (start, end == std::string_view::npos ? std::string_view::npos
                                                                                     : end - start));
                at = end == std::string_view::npos ? text.size() : end;
            }

            return out;
        }

        Event commandFrom (const std::string& origin, const char* name, std::vector<osc::Value> args = {})
        {
            Event event;
            event.origin = origin;
            event.command = name;
            event.args = std::move (args);
            return event;
        }

        /*  A level to a tenth of a decibel as text, digit by digit, as
            `send.create` takes it (SurfaceBridge's `tenthsText`). */
        std::string tenthsText (double decibels)
        {
            const auto tenths = std::llround (decibels * 10.0);
            const auto magnitude = tenths < 0 ? -tenths : tenths;
            return (tenths < 0 ? "-" : "") + std::to_string (magnitude / 10) + "."
                 + std::to_string (magnitude % 10);
        }

        std::optional<DeckColour> colourOfHex (std::string_view text)
        {
            if (text.size() != 7 || text.front() != '#')
                return std::nullopt;

            const auto digit = [] (char c) -> int
            {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };

            std::array<int, 6> nibbles {};

            for (std::size_t at = 0; at < 6; ++at)
                if ((nibbles[at] = digit (text[at + 1])) < 0)
                    return std::nullopt;

            return DeckColour { static_cast<std::uint8_t> (nibbles[0] * 16 + nibbles[1]),
                                static_cast<std::uint8_t> (nibbles[2] * 16 + nibbles[3]),
                                static_cast<std::uint8_t> (nibbles[4] * 16 + nibbles[5]) };
        }

        DeckColour colourOfArgb (std::uint32_t argb)
        {
            return DeckColour { static_cast<std::uint8_t> ((argb >> 16) & 0xffu),
                                static_cast<std::uint8_t> ((argb >> 8) & 0xffu),
                                static_cast<std::uint8_t> (argb & 0xffu) };
        }

        std::string hexOf (DeckColour colour)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out = "#";

            for (const auto component : { colour.red, colour.green, colour.blue })
            {
                out += digits[(component >> 4) & 0x0f];
                out += digits[component & 0x0f];
            }

            return out;
        }

        /*  WHAT A KEY DOES, as the published key list spells it (§95). */
        std::string_view roleWord (DeckRole role) noexcept
        {
            switch (role)
            {
                case DeckRole::blank:            return "";
                case DeckRole::pad:              return "cue";
                case DeckRole::learnt:           return "learnt";
                case DeckRole::cartPage:         return "cartPage";
                case DeckRole::go:               return "go";
                case DeckRole::esc:              return "esc";
                case DeckRole::doh:              return "doh";
                case DeckRole::pause:            return "pause";
                case DeckRole::standbyUp:        return "standbyUp";
                case DeckRole::standbyDown:      return "standbyDown";
                case DeckRole::pageSend:         return "send";
                case DeckRole::pageEq:           return "eq";
                case DeckRole::pageFx:           return "fx";
                case DeckRole::pageLoop:         return "loop";
                case DeckRole::aimPrevious:      return "aimPrevious";
                case DeckRole::aimNext:          return "aimNext";
                case DeckRole::controlsPrevious: return "controlsPrevious";
                case DeckRole::controlsNext:     return "controlsNext";
            }

            return "";
        }

        /*  What a going run of a cue is doing, in a word for its key; empty
            for none. A sampler member only armed is not going. */
        std::string_view goingWord (std::string_view state) noexcept
        {
            if (state == "playing")  return "playing";
            if (state == "waiting")  return "waiting";
            if (state == "stopping") return "stopping";
            if (state == "postWait") return "post-wait";
            return {};
        }

        std::string pageTitleOf (Page page)
        {
            switch (page)
            {
                case Page::send: return "Send";
                case Page::eq:   return "EQ";
                case Page::fx:   return "FX";
                case Page::loop: return "Loop";
                case Page::pan:  return "Pan";
                case Page::width: return "Width";
                case Page::show: break;
            }

            return {};
        }

        /** The pages laid out one dial a mix channel: the sends, their pans, their widths (§113). */
        constexpr bool isSendKind (Page page) noexcept
        {
            return page == Page::send || page == Page::pan || page == Page::width;
        }
    }

    //==============================================================================
    struct DeckBridge::State
    {
        /*  ONE DIAL OF THE STREAM DECK+: its detents since the last write, and
            its click - a press and a let-go with no turn between them, the
            D700's rotary press. Turned while held, it only turns. */
        struct Dial
        {
            int steps = 0;
            bool down = false;
            bool turnedWhileDown = false;
            bool clicked = false;
        };

        /*  WHAT A DIAL TURNS on the page up: SurfaceBridge's `composePage`,
            for four. */
        struct Control
        {
            int control = -1;
            std::string controlAt, switchAt, busId, busNameAt, paramAt, fxTextAt;
            std::string placementAt, onAt;      // a pan or width page: the send's shape and its switch (§113)
        };

        struct FxPage
        {
            std::string fxId, pluginId, name;
            int first = 0;
            int count = 0;
            int page = 0;
            int pages = 1;
        };

        struct Deck
        {
            DeckSurfaceSpec spec;
            std::string origin;

            std::string serial;                     // the deck this surface has tonight, or empty
            std::optional<DeckSpec> model;
            bool open = false;
            bool busy = false;

            //  The keys, as the last afterTick laid them out.
            DeckLayout layout;
            std::vector<std::string> padCues;
            int cartPage = 0;
            std::vector<bool> down;
            std::map<int, std::string> held;        // key -> the cue it pressed
            std::map<int, std::string> heldTriggers; // key -> the learnt trigger it fired (§104)

            std::vector<std::optional<KeyPicture>> shownKeys;
            std::array<std::optional<ZonePicture>, zones> shownZones {};
            std::optional<ScreenPicture> shownScreen;
            std::optional<int> brightnessSent;
            std::optional<std::int64_t> lastStop;
            std::string keysText;

            //  The Stream Deck+'s page.
            Page page = Page::send;
            int pageIndex = 0;
            int pageCount = 1;
            std::string edited;
            int moves = 0;                          // a hand's page keys and dials, counted (§101)
            std::array<Dial, zones> dials {};
            std::array<Control, zones> controls {};
            std::string madeFor;                    // what the controls were made from
            std::string fxTitle;
            bool fxNone = false;
            bool loopNone = false;
            std::string loopInAt, loopOutAt, loopLengthAt;

            void forgetShown()
            {
                shownKeys.assign (model.has_value() ? static_cast<std::size_t> (model->keys) : 0, std::nullopt);
                shownZones = {};
                shownScreen.reset();
                brightnessSent.reset();
            }
        };

        DeckHost& host;
        SurfaceTable& table;
        std::vector<Deck> decks;
        std::shared_ptr<const tree::TreeSnapshot> published;
        std::vector<DeckHost::Plugged> lastPlugged;

        /*  THE KEYS LEARNT FOR CUES (namespace draft §104): every deck trigger
            the show has, by surface and key, and every cue's first key trigger
            - the computer's or a deck's, the one the Key line shows - read
            again when the show's revision moves, never per tick. */
        struct Learnt
        {
            std::string trigger;
            std::string cue;
            bool enabled = true;
        };

        std::map<std::string, std::map<int, Learnt>> learnt;
        std::map<std::string, std::string> keyTriggerOfCue;
        double triggersRead = -1.0;

        void readTriggers()
        {
            const auto revision = numberAt (at(), "/godot/document/revision").value_or (-1.0);

            if (at() == nullptr || std::abs (revision - triggersRead) < 0.5)
                return;

            triggersRead = revision;
            learnt.clear();
            keyTriggerOfCue.clear();

            constexpr std::string_view prefix = "/godot/trigger/";
            constexpr std::string_view kindRow = "/kind";

            for (const auto* node : at()->all())
            {
                const std::string_view address = node->address;

                if (address.size() <= prefix.size() + kindRow.size()
                      || address.substr (0, prefix.size()) != prefix
                      || address.substr (address.size() - kindRow.size()) != kindRow
                      || node->values.size() != 1 || ! node->values.front().isString())
                    continue;

                const auto& kind = node->values.front().getString();

                if (kind != "key" && kind != "deck")
                    continue;

                const auto id = std::string (address.substr (prefix.size(),
                                                             address.size() - prefix.size() - kindRow.size()));
                const auto base = std::string (prefix) + id + "/";
                const auto cue = textAt (at(), base + "cue");

                keyTriggerOfCue.emplace (cue, id);

                if (kind != "deck")
                    continue;

                const auto* enabled = soleAt (at(), base + "enabled");
                Learnt entry { id, cue, enabled == nullptr || ! enabled->isBool() || enabled->getBool() };
                const auto button = static_cast<int> (numberAt (at(), base + "button").value_or (-1.0));

                if (button >= 0)
                    learnt[textAt (at(), base + "surface")].emplace (button, std::move (entry));
            }
        }

        std::vector<int> learntKeysOf (const Deck& deck) const
        {
            std::vector<int> keys;

            if (const auto found = learnt.find (deck.spec.id); found != learnt.end())
                for (const auto& [key, entry] : found->second)
                    keys.push_back (key);

            return keys;
        }

        const Learnt* learntAt (const Deck& deck, int key) const
        {
            const auto found = learnt.find (deck.spec.id);

            if (found == learnt.end())
                return nullptr;

            const auto entry = found->second.find (key);
            return entry != found->second.end() ? &entry->second : nullptr;
        }

        State (DeckHost& hostToUse, SurfaceTable& tableToUse) : host (hostToUse), table (tableToUse) {}

        const tree::TreeSnapshot* at() const { return published.get(); }

        //======================================================================
        //  The show, as the tree last said it.

        const std::string& aimNow() const
        {
            static const std::string address { "/godot/surface/aim" };
            return textAt (at(), address);
        }

        std::string cueLabel (const std::string& cue) const
        {
            const auto base = "/godot/cue/" + cue + "/";
            const auto name = textAt (at(), base + "name");
            return name.empty() ? textAt (at(), base + "autoName") : name;
        }

        std::string cueNumber (const std::string& cue) const
        {
            return textAt (at(), "/godot/cue/" + cue + "/number");
        }

        /*  THE CART A DECK SHOWS: the one it names, while that is a cart, or
            the show's first. */
        std::string cartOf (const Deck& deck) const
        {
            const auto lists = wordsOf (textAt (at(), "/godot/list/order"));
            const auto isCart = [this] (const std::string& list)
            {
                return textAt (at(), "/godot/list/" + list + "/kind") == "cart";
            };

            if (! deck.spec.cart.empty() && isCart (deck.spec.cart))
                return deck.spec.cart;

            for (const auto& list : lists)
                if (isCart (list))
                    return list;

            return {};
        }

        /*  ITS PADS, the tablet's Cart tab's way (AQQ): a cue at the cart's top
            level is a pad, a sampler group's members are pads, in order. */
        std::vector<std::string> padsOf (const std::string& cart) const
        {
            std::vector<std::string> pads;

            if (cart.empty())
                return pads;

            for (const auto& cue : wordsOf (textAt (at(), "/godot/list/" + cart + "/order")))
            {
                const auto base = "/godot/cue/" + cue + "/";

                if (textAt (at(), base + "mode") == "sampler")
                {
                    for (const auto& member : wordsOf (textAt (at(), base + "order")))
                        pads.push_back (member);
                }
                else
                {
                    pads.push_back (cue);
                }
            }

            return pads;
        }

        /*  What every going cue's run is doing, once a tick. */
        std::map<std::string, std::string> goingCues() const
        {
            std::map<std::string, std::string> going;

            for (const auto& run : wordsOf (textAt (at(), "/godot/run/order")))
            {
                const auto base = "/godot/run/" + run + "/";
                const auto word = goingWord (textAt (at(), base + "state"));

                if (! word.empty())
                    going.emplace (textAt (at(), base + "cue"), std::string (word));
            }

            return going;
        }

        /*  THE CUES THE STREAM DECK+'S ARROWS WALK: the media and mic cues of
            the list GO acts on, in the order the list reads, groups opened. */
        std::vector<std::string> aimable() const
        {
            std::vector<std::string> out;
            const auto focus = textAt (at(), "/godot/list/focus");

            if (focus.empty())
                return out;

            std::vector<std::string> pending = wordsOf (textAt (at(), "/godot/list/" + focus + "/order"));
            std::reverse (pending.begin(), pending.end());

            //  Depth first, in order, with no recursion and a bound.
            for (std::size_t visited = 0; ! pending.empty() && visited < 100000; ++visited)
            {
                const auto cue = pending.back();
                pending.pop_back();

                const auto base = "/godot/cue/" + cue + "/";
                const auto kind = textAt (at(), base + "kind");

                if (kind == "media" || kind == "mic")
                    out.push_back (cue);
                else if (kind == "group")
                {
                    auto children = wordsOf (textAt (at(), base + "order"));
                    pending.insert (pending.end(), children.rbegin(), children.rend());
                }
            }

            return out;
        }

        //======================================================================
        //  Which deck each surface has tonight.

        bool resolve()
        {
            lastPlugged = host.plugged();

            std::set<std::string> named;

            for (const auto& deck : decks)
                if (deck.spec.enabled && ! deck.spec.device.empty())
                    named.insert (deck.spec.device);

            std::set<std::string> taken;
            std::vector<std::string> wanted;
            auto changed = false;

            for (auto& deck : decks)
            {
                std::string serial;
                const DeckHost::Plugged* found = nullptr;

                if (deck.spec.enabled)
                {
                    for (const auto& plugged : lastPlugged)
                    {
                        const auto mine = deck.spec.device.empty()
                                            ? named.count (plugged.serial) == 0 && taken.count (plugged.serial) == 0
                                            : plugged.serial == deck.spec.device && taken.count (plugged.serial) == 0;

                        if (mine)
                        {
                            found = &plugged;
                            break;
                        }
                    }
                }

                if (found != nullptr)
                {
                    serial = found->serial;
                    taken.insert (serial);
                    wanted.push_back (serial);
                }

                if (serial != deck.serial)
                {
                    deck.serial = serial;
                    deck.open = false;
                    changed = true;
                }

                deck.model = found != nullptr ? std::optional<DeckSpec> (found->spec) : std::nullopt;
                deck.busy = found != nullptr && found->busy;

                //  The host says whether it is open: an event may come after.
                if (found != nullptr && ! found->open && deck.open)
                    deck.open = false;
            }

            host.want (wanted);
            return changed;
        }

        SurfaceTable::Status statusOf (const Deck& deck) const
        {
            SurfaceTable::Status status;

            if (! deck.spec.enabled)
            {
                status.problem = "it is switched off in the show";
                return status;
            }

            if (deck.serial.empty())
            {
                if (! deck.spec.device.empty())
                    status.problem = "the Stream Deck " + deck.spec.device + " is not plugged in";
                else if (lastPlugged.empty())
                    status.problem = "no Stream Deck is plugged in";
                else
                    status.problem = "every Stream Deck plugged in already carries another surface";

                return status;
            }

            status.serial = deck.serial;
            status.model = deck.model.has_value() ? std::string (deck.model->word) : std::string {};

            if (deck.open)
            {
                status.connected = true;
                return status;
            }

            if (deck.busy)
                status.problem = host.appRunning() ? "Elgato's Stream Deck app holds it: close the app to use it here"
                                                   : "another program holds it";
            else
                status.problem = "it is being opened";

            return status;
        }

        //======================================================================
        //  The hands.

        void release (Deck& deck, const Submit& submit)
        {
            for (const auto& [key, cue] : deck.held)
                submit (commandFrom (deck.origin, "cue.release", { osc::Value::string (cue) }));

            for (const auto& [key, trigger] : deck.heldTriggers)
                submit (commandFrom (deck.origin, "trigger.release", { osc::Value::string (trigger) }));

            deck.held.clear();
            deck.heldTriggers.clear();
            deck.down.clear();
            deck.dials = {};
        }

        /*  A KEY LEARNT FOR A CUE (namespace draft §104): while the Key line
            listens, the next key of a deck of keys is the cue's - a pad's key
            or an empty one, the cart flowing around it; a show key or the
            page's key is refused and the line goes on listening. The cue's key
            trigger is rewritten as this key, or made; a deck key another cue
            had is taken from it (AZG). Then the listening stops. The press
            fires nothing. */
        void learn (Deck& deck, int key, const std::string& cue, const Submit& submit)
        {
            const auto role = deck.layout.keys[static_cast<std::size_t> (key)].role;

            if (role != DeckRole::blank && role != DeckRole::pad && role != DeckRole::learnt)
                return;

            const auto button = std::to_string (key);

            if (const auto* other = learntAt (deck, key); other != nullptr && other->cue != cue)
                submit (commandFrom (deck.origin, "object.delete", { osc::Value::string (other->trigger) }));

            if (const auto found = keyTriggerOfCue.find (cue); found != keyTriggerOfCue.end())
            {
                const auto base = "/godot/trigger/" + found->second + "/";
                submit (commandFrom (deck.origin, "node.setMany",
                                     { osc::Value::string (base + "kind"), osc::Value::string ("deck"),
                                       osc::Value::string (base + "surface"), osc::Value::string (deck.spec.id),
                                       osc::Value::string (base + "button"), osc::Value::string (button),
                                       osc::Value::string (base + "key"), osc::Value::string (std::string {}) }));
            }
            else
            {
                submit (commandFrom (deck.origin, "trigger.create",
                                     { osc::Value::string (cue), osc::Value::string ("deck"),
                                       osc::Value::string (std::string {}),
                                       osc::Value::string ("surface"), osc::Value::string (deck.spec.id),
                                       osc::Value::string ("button"), osc::Value::string (button) }));
            }

            submit (commandFrom (deck.origin, "surface.learnKey", { osc::Value::string (std::string {}) }));
            table.setLearningKey ({});
        }

        void keyDown (Deck& deck, int key, const Submit& submit, std::int64_t tick)
        {
            if (key < 0 || key >= static_cast<int> (deck.layout.keys.size()))
                return;

            if (const auto cue = table.learningKey(); ! cue.empty() && deck.model.has_value() && ! hasDials (*deck.model))
            {
                learn (deck, key, cue, submit);
                return;
            }

            const auto& role = deck.layout.keys[static_cast<std::size_t> (key)];

            switch (role.role)
            {
                case DeckRole::blank:
                    break;

                /*  A LEARNT KEY IS ITS TRIGGER'S (§104): a GO from its cue, which
                    `trigger.fire` decides; let go, `trigger.release`. */
                case DeckRole::learnt:
                    if (const auto* entry = learntAt (deck, key))
                    {
                        submit (commandFrom (deck.origin, "trigger.fire", { osc::Value::string (entry->trigger) }));
                        deck.heldTriggers[key] = entry->trigger;
                    }
                    break;

                case DeckRole::pad:
                    if (role.pad >= 0 && role.pad < static_cast<int> (deck.padCues.size()))
                    {
                        const auto& cue = deck.padCues[static_cast<std::size_t> (role.pad)];
                        submit (commandFrom (deck.origin, "cue.press", { osc::Value::string (cue) }));
                        deck.held[key] = cue;
                    }
                    break;

                case DeckRole::cartPage:
                    ++deck.cartPage;
                    break;

                case DeckRole::go:
                    submit (commandFrom (deck.origin, "go"));
                    break;

                case DeckRole::esc:
                {
                    /*  ESC, AND ESC AGAIN (PRD §4.4), the D700's STOP: the second
                        inside the window is the immediate level. */
                    const auto again = deck.lastStop.has_value() && tick >= *deck.lastStop
                                         && tick - *deck.lastStop <= doubleStopTicks;
                    deck.lastStop = tick;
                    submit (commandFrom (deck.origin, again ? "run.killAll" : "run.stopAll"));
                    break;
                }

                case DeckRole::doh:
                    submit (commandFrom (deck.origin, "go.doh"));
                    break;

                /*  PAUSE / PLAY (PRD §3.34): what the key says, read off the
                    tree it was drawn from - two commands, never a toggle. */
                case DeckRole::pause:
                    submit (commandFrom (deck.origin, flagAt (at(), "/godot/engine/paused") ? "run.playAll"
                                                                                            : "run.pauseAll"));
                    break;

                case DeckRole::standbyUp:
                    submit (commandFrom (deck.origin, "standby.previous"));
                    break;

                case DeckRole::standbyDown:
                    submit (commandFrom (deck.origin, "standby.next"));
                    break;

                case DeckRole::pageSend: nextSendPart (deck); break;
                case DeckRole::pageEq:   turnPage (deck, Page::eq);   break;
                case DeckRole::pageFx:   turnPage (deck, Page::fx);   break;
                case DeckRole::pageLoop: turnPage (deck, Page::loop); break;

                case DeckRole::aimPrevious:
                    aimStep (deck, -1, submit);
                    break;

                case DeckRole::aimNext:
                    aimStep (deck, 1, submit);
                    break;

                case DeckRole::controlsPrevious:
                    showControls (deck, deck.pageIndex - 1);
                    break;

                case DeckRole::controlsNext:
                    showControls (deck, deck.pageIndex + 1);
                    break;
            }
        }

        void keyUp (Deck& deck, int key, const Submit& submit)
        {
            if (const auto trigger = deck.heldTriggers.find (key); trigger != deck.heldTriggers.end())
            {
                submit (commandFrom (deck.origin, "trigger.release", { osc::Value::string (trigger->second) }));
                deck.heldTriggers.erase (trigger);
            }

            const auto found = deck.held.find (key);

            if (found == deck.held.end())
                return;

            submit (commandFrom (deck.origin, "cue.release", { osc::Value::string (found->second) }));
            deck.held.erase (found);
        }

        void input (Deck& deck, const DeckInput& in, const Submit& submit, std::int64_t tick)
        {
            if (! deck.model.has_value())
                return;

            const auto& spec = *deck.model;

            switch (in.kind)
            {
                case DeckInput::Kind::none:
                    break;

                case DeckInput::Kind::keys:
                {
                    const auto count = static_cast<std::size_t> (spec.keys + spec.touchKeys);
                    deck.down.resize (count, false);

                    for (std::size_t key = 0; key < count; ++key)
                    {
                        const auto now = in.keys[key];

                        if (now == deck.down[key])
                            continue;

                        deck.down[key] = now;
                        const auto index = static_cast<int> (key);

                        //  The Neo's touch keys turn the cart's page.
                        if (index >= spec.keys)
                        {
                            if (now)
                                deck.cartPage += index == spec.keys ? -1 : 1;

                            continue;
                        }

                        if (now)
                            keyDown (deck, index, submit, tick);
                        else
                            keyUp (deck, index, submit);
                    }

                    break;
                }

                case DeckInput::Kind::dialTurns:
                    for (std::size_t dial = 0; dial < static_cast<std::size_t> (zones); ++dial)
                    {
                        auto& held = deck.dials[dial];
                        held.steps += in.detents[dial];

                        if (held.down && in.detents[dial] != 0)
                            held.turnedWhileDown = true;
                    }
                    break;

                case DeckInput::Kind::dialPresses:
                    for (std::size_t dial = 0; dial < static_cast<std::size_t> (zones); ++dial)
                    {
                        auto& held = deck.dials[dial];
                        const auto now = in.dialsDown[dial];

                        if (now && ! held.down)
                        {
                            held.down = true;
                            held.turnedWhileDown = false;
                        }
                        else if (! now && held.down)
                        {
                            held.down = false;

                            if (! held.turnedWhileDown)
                                held.clicked = true;
                        }
                    }
                    break;

                /*  A SWIPE MOVES ALONG THE PAGE'S CONTROLS: the finger to the
                    left brings the next four, as a page is turned. */
                case DeckInput::Kind::swipe:
                {
                    const auto along = in.toX - in.x;

                    if (along <= -swipeAtLeast)
                        showControls (deck, deck.pageIndex + 1);
                    else if (along >= swipeAtLeast)
                        showControls (deck, deck.pageIndex - 1);

                    break;
                }

                case DeckInput::Kind::tap:
                case DeckInput::Kind::longTap:
                    break;
            }
        }

        //======================================================================
        //  The Stream Deck+'s pages: SurfaceBridge's laws, for four dials.

        std::vector<FxPage> fxPagesOf (const std::string& aim) const
        {
            std::vector<FxPage> out;

            if (aim.empty())
                return out;

            for (const auto& fxId : wordsOf (textAt (at(), "/godot/cue/" + aim + "/fx")))
            {
                const auto pluginId = textAt (at(), "/godot/fx/" + fxId + "/plugin");
                const auto params = static_cast<int> (numberAt (at(), "/godot/plugin/" + pluginId + "/paramCount").value_or (0.0));
                const auto pages = pageCount (params, zones);

                for (int page = 0; page < pages; ++page)
                    out.push_back ({ fxId, pluginId, textAt (at(), "/godot/fx/" + fxId + "/name"),
                                     page * zones, std::clamp (params - page * zones, 0, zones), page, pages });
            }

            return out;
        }

        int countOf (const Deck& deck) const
        {
            switch (deck.page)
            {
                case Page::eq:   return deckEqPageCount;
                case Page::send:
                case Page::pan:
                case Page::width:
                    return pageCount (static_cast<int> (wordsOf (textAt (at(), "/godot/audio/mixes")).size()), zones);
                case Page::fx:   return std::max (1, static_cast<int> (fxPagesOf (aimNow()).size()));
                case Page::loop:
                case Page::show: break;
            }

            return 1;
        }

        /*  A PAGE KEY PRESSED: that page, or its next four again when it is up
            - round to its first after its last. */
        void turnPage (Deck& deck, Page kind)
        {
            if (deck.page != kind)
            {
                deck.page = kind;
                deck.pageIndex = 0;
            }
            else
            {
                deck.pageIndex = deck.pageIndex + 1 < countOf (deck) ? deck.pageIndex + 1 : 0;
            }

            deck.edited.clear();
            deck.madeFor.clear();
            deck.dials = {};
            ++deck.moves;
        }

        /*  WHETHER THE AIMED CUE HAS A SEND TWO CHANNELS WIDE INTO TWO (§113):
            the one shape with a width to turn. */
        bool hasStereoSend (const std::string& aim) const
        {
            for (const auto& sendId : wordsOf (textAt (at(), "/godot/cue/" + aim + "/sends")))
                if (textAt (at(), "/godot/send/" + sendId + "/placement") == "stereo")
                    return true;

            return false;
        }

        /*  THE SEND KEY WALKS ON (namespace draft §113, the author's pick, no
            key being free): the sends' levels four a page, then their pans,
            then - when the aimed cue has a send two channels wide into two -
            their widths, and round to the levels again. The key's picture
            says which part is up. */
        void nextSendPart (Deck& deck)
        {
            if (! isSendKind (deck.page))
            {
                turnPage (deck, Page::send);
                return;
            }

            if (deck.pageIndex + 1 < countOf (deck))
            {
                turnPage (deck, deck.page);
                return;
            }

            const auto next = deck.page == Page::send                                 ? Page::pan
                            : deck.page == Page::pan && hasStereoSend (aimNow())      ? Page::width
                                                                                      : Page::send;
            turnPage (deck, next);
        }

        void showControls (Deck& deck, int index)
        {
            const auto count = std::max (1, countOf (deck));
            const auto next = ((index % count) + count) % count;

            if (next == deck.pageIndex)
                return;

            deck.pageIndex = next;
            deck.madeFor.clear();
            deck.dials = {};
            ++deck.moves;
        }

        void aimStep (const Deck& deck, int by, const Submit& submit) const
        {
            const auto cues = aimable();

            if (cues.empty())
                return;

            const auto& current = aimNow();
            const auto found = std::find (cues.begin(), cues.end(), current);
            std::size_t next = 0;

            if (found == cues.end())
                next = by > 0 ? 0 : cues.size() - 1;
            else
            {
                const auto index = static_cast<std::size_t> (found - cues.begin());

                //  At an end it stays: the arrow says so by being dim.
                if ((by < 0 && index == 0) || (by > 0 && index + 1 >= cues.size()))
                    return;

                next = by > 0 ? index + 1 : index - 1;
            }

            submit (commandFrom (deck.origin, "surface.aim", { osc::Value::string (cues[next]) }));
        }

        /*  THE CONTROLS UNDER THE DIALS, made again when the page, its index,
            the aim, the mixes, the sends or the inserts move. */
        void compose (Deck& deck)
        {
            const auto& aim = aimNow();
            const auto base = aim.empty() ? std::string {} : "/godot/cue/" + aim + "/";
            const auto mixes = textAt (at(), "/godot/audio/mixes");
            const auto sends = base.empty() ? std::string {} : textAt (at(), base + "sends");
            const auto fxPages = deck.page == Page::fx ? fxPagesOf (aim) : std::vector<FxPage> {};

            std::string key = aim + "|" + pageTitleOf (deck.page) + "|" + std::to_string (deck.pageIndex) + "|";

            if (isSendKind (deck.page))
                key += mixes + "|" + sends;

            //  A pan or width page is made again when a send's shape moves (§113).
            if (deck.page == Page::pan || deck.page == Page::width)
                for (const auto& sendId : wordsOf (sends))
                    key += ":" + textAt (at(), "/godot/send/" + sendId + "/placement");

            for (const auto& page : fxPages)
                key += page.fxId + ':' + std::to_string (page.first + page.count) + ' ';

            if (deck.page == Page::loop && ! base.empty())
            {
                const auto channel = textAt (at(), base + "channel");
                key += "loop:" + channel + ":" + textAt (at(), "/godot/slot/" + channel + "/takeSeconds");
            }

            deck.pageCount = countOf (deck);

            if (deck.pageIndex >= deck.pageCount)
                deck.pageIndex = 0;

            if (key == deck.madeFor)
                return;

            deck.madeFor = key;
            deck.controls = {};
            deck.fxTitle.clear();
            deck.fxNone = false;
            deck.loopNone = false;
            deck.loopInAt.clear();
            deck.loopOutAt.clear();
            deck.loopLengthAt.clear();

            if (base.empty())
                return;

            if (isSendKind (deck.page))
            {
                std::map<std::string, std::string> sendInto;

                for (const auto& sendId : wordsOf (sends))
                    sendInto[textAt (at(), "/godot/send/" + sendId + "/bus")] = sendId;

                const auto channels = wordsOf (mixes);

                for (int position = 0; position < zones; ++position)
                {
                    const auto index = deck.pageIndex * zones + position;

                    if (index >= static_cast<int> (channels.size()))
                        break;

                    auto& control = deck.controls[static_cast<std::size_t> (position)];
                    control.control = index;
                    control.busId = channels[static_cast<std::size_t> (index)];
                    control.busNameAt = "/godot/bus/" + control.busId + "/name";

                    if (const auto found = sendInto.find (control.busId); found != sendInto.end())
                    {
                        const auto sendBase = "/godot/send/" + found->second + "/";

                        if (deck.page == Page::send)
                        {
                            control.controlAt = sendBase + "level";
                            control.switchAt = sendBase + "on";
                            continue;
                        }

                        /*  A PAN OR WIDTH PAGE (namespace draft §113), as the
                            D700's: the row the send's shape hears, the swap
                            under a width's press, nothing where the shape has
                            no such row. */
                        const auto shapeWord = textAt (at(), sendBase + "placement");
                        control.placementAt = sendBase + "placement";
                        control.onAt = sendBase + "on";

                        if (deck.page == Page::pan)
                        {
                            if (shapeWord == "pan")
                                control.controlAt = sendBase + "pan";
                            else if (shapeWord == "stereo" || shapeWord == "fold")
                                control.controlAt = sendBase + "balance";
                        }
                        else if (shapeWord == "stereo")
                        {
                            control.controlAt = sendBase + "width";
                            control.switchAt = sendBase + "swap";
                        }
                    }
                }

                return;
            }

            if (deck.page == Page::fx)
            {
                if (fxPages.empty())
                {
                    deck.fxNone = true;
                    return;
                }

                const auto& page = fxPages[static_cast<std::size_t> (std::clamp (deck.pageIndex, 0,
                                                                              static_cast<int> (fxPages.size()) - 1))];
                deck.fxTitle = page.name.empty() ? page.pluginId : page.name;

                if (page.pages > 1)
                    deck.fxTitle += " " + std::to_string (page.page + 1) + "/" + std::to_string (page.pages);

                for (int position = 0; position < page.count && position < zones; ++position)
                {
                    const auto parameter = page.first + position;
                    auto& control = deck.controls[static_cast<std::size_t> (position)];
                    control.control = parameter;
                    control.controlAt = "/godot/fx/" + page.fxId + "/p" + std::to_string (parameter);
                    control.fxTextAt = "/godot/fx/" + page.fxId + "/t" + std::to_string (parameter);
                    control.paramAt = "/godot/plugin/" + page.pluginId + "/param/" + std::to_string (parameter) + "/";
                }

                return;
            }

            if (deck.page == Page::loop)
            {
                const auto channel = textAt (at(), base + "channel");
                const auto slot = "/godot/slot/" + channel + "/";

                if (channel.empty() || ! (numberAt (at(), slot + "takeSeconds").value_or (0.0) > 0.0))
                {
                    deck.loopNone = true;
                    return;
                }

                deck.loopInAt = slot + "loopIn";
                deck.loopOutAt = slot + "loopOut";
                deck.loopLengthAt = slot + "takeLength";

                for (int position = 0; position < loopControlCount && position < zones; ++position)
                {
                    auto& control = deck.controls[static_cast<std::size_t> (position)];
                    control.control = position;
                    control.controlAt = position == 0 ? deck.loopInAt
                                      : position == 1 ? deck.loopOutAt
                                      : position == 3 ? base + "level"
                                                      : std::string {};
                }

                return;
            }

            //  A band a page, the cuts first (`deckEqPages`), not the D700's run of sixteen.
            const auto& slots = deckEqPages[static_cast<std::size_t> (std::clamp (deck.pageIndex, 0, deckEqPageCount - 1))];

            for (int position = 0; position < zones && position < deckEqDials; ++position)
            {
                const auto index = slots[static_cast<std::size_t> (position)];

                if (index < 0 || index >= eqControlCount)
                    continue;

                const auto& eq = eqControls[static_cast<std::size_t> (index)];
                auto& control = deck.controls[static_cast<std::size_t> (position)];
                control.control = index;
                control.controlAt = base + std::string (eq.row);
                control.switchAt = base + std::string (eq.switchRow);
            }
        }

        void write (Deck& deck, const Submit& submit, const std::string& address, osc::Value value) const
        {
            submit (commandFrom (deck.origin, "node.set", { osc::Value::string (address), std::move (value) }));
            deck.edited = address;
        }

        void eqWrite (Deck& deck, const Control& control, int steps, bool pressed, const Submit& submit) const
        {
            const auto& eq = eqControls[static_cast<std::size_t> (control.control)];

            if (pressed)
            {
                if (eq.press == Press::toggleSwitch && soleAt (at(), control.switchAt) != nullptr)
                    write (deck, submit, control.switchAt, osc::Value::boolean (! flagAt (at(), control.switchAt)));
                else if (eq.press == Press::toggleShape && soleAt (at(), control.controlAt) != nullptr)
                    write (deck, submit, control.controlAt,
                           osc::Value::string (std::string (pressedShape (textAt (at(), control.controlAt), eq.shelf))));
            }

            if (steps == 0)
                return;

            if (eq.law == Law::shape)
            {
                const auto& shape = textAt (at(), control.controlAt);
                const auto next = turnedShape (shape, steps, eq.shelf);

                if (soleAt (at(), control.controlAt) != nullptr && next != shape)
                    write (deck, submit, control.controlAt, osc::Value::string (std::string (next)));

                return;
            }

            const auto current = numberAt (at(), control.controlAt);

            if (! current.has_value())
                return;

            const auto next = turned (eq.law, *current, steps, eq.minimum, eq.maximum, FaderLaw::generic);

            if (std::abs (next - *current) >= 1.0e-9)
                write (deck, submit, control.controlAt, osc::Value::float64 (next));
        }

        void sendWrite (Deck& deck, const Control& control, int steps, bool pressed, const Submit& submit) const
        {
            if (control.controlAt.empty())
            {
                if (! pressed && steps <= 0)
                    return;

                const auto level = pressed ? 0.0
                                           : turned (Law::level, faderSilenceDb, steps, faderSilenceDb,
                                                     faderLoudestDb, FaderLaw::generic);

                submit (commandFrom (deck.origin, "send.create",
                                     { osc::Value::string (aimNow()), osc::Value::string (control.busId),
                                       osc::Value::string (std::string {}), osc::Value::string (tenthsText (level)) }));
                deck.edited = "/godot/cue/" + aimNow() + "/sends";
                return;
            }

            if (pressed && soleAt (at(), control.switchAt) != nullptr)
                write (deck, submit, control.switchAt, osc::Value::boolean (! flagAt (at(), control.switchAt)));

            if (steps == 0)
                return;

            const auto current = numberAt (at(), control.controlAt);

            if (! current.has_value())
                return;

            const auto next = turned (Law::level, *current, steps, faderSilenceDb, faderLoudestDb, FaderLaw::generic);

            if (std::abs (next - *current) >= 1.0e-9)
                write (deck, submit, control.controlAt, osc::Value::float64 (next));
        }

        /*  A SEND'S PLACE ON A DIAL (namespace draft §113): a pan or a balance
            turned two hundredths a detent and clicked back to the centre; a
            width turned the same and clicked to swap the sides. A mix the cue
            does not send to, or a shape with no such row, turns nothing. */
        void placeWrite (Deck& deck, const Control& control, int steps, bool pressed, const Submit& submit) const
        {
            if (control.controlAt.empty())
                return;

            const auto onWidth = deck.page == Page::width;
            const auto current = numberAt (at(), control.controlAt);

            if (pressed)
            {
                if (onWidth)
                {
                    if (soleAt (at(), control.switchAt) != nullptr)
                        write (deck, submit, control.switchAt, osc::Value::boolean (! flagAt (at(), control.switchAt)));
                }
                else if (current.has_value() && std::abs (*current) > 1.0e-9)
                {
                    write (deck, submit, control.controlAt, osc::Value::float64 (0.0));
                }
            }

            if (steps == 0 || ! current.has_value())
                return;

            const auto next = onWidth
                                ? turned (Law::stereoWidth, *current, steps, placeWidthMin, placeWidthMax, FaderLaw::generic)
                                : turned (Law::pan, *current, steps, placeLeanMin, placeLeanMax, FaderLaw::generic);

            if (std::abs (next - *current) >= 1.0e-9)
                write (deck, submit, control.controlAt, osc::Value::float64 (next));
        }

        void fxWrite (Deck& deck, const Control& control, int steps, bool pressed, const Submit& submit) const
        {
            const auto current = numberAt (at(), control.controlAt);

            if (! current.has_value())
                return;

            auto next = *current;

            if (pressed)
                next = std::clamp (numberAt (at(), control.paramAt + "default").value_or (next), 0.0, 1.0);

            if (steps != 0)
                next = turnedParameter (next, steps, static_cast<int> (numberAt (at(), control.paramAt + "steps").value_or (0.0)));

            if (std::abs (next - *current) >= 1.0e-9)
                write (deck, submit, control.controlAt, osc::Value::float64 (next));
        }

        void loopWrite (Deck& deck, const Control& control, int steps, bool pressed, const Submit& submit) const
        {
            const auto toMillisecond = [] (double seconds) { return std::round (seconds * 1000.0) / 1000.0; };

            const auto in = numberAt (at(), deck.loopInAt).value_or (0.0);
            const auto out = numberAt (at(), deck.loopOutAt).value_or (0.0);
            const auto length = numberAt (at(), deck.loopLengthAt).value_or (0.0);

            switch (static_cast<LoopControl> (control.control))
            {
                case LoopControl::in:
                {
                    const auto next = pressed ? 0.0 : loopPointTurned (in, steps);

                    if (std::abs (next - in) >= 1.0e-9)
                        write (deck, submit, deck.loopInAt, osc::Value::float64 (next));

                    break;
                }

                case LoopControl::out:
                {
                    const auto next = pressed ? length : loopPointTurned (out, steps);

                    if (std::abs (next - out) >= 1.0e-9)
                        write (deck, submit, deck.loopOutAt, osc::Value::float64 (next));

                    break;
                }

                case LoopControl::slide:
                {
                    if (steps == 0)
                        break;

                    const auto by = std::clamp (loopPointTurned (in, steps) - in, -in, std::max (0.0, length - out));

                    if (std::abs (by) < 1.0e-9)
                        break;

                    const auto& first = by > 0.0 ? deck.loopOutAt : deck.loopInAt;
                    const auto& second = by > 0.0 ? deck.loopInAt : deck.loopOutAt;

                    write (deck, submit, first, osc::Value::float64 (toMillisecond ((by > 0.0 ? out : in) + by)));
                    write (deck, submit, second, osc::Value::float64 (toMillisecond ((by > 0.0 ? in : out) + by)));
                    break;
                }

                case LoopControl::level:
                {
                    const auto current = numberAt (at(), control.controlAt);

                    if (! current.has_value())
                        break;

                    const auto next = pressed ? 0.0
                                              : turned (Law::level, *current, steps, faderSilenceDb, faderLoudestDb,
                                                        FaderLaw::generic);

                    if (std::abs (next - *current) >= 1.0e-9)
                        write (deck, submit, control.controlAt, osc::Value::float64 (next));

                    break;
                }
            }
        }

        /*  THE TICK'S DIAL WRITES, one per dial that turned or clicked: the
            control under it turned by its law from what the tree last said,
            or switched - SurfaceBridge's `flushPageWrites`, word for word. */
        void flushDials (Deck& deck, const Submit& submit)
        {
            if (! deck.open || ! deck.model.has_value() || ! hasDials (*deck.model))
                return;

            if (aimNow().empty())
            {
                for (auto& dial : deck.dials)
                {
                    dial.steps = 0;
                    dial.clicked = false;
                }

                return;
            }

            compose (deck);

            for (std::size_t position = 0; position < static_cast<std::size_t> (zones); ++position)
            {
                auto& dial = deck.dials[position];
                const auto steps = std::exchange (dial.steps, 0);
                const auto pressed = std::exchange (dial.clicked, false);
                const auto& control = deck.controls[position];

                if ((steps == 0 && ! pressed) || control.control < 0)
                    continue;

                ++deck.moves;

                switch (deck.page)
                {
                    case Page::eq:   eqWrite (deck, control, steps, pressed, submit);   break;
                    case Page::send: sendWrite (deck, control, steps, pressed, submit); break;
                    case Page::pan:
                    case Page::width: placeWrite (deck, control, steps, pressed, submit); break;
                    case Page::fx:   fxWrite (deck, control, steps, pressed, submit);   break;
                    case Page::loop: loopWrite (deck, control, steps, pressed, submit); break;
                    case Page::show: break;
                }
            }
        }

        //======================================================================
        //  The pictures.

        KeyPicture padPicture (const std::string& cue, const std::map<std::string, std::string>& going) const
        {
            KeyPicture picture;
            const auto base = "/godot/cue/" + cue + "/";

            picture.top = cueNumber (cue);
            picture.label = cueLabel (cue);
            picture.background = colourOfHex (textAt (at(), base + "colour")).value_or (DeckColour {});

            if (const auto found = going.find (cue); found != going.end())
            {
                picture.lit = true;
                picture.bottom = found->second;
            }
            else if (const auto* enabled = soleAt (at(), base + "enabled"); enabled != nullptr && enabled->isBool()
                                                                               && ! enabled->getBool())
            {
                picture.dim = true;
                picture.bottom = "off";
            }

            return picture;
        }

        KeyPicture showKeyPicture (DeckRole role) const
        {
            KeyPicture picture;

            switch (role)
            {
                case DeckRole::go:
                {
                    picture.glyph = Glyph::go;
                    picture.label = "GO";

                    const auto focus = textAt (at(), "/godot/list/focus");
                    const auto standby = focus.empty() ? noText() : textAt (at(), "/godot/list/" + focus + "/standby");

                    if (standby.empty())
                    {
                        picture.bottom = "nothing next";
                        picture.dim = true;
                    }
                    else
                    {
                        const auto number = cueNumber (standby);
                        picture.bottom = number.empty() ? cueLabel (standby) : number + " " + cueLabel (standby);
                    }

                    break;
                }

                case DeckRole::esc:
                    picture.glyph = Glyph::esc;
                    picture.label = "Esc";
                    picture.bottom = "twice: all";
                    break;

                case DeckRole::doh:
                    picture.glyph = Glyph::doh;
                    picture.label = "Doh!";

                    if (textAt (at(), "/godot/list/doh").empty())
                    {
                        picture.dim = true;
                        picture.bottom = "nothing to undo";
                    }

                    break;

                /*  ITS WORD THE STATE'S (§4.8): "Pause" while the show runs,
                    "Play" and "PAUSED" while it is held. */
                case DeckRole::pause:
                    picture.glyph = Glyph::pause;

                    if (flagAt (at(), "/godot/engine/paused"))
                    {
                        picture.label = "Play";
                        picture.bottom = "PAUSED";
                    }
                    else
                    {
                        picture.label = "Pause";
                        picture.bottom = "hold all";
                    }

                    break;

                case DeckRole::standbyUp:
                    picture.glyph = Glyph::standbyUp;
                    picture.label = "Standby";
                    break;

                case DeckRole::standbyDown:
                    picture.glyph = Glyph::standbyDown;
                    picture.label = "Standby";
                    break;

                case DeckRole::blank:
                case DeckRole::pad:
                case DeckRole::learnt:
                case DeckRole::cartPage:
                case DeckRole::pageSend:
                case DeckRole::pageEq:
                case DeckRole::pageFx:
                case DeckRole::pageLoop:
                case DeckRole::aimPrevious:
                case DeckRole::aimNext:
                case DeckRole::controlsPrevious:
                case DeckRole::controlsNext:
                    break;
            }

            return picture;
        }

        KeyPicture pictureFor (const Deck& deck, const DeckKey& key, const std::map<std::string, std::string>& going) const
        {
            switch (key.role)
            {
                case DeckRole::blank:
                    return {};

                case DeckRole::pad:
                    if (key.pad >= 0 && key.pad < static_cast<int> (deck.padCues.size()))
                        return padPicture (deck.padCues[static_cast<std::size_t> (key.pad)], going);

                    return {};

                /*  A LEARNT KEY WEARS ITS CUE, as a pad does - and says "off"
                    while its trigger is switched off (§104). */
                case DeckRole::learnt:
                    if (const auto* entry = learntAt (deck, key.pad))
                    {
                        auto picture = padPicture (entry->cue, going);

                        if (! entry->enabled)
                        {
                            picture.dim = true;
                            picture.lit = false;
                            picture.bottom = "off";
                        }

                        return picture;
                    }

                    return {};

                case DeckRole::cartPage:
                {
                    KeyPicture picture;
                    picture.glyph = Glyph::pageNext;
                    picture.label = "More";
                    picture.top = std::to_string (deck.layout.page + 1) + "/" + std::to_string (deck.layout.pages);
                    return picture;
                }

                case DeckRole::go:
                case DeckRole::esc:
                case DeckRole::doh:
                case DeckRole::pause:
                case DeckRole::standbyUp:
                case DeckRole::standbyDown:
                    return showKeyPicture (key.role);

                case DeckRole::pageSend:
                case DeckRole::pageEq:
                case DeckRole::pageFx:
                case DeckRole::pageLoop:
                {
                    const auto page = key.role == DeckRole::pageSend ? Page::send
                                    : key.role == DeckRole::pageEq   ? Page::eq
                                    : key.role == DeckRole::pageFx   ? Page::fx
                                                                     : Page::loop;
                    /*  THE SEND KEY IS LIT FOR THE PANS AND WIDTHS TOO (§113), and
                        says which part is up. */
                    const auto sendPart = key.role == DeckRole::pageSend
                                            && (deck.page == Page::pan || deck.page == Page::width);
                    KeyPicture picture;
                    picture.glyph = key.role == DeckRole::pageSend ? Glyph::pageSend
                                  : key.role == DeckRole::pageEq   ? Glyph::pageEq
                                  : key.role == DeckRole::pageFx   ? Glyph::pageFx
                                                                   : Glyph::pageLoop;
                    picture.label = pageTitleOf (sendPart ? deck.page : page);
                    picture.lit = deck.page == page || sendPart;

                    if (picture.lit && deck.pageCount > 1)
                        picture.top = std::to_string (deck.pageIndex + 1) + "/" + std::to_string (deck.pageCount);

                    return picture;
                }

                case DeckRole::aimPrevious:
                case DeckRole::aimNext:
                {
                    KeyPicture picture;
                    picture.glyph = key.role == DeckRole::aimPrevious ? Glyph::aimPrevious : Glyph::aimNext;
                    picture.label = key.role == DeckRole::aimPrevious ? "Prev cue" : "Next cue";

                    const auto cues = aimable();
                    const auto& aim = aimNow();
                    const auto found = std::find (cues.begin(), cues.end(), aim);

                    if (cues.empty())
                        picture.dim = true;
                    else if (found != cues.end())
                    {
                        const auto index = found - cues.begin();
                        picture.dim = key.role == DeckRole::aimPrevious ? index == 0
                                                                        : index + 1 >= static_cast<std::ptrdiff_t> (cues.size());
                    }

                    return picture;
                }

                case DeckRole::controlsPrevious:
                case DeckRole::controlsNext:
                {
                    KeyPicture picture;
                    picture.glyph = key.role == DeckRole::controlsPrevious ? Glyph::controlsPrevious : Glyph::controlsNext;
                    picture.label = "More";
                    picture.dim = deck.pageCount <= 1;
                    return picture;
                }
            }

            return {};
        }

        std::vector<ZonePicture> zonePictures (const Deck& deck) const
        {
            std::vector<ZonePicture> out (static_cast<std::size_t> (zones));
            const auto& aim = aimNow();

            std::string title = pageTitleOf (deck.page);

            if (deck.pageCount > 1)
                title += " " + std::to_string (deck.pageIndex + 1) + "/" + std::to_string (deck.pageCount);

            if (aim.empty())
            {
                out[0].title = title;
                out[0].label = "No cue";
                out[0].value = "pick one";
                return out;
            }

            const auto number = cueNumber (aim);
            title += " - " + (number.empty() ? std::string {} : number + " ") + cueLabel (aim);
            out[0].title = title;

            const auto cueColour = colourOfHex (textAt (at(), "/godot/cue/" + aim + "/colour")).value_or (DeckColour {});

            if (deck.page == Page::fx && deck.fxNone)
            {
                out[0].label = "No FX";
                return out;
            }

            if (deck.page == Page::loop && deck.loopNone)
            {
                out[0].label = "No take";
                return out;
            }

            if (deck.page == Page::fx && ! deck.fxTitle.empty())
                out[1].title = deck.fxTitle;

            for (std::size_t position = 0; position < static_cast<std::size_t> (zones); ++position)
            {
                const auto& control = deck.controls[position];
                auto& zone = out[position];

                if (control.control < 0)
                    continue;

                std::string value;

                switch (deck.page)
                {
                    case Page::eq:
                    {
                        const auto& eq = eqControls[static_cast<std::size_t> (control.control)];
                        zone.label = std::string (eq.label);
                        zone.colour = colourOfArgb (audio::eqColours[static_cast<std::size_t> (std::clamp (eq.colour, 0, 5))]);
                        zone.off = soleAt (at(), control.switchAt) != nullptr && ! flagAt (at(), control.switchAt);

                        if (eq.law == Law::shape)
                        {
                            const auto& shape = textAt (at(), control.controlAt);
                            zone.value = std::string (shapeText (shape, false));
                            zone.fill = shape == "peak" || shape.empty() ? 0.0 : 1.0;
                        }
                        else if (const auto amount = numberAt (at(), control.controlAt))
                        {
                            valueText (eq.law, *amount, false, value);
                            zone.value = value;
                            const auto ring = d700RingFor (eq.law, *amount, eq.minimum, eq.maximum, FaderLaw::generic);
                            zone.fill = static_cast<double> (ring.value) / 127.0;
                            zone.fromCentre = ring.mode == 1;
                        }

                        break;
                    }

                    case Page::send:
                    {
                        zone.label = textAt (at(), control.busNameAt);

                        if (zone.label.empty())
                            zone.label = control.busId;

                        zone.colour = cueColour;

                        if (control.controlAt.empty())
                        {
                            zone.value = "no send";
                            zone.off = true;
                        }
                        else if (const auto level = numberAt (at(), control.controlAt))
                        {
                            valueText (Law::level, *level, false, value);
                            zone.value = value;
                            zone.fill = d700RingFor (Law::level, *level, faderSilenceDb, faderLoudestDb,
                                                     FaderLaw::generic).value / 127.0;
                            zone.off = soleAt (at(), control.switchAt) != nullptr && ! flagAt (at(), control.switchAt);
                        }

                        break;
                    }

                    case Page::pan:
                    case Page::width:
                    {
                        /*  A SEND'S PLACE (namespace draft §113): the mix's name;
                            its lean or its width, "swap" after the width while
                            the sides are exchanged; "no send" where the cue
                            does not reach, "mono" where the shape has no such
                            row; the bar from the centre for a lean. */
                        zone.label = textAt (at(), control.busNameAt);

                        if (zone.label.empty())
                            zone.label = control.busId;

                        zone.colour = cueColour;
                        const auto lean = deck.page == Page::pan;

                        if (control.placementAt.empty())
                        {
                            zone.value = "no send";
                            zone.off = true;
                        }
                        else if (control.controlAt.empty())
                        {
                            zone.value = "mono";
                            zone.off = true;
                        }
                        else if (const auto placed = numberAt (at(), control.controlAt))
                        {
                            const auto law = lean ? Law::pan : Law::stereoWidth;
                            valueText (law, *placed, false, value);
                            zone.value = value;

                            if (! lean && soleAt (at(), control.switchAt) != nullptr && flagAt (at(), control.switchAt))
                                zone.value += " swap";

                            const auto ring = d700RingFor (law, *placed, lean ? placeLeanMin : placeWidthMin,
                                                           lean ? placeLeanMax : placeWidthMax, FaderLaw::generic);
                            zone.fill = static_cast<double> (ring.value) / 127.0;
                            zone.fromCentre = ring.mode == 1;
                            zone.off = soleAt (at(), control.onAt) != nullptr && ! flagAt (at(), control.onAt);
                        }

                        break;
                    }

                    case Page::fx:
                    {
                        const auto shortName = textAt (at(), control.paramAt + "shortName");
                        zone.label = shortName.empty() ? textAt (at(), control.paramAt + "name") : shortName;
                        zone.value = textAt (at(), control.fxTextAt);
                        zone.colour = cueColour;

                        if (const auto parameter = numberAt (at(), control.controlAt))
                        {
                            zone.fill = std::clamp (*parameter, 0.0, 1.0);
                            zone.fromCentre = flagAt (at(), control.paramAt + "bipolar");
                        }

                        break;
                    }

                    case Page::loop:
                    {
                        zone.label = std::string (loopControls[static_cast<std::size_t> (control.control)].label);
                        zone.colour = cueColour;

                        const auto length = numberAt (at(), deck.loopLengthAt).value_or (0.0);

                        if (control.control == static_cast<int> (LoopControl::slide))
                        {
                            const auto in = numberAt (at(), deck.loopInAt).value_or (0.0);
                            const auto loopOut = numberAt (at(), deck.loopOutAt).value_or (0.0);
                            secondsText (loopOut - in, false, value);
                            zone.value = value;
                        }
                        else if (control.control == static_cast<int> (LoopControl::level))
                        {
                            if (const auto level = numberAt (at(), control.controlAt))
                            {
                                valueText (Law::level, *level, false, value);
                                zone.value = value;
                                zone.fill = d700RingFor (Law::level, *level, faderSilenceDb, faderLoudestDb,
                                                         FaderLaw::generic).value / 127.0;
                            }
                        }
                        else if (const auto seconds = numberAt (at(), control.controlAt))
                        {
                            secondsText (*seconds, false, value);
                            zone.value = value;
                            zone.fill = length > 0.0 ? std::clamp (*seconds / length, 0.0, 1.0) : -1.0;
                        }

                        break;
                    }

                    case Page::show:
                        break;
                }
            }

            return out;
        }

        ScreenPicture screenPicture (const Deck& deck) const
        {
            ScreenPicture picture;
            const auto focus = textAt (at(), "/godot/list/focus");
            const auto standby = focus.empty() ? noText() : textAt (at(), "/godot/list/" + focus + "/standby");

            if (standby.empty())
                picture.first = "Nothing next";
            else
            {
                const auto number = cueNumber (standby);
                picture.first = "Next: " + (number.empty() ? std::string {} : number + " ") + cueLabel (standby);
            }

            const auto cart = cartOf (deck);
            picture.second = cart.empty() ? std::string ("No cart")
                                          : textAt (at(), "/godot/list/" + cart + "/name");

            if (deck.layout.pages > 1)
                picture.second += " " + std::to_string (deck.layout.page + 1) + "/" + std::to_string (deck.layout.pages);

            return picture;
        }

        std::string keyLine (const DeckKey& key, const KeyPicture& picture, const Deck& deck) const
        {
            std::string action (roleWord (key.role));

            if (key.role == DeckRole::pad && key.pad >= 0 && key.pad < static_cast<int> (deck.padCues.size()))
                action += " " + deck.padCues[static_cast<std::size_t> (key.pad)];

            //  A learnt key: its trigger, then its cue (§104).
            if (key.role == DeckRole::learnt)
                if (const auto* entry = learntAt (deck, key.pad))
                    action += " " + entry->trigger + " " + entry->cue;

            const auto clean = [] (std::string text)
            {
                std::replace (text.begin(), text.end(), '\t', ' ');
                std::replace (text.begin(), text.end(), '\n', ' ');
                return text;
            };

            return action + "\t" + clean (picture.top) + "\t" + clean (picture.label) + "\t" + clean (picture.bottom)
                 + "\t" + hexOf (picture.background) + "\t" + (picture.lit ? "lit" : picture.dim ? "dim" : "");
        }

        void paint (Deck& deck)
        {
            /*  NO DECK FOUND, ITS KEYS ARE STILL LAID OUT - as an MK.2's, the
                Surfaces page's picture with nothing plugged in (S21_HiJack's
                offline templates' reason) - and published, and sent nowhere. */
            static const auto standIn = *deckSpecFor (std::string_view ("mk2"));
            const auto& spec = deck.model.has_value() ? *deck.model : standIn;
            const auto going = goingCues();

            //  The layout: a cart on a deck of keys, pages on the Stream Deck+.
            if (hasDials (spec))
            {
                deck.layout = plusLayout (deck.spec.showKeys);
                deck.padCues.clear();
                compose (deck);
            }
            else
            {
                deck.padCues = padsOf (cartOf (deck));
                readTriggers();
                deck.layout = deckLayoutFor (spec, deck.spec.showKeys, static_cast<int> (deck.padCues.size()),
                                             deck.cartPage, learntKeysOf (deck));
                deck.cartPage = deck.layout.page;
            }

            std::string keysText;
            const auto send = deck.open && deck.model.has_value();

            if (deck.shownKeys.size() != deck.layout.keys.size())
                deck.shownKeys.assign (deck.layout.keys.size(), std::nullopt);

            for (std::size_t key = 0; key < deck.layout.keys.size(); ++key)
            {
                const auto picture = pictureFor (deck, deck.layout.keys[key], going);

                if (! keysText.empty())
                    keysText += "\n";

                keysText += keyLine (deck.layout.keys[key], picture, deck);

                if (send && spec.image != DeckImage::none && deck.shownKeys[key] != picture)
                {
                    host.key (deck.serial, static_cast<int> (key), picture);
                    deck.shownKeys[key] = picture;
                }
            }

            if (keysText != deck.keysText)
            {
                deck.keysText = keysText;
                table.setKeys (deck.spec.id, keysText);
            }

            if (! send)
                return;

            if (deck.brightnessSent != deck.spec.brightness)
            {
                host.brightness (deck.serial, deck.spec.brightness);
                deck.brightnessSent = deck.spec.brightness;
            }

            if (hasDials (spec))
            {
                const auto pictures = zonePictures (deck);

                for (std::size_t zone = 0; zone < pictures.size(); ++zone)
                {
                    if (deck.shownZones[zone] != pictures[zone])
                    {
                        host.zone (deck.serial, static_cast<int> (zone), pictures[zone]);
                        deck.shownZones[zone] = pictures[zone];
                    }
                }
            }

            if (spec.model == DeckModel::neo)
            {
                const auto picture = screenPicture (deck);

                if (deck.shownScreen != picture)
                {
                    host.screen (deck.serial, picture);
                    deck.shownScreen = picture;
                }
            }
        }

        void publishPage (const Deck& deck)
        {
            SurfaceTable::Page page;

            /*  ONLY WHILE THE DECK IS THERE TO TURN (namespace draft §101): a
                Stream Deck+'s dials are always on some page, and one unplugged,
                closed or switched off in the show would leave the window's
                foot held on a page no hand can put down. */
            if (deck.open && deck.spec.enabled && deck.model.has_value() && hasDials (*deck.model))
            {
                page.word = std::string (pageWord (deck.page));
                page.index = deck.pageIndex;
                page.count = deck.pageCount;
                page.edited = deck.edited;
                page.moves = deck.moves;
            }

            if (table.pageOf (deck.spec.id) != page)
                table.setPage (deck.spec.id, page);
        }
    };

    //==============================================================================
    DeckBridge::DeckBridge (DeckHost& host, SurfaceTable& table) : impl (std::make_unique<State> (host, table)) {}

    DeckBridge::~DeckBridge() = default;

    bool DeckBridge::declare (std::vector<DeckSurfaceSpec> specs)
    {
        auto& s = *impl;
        auto changed = false;
        std::vector<State::Deck> next;
        next.reserve (specs.size());

        for (auto& spec : specs)
        {
            const auto old = std::find_if (s.decks.begin(), s.decks.end(),
                                           [&spec] (const State::Deck& deck) { return deck.spec.id == spec.id; });

            State::Deck deck;

            //  The same surface keeps what its deck shows and holds.
            if (old != s.decks.end())
                deck = std::move (*old);

            deck.spec = std::move (spec);
            deck.origin = "surface:" + deck.spec.id;
            next.push_back (std::move (deck));
        }

        //  A surface gone lets go of its deck: nothing it held is held now.
        for (const auto& old : s.decks)
        {
            const auto kept = std::any_of (next.begin(), next.end(),
                                           [&old] (const State::Deck& deck) { return deck.spec.id == old.spec.id; });

            if (! kept && ! old.spec.id.empty())
            {
                s.table.forget (old.spec.id);
                changed = true;
            }
        }

        s.decks = std::move (next);
        return changed;
    }

    bool DeckBridge::beforeTick (const Submit& submit, std::int64_t tick)
    {
        auto& s = *impl;
        auto changed = false;

        //  Held cues of a deck that lost or changed its surface's deck are let go.
        std::map<std::string, std::string> before;

        for (const auto& deck : s.decks)
            before[deck.spec.id] = deck.serial;

        s.resolve();

        for (auto& deck : s.decks)
            if (before[deck.spec.id] != deck.serial && ! deck.held.empty())
                s.release (deck, submit);

        for (const auto& event : s.host.drain())
        {
            const auto found = std::find_if (s.decks.begin(), s.decks.end(),
                                             [&event] (const State::Deck& deck) { return deck.serial == event.serial; });

            if (found == s.decks.end())
                continue;

            auto& deck = *found;

            switch (event.kind)
            {
                case DeckEvent::Kind::opened:
                    deck.open = true;
                    deck.moves = 0;         // opened again, its hands have moved nothing (§101)
                    deck.down.clear();
                    deck.forgetShown();
                    break;

                case DeckEvent::Kind::closed:
                    s.release (deck, submit);
                    deck.open = false;
                    deck.forgetShown();
                    break;

                case DeckEvent::Kind::input:
                    if (deck.open)
                        s.input (deck, event.input, submit, tick);
                    break;
            }
        }

        for (auto& deck : s.decks)
        {
            s.flushDials (deck, submit);

            if (s.table.set (deck.spec.id, s.statusOf (deck)))
                changed = true;

            s.publishPage (deck);
        }

        return changed;
    }

    void DeckBridge::afterTick (std::shared_ptr<const tree::TreeSnapshot> snapshot, std::int64_t)
    {
        auto& s = *impl;
        s.published = std::move (snapshot);

        for (auto& deck : s.decks)
        {
            s.paint (deck);
            s.publishPage (deck);
        }
    }

    std::string DeckBridge::pluggedText() const
    {
        std::string out;

        for (const auto& plugged : impl->lastPlugged)
        {
            if (! out.empty())
                out += "\n";

            out += std::string (plugged.spec.word) + " " + plugged.serial;
        }

        return out;
    }
}
