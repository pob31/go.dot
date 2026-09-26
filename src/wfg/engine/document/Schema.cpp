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

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/SchemaTable.generated.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace wfg::doc
{
    //==============================================================================
    bool Attribute::isEnumValue (std::string_view v) const noexcept
    {
        for (std::size_t i = 0; i < row->numEnumValues; ++i)
            if (row->enumValues[i] == v)
                return true;

        return false;
    }

    bool Attribute::isInRange (double value) const noexcept
    {
        if (row->hasMin && value < row->minimum)
            return false;

        if (row->hasMax && value > row->maximum)
            return false;

        return true;
    }

    const Attribute* Element::attribute (std::string_view attributeName) const
    {
        const auto it = std::find_if (attributes.begin(), attributes.end(),
                                      [attributeName] (const Attribute& a)
                                      { return a.name() == attributeName; });

        return it == attributes.end() ? nullptr : &*it;
    }

    const Attribute* Element::derivedAttribute (std::string_view attributeName) const
    {
        const auto it = std::find_if (derivedAttributes.begin(), derivedAttributes.end(),
                                      [attributeName] (const Attribute& a)
                                      { return a.name() == attributeName; });

        return it == derivedAttributes.end() ? nullptr : &*it;
    }

    bool Element::mayContain (std::string_view childName) const
    {
        return std::find (childElements.begin(), childElements.end(), childName)
                 != childElements.end();
    }

    //==============================================================================
    namespace
    {
        /*  CONTAINMENT, and it is the only structural fact in this layer that
            the parameter table does not carry.

            The table has one row per attribute; "a List may hold Cues and
            Groups" is a property of elements, so writing it here is honest
            rather than lazy. It is also short enough to read in one go, which a
            thirteenth CSV column would not be.

            The shape is PRD §3.5 and §3.6: a List has the same child grammar as
            a Group, because "the cue list is, virtually, a sequence group in
            manual mode - one model, no special case at the top".

                Show
                  Lists
                    List           <- id, name
                      Cue          <- id and the cue attributes
                      Group        <- a Cue that also holds Cues and Groups
                  Mounts
                    Mount          <- id, a foreign namespace
                  Audio            <- tracks: the polyphony ceiling
                    Bus            <- id, a named range of output channels

            Containers (Lists, Mounts) carry nothing and exist so the file has
            somewhere obvious to put a new List, and so a diff of one list does
            not touch the mounts.

            Audio is not one of those: it carries `tracks` itself, because the
            track count is one number about the whole show rather than a
            property of any bus. It is the shape of the machine the show expects
            to run on - which is a thing someone decided (§4.10), so it lives in
            the document and not in a preference.
        */
        struct Containment
        {
            std::string_view element;
            bool hasIdentity;
            std::vector<std::string_view> children;
            std::vector<std::string_view> attributeOwners;   // which table rows land here
        };

        const std::vector<Containment>& containmentTable()
        {
            static const std::vector<Containment> table {
                { "Show",   false, { "Lists", "Mounts", "Audio", "MidiPorts", "Network",
                                     "Surfaces", "Dcas" },                { "document" } },
                /*  THE CONTAINER CARRIES A VALUE, which is why it is no
                    longer an empty pair of brackets. `focus` is a fact about
                    the collection of lists rather than about any list in it -
                    exactly as `Audio` carries the track count, which is a fact
                    about the whole show and not about any bus. */
                { "Lists",  false, { "List" },             { "lists" } },
                { "List",   true,  { "Cue", "Group", "Media", "Mic", "Fade", "Transport", "Osc",
                                     "Midi", "Start", "Persistent" }, { "list" } },
                { "Cue",    true,  { "Trigger" },                     { "cue" } },
                { "Group",  true,  { "Cue", "Group", "Media", "Mic", "Fade", "Transport", "Osc",
                                     "Midi", "Start", "Header", "Footer", "Trigger" },
                                                                          { "cue", "group" } },

                /*  A HEADER AND A FOOTER ARE ORDINARY CUE LISTS (§3.6), which
                    is why they are elements holding cues rather than a word on
                    each cue: "an ordinary cue list that runs at group exit" is
                    what the section says a footer IS, and a container is what a
                    cue list is made of.

                    They carry an identifier and nothing else. The identifier is
                    not decoration - `cue.create` and `object.move` address a
                    parent BY id, so without one there would be no way to put a
                    cue in a footer.

                    AT MOST ONE OF EACH, checked by validate() rather than by
                    the grammar. The generator emits a container's children as
                    an unordered `zeroOrMore` of a choice, which is the right
                    shape for the eight things a group can hold and the wrong
                    one for "optionally one of these"; teaching it ordered
                    content models to express a rule that fits in one line of
                    validate() would be paying a great deal for a smaller
                    diagnostic. */
                { "Header", true,  { "Cue", "Group", "Media", "Mic", "Fade", "Transport", "Osc",
                                     "Midi", "Start" }, {} },
                { "Footer", true,  { "Cue", "Group", "Media", "Mic", "Fade", "Transport", "Osc",
                                     "Midi", "Start" }, {} },

                /*  THE PERSISTENT SECTION IS A LIST'S, not a group's (§3.29,
                    decision S): the thing that should be running at all times
                    and is relaunched if it is not. The same shape as a header -
                    an identified container of cues, at most one, checked by
                    validate() - and the same children, so that a fade or a stop
                    put there is a validate WARNING that the section ignores
                    rather than a file that refuses to open. */
                { "Persistent", true, { "Cue", "Group", "Media", "Mic", "Fade", "Transport", "Osc",
                                        "Midi", "Start" }, {} },

                /*  ONE ELEMENT PER CUE KIND (author, 2026-09-05), which is the
                    pattern a Group already set. `kind` stays derived from the
                    element and read-only, so a client cannot turn a memo into a
                    media cue by writing a word - and the grammar can refuse a
                    `file` attribute on a cue that plays nothing, which it could
                    not do if every cue were a <Cue> with a stored kind.

                    Media carries the `cue` rows as well as its own, exactly as
                    Group does: a media cue has a number, a name and a pre-wait
                    like any other, and it is addressed at /godot/cue/<id> so a
                    client holding an identifier never has to know which kind it
                    got.

                    AND THE `sound` ROWS BETWEEN THE TWO (Phase 9b, namespace
                    draft 18.2): what a cue that sounds carries whatever its
                    source - level, routing, the DCA, the EQ, the inserts, the
                    sends - which a mic cue carries too. `media` keeps what is
                    about a file. The addresses do not move: every row is still
                    /godot/cue/<id>/<row>. */
                { "Media",  true,  { "Route", "Send", "Feed", "Insert", "Range", "Trigger", "Fx" },
                                                          { "cue", "sound", "media" } },

                /*  A LIVE INPUT PLAYED AS A CUE (Phase 9b, decisions BW and CE):
                    a cue first, a sound second - the same level, routing, DCA,
                    EQ, inserts and sends as a media cue - and a mic third: the
                    named input it takes, the rack channel it plays through, its
                    fade-in. Without what names a file: no Range, and no Insert,
                    whose claim a mic cue's channel IS (namespace draft 18.2). */
                { "Mic",    true,  { "Route", "Send", "Feed", "Fx", "Trigger" },
                                                          { "cue", "sound", "mic" } },

                /*  A DESTINATION IS AN OBJECT (author, 2026-09-05). PRD §3.9b
                    says a cue's destinations are a list rather than a choice,
                    so Route repeats - and it is identified rather than
                    positional because a client changing one destination's gains
                    must not have to rewrite the others, and because an index is
                    a position: deleting the first route would silently
                    re-point a client holding the second. */
                { "Route",  true,  {},                     { "route" } },

                /*  A RANGE IS AN OBJECT ON A MEDIA CUE, and on that kind only:
                    §3.24 makes it a region of the cue's own file, so a cue that
                    plays no file can hold none. That is the difference between
                    it and a Trigger, which every kind carries.

                    Identified and repeating, for the Route argument exactly: a
                    cue's ranges are a list somebody edits one at a time, and an
                    index would be a position - deleting the first range would
                    silently re-point a client holding the second. The order IS
                    the playlist, though, so `index` is published as a derived
                    row and moving a range is `object.move` like any other
                    ordered thing.

                    ONE SLOT IN THE GRAPH PER RANGE, which is the part that
                    reaches past the document: the show's widest range count
                    fixes how many launcher slots every track is built with,
                    once, when the graph is built (§3.25). A range added past
                    that during a show has nowhere to be armed and is refused
                    with `no-slot` until the show is reloaded. */
                { "Range",  true,  {},                     { "range" } },

                /*  A TRIGGER IS AN OBJECT ON A CUE, and on ANY cue: §3.7 says a
                    cue or a group carries a list of them, so it is a child of
                    every kind rather than a row on one.

                    Identified, and a list rather than a choice, for the reason
                    a Route is: a cue fired by a note AND by a time of day is
                    two triggers, and a client editing one must not have to
                    rewrite the other. An index would be a position, and
                    deleting the first would silently re-point a client holding
                    the second.

                    IT CARRIES EVERY KIND'S ROWS, because the grammar cannot
                    refuse `channel` on an OSC trigger without an element per
                    kind - and three elements for a thing that is one concept
                    with three sources would be worse. `wfg validate` is where
                    a MIDI field on a clock trigger gets mentioned. */
                { "Trigger", true, {},                     { "trigger" } },

                /*  A FADE AND A STOP ARE CUES, and elements of their own for
                    the reason a Media is: `kind` stays derived and read-only,
                    and the grammar can refuse a `duration` on a cue that has
                    nothing to fade. Neither carries a Route - they act on a run
                    that already has one, which is the whole difference between
                    them and a media cue. */
                { "Fade",   true,  { "Trigger" },                     { "cue", "fade" } },
                { "Transport",   true,  { "Trigger" },                     { "cue", "transport" } },

                /*  A START CUE PRESSES A BUTTON (2026-09-19): it fires another
                    cue by name and is done. Its own element for the reason the
                    others have one - `kind` stays derived - and it carries a
                    target and nothing else of its own. What the live recorder
                    writes into a take. */
                { "Start",  true,  { "Trigger" },                     { "cue", "start" } },

                /*  And a network cue is one too, for the same reason. It
                    carries no Route either: what it writes is somebody
                    else's node, named by address, and the mount it belongs
                    to already says where that is. */
                { "Osc",    true,  { "Trigger" },                     { "cue", "osc" } },

                /*  A MIDI CUE IS A NETWORK CUE ON A DIFFERENT WIRE, and it is
                    an element of its own for the reason every other kind is:
                    `kind` stays derived and read-only, and the grammar can
                    refuse a `sysex` on a cue that plays a file.

                    It names a PORT the show declares rather than a device
                    (§4.10 - the document holds what somebody decided), which is
                    the same separation a Route's bus has from a hardware
                    channel: a show travels to another rig and the ports are
                    re-bound rather than every cue rewritten. */
                { "Midi",   true,  { "Trigger" },                     { "cue", "midi" } },

                /*  WHERE THE SHOW SAYS WHAT ITS PORTS ARE CALLED. A section of
                    the Show beside Audio, holding one Port per name a cue may
                    address - "Lights", "The desk" - and nothing about which
                    cable that is. `wfg serve --midi-out=<name>=<device>` says
                    that, because it is a fact about this building.

                    Anonymous, like Lists and Audio: there is one of it.

                    NAMED `MidiPorts` AND NOT `Midi`, which the cue kind already
                    is. Two elements of one name would be one element as far as
                    the schema is concerned, and the one that lost would be the
                    one nobody could find - a <Midi> section reading as a cue
                    with no list to be in. */
                { "MidiPorts", false, { "Port" },          { "ports" } },
                { "Port",   true,  {},                     { "port" } },
                { "Mounts", false, { "Mount" },            {} },
                { "Mount",  true,  { "Slot" },             { "mount" } },

                /*  THE SHOW'S OWN NETWORK SIDE. A container beside Audio, and
                    like Audio it carries a value rather than being an empty
                    pair of brackets: whether this show takes messages from
                    senders nobody declared is a fact about the whole show.

                    Empty of children today. The interfaces Go.dot listens on
                    land here next, which is why it is a container at all
                    rather than an attribute on <Show>: one of them will have
                    an identifier, a name and two port numbers, and an
                    attribute cannot grow children. */
                { "Network", false, {},                    { "network" } },
                { "Audio",  false, { "Bus", "Inputs", "Rack", "Plugins" }, { "audio" } },
                { "Bus",    true,  {},                     { "bus" } },

                /*  PHASE 4'S SLOTS (PRD §3.9e). A slot is one position in a
                    pool of fixed size declared at load; typed; exclusive; held
                    for a live range. Two of the four instances are document
                    objects and are declared here.

                    BOTH CARRY THE OWNER `slot`, which is what they share - a
                    name and a derived kind - and each adds its own. The `Media`
                    precedent exactly: a media cue is `{ "cue", "media" }`,
                    because it is a cue first and a media cue second.

                    `Rack` carries no owner at all, like `Mounts`: it is a
                    container holding channels and says nothing itself. */
                { "Rack",    false, { "Channel" },         {} },
                /*  A RACK CHANNEL'S CHAIN is its own `Plugin` children, in
                    order (Phase 9b, decision BX): the same element, and so the
                    same rows, as an entry of the show's set. */
                { "Channel", true,  { "Plugin" },          { "slot", "rackChannel" } },
                { "Slot",    true,  {},                    { "slot", "processorInput" } },

                /*  And what a cue says about them. A `Route` sends a cue to a
                    bus; a `Feed` sends it to a SLOT, which means to that slot's
                    own channels of the slot's bus, and claims the slot; an
                    `Insert` claims a rack channel. Destinations are a list and
                    not a choice (§3.9b). */
                { "Feed",    true,  {},                    { "feed" } },
                { "Insert",  true,  {},                    { "insert" } },

                /*  AND A SEND IS THE OTHER HALF OF THE OUTPUT LIST. A direct
                    out is an attribute of the cue, because a cue has one or
                    none; a mix channel is a child, because a cue sends into as
                    many as it likes and each at its own level. Identified for
                    the reason `Route` is: deleting the first send must not
                    re-point a client holding the second. */
                { "Send",    true,  {},                    { "send" } },
                { "Fx",      true,  {},                    { "fx" } },

                /*  PHASE 6'S SURFACES (PRD §3.16, 2026-09-23). A surface is a
                    box of faders, pads and displays the show talks to through
                    ports it already declares; what it is called and which
                    ports it is on are decisions about the rig, so they are
                    here, and whether anything answers tonight is not.

                    A STRIP IS THE FOURTH SLOT KIND (§3.9e), which is why it
                    carries the owner `slot` beside its own - the `Channel`
                    shape exactly. It is published at /godot/slot/<id> with
                    the other three, a run holds it and waits for it the way
                    a Feed holds a processor input, and a sampler group's
                    takeover is the eviction §13.15 left for this phase.

                    `Surfaces` carries the owner `surfaces` for its `order`,
                    as `Lists` carries `lists`; a container with an order and
                    nothing else to say. */
                { "Surfaces", false, { "Surface" },        { "surfaces" } },
                { "Surface",  true,  { "Strip" },          { "surface" } },
                { "Strip",    true,  {},                   { "slot", "strip" } },

                /*  AND THE DCAS (PRD §3.28): an object of its own, cross-cutting
                    the hierarchy. It holds only a name, a short name and the
                    DCA it sits inside; which cues it trims is a mark on each
                    member (`media/dca`, `group/dca`), so nothing flows down
                    (§4.12). A container beside Surfaces rather than inside
                    Audio: a DCA trims a level, and one day an opacity, and is
                    no more an audio object than a group is. */
                { "Dcas",     false, { "Dca" },            { "dcas" } },
                { "Dca",      true,  {},                   { "dca" } },

                /*  PHASE 9a'S PLUGIN SET (PRD §3.18, decision AE, 2026-09-23):
                    the processors every voice carries, declared once under
                    Audio as the tracks are, in the order of the chain. Made
                    on demand by the first plugin.create, at a fixed place
                    after the buses, so no fixture gains a line and the
                    canonical bytes do not depend on which container was
                    asked for first. `Plugins` carries `plugins` for its
                    `order`, as `Dcas` carries `dcas`. */
                { "Plugins",  false, { "Plugin" },         { "plugins" } },
                { "Plugin",   true,  {},                   { "plugin" } },

                /*  PHASE 9b'S NAMED INPUTS (PRD §3.18, §6.2, 2026-09-26): the
                    other side of the interface from the buses, and named for
                    §3.9b's reason - "Voix solo" is what somebody wrote down and
                    "input 3" is a fact about a patch. A container like
                    `Plugins`, made on demand at a fixed place after the buses,
                    so an input's position counts from nought whatever the
                    buses are doing and the canonical bytes do not depend on
                    which container was asked for first. `Inputs` carries
                    `inputs` for its `order`, as `Plugins` carries `plugins`. */
                { "Inputs",   false, { "Input" },          { "inputs" } },
                { "Input",    true,  {},                   { "input" } },
            };

            return table;
        }
    }

    //==============================================================================
    Schema::Schema()
    {
        for (const auto& c : containmentTable())
        {
            Element element;
            element.name = c.element;
            element.hasIdentity = c.hasIdentity;
            element.childElements = c.children;

            /*  Rows land on an element when their owner is one this element
                takes AND they persist somewhere. A row that persists nowhere is
                a runtime projection - a cue's index among its siblings, the
                engine's tick - published by the parameter tree and held in no
                file. Those rows stay in the generated table because Phase 1.5
                needs them; they are simply not document attributes.

                Owners are visited in the order the containment table lists
                them, so a Group's own rows follow the Cue rows it inherits,
                which is the order they read best in. */
            for (const auto& owner : c.attributeOwners)
                for (const auto& row : generated::attributes)
                {
                    if (row.owner != owner)
                        continue;

                    if (row.persist == Persist::none)
                        element.derivedAttributes.push_back (Attribute { c.element, &row });
                    else
                        element.attributes.push_back (Attribute { c.element, &row });
                }

            elementList.push_back (std::move (element));
        }
    }

    const Schema& Schema::instance()
    {
        /*  Function-local static: built on first use, thread-safe since C++11,
            and with no dependence on the order static objects elsewhere are
            constructed. */
        static const Schema schema;
        return schema;
    }

    std::vector<const AttributeRow*> Schema::rowsForOwner (std::string_view owner)
    {
        std::vector<const AttributeRow*> rows;

        for (const auto& row : generated::attributes)
            if (row.owner == owner)
                rows.push_back (&row);

        return rows;
    }

    int Schema::formatVersion()
    {
        const auto* attribute = instance().attribute (rootElement, "formatVersion");

        if (attribute == nullptr || ! attribute->hasDefault())
            return 1;

        const auto text = attribute->defaultText();
        int value = 1;

        /*  from_chars, not stoi: it does not throw, does not consult the
            locale, and is present for integers on every toolchain here - the
            floating-point overload is the one macOS is missing. */
        const auto* first = text.data();
        const auto result = std::from_chars (first, first + text.size(), value);

        return result.ec == std::errc {} ? value : 1;
    }

    const Element* Schema::element (std::string_view name) const
    {
        const auto it = std::find_if (elementList.begin(), elementList.end(),
                                      [name] (const Element& e) { return e.name == name; });

        return it == elementList.end() ? nullptr : &*it;
    }

    const Attribute* Schema::attribute (std::string_view elementName,
                                        std::string_view attributeName) const
    {
        const auto* e = element (elementName);
        return e == nullptr ? nullptr : e->attribute (attributeName);
    }

    //==============================================================================
    Value Value::string (std::string v)
    {
        Value out;
        out.valueType = ValueType::string;
        out.text = std::move (v);
        return out;
    }

    Value Value::integer (long long v)
    {
        Value out;
        out.valueType = ValueType::integer;
        out.integerValue = v;
        return out;
    }

    Value Value::number (double v)
    {
        Value out;
        out.valueType = ValueType::number;
        out.numberValue = v;
        return out;
    }

    Value Value::boolean (bool v)
    {
        Value out;
        out.valueType = ValueType::boolean;
        out.booleanValue = v;
        return out;
    }

    bool Value::operator== (const Value& other) const noexcept
    {
        if (valueType != other.valueType)
            return false;

        switch (valueType)
        {
            case ValueType::string:    return text == other.text;
            case ValueType::integer:
            case ValueType::integer64: return integerValue == other.integerValue;
            case ValueType::boolean:   return booleanValue == other.booleanValue;
            case ValueType::number:
            {
                /*  Bit comparison, not numeric: a replay compares documents, and
                    two numbers that print the same must not be called equal if
                    they are different doubles. It also keeps -Wfloat-equal
                    quiet, which is on under the strict preset. */
                const auto a = numberValue;
                const auto b = other.numberValue;
                return std::memcmp (&a, &b, sizeof (double)) == 0;
            }
            case ValueType::blob:      return true;   // nothing in Phase 1 holds one
        }

        return false;
    }

    //==============================================================================
    Schema::Parsed Schema::parseList (const Attribute& attribute, std::string_view text,
                                      std::string& canonical)
    {
        Parsed result;
        canonical.clear();

        std::size_t i = 0;
        int position = 0;

        while (i < text.size())
        {
            while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) != 0)
                ++i;

            const auto start = i;

            while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) == 0)
                ++i;

            if (i == start)
                break;

            const auto token = text.substr (start, i - start);

            Value value;
            const auto parsed = parseValue (attribute, token, value);

            if (! parsed.ok)
            {
                result.ok = false;
                result.error = "element " + std::to_string (position) + ": " + parsed.error;
                return result;
            }

            if (! canonical.empty())
                canonical += ' ';

            /*  Through the same formatter a scalar goes through, which is what
                makes `1.50` and `1.5` the same document rather than two. */
            switch (value.type())
            {
                case ValueType::string:    canonical += value.getString(); break;
                case ValueType::integer:
                case ValueType::integer64: canonical += std::to_string (value.getInteger()); break;
                case ValueType::number:    canonical += osc::formatDouble (value.getNumber()); break;

                case ValueType::boolean:
                case ValueType::blob:
                    /*  The generator refuses `T*` and `b*`, so neither reaches
                        here. Named because the strict preset compiles with
                        -Wswitch-enum. */
                    break;
            }

            ++position;
        }

        result.ok = true;
        return result;
    }

    Schema::Parsed Schema::parseValue (const Attribute& attribute, std::string_view text,
                                       Value& out)
    {
        Parsed result;

        const auto reject = [&result] (std::string why)
        {
            result.ok = false;
            result.error = std::move (why);
            return result;
        };

        switch (attribute.type())
        {
            case ValueType::string:
            {
                /*  An enum is a string with a list. Rejecting an unknown value
                    rather than keeping it is the point: a mode of "sequnce" is a
                    typo that would otherwise sit in the file behaving like a
                    timeline. */
                if (attribute.isEnum() && ! attribute.isEnumValue (text))
                {
                    std::string allowed;

                    for (std::size_t i = 0; i < attribute.row->numEnumValues; ++i)
                    {
                        if (! allowed.empty())
                            allowed += '|';

                        allowed += std::string (attribute.row->enumValues[i]);
                    }

                    return reject ("expected one of " + allowed + ", found \""
                                   + std::string (text) + "\"");
                }

                out = Value::string (std::string (text));
                break;
            }

            case ValueType::integer:
            case ValueType::integer64:
            {
                long long parsed = 0;
                const auto* first = text.data();
                const auto* last = text.data() + text.size();
                const auto r = std::from_chars (first, last, parsed);

                if (r.ec != std::errc {} || r.ptr != last)
                    return reject ("expected a whole number, found \"" + std::string (text) + "\"");

                if (! attribute.isInRange (static_cast<double> (parsed)))
                    return reject ("out of range: " + std::string (text));

                out = Value::integer (parsed);
                break;
            }

            case ValueType::number:
            {
                const auto parsed = osc::parseDouble (text);

                if (! parsed)
                    return reject ("expected a number, found \"" + std::string (text) + "\"");

                if (! attribute.isInRange (*parsed))
                    return reject ("out of range: " + std::string (text));

                out = Value::number (*parsed);
                break;
            }

            case ValueType::boolean:
            {
                /*  Exactly "true" and "false". Not 1 and 0, not "yes": a
                    document is written by this program and read by a person, and
                    accepting four spellings means writing one and reading four
                    forever. */
                if (text == "true")       out = Value::boolean (true);
                else if (text == "false") out = Value::boolean (false);
                else return reject ("expected true or false, found \"" + std::string (text) + "\"");

                break;
            }

            case ValueType::blob:
                return reject ("blob attributes are not supported in a document");
        }

        result.ok = true;
        return result;
    }

    std::string Schema::formatValue (const Attribute& attribute, const Value& value)
    {
        switch (attribute.type())
        {
            case ValueType::string:    return value.getString();
            case ValueType::integer:
            case ValueType::integer64: return std::to_string (value.getInteger());
            case ValueType::boolean:   return value.getBoolean() ? "true" : "false";

            case ValueType::number:
                /*  The same formatter the event log uses: shortest text that
                    reads back as the identical value. A document whose numbers
                    change when it is reopened is not a document. */
                return osc::formatDouble (value.getNumber());

            case ValueType::blob:
                return {};
        }

        return {};
    }
}
