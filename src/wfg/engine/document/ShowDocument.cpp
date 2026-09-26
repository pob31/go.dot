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

#include <wfg/engine/document/ShowDocument.h>

#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/FadePoints.h>
#include <wfg/engine/document/OutputLayout.h>
#include <wfg/engine/document/Sequence.h>
#include <wfg/engine/osc/OscValue.h>

#include <map>
#include <cctype>
#include <cstddef>
#include <algorithm>
#include <string>
#include <unordered_set>

namespace wfg::doc
{
    namespace
    {
        const juce::Identifier idProperty { "id" };
        const juce::Identifier standbyProperty { "standby" };
        const juce::Identifier lockedProperty { "locked" };

        /*  Whether a value is a legal standby for this list: one of the places
            the pointer may stand, or nothing at all.

            Named for the question this door asks rather than for the shape of
            the answer, and it keeps its own line for the empty string rather
            than leaning on the one `cue::mayStandOn` has. An empty pointer is
            nowhere at all and IS a legal standby - a resting state (§3.5),
            never a failure - and that is a rule about what a SHOW may hold,
            stated here where the write is refused, not a detail borrowed from a
            predicate that could be narrowed one day without anyone thinking
            about this line. */
        bool isLegalStandbyFor (const juce::ValueTree& list, const std::string& cueId)
        {
            if (cueId.empty())
                return true;             // empty is the resting value, always legal

            /*  ANYWHERE THE POINTER MAY STAND, which has widened twice: to the
                manual path in PR 3.4, and to any enabled cue this list holds on
                2026-09-16, when the author asked to be able to park inside a
                timeline or an automatic group and try one cue of a scene on its
                own. What is refused now is a cue in a group's header, in its
                footer, or in a persistent section - lists a group runs for
                itself (§3.6), never the operator's position in the show.

                It asks `cue::mayStandOn`, which is the same question
                `standby.set` asks, so a client cannot learn one answer from the
                command and another from the node. Two answers to that would
                eventually be two DIFFERENT answers, and the load path - a
                state.xml written against a show that has since been edited -
                only ever comes through here. */
            return cue::mayStandOn (list, cueId);
        }

        /*  The list a cue belongs to, however deep it is - or an invalid tree
            when it is not in one. The standby repairs below need it because a
            cue can now be several levels down. */
        juce::ValueTree listContaining (juce::ValueTree node)
        {
            while (node.isValid() && node.getType().toString() != "List")
                node = node.getParent();

            return node;
        }

        /*  The identifier after `cueId` among a container's MEMBERS, or empty
            when it is the last one or is not there. */
        std::string siblingAfter (const juce::ValueTree& container, const std::string& cueId)
        {
            bool found = false;

            for (const auto& child : container)
            {
                /*  MEMBERS, and not every identified child — the same rule the
                    index in a command is read against (`document/Sequence.h`).

                    It asked only for an identifier until the index became a
                    member position, and on a group whose last member was being
                    deleted the answer it gave was the <Footer> element's own
                    identifier. A standby that names a footer is refused by the
                    write door, so the repair silently did nothing and left the
                    list parked on the cue that had just gone — the frozen GO
                    key `remove` describes at length below. A list with a
                    <Persistent> section had the same hole. Skipped, the answer
                    is empty instead, `cue::nextStandby` is consulted, and the
                    pointer lands where `next` would have carried it.

                    NOT THE WHOLE HOLE, and the rest is somebody's decision
                    rather than this line's: a <Trigger> is a member by the rule
                    above, and a DISABLED cue is a member too, and the write
                    door refuses both as a standby - so both still answer here
                    with an identifier that is quietly thrown away. What knows
                    where a pointer may stand is `cue::stops`, and the honest
                    repair is for this to ask the cursor instead of walking the
                    children itself. That is a question about the standby
                    repair, not about the index a command carries. */
                if (! isSequenceChild (child))
                    continue;

                const auto childId = child[idProperty].toString().toStdString();

                if (found)
                    return childId;

                if (childId == cueId)
                    found = true;
            }

            return {};
        }

        /*  THE ONE PLACE a typed value becomes a juce::var, and the reason this
            function exists rather than being inlined at three call sites.

            juce::var has no separate integer and string identity as far as
            ValueTree's change detection is concerned: var("1") == var(1) is
            true, so writing a typed 1 over a string "1" looks like "no change"
            and is dropped. Every value in this document is therefore written
            through here, from text that the schema has already parsed, so the
            var's type is decided by the schema and never by what the text
            happened to look like. */
        juce::var toVar (const Value& value)
        {
            switch (value.type())
            {
                case ValueType::string:    return juce::var (juce::String (value.getString()));
                case ValueType::integer:
                case ValueType::integer64: return juce::var (static_cast<juce::int64> (value.getInteger()));
                case ValueType::number:    return juce::var (value.getNumber());
                case ValueType::boolean:   return juce::var (value.getBoolean());
                case ValueType::blob:      break;
            }

            return {};
        }

        /*  A var back to canonical text. Not var::toString(): that would write a
            double through JUCE's formatter, which loses 46% of doubles to a
            round trip (measured; see osc/OscValue.cpp). Numbers go through the
            schema's formatter, the same one the event log uses. */
        std::string toText (const Attribute& attribute, const juce::var& value)
        {
            /*  A LIST IS HELD AS ITS CANONICAL TEXT (CanonicalXml says why), so
                its text is the var's string. Before PR 5.16a it fell through to
                the `number` case below, which read "1 0 0 1" as one double and
                handed back the first gain of a matrix as if it were the matrix. */
            if (attribute.isList())
                return value.toString().toStdString();

            switch (attribute.type())
            {
                case ValueType::string:
                    return value.toString().toStdString();

                case ValueType::integer:
                case ValueType::integer64:
                    return std::to_string (static_cast<long long> (value));

                case ValueType::boolean:
                    return static_cast<bool> (value) ? "true" : "false";

                case ValueType::number:
                    return osc::formatDouble (static_cast<double> (value));

                case ValueType::blob:
                    break;
            }

            return {};
        }

        /*  How many elements a list's text holds: XSD's list form, whitespace
            between, and nothing at all is none. */
        std::size_t countTokens (const std::string& text)
        {
            std::size_t count = 0, i = 0;

            while (i < text.size())
            {
                while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) != 0)
                    ++i;

                if (i >= text.size())
                    break;

                ++count;

                while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) == 0)
                    ++i;
            }

            return count;
        }

        /*  THE ONE THING ABOUT A ROUTING MATRIX THE DOCUMENT CAN CHECK: that it
            is a whole number of rows, one per channel of the cue, each as wide
            as the destination. How many channels the cue HAS is the file's, and
            the file arrives on a different machine from the one the show was
            written on.

            Empty is a cue routed nowhere YET, which is a show being written and
            not a broken one - `resolveRouting` says so in as many words at arm
            time. Asked by validate() and by the write door, so that the two
            cannot come to disagree about what a matrix is. */
        bool coefficientsFit (std::size_t count, int width) noexcept
        {
            return count == 0 || count % static_cast<std::size_t> (std::max (1, width)) == 0;
        }

        /*  The width a Route's bus or a Feed's slot declares, for the write
            door. A pointer at nothing, or at the wrong kind of thing, answers 1
            - which every count fits - because that is the `refers` column's
            warning and never a refusal, exactly as validate() treats it. */
        int destinationWidth (const ShowDocument& document, const juce::ValueTree& destination)
        {
            const auto element = destination.getType().toString();
            const auto isRoute = element == "Route";

            if (! isRoute && element != "Feed")
                return 1;

            const auto targetId = destination[juce::Identifier (isRoute ? "bus" : "slot")]
                                      .toString().toStdString();
            const auto target = document.findById (targetId);

            if (! target.isValid() || target.getType().toString() != (isRoute ? "Bus" : "Slot"))
                return 1;

            const juce::Identifier width { "width" };

            return target.hasProperty (width) ? static_cast<int> (target[width]) : 1;
        }

        std::vector<std::string> splitAddress (const std::string& address)
        {
            std::vector<std::string> parts;
            std::size_t i = 0;

            while (i < address.size())
            {
                while (i < address.size() && address[i] == '/')
                    ++i;

                const auto start = i;

                while (i < address.size() && address[i] != '/')
                    ++i;

                if (i > start)
                    parts.push_back (address.substr (start, i - start));
            }

            return parts;
        }
    }

    //==============================================================================
    std::string_view ShowDocument::elementForKind (std::string_view kind)
    {
        if (kind == "memo")  return "Cue";
        if (kind == "group") return "Group";
        if (kind == "media") return "Media";
        if (kind == "fade")  return "Fade";
        if (kind == "transport") return "Transport";
        if (kind == "osc")   return "Osc";
        if (kind == "midi")  return "Midi";
        if (kind == "start") return "Start";
        return {};
    }

    std::string_view ShowDocument::containerSegmentFor (std::string_view element)
    {
        /*  THE ELEMENTS THAT ARE ADDRESSED WITHOUT AN IDENTIFIER, because there
            is only one of each: `/godot/document/name`, `/godot/audio/tracks`,
            `/godot/list/focus`. A container carries facts about the collection
            rather than about any member of it, and a collection has no id to
            look it up by.

            THE SEGMENT STAYS SINGULAR for `Lists`, and that is deliberate
            rather than an inconsistency: `/godot/list/<id>/standby` and
            `/godot/list/focus` are one container read two ways, and a client
            walking the tree should not have to learn that one of them is
            spelled differently. The parameter table's owner token is `lists`
            because a table row belongs to an element; the address is what a
            person types. */
        if (element == "Show")    return "document";
        if (element == "Audio")   return "audio";
        if (element == "Lists")   return "list";
        if (element == "Network") return "network";

        /*  SINGULAR, as `Lists` is: `/godot/port/inputs` and
            `/godot/port/<id>/name` are one container read two ways, and a
            client walking the tree should not have to learn that one of them
            is spelled differently. An identifier is eight characters of
            Crockford base32, so it can never be the word `inputs`. */
        if (element == "MidiPorts") return "port";

        /*  And the same for Phase 6's two: `/godot/surface/order` beside
            `/godot/surface/<id>/name`, `/godot/dca/order` beside
            `/godot/dca/<id>/trim`. */
        if (element == "Surfaces") return "surface";
        if (element == "Dcas")     return "dca";

        /*  And Phase 9a's: `/godot/plugin/order` beside `/godot/plugin/<id>/name`. */
        if (element == "Plugins")  return "plugin";

        /*  And Phase 9b's: `/godot/input/order` beside `/godot/input/<id>/name`. */
        if (element == "Inputs")   return "input";
        return {};
    }

    std::string_view ShowDocument::ownerForElement (std::string_view element)
    {
        /*  A Group is a Cue (PRD §3.6), so both are addressed as `cue` — a
            client that has an identifier does not have to know which it got,
            and a cue that becomes a group keeps its address. */
        if (element == "Cue" || element == "Group" || element == "Media"
              || element == "Fade" || element == "Transport"
              || element == "Osc" || element == "Midi"
              || element == "Start")                                return "cue";
        if (element == "Lists")                     return "lists";
        /*  A header and a footer are addressed by nothing: they carry no
            attribute but their identifier, and the cues inside them are
            addressed as cues like any other. Naming an owner for them would be
            promising a `/godot/header/<id>/…` that has nothing in it. */
        if (element == "Header" || element == "Footer" || element == "Persistent")
            return {};
        if (element == "Route")                     return "route";
        if (element == "Range")                     return "range";
        if (element == "Port")                      return "port";
        if (element == "Trigger")                   return "trigger";
        if (element == "List")                      return "list";
        if (element == "Mount")                     return "mount";
        if (element == "Bus")                       return "bus";
        if (element == "Show")                      return "document";
        if (element == "Audio")                     return "audio";

        /*  WHETHER THIS SHOW LISTENS TO STRANGERS, and later which interfaces
            it listens on. A container beside Audio carrying a value of its
            own, for the reason Audio does: whether a message from a sender
            nobody declared is obeyed is a fact about the whole show, not
            about any one device in it. */
        if (element == "Network")                   return "network";
        if (element == "MidiPorts")                 return "ports";

        /*  A SLOT AND A RACK CHANNEL ANSWER DIFFERENTLY, and they have to: this
            is the by-kind half of the `refers` check, so `feed/@slot` naming a
            rack channel and `insert/@channel` naming a processor input are both
            caught here rather than at run time. They share the owner `slot` for
            the rows they have in common; what they do not share is the word
            this returns. */
        if (element == "Slot")                      return "slot";
        if (element == "Channel")                   return "rackChannel";
        if (element == "Feed")                      return "feed";
        if (element == "Insert")                    return "insert";
        if (element == "Send")                      return "send";
        if (element == "Fx")                        return "fx";

        /*  `Rack` is addressed by nothing, like `Mounts` and like a header: a
            container that holds channels and carries nothing of its own. */
        if (element == "Rack")                      return {};

        /*  PHASE 6. A STRIP ANSWERS `strip` and not `slot`, for the reason a
            rack channel answers `rackChannel`: this is the by-kind half of the
            `refers` check, and a Feed whose `slot` named a fader would
            otherwise pass it. It is still ADDRESSED as a slot - see
            `addressOwnerFor` below. */
        if (element == "Surfaces")                  return "surfaces";
        if (element == "Surface")                   return "surface";
        if (element == "Strip")                     return "strip";
        if (element == "Dcas")                      return "dcas";
        if (element == "Dca")                       return "dca";

        /*  PHASE 9a. The plugin set and its entries, under Audio. */
        if (element == "Plugins")                   return "plugins";
        if (element == "Plugin")                    return "plugin";

        /*  PHASE 9b. The named inputs and their container, under Audio. */
        if (element == "Inputs")                    return "inputs";
        if (element == "Input")                     return "input";

        return {};
    }

    std::string_view ShowDocument::addressOwnerFor (std::string_view element)
    {
        /*  The places the two answers differ, and the reason is in the header:
            a rack channel, a processor input and a strip share an address
            space and not a kind. Everything else is addressed under its own
            owner. */
        if (element == "Channel")                   return "slot";
        if (element == "Strip")                     return "slot";

        return ownerForElement (element);
    }

    //==============================================================================
    void ShowDocument::ensureContainers (juce::ValueTree& root)
    {
        /*  THE CONTAINERS EVERY SHOW HAS, made here so that a document which
            arrived from a file has them too.

            A fresh document gets them from the constructor, and for a long
            while that was the whole story - but `adopt` REPLACES the root, so
            a show read off a disk has exactly the containers its file happened
            to carry. A hand-written show.xml with no <Mounts/> could not be
            given a device at all: `createMount` asks for the container by name
            and inserts into whatever it gets, which for an absent one is an
            invalid tree and a refusal nobody could act on. That was latent
            long before this file grew a <Network/>; it is fixed here rather
            than in each create, because the next container would forget it too.

            EMPTY AND SILENT. The caller is either the constructor, before the
            listener is attached, or `adopt`, which is a swap and not an edit -
            so no history, no change count, and a show that is opened and saved
            untouched gains an empty element or two in its file and nothing
            else. */
        /*  IN THE ORDER THE SCHEMA DECLARES THEM, because the canonical
            writer keeps the order it is given and a golden file compares byte
            for byte: adding them in any other order would write a show whose
            containers read differently from one somebody opened. */
        for (const auto* name : { "Lists", "Mounts", "MidiPorts", "Network", "Surfaces", "Dcas" })
            if (! root.getChildWithName (name).isValid())
                root.addChild (juce::ValueTree (name), -1, nullptr);
    }

    ShowDocument::ShowDocument()
        : showNode (juce::Identifier (juce::String (std::string (Schema::rootElement)))),
          registry (IdRegistry::withSystemEntropy())
    {
        /*  The containers exist from the start, empty. A show with no lists
            still has a Lists element, so the file has an obvious place to put
            the first one and a diff that adds a list touches one line rather
            than three. */
        ensureContainers (showNode);

        /*  Audio is a container like the other two, but unlike them it carries
            a value of its own, and that value has no default: `tracks` is the
            polyphony ceiling and every show has to state it. A fresh document
            says zero, which is a real answer - a show with no audio - and not
            a placeholder standing in for one. */
        juce::ValueTree audio { "Audio" };
        audio.setProperty ("tracks", 0, nullptr);
        showNode.addChild (audio, -1, nullptr);

        makeHistories();

        /*  LAST, so that building the empty containers above does not count as
            three changes to a document nobody has opened yet. A fresh document
            is at revision 1 and stays there until somebody writes something. */
        showNode.addListener (this);
    }

    EditResult ShowDocument::configureAudio (const audio::AudioSettings& settings)
    {
        if (isLocked()) return EditResult::failed (reason::locked);
        if (! audio::validAudioSettings (settings)) return EditResult::failed (reason::badValue);
        const std::pair<const char*, std::string> fields[] {
            { "enabled", settings.enabled ? "true" : "false" },
            { "deviceType", settings.deviceType }, { "outputDevice", settings.outputDevice },
            { "inputDevice", settings.inputDevice }, { "bufferSize", std::to_string (settings.bufferSize) },
            { "inputPatch", settings.inputPatch }, { "outputPatch", settings.outputPatch }
        };
        for (const auto& [name, value] : fields)
            if (const auto result = setAttribute (std::string ("/godot/audio/") + name, value); ! result.ok)
                return result;
        return EditResult::succeeded();
    }

    ShowDocument::ShowDocument (ShowDocument&& other)
        : showNode (juce::ValueTree()), registry (IdRegistry::withSystemEntropy())
    {
        *this = std::move (other);
    }

    ShowDocument& ShowDocument::operator= (ShowDocument&& other)
    {
        if (this == &other)
            return *this;

        showNode.removeListener (this);
        other.showNode.removeListener (&other);

        showNode = std::move (other.showNode);
        registry = std::move (other.registry);
        changeCount = other.changeCount;
        showChangeCount = other.showChangeCount;

        /*  REBUILT EMPTY, NOT CARRIED. The header argues it; what it costs here
            is one line, and what a carried history would have cost is a stack of
            actions holding ref-counted handles into a tree that has just changed
            owner. The coalescing key goes with them: a window measured against a
            write made by the document this one was moved from would be a window
            about somebody else's gesture. */
        makeHistories();
        forgetCoalescing();
        undoSuppressions = 0;

        showNode.addListener (this);
        return *this;
    }

    void ShowDocument::makeHistories()
    {
        for (auto& domain : histories)
            domain = std::make_unique<juce::UndoManager>();
    }

    void ShowDocument::forgetCoalescing() noexcept
    {
        lastWriteAddress.clear();
        lastWriteOrigin.clear();
        lastWriteTick = 0;
    }

    ShowDocument::~ShowDocument()
    {
        showNode.removeListener (this);
    }

    void ShowDocument::adopt (juce::ValueTree newRoot, IdRegistry newRegistry)
    {
        /*  A HATCH, NOT A DOOR, and the edit lock is deliberately not asked
            here.

            This is the fifth writer of the document and it goes through none
            of the four doors: it is how `Bundle::open` loads a show, and since
            PR 5.5 it is how `document.revert` and `document.recover` replace one
            (Bundle.cpp).
            It does not edit the show, it swaps it - and because `locked` is an
            attribute of the root it swaps THE LOCK too, for whatever the
            loaded state.xml says. A revert of a bundle saved unlocked would
            silently unlock a locked session.

            Guarding it here would be wrong twice. The load that opens a
            locked bundle comes through here with nothing locked yet (the lock
            arrives a moment later, restored from state.xml through
            `setAttribute`), so a guard would never fire when it mattered and
            would refuse the loads that are not edits at all. And a guard that
            did fire would leave the caller holding a half-opened bundle. So
            the refusal belongs to the COMMANDS that open this hatch on a live
            show - revert and recover check `isLocked()` in their handlers,
            before they read a byte - and the hatch itself stays what it is: a
            replacement of the show, lock included (namespace draft §14.11). */

        /*  The listener follows the document. A listener left on the tree that
            was just replaced would report nothing (nobody writes to it any
            more) and would outlive nothing, but it would also mean the NEW tree
            is unwatched - so every edit after a load would be invisible to
            `revision()`, and a cache built before the load would look current
            for ever. */
        /*  AND THE ONE PLACE A SUPPRESSION EARNS ITS KEEP, today. Nothing under
            here writes through a door, so what the scope buys is the rule rather
            than a behaviour: a hatch is not an edit, and anything a later phase
            adds inside it is not one either. */
        const ScopedUndoSuppression suppressed { *this };

        /*  THE HISTORY GOES WITH THE SHOW IT IS ABOUT. Every action on the stack
            holds a ref-counted handle into the graph that is about to be
            replaced, so an uncleared stack would keep the previous show alive in
            memory and undo into a tree nobody can see. Cleared BEFORE the swap,
            so the old show's last handles are dropped while it is still the
            show. */
        for (auto& domain : histories)
            domain->clearUndoHistory();

        forgetCoalescing();

        showNode.removeListener (this);

        showNode = std::move (newRoot);
        registry = std::move (newRegistry);

        /*  BEFORE THE LISTENER, so that giving a loaded show the containers it
            was missing does not count as an edit to it. See ensureContainers. */
        ensureContainers (showNode);

        showNode.addListener (this);

        /*  A load is the largest change there is - to the show half as much as
            to the whole, which is why a verb that has just opened a bundle
            stamps its session AFTER the load rather than before it. */
        bumpStructure();
    }

    void ShowDocument::valueTreePropertyChanged (juce::ValueTree& node,
                                                 const juce::Identifier& property)
    {
        ++changeCount;

        /*  WHICH FILE THIS VALUE LIVES IN, asked of the same column the two
            writers ask (CanonicalXml.cpp and EphemeralState.cpp both filter on
            `persist`), so the dot and the files cannot disagree about what a
            save would write. Looked up by the element the property landed on,
            which is what makes a Group's inherited Cue rows answer as Cue rows
            do - Schema applies that rule once, and this reads its result.

            A row the schema does not know counts as show: see `showRevision()`
            for why guessing dirty is the safe way to be wrong. */
        const auto* attribute = Schema::instance().attribute (node.getType().toString().toStdString(),
                                                              property.toString().toStdString());

        if (attribute == nullptr || attribute->persist() == Persist::show)
            ++showChangeCount;
    }

    //==============================================================================
    juce::ValueTree ShowDocument::findById (std::string_view id) const
    {
        if (id.empty())
            return {};

        const juce::String wanted { juce::String (std::string (id)) };

        /*  Depth-first over the whole tree. Linear, and deliberately so: a show
            holds thousands of objects, not millions, and an index would be a
            second source of truth to keep correct across every structural edit.
            If this ever shows up in a profile, the fix is a cache invalidated in
            one place, not a second map maintained in five. */
        struct Search
        {
            static juce::ValueTree find (const juce::ValueTree& node, const juce::String& target)
            {
                if (node.hasProperty (idProperty) && node[idProperty].toString() == target)
                    return node;

                for (const auto& child : node)
                    if (auto found = find (child, target); found.isValid())
                        return found;

                return {};
            }
        };

        return Search::find (showNode, wanted);
    }

    //==============================================================================
    juce::ValueTree ShowDocument::containerElementFor (std::string_view segment) const
    {
        if (segment == "document") return showNode;
        if (segment == "audio")    return showNode.getChildWithName ("Audio");
        if (segment == "list")     return showNode.getChildWithName ("Lists");
        if (segment == "network")  return showNode.getChildWithName ("Network");
        if (segment == "port")     return showNode.getChildWithName ("MidiPorts");
        if (segment == "surface")  return showNode.getChildWithName ("Surfaces");
        if (segment == "dca")      return showNode.getChildWithName ("Dcas");
        return {};
    }

    Resolved ShowDocument::resolve (const std::string& address) const
    {
        Resolved out;

        const auto parts = splitAddress (address);

        // /godot/<owner>/<id>/<attribute>, or /godot/document/<attribute>.
        if (parts.size() < 3 || parts[0] != "godot")
            return out;

        const auto& owner = parts[1];

        juce::ValueTree node;
        std::string attributeName;

        /*  A CONTAINER ADDRESS HAS THREE PARTS, an object address has four, and
            the count is what tells them apart rather than the word. `list` is
            now BOTH - `/godot/list/focus` names the collection and
            `/godot/list/<id>/standby` names one of its members - so a branch on
            the owner word alone would have had to choose, and choosing would
            have meant spelling the container differently for no reason a client
            could see. */
        const auto container = parts.size() == 3 ? containerElementFor (owner)
                                                 : juce::ValueTree {};

        if (container.isValid())
        {
            /*  The elements that carry facts about a COLLECTION rather than
                about any member: the show itself, the audio rig's track count,
                and which list has the focus. None has an identifier, because
                there is only one of each. */
            node = container;
            attributeName = parts[2];
        }
        else
        {
            if (parts.size() == 3)
                return out;

            if (parts.size() != 4)
                return out;

            node = findById (parts[2]);
            attributeName = parts[3];

            if (! node.isValid())
                return out;

            /*  The owner word has to match what was actually found, so that
                /godot/list/<a cue's id>/name does not quietly address the cue.
                An identifier alone is unambiguous; the word is there for the
                reader, and a reader that can be wrong is worse than no reader. */
            if (addressOwnerFor (node.getType().toString().toStdString()) != owner)
                return out;
        }

        const auto element = node.getType().toString().toStdString();
        out.attribute = Schema::instance().attribute (element, attributeName);

        if (out.attribute == nullptr)
        {
            /*  Not an attribute of the document - but it may still be a node
                the parameter tree publishes, computed from the structure. Say
                which, so a refusal can name the real reason. */
            if (const auto* schemaElement = Schema::instance().element (element))
            {
                out.attribute = schemaElement->derivedAttribute (attributeName);
                out.isDerived = out.attribute != nullptr;
            }
        }

        if (out.attribute != nullptr)
            out.node = node;

        return out;
    }

    //==============================================================================
    bool ShowDocument::isLocked() const
    {
        /*  An absent attribute IS its default, and the row's default is
            false: a show nobody has locked is not locked.

            Read as the stored var rather than through `getAttribute`, because
            there is exactly one way a lock reaches the tree - `setAttribute`,
            from a client's write or from state.xml; the reader refuses a
            show.xml carrying one as engine state - and that door typed it as
            a boolean on the way in, never as the text "true". And because
            this is asked by every door, and need not parse an address to
            answer. */
        return static_cast<bool> (showNode[lockedProperty]);
    }

    std::optional<EditResult> ShowDocument::refuseIfLocked() const
    {
        if (! isLocked())
            return std::nullopt;

        return EditResult::failed (reason::locked);
    }

    //==============================================================================
    std::string_view undoDomainWord (UndoDomain domain) noexcept
    {
        /*  Spelled out rather than defaulted, so that adding Phase 6's domain
            to the enum and forgetting its word is a build failure here rather
            than an empty string a client cannot address. */
        switch (domain)
        {
            case UndoDomain::document: return "document";
        }

        return {};
    }

    std::optional<UndoDomain> undoDomainForWord (std::string_view word) noexcept
    {
        if (word == undoDomainWord (UndoDomain::document))
            return UndoDomain::document;

        return std::nullopt;
    }

    void ShowDocument::beginTransaction (const std::string& commandName,
                                         std::int64_t tick,
                                         const std::string& writeOrigin,
                                         const std::vector<osc::Value>& args)
    {
        /*  ONLY A `node.set` HAS AN ADDRESS TO COALESCE ON, and everything else
            leaves this empty - which is how a create, a delete, a move and an
            `undo` each break a run without needing a rule of their own. */
        const auto address = (commandName == "node.set" && ! args.empty() && args[0].isString())
                               ? args[0].getString()
                               : std::string {};

        /*  The address is tested FIRST and the arithmetic is behind it, so the
            tick difference is only ever computed against a tick a real write
            stamped. `tick >= lastWriteTick` is there for the caller that hands
            them out of order: a window measured backwards is not a window. */
        const auto joinsOpenTransaction = ! address.empty()
                                            && address == lastWriteAddress
                                            && writeOrigin == lastWriteOrigin
                                            && tick >= lastWriteTick
                                            && tick - lastWriteTick <= coalescingWindowTicks;

        /*  A PLUGIN'S STATE JOINS THE TURN THAT LEFT IT (the author's decision
            of 2026-09-25: "one Undo"). The editing helper keeps a plugin's
            whole state with the cue a moment after the hand stops, as
            `fx.capture` - and when the last write was a turn of THAT insert's
            parameters, from the same origin, within the window, the capture
            is part of the same thing somebody did and joins its step. Keyed on
            logged ticks and origins, so a replay splits exactly as the session
            did. A capture after anything else - an impulse response loaded,
            which moves no parameter - is a step of its own. */
        const auto captured = (commandName == "fx.capture" && ! args.empty() && args[0].isString())
                                ? args[0].getString()
                                : std::string {};
        const auto turnPrefix = "/godot/fx/" + captured + "/p";

        const auto joinsTurn = ! captured.empty()
                                 && lastWriteAddress.size() > turnPrefix.size()
                                 && lastWriteAddress.compare (0, turnPrefix.size(), turnPrefix) == 0
                                 && lastWriteAddress[turnPrefix.size()] >= '0'
                                 && lastWriteAddress[turnPrefix.size()] <= '9'
                                 && writeOrigin == lastWriteOrigin
                                 && tick >= lastWriteTick
                                 && tick - lastWriteTick <= captureJoinWindowTicks;

        if (! joinsOpenTransaction && ! joinsTurn)
        {
            /*  EVERY DOMAIN, because which one a command writes to is the
                command's business and not this hook's, and naming a transaction
                on a history the command turns out not to touch costs nothing:
                `beginNewTransaction` sets a flag and a name and allocates
                nothing at all. An empty transaction never becomes a step. */
            for (auto& domain : histories)
                domain->beginNewTransaction (juce::String (commandName));
        }

        lastWriteAddress = address;
        lastWriteOrigin = writeOrigin;
        lastWriteTick = tick;
    }

    std::optional<std::string> ShowDocument::undo (UndoDomain domain)
    {
        auto& manager = *histories[static_cast<std::size_t> (domain)];

        if (! manager.canUndo())
            return std::nullopt;

        /*  READ BEFORE IT IS POPPED, because afterwards it names the transaction
            BEFORE the one that was taken back - and what the record has to carry
            is the name of the one this command actually unmade. */
        const auto name = manager.getUndoDescription().toStdString();

        if (! manager.undo())
            return std::nullopt;

        rebuildRegistry();
        forgetCoalescing();

        return name;
    }

    std::optional<std::string> ShowDocument::redo (UndoDomain domain)
    {
        auto& manager = *histories[static_cast<std::size_t> (domain)];

        if (! manager.canRedo())
            return std::nullopt;

        const auto name = manager.getRedoDescription().toStdString();

        if (! manager.redo())
            return std::nullopt;

        /*  A REDO REBUILDS THE REGISTRY TOO, and for the mirrored reason: redoing
            a delete takes a subtree out again, and the identifiers under it have
            to go back to being free or the show slowly runs out of names it is
            allowed to draw. */
        rebuildRegistry();
        forgetCoalescing();

        return name;
    }

    const juce::UndoManager& ShowDocument::history (UndoDomain domain) const noexcept
    {
        return *histories[static_cast<std::size_t> (domain)];
    }

    juce::UndoManager* ShowDocument::structuralHistory() noexcept
    {
        if (undoSuppressions > 0)
            return nullptr;

        return histories[static_cast<std::size_t> (UndoDomain::document)].get();
    }

    juce::UndoManager* ShowDocument::historyFor (const Attribute& attribute) noexcept
    {
        if (attribute.persist() != Persist::show)
            return nullptr;

        return structuralHistory();
    }

    void ShowDocument::rebuildRegistry()
    {
        std::vector<std::string> present;
        collectIds (showNode, present);

        registry.clear();

        for (const auto& id : present)
            registry.reserve (id);
    }

    ShowDocument::ScopedUndoSuppression::ScopedUndoSuppression (ShowDocument& documentToSuppress) noexcept
        : target (documentToSuppress)
    {
        ++target.undoSuppressions;
    }

    ShowDocument::ScopedUndoSuppression::~ScopedUndoSuppression()
    {
        --target.undoSuppressions;
    }

    //==============================================================================
    EditResult ShowDocument::setAttribute (const std::string& address, std::string_view text)
    {
        auto target = resolve (address);

        if (! target.isValid())
            return EditResult::failed (reason::badAddress);

        /*  A derived value is read-only by construction, whatever its row
            says: it is computed from the structure, so there is nowhere to put
            a written one and the structure is what would have to change. */
        if (target.isDerived || target.attribute->access() == Access::read)
            return EditResult::failed (reason::readOnly);

        /*  THE EDIT LOCK, for the show half and only the show half.

            `persist == show` AND NOTHING ELSE - not `!= state`. A value that
            reaches this line is `show` or `state` (a `none` row is derived and
            was refused above), so today the two spellings agree; they stop
            agreeing the day a writable `none` row is resolved here, and the
            question this line asks is "would show.xml record it?", which is
            the one that stays right. The state half is where the operator is
            standing: the standby GO moves, the focus, and the lock itself,
            whose release has to get through or the lock could never be lifted.
            It is also what EphemeralState restores a bundle through, so a lock
            that refused it would make a locked bundle refuse its own lock.

            HERE, BESIDE THE READ-ONLY CHECK AND BEFORE THE PARSE, and the
            position is the point. Asked after the parse, a locked show would
            answer `type-mismatch` for a badly typed value and `locked` only
            for a well-typed one, sending an operator to look at their encoder
            when the answer was that the show is fixed. The address has to be
            resolved first, because which half a value lives in is a property
            of its row; everything after that is a question a locked show
            would refuse anyway. */
        if (target.attribute->persist() == Persist::show)
        {
            if (auto refusal = refuseIfLocked())
                return *refusal;
        }

        /*  A LIST, which this door could not take until PR 5.16a (§14.6):
            `parseValue` read "0 -60 1 0" as one number and refused it, so the
            two `gains` rows had never been written by a client. Parsed element
            by element through the reader's own function and stored as its
            canonical text, which is exactly how the reader stores one - so a
            list written here and a list loaded from a file are the same value.
            A bad element is the wrong kind of thing and answers `type-mismatch`,
            as a bad scalar does.

            AND THE LIST MUST BE WHAT ITS ROW SAYS IT IS, or it answers
            `bad-value`: every element was a number, and together they are not
            a curve or not a matrix. A fade's points must pair up, climb from 0
            to 1 and stay in a fade's range; a destination's gains must be whole
            rows as wide as its bus or slot. Asked here as well as in
            validate(), by the same functions, because this is the one door a
            client writes through - and validate() REFUSES THE FILE. A write
            applied here that the next load refused would have turned one
            datagram into a show that does not open, which is a great deal
            worse than a refusal the client can read. */
        if (target.attribute->isList())
        {
            std::string canonical;

            if (! Schema::parseList (*target.attribute, text, canonical).ok)
                return EditResult::failed (reason::typeMismatch);

            const auto name = target.attribute->name();

            if (target.attribute->element == "Fade" && name == "points"
                && ! readFadePoints (canonical).problem.empty())
                return EditResult::failed (reason::badValue);

            if (name == "gains"
                && ! coefficientsFit (countTokens (canonical), destinationWidth (*this, target.node)))
                return EditResult::failed (reason::badValue);

            target.node.setProperty (juce::Identifier (juce::String (std::string (name))),
                                     juce::var (juce::String (canonical)), historyFor (*target.attribute));

            return EditResult::succeeded (target.node[idProperty].toString().toStdString());
        }

        Value value;
        const auto parsed = Schema::parseValue (*target.attribute, text, value);

        if (! parsed.ok)
            return EditResult::failed (reason::typeMismatch);

        if (target.attribute->element == "Audio"
            && (target.attribute->name() == "inputPatch" || target.attribute->name() == "outputPatch"))
        {
            std::vector<int> patch;
            if (! audio::readPatch (std::string (text), patch)) return EditResult::failed (reason::badValue);
        }

        /*  ONE REFERENTIAL INVARIANT, and it is named rather than generalised.

            A list's standby must name a cue that list may be parked on - one of
            its stops, in `cue/CueList.h`'s word - or be empty. (It read "one of
            that list's own top-level children" until PR 3.4 and was true when it
            was written; the pointer has descended into groups since.) The schema
            can say a value is a string in range; it has no way to say a value
            must be the identifier of a child of the element carrying it, and
            adding a referential column to the parameter table for a single
            attribute would be building the generalisation before there are two
            cases to generalise. Phase 3's run pointer is the second case; that
            is when the column earns itself.

            IT LIVES HERE because here is the only door. The standby commands,
            a client's node.set and EphemeralState restoring a saved show all
            arrive through setAttribute, and a check anywhere else would guard
            one of those three and miss the other two - the load path in
            particular, which is where a state file written against a different
            show shows up. */
        if (target.attribute->name() == "standby"
            && target.node.getType().toString() == "List"
            && ! isLegalStandbyFor (target.node, value.getString()))
        {
            /*  TWO REFUSALS, because they send somebody somewhere different -
                and this door has to give the same answer `standby.set` gives,
                or a client would learn one thing from the command and another
                from the node.

                `not-in-list`: that cue belongs somewhere else, or is not a cue.
                `not-a-stop`: it is in THIS list and is not one of the places the
                pointer may stand - a cue in a group's header, in its footer, or
                in a persistent section - and the remedy is to park on the group
                that owns it. */
            const auto elsewhere = ! cue::isInList (target.node, value.getString());

            return EditResult::failed (elsewhere ? reason::notInList
                                                 : reason::notAStop);
        }

        /*  AND A SECOND ONE, for the same reason and in the same place: a DCA
            may sit inside a DCA (PRD §3.28), and one that ended up inside
            itself would make its members' level a sum with no end. The walk
            starts at the DCA being named and climbs; reaching the one being
            written is the refusal. Bounded by the number of DCAs, so a file
            that already carries a cycle - which `validate()` refuses to open,
            but a reader should not have to trust that - cannot hang the door. */
        if (target.attribute->name() == "dca"
            && target.node.getType().toString() == "Dca"
            && dcaChainReaches (value.getString(),
                                target.node[idProperty].toString().toStdString()))
            return EditResult::failed (reason::badValue);

        /*  THE HISTORY THE ROW BELONGS ON, which is the document's for a
            `persist == show` value and nothing at all for a state one: undo
            restores what someone decided, and where the operator is standing is
            not among those things (PRD §4.10). The manager is the THIRD
            argument here and the SECOND to the `removeChild` overload `remove`
            and `move` use, which is the detail a reader who learns "third" gets
            wrong in exactly one place. */
        target.node.setProperty (juce::Identifier (juce::String (std::string (target.attribute->name()))),
                                 toVar (value), historyFor (*target.attribute));

        return EditResult::succeeded (target.node[idProperty].toString().toStdString());
    }

    std::optional<std::string> ShowDocument::getAttribute (const std::string& address) const
    {
        const auto target = resolve (address);

        if (! target.isValid())
            return std::nullopt;

        /*  The document does not hold a derived value - that is what derived
            means - so it says so rather than handing back a default that would
            look like an answer. The parameter tree computes these. */
        if (target.isDerived)
            return std::nullopt;

        const juce::Identifier property { juce::String (std::string (target.attribute->name())) };

        /*  An absent attribute IS its default. That equivalence is what lets the
            writer omit defaults and still round-trip, so it has to hold here
            too, not only in the writer. */
        if (! target.node.hasProperty (property))
            return std::string (target.attribute->defaultText());

        return toText (*target.attribute, target.node[property]);
    }

    //==============================================================================
    EditResult ShowDocument::insertObject (juce::ValueTree parent, int index,
                                           std::string_view elementName,
                                           const std::string& id,
                                           const std::vector<std::pair<std::string_view, std::string>>& attributes)
    {
        /*  THE DOOR EVERY CREATE COMES THROUGH - the ten creates, and the
            header, footer and persistent section that are made on first ask.
            Asked before an identifier is drawn or reserved, so a refused
            create leaves the registry exactly as it found it.

            Two things follow from its being here and not in each create. The
            creates that answer with an existing child (`createRole`,
            `createPersistent`) return before they reach this line and are
            applied under the lock, which is right: they change nothing. And
            one create that changes the document on its way here has to ask
            for itself - see `createRackChannel`. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

        const auto* element = Schema::instance().element (elementName);

        if (element == nullptr || ! parent.isValid())
            return EditResult::failed (reason::badAddress);

        const auto* parentElement = Schema::instance().element (parent.getType().toString().toStdString());

        if (parentElement == nullptr || ! parentElement->mayContain (elementName))
            return EditResult::failed (reason::badAddress);

        if (index < 0)
            return EditResult::failed (reason::badAddress);

        std::string objectId = id;

        if (objectId.empty())
        {
            objectId = registry.generate();

            if (objectId.empty())
                return EditResult::failed (reason::unknownId);
        }
        else if (! Id::isValid (objectId) || ! registry.reserve (objectId))
        {
            /*  A supplied identifier that is malformed or already taken. Both
                happen during replay if a log has been edited, and both must be
                refusals rather than a second object wearing the same name. */
            return EditResult::failed (reason::unknownId);
        }

        /*  Built complete, THEN added. A listener that saw the child appear and
            the identifier arrive afterwards would publish an object with no
            address — which is exactly what the parameter tree does on
            valueTreeChildAdded in Phase 1.5.

            AND THAT IS WHY THE WRITES BELOW STAY ON nullptr while the add
            further down takes the history. The node is not in the tree when they
            run, so an action recording them would hold a handle on an object
            nobody could reach; the one action at the add carries the whole
            finished object, identifier and all, and undoing it takes the object
            away entire. */
        juce::ValueTree node { juce::Identifier (juce::String (std::string (elementName))) };
        node.setProperty (idProperty, juce::String (objectId), nullptr);

        for (const auto& [name, text] : attributes)
        {
            const auto* attribute = element->attribute (name);

            if (attribute == nullptr)
            {
                registry.release (objectId);
                return EditResult::failed (reason::badAddress);
            }

            if (attribute->isList())
            {
                std::string canonical;
                if (! Schema::parseList (*attribute, text, canonical).ok)
                {
                    registry.release (objectId);
                    return EditResult::failed (reason::typeMismatch);
                }
                node.setProperty (juce::Identifier (juce::String (std::string (name))), juce::String (canonical), nullptr);
                continue;
            }

            Value value;

            if (! Schema::parseValue (*attribute, text, value).ok)
            {
                registry.release (objectId);
                return EditResult::failed (reason::typeMismatch);
            }

            node.setProperty (juce::Identifier (juce::String (std::string (name))),
                              toVar (value), nullptr);
        }

        /*  THE INDEX IS A MEMBER POSITION, translated here into the raw child
            index juce::ValueTree wants - `document/Sequence.h` says why, and
            what reading the two as one number cost. No clamp is needed: the
            translation never answers past the last child, because "past the
            last member" already answers `getNumChildren()`, which is where an
            append belongs. */
        parent.addChild (node, rawIndexForPosition (parent, index), structuralHistory());

        return EditResult::succeeded (objectId);
    }

    EditResult ShowDocument::createList (const std::string& name, const std::string& id)
    {
        /*  `endOfSequence` here and in every create below that appends rather
            than places: the index those creates pass is a member position like
            any other now, and a raw child count passed as one would be the very
            confusion `document/Sequence.h` exists to end - it happens to mean
            the same thing only because a container never has fewer children
            than members. */
        return insertObject (showNode.getChildWithName ("Lists"),
                             endOfSequence, "List", id, { { "name", name } });
    }

    EditResult ShowDocument::createCue (const std::string& parentId, int index,
                                        const std::string& kind, const std::string& name,
                                        const std::string& id)
    {
        const auto elementName = elementForKind (kind);

        if (elementName.empty())
            return EditResult::failed (reason::typeMismatch);

        auto parent = findById (parentId);

        if (! parent.isValid())
            return EditResult::failed (reason::unknownId);

        /*  `kind` is a read-only attribute derived from the element, so it is
            not written: /godot/cue/<id>/kind reports "group" because the element
            is a Group, and a client cannot turn one into the other by writing
            to it. */
        return insertObject (parent, index, elementName, id, { { "name", name } });
    }

    EditResult ShowDocument::groupSelection (const std::vector<std::string>& ids, const std::string& id)
    {
        if (auto refusal = refuseIfLocked()) return *refusal;
        if (ids.empty()) return EditResult::failed (reason::badValue);
        std::vector<juce::ValueTree> selected;
        const auto* groupSchema = Schema::instance().element ("Group");
        for (const auto& selectedId : ids)
        {
            const auto node = findById (selectedId);
            if (! node.isValid()) return EditResult::failed (reason::unknownId);
            if (! groupSchema->mayContain (node.getType().toString().toStdString())
                || ! isSequenceChild (node)) return EditResult::failed (reason::typeMismatch);
            if (std::find (selected.begin(), selected.end(), node) == selected.end()) selected.push_back (node);
        }
        // A selected group already carries its selected descendants.
        const auto all = selected;
        selected.erase (std::remove_if (selected.begin(), selected.end(), [&all] (const auto& node)
        {
            for (auto up = node.getParent(); up.isValid(); up = up.getParent())
                if (std::find (all.begin(), all.end(), up) != all.end()) return true;
            return false;
        }), selected.end());
        auto parent = selected.front().getParent();
        const auto containsAll = [&selected] (const juce::ValueTree& candidate)
        {
            for (const auto& node : selected)
            {
                auto up = node.getParent();
                while (up.isValid() && up != candidate) up = up.getParent();
                if (! up.isValid()) return false;
            }
            return true;
        };
        while (parent.isValid() && ! containsAll (parent)) parent = parent.getParent();
        const auto* parentSchema = Schema::instance().element (parent.getType().toString().toStdString());
        if (! parentSchema || ! parentSchema->mayContain ("Group")) return EditResult::failed (reason::badAddress);
        std::vector<juce::ValueTree> ordered;
        const auto walk = [&] (auto&& self, const juce::ValueTree& node) -> void
        {
            if (std::find (selected.begin(), selected.end(), node) != selected.end()) ordered.push_back (node);
            else for (const auto& child : node) self (self, child);
        };
        walk (walk, parent);
        auto first = ordered.front();
        while (first.getParent() != parent) first = first.getParent();
        int position = 0;
        for (const auto& sibling : parent)
        {
            if (sibling == first) break;
            if (isSequenceChild (sibling)) ++position;
        }
        // All sources and the destination are validated before the first edit.
        const auto created = createCue (parent[idProperty].toString().toStdString(), position, "group", "", id);
        if (! created.ok) return created;
        for (const auto& node : ordered)
            move (node[idProperty].toString().toStdString(), created.id, endOfSequence);
        return created;
    }

    EditResult ShowDocument::createRoute (const std::string& cueId,
                                          const std::string& busId,
                                          const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        /*  Only a media cue has anywhere for a sound to go. Refusing here
            rather than in the grammar means the client is told which of its two
            identifiers was wrong, and told it at the moment it asked. */
        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (cue, endOfSequence, "Route", id,
                             { { "bus", busId } });
    }

    EditResult ShowDocument::defaultMediaRoute (const std::string& cueId, int channels,
                                               const std::string& id)
    {
        if (auto refusal = refuseIfLocked()) return *refusal;
        const auto cue = findById (cueId);
        if (! cue.isValid()) return EditResult::failed (reason::unknownId);
        if (! cue.hasType ("Media") || channels < 1 || channels > 512)
            return EditResult::failed (reason::typeMismatch);
        for (const auto& child : cue)
            if (child.hasType ("Route") || child.hasType ("Feed"))
                return EditResult::succeeded (child[idProperty].toString().toStdString());

        juce::ValueTree destination;
        for (const auto& bus : showNode.getChildWithName ("Audio"))
            if (bus.hasType ("Bus") && (! destination.isValid()
                || static_cast<int> (bus.getProperty ("firstChannel", 0)) < static_cast<int> (destination.getProperty ("firstChannel", 0))))
                destination = bus;
        if (! destination.isValid()) return EditResult::failed (reason::badAddress);
        const auto width = static_cast<int> (destination.getProperty ("width", 1));
        if (width < 1 || width > 512) return EditResult::failed (reason::typeMismatch);
        // Input-major coefficients: mono feeds both sides of a stereo bus;
        // wider files map matching channels, without an implicit downmix.
        std::string gains;
        for (int input = 0; input < channels; ++input)
            for (int output = 0; output < width; ++output)
            {
                if (! gains.empty()) gains += ' ';
                gains += (input == output || (channels == 1 && width == 2)) ? '1' : '0';
            }
        return insertObject (cue, endOfSequence, "Route", id,
                             { { "bus", destination[idProperty].toString().toStdString() }, { "gains", gains } });
    }

    EditResult ShowDocument::createSlot (const std::string& mountId,
                                         const std::string& address,
                                         const std::string& id)
    {
        auto mount = findById (mountId);

        if (! mount.isValid())
            return EditResult::failed (reason::unknownId);

        /*  A slot belongs to the processor it is an input of. Declared on the
            mount rather than in a pool of its own, because "which of WFS-DIY's
            inputs this show uses" is a fact about that mount and moves with it
            when somebody re-points the show at another box. */
        if (mount.getType().toString() != "Mount")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (mount, endOfSequence, "Slot", id,
                             { { "address", address } });
    }

    EditResult ShowDocument::createRackChannel (const std::string& channelClass,
                                                const std::string& id)
    {
        /*  ASKED HERE AS WELL AS AT THE DOOR, because this is the one create
            that changes the document before it reaches `insertObject`: the
            `<Rack/>` below is made on demand and added to the tree first. A
            lock asked only at the door would let a locked show gain an empty
            rack, move `revision()` and `showRevision()` through the listener,
            light the dirty dot - and THEN refuse. A document changed by a
            refusal, in the phase whose subject is trusting the save.

            No other create has that shape: `createRole` and
            `createPersistent` answer with an existing child before they
            insert anything, and every other create only reads on its way to
            the door. A create added later that makes a container on demand
            needs this line too. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

        auto audio = showNode.getChildWithName ("Audio");

        if (! audio.isValid())
            return EditResult::failed (reason::unknownId);

        /*  THE RACK IS MADE ON DEMAND, the way a group's header is: there is at
            most one, it carries nothing itself, and a show with no rack should
            not have an empty one in the file for the sake of it. */
        auto rack = audio.getChildWithName ("Rack");

        if (! rack.isValid())
        {
            rack = juce::ValueTree ("Rack");
            audio.addChild (rack, -1, nullptr);
        }

        return insertObject (rack, endOfSequence, "Channel", id,
                             { { "class", channelClass } });
    }

    //==============================================================================
    EditResult ShowDocument::writeOwned (juce::ValueTree node, std::string_view element,
                                         std::string_view name, std::string_view text)
    {
        const auto* attribute = Schema::instance().attribute (element, name);

        if (attribute == nullptr || ! node.isValid())
            return EditResult::failed (reason::badAddress);

        Value value;

        if (! Schema::parseValue (*attribute, text, value).ok)
            return EditResult::failed (reason::typeMismatch);

        node.setProperty (juce::Identifier (juce::String (std::string (name))),
                          toVar (value), historyFor (*attribute));

        return EditResult::succeeded (node[idProperty].toString().toStdString());
    }

    std::vector<juce::ValueTree> ShowDocument::busNodes() const
    {
        const auto audio = showNode.getChildWithName ("Audio");

        std::vector<juce::ValueTree> buses;

        if (! audio.isValid())
            return buses;

        for (const auto& child : audio)
            if (child.hasType ("Bus"))
                buses.push_back (child);

        /*  READ ORDER IS CHANNEL ORDER. A show written by the layout commands
            has the two the same and this changes nothing; a show written by
            hand may not, and then the list the designer is shown reads up the
            interface rather than down the file, which is the order they wired
            it in. Stable, so two buses that somehow start on the same channel
            keep the order the file gave them rather than swapping about
            between one command and the next. */
        const auto* first = Schema::instance().attribute ("Bus", "firstChannel");
        const auto defaultFirst = first != nullptr
                                    ? juce::String (juce::CharPointer_UTF8 (first->defaultText().data()),
                                                    first->defaultText().size()).getIntValue()
                                    : 0;

        const auto channelOf = [defaultFirst] (const juce::ValueTree& bus)
        {
            static const juce::Identifier property { "firstChannel" };

            return bus.hasProperty (property) ? static_cast<int> (bus[property]) : defaultFirst;
        };

        std::stable_sort (buses.begin(), buses.end(),
                          [&channelOf] (const juce::ValueTree& a, const juce::ValueTree& b)
                          { return channelOf (a) < channelOf (b); });

        return buses;
    }

    std::vector<juce::ValueTree> ShowDocument::inputNodes() const
    {
        std::vector<juce::ValueTree> inputs;

        const auto container = showNode.getChildWithName ("Audio").getChildWithName ("Inputs");

        if (! container.isValid())
            return inputs;

        for (const auto& child : container)
            if (child.hasType ("Input"))
                inputs.push_back (child);

        /*  READ ORDER IS CHANNEL ORDER, as the buses' is and for their reason:
            the list is read beside an interface, not beside a file. The default
            first channel is nought, which is what the table says. */
        const auto channelOf = [] (const juce::ValueTree& input)
        {
            static const juce::Identifier property { "firstChannel" };

            return input.hasProperty (property) ? static_cast<int> (input[property]) : 0;
        };

        std::stable_sort (inputs.begin(), inputs.end(),
                          [&channelOf] (const juce::ValueTree& a, const juce::ValueTree& b)
                          { return channelOf (a) < channelOf (b); });

        return inputs;
    }

    juce::ValueTree ShowDocument::inputsContainer (bool make)
    {
        auto audio = showNode.getChildWithName ("Audio");

        if (! audio.isValid())
            return {};

        auto inputs = audio.getChildWithName ("Inputs");

        if (! inputs.isValid() && make)
        {
            /*  AT A FIXED PLACE - after the last bus - whichever container was
                asked for first, so the canonical bytes of a show do not depend
                on the order two creates happened in: `createPlugin` puts the
                plugin set after the last bus AND after this. Outside the
                history, as the plugin set's container is: it carries nothing,
                and the input that made it is the step Undo takes back. */
            int at = 0;

            for (int i = 0; i < audio.getNumChildren(); ++i)
                if (audio.getChild (i).hasType ("Bus"))
                    at = i + 1;

            inputs = juce::ValueTree ("Inputs");
            audio.addChild (inputs, at, nullptr);
        }

        return inputs;
    }

    EditResult ShowDocument::applyLayout (const LayoutEdit& edit, const std::string& id,
                                          const std::string& kind, LayoutSide side)
    {
        /*  ASKED HERE, not only at `insertObject`'s door, for the reason
            `createRackChannel` gives: everything below changes the document
            before any create is reached - the repacked channels, the reorder,
            the patch - so a lock asked later would let a locked show be
            rearranged and THEN refuse. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

        auto audio = showNode.getChildWithName ("Audio");

        if (! audio.isValid())
            return EditResult::failed (reason::unknownId);

        /*  THE TWO SIDES OF THE INTERFACE (2026-09-26, namespace draft §18.2).
            The buses are the outputs and live in <Audio> itself; the named
            inputs are the other side and live in its <Inputs>. One arithmetic,
            and these are everything that differs. */
        const auto outputs = side == LayoutSide::outputs;
        const std::string element = outputs ? "Bus" : "Input";
        const juce::Identifier elementType { element.c_str() };
        const std::string prefix = outputs ? "/godot/bus/" : "/godot/input/";
        const std::string patchRow = outputs ? "/godot/audio/outputPatch" : "/godot/audio/inputPatch";
        const std::string settledRow = outputs ? "/godot/audio/patchSettled"
                                               : "/godot/audio/inputPatchSettled";

        const auto nodes = outputs ? busNodes() : inputNodes();

        std::vector<BusShape> before;
        before.reserve (nodes.size());

        for (const auto& bus : nodes)
        {
            BusShape shape;
            shape.id = bus[idProperty].toString().toStdString();

            /*  Through `getAttribute`, so a bus whose width the canonical
                writer dropped for being its default reads 1 rather than
                nought. Reading the property raw is the bug that made every
                saved-and-reopened show refuse its feeds as `bad-route`
                (`cue/ShowWalk.h` records it), and it is the same tree and the
                same omission here. */
            const auto width = getAttribute (prefix + shape.id + "/width");
            const auto first = getAttribute (prefix + shape.id + "/firstChannel");

            /*  `getIntValue` rather than `std::stoi`, which throws: every value
                here has been through the schema, so a number is what is there -
                but a door that reads a document must not be the one place a
                malformed file becomes an exception. */
            shape.width = width.has_value() ? juce::String (*width).getIntValue() : 1;
            shape.firstChannel = first.has_value() ? juce::String (*first).getIntValue() : 0;
            before.push_back (std::move (shape));
        }

        const auto settledText = getAttribute (settledRow);
        const auto patchText = getAttribute (patchRow);

        std::vector<int> patch;

        if (patchText.has_value() && ! audio::readPatch (*patchText, patch))
            return EditResult::failed (reason::badValue);

        const auto layout = applyLayoutEdit (before, patch,
                                             settledText.value_or ("false") == "true", edit);

        if (! layout.problem.empty())
            return EditResult::failed (edit.kind == LayoutEdit::Kind::create
                                         || edit.kind == LayoutEdit::Kind::resize
                                       ? reason::badValue : reason::unknownId);

        /*  AN INPUT IS ONE TO EIGHT CHANNELS - a microphone, a stereo line, a
            small multichannel feed - where a bus may be as wide as a processor
            send. Said here rather than by the arithmetic, which knows nothing
            of either side. */
        if (! outputs && (edit.kind == LayoutEdit::Kind::create || edit.kind == LayoutEdit::Kind::resize)
              && (edit.width < 1 || edit.width > 8))
            return EditResult::failed (reason::badValue);

        /*  WHERE THE LIST LIVES: <Audio> for the buses, its <Inputs> for the
            named inputs - made by the first input, after every check above has
            passed, so a refused create leaves no container behind. */
        auto parent = outputs ? audio : inputsContainer (edit.kind == LayoutEdit::Kind::create);

        if (! parent.isValid())
            return EditResult::failed (reason::unknownId);

        /*  THE STRUCTURAL STEP FIRST, so that a refusal inside it - an
            identifier already taken, a lock that arrived between two lines -
            leaves the channels as they were rather than repacked around a bus
            that was never made. */
        std::string made = id;

        switch (edit.kind)
        {
            case LayoutEdit::Kind::create:
            {
                /*  AN INPUT IS NAMED AS AN OUTPUT IS, by how many there are:
                    "Input 3", renamed when somebody knows it is Voix solo. */
                if (! outputs)
                {
                    const auto created = insertObject (parent, edit.index < 0 ? endOfSequence : edit.index,
                                                       element, id,
                                                       { { "name", "Input " + std::to_string (nodes.size() + 1) } });

                    if (! created.ok)
                        return created;

                    made = created.id;
                    break;
                }

                /*  A NAME IT CAN BE CALLED BY AT ONCE. An output with no name
                    is a row reading "Bus" in a menu of them, and the first
                    thing anybody would do is type one - so it arrives with
                    "Direct 3" or "Mix 2" and is renamed if that is wrong.
                    Counted over the outputs of its own kind rather than over
                    all of them, and by how many there are rather than by the
                    highest number in use: two outputs called "Mix 2" is a
                    smaller surprise than a designer's own names being read for
                    numbering.

                    `name` is `rw`, so a rename is an ordinary `node.set` and
                    nothing here has to know about it. */
                auto sameKind = 0;

                for (const auto& bus : nodes)
                    if (getAttribute ("/godot/bus/" + bus[idProperty].toString().toStdString() + "/kind")
                          .value_or ("direct") == kind)
                        ++sameKind;

                const auto name = (kind == "mix" ? std::string ("Mix ") : std::string ("Direct "))
                                    + std::to_string (sameKind + 1);

                const auto created = insertObject (audio, edit.index < 0 ? endOfSequence : edit.index,
                                                   "Bus", id, { { "kind", kind }, { "name", name } });

                if (! created.ok)
                    return created;

                made = created.id;
                break;
            }

            case LayoutEdit::Kind::remove:
            {
                auto node = findById (id);

                if (! node.isValid() || ! node.hasType (elementType))
                    return EditResult::failed (reason::unknownId);

                /*  AN INPUT TAKES NOTHING WITH IT today: no cue names one until
                    the mic cue does (namespace draft §18.2). */
                if (! outputs)
                {
                    std::vector<std::string> released;
                    collectIds (node, released);
                    parent.removeChild (node, structuralHistory());

                    for (const auto& gone : released)
                        registry.release (gone);

                    break;
                }

                /*  EVERY DESTINATION THAT NAMED IT GOES WITH IT, in this same
                    transaction. A route left naming a bus that has gone is a
                    run that fails `bad-route`, and it fails at GO rather than
                    here - months later, in a room with an audience in it. */
                std::vector<juce::ValueTree> orphans;
                std::vector<juce::ValueTree> clear;
                std::vector<juce::ValueTree> unpoint;

                const auto visit = [&] (const juce::ValueTree& node_, auto&& recurse) -> void
                {
                    for (const auto& child : node_)
                    {
                        /*  A SEND GOES THE WAY A ROUTE DOES. Both are objects
                            whose whole content is a destination and what
                            reaching it costs; with the destination gone there
                            is nothing left for either to be, and a send
                            naming no bus would be a strip in the mixer that
                            feeds nowhere. */
                        if ((child.hasType ("Route") || child.hasType ("Send"))
                              && child["bus"].toString().toStdString() == id)
                            orphans.push_back (child);
                        else if (child.hasType ("Slot") && child["bus"].toString().toStdString() == id)
                            clear.push_back (child);
                        else if (child.hasType ("Media")
                                   && child["directOut"].toString().toStdString() == id)
                            unpoint.push_back (child);

                        recurse (child, recurse);
                    }
                };

                visit (showNode, visit);

                for (const auto& orphan : orphans)
                {
                    std::vector<std::string> released;
                    collectIds (orphan, released);
                    orphan.getParent().removeChild (orphan, structuralHistory());

                    for (const auto& gone : released)
                        registry.release (gone);
                }

                /*  A processor input keeps its name and its width and loses the
                    bus it fed from: the slot is a declaration about the
                    processor, which has not changed, and `wfg validate` then
                    says it feeds from nowhere. Deleting it would throw away
                    an address and a width somebody typed. */
                for (const auto& slot : clear)
                    setAttribute ("/godot/slot/" + slot[idProperty].toString().toStdString() + "/bus", "");

                /*  AND A CUE KEEPS EVERYTHING BUT THE POINTER. Deleting the
                    cue, or leaving it aimed at a bus that is gone, would both
                    be worse than saying it lands nowhere: the file, the level
                    and the ranges are what somebody wrote, and only the
                    destination has stopped existing. */
                for (const auto& cue : unpoint)
                    setAttribute ("/godot/cue/" + cue[idProperty].toString().toStdString() + "/directOut", "");

                std::vector<std::string> released;
                collectIds (node, released);
                audio.removeChild (node, structuralHistory());

                for (const auto& gone : released)
                    registry.release (gone);

                break;
            }

            case LayoutEdit::Kind::move:
            case LayoutEdit::Kind::resize:
            {
                const auto node = findById (id);

                if (! node.isValid() || ! node.hasType (elementType))
                    return EditResult::failed (reason::unknownId);

                break;
            }
        }

        /*  THE DOCUMENT IS PUT INTO THE LIST'S ORDER, which for a show written
            by these commands it already is. `moveChild` one at a time rather
            than a sort, because every step has to be an undoable action on the
            same history - and because the answer is one pass: each bus in
            turn is moved to where the layout says it goes. */
        for (std::size_t at = 0; at < layout.buses.size(); ++at)
        {
            const auto& wanted = layout.buses[at];
            const auto node = wanted.id.empty() ? findById (made) : findById (wanted.id);

            if (! node.isValid())
                continue;

            if (const auto raw = parent.indexOf (node);
                raw >= 0 && raw != rawIndexForPosition (parent, static_cast<int> (at)))
                parent.moveChild (raw, std::min (rawIndexForPosition (parent, static_cast<int> (at)),
                                                 parent.getNumChildren() - 1),
                                  structuralHistory());
        }

        for (const auto& wanted : layout.buses)
        {
            const auto node = wanted.id.empty() ? findById (made) : findById (wanted.id);

            if (! node.isValid())
                continue;

            if (const auto result = writeOwned (node, element, "firstChannel",
                                                std::to_string (wanted.firstChannel));
                ! result.ok)
                return result;

            if (const auto result = writeOwned (node, element, "width",
                                                std::to_string (wanted.width));
                ! result.ok)
                return result;
        }

        /*  AND THE PATCH LAST. `patchChanged` is false in the two cases that
            must not write: a show still following its list, where the patch
            stays empty and the outputs follow the order, and an edit that
            moved nothing the patch could see. */
        if (layout.patchChanged)
            if (const auto result = setAttribute (patchRow, audio::writePatch (layout.outputPatch));
                ! result.ok)
                return result;

        return EditResult::succeeded (made);
    }

    EditResult ShowDocument::createBus (const std::string& kind, int width, int index,
                                        const std::string& id)
    {
        if (kind != "direct" && kind != "mix")
            return EditResult::failed (reason::badValue);

        LayoutEdit edit;
        edit.kind = LayoutEdit::Kind::create;
        edit.index = index;
        edit.width = width;

        return applyLayout (edit, id, kind);
    }

    EditResult ShowDocument::startNewShow (int tracks)
    {
        if (tracks < 0)
            return EditResult::failed (reason::badValue);

        if (const auto list = createList ("Main"); ! list.ok)
            return list;

        if (const auto count = setAttribute ("/godot/audio/tracks", std::to_string (tracks));
            ! count.ok)
            return count;

        /*  A STEREO DIRECT OUT ON THE FIRST TWO CHANNELS, named the way every
            fixture in this repository names it and the way anybody wiring a
            rig would. A show with tracks and no bus is refused at the door of
            the audio graph, so this is not a convenience. */
        const auto out = createBus ("direct", 2);

        if (! out.ok)
            return out;

        if (const auto named = setAttribute ("/godot/bus/" + out.id + "/name", "Main L/R");
            ! named.ok)
            return named;

        return EditResult::succeeded (out.id);
    }

    EditResult ShowDocument::removeBus (const std::string& id)
    {
        LayoutEdit edit;
        edit.kind = LayoutEdit::Kind::remove;
        edit.id = id;

        return applyLayout (edit, id, {});
    }

    EditResult ShowDocument::moveBus (const std::string& id, int index)
    {
        LayoutEdit edit;
        edit.kind = LayoutEdit::Kind::move;
        edit.id = id;
        edit.index = index;

        return applyLayout (edit, id, {});
    }

    EditResult ShowDocument::resizeBus (const std::string& id, int width)
    {
        LayoutEdit edit;
        edit.kind = LayoutEdit::Kind::resize;
        edit.id = id;
        edit.width = width;

        return applyLayout (edit, id, {});
    }

    EditResult ShowDocument::createInput (int width, int index, const std::string& id)
    {
        LayoutEdit edit;
        edit.kind = LayoutEdit::Kind::create;
        edit.index = index;
        edit.width = width;

        return applyLayout (edit, id, {}, LayoutSide::inputs);
    }

    EditResult ShowDocument::removeInput (const std::string& id)
    {
        LayoutEdit edit;
        edit.kind = LayoutEdit::Kind::remove;
        edit.id = id;

        return applyLayout (edit, id, {}, LayoutSide::inputs);
    }

    EditResult ShowDocument::moveInput (const std::string& id, int index)
    {
        LayoutEdit edit;
        edit.kind = LayoutEdit::Kind::move;
        edit.id = id;
        edit.index = index;

        return applyLayout (edit, id, {}, LayoutSide::inputs);
    }

    EditResult ShowDocument::resizeInput (const std::string& id, int width)
    {
        LayoutEdit edit;
        edit.kind = LayoutEdit::Kind::resize;
        edit.id = id;
        edit.width = width;

        return applyLayout (edit, id, {}, LayoutSide::inputs);
    }

    EditResult ShowDocument::createFeed (const std::string& cueId,
                                         const std::string& slotId,
                                         const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (cue, endOfSequence, "Feed", id,
                             { { "slot", slotId } });
    }

    EditResult ShowDocument::createInsert (const std::string& cueId,
                                           const std::string& channelId,
                                           const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (cue, endOfSequence, "Insert", id,
                             { { "channel", channelId } });
    }

    EditResult ShowDocument::createSend (const std::string& cueId,
                                         const std::string& busId,
                                         const std::string& id,
                                         const std::string& level)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        /*  ONE SEND PER BUS PER CUE, refused here rather than tolerated.

            Two sends into one mix channel would sum to a level that is on no
            fader: the mixer would draw two strips for one destination and the
            sound would be the sum of both, so moving either would be a
            surprise. A send is the level of a cue at a mix, and there is one
            of those. `badValue` rather than `typeMismatch` because both
            identifiers name what they claim to - it is the pairing that
            already exists. */
        for (const auto& child : cue)
            if (child.hasType ("Send")
                 && child.getProperty ("bus").toString().toStdString() == busId)
                return EditResult::failed (reason::badValue);

        /*  BORN AT ITS LEVEL WHEN ONE IS GIVEN: in the object before it is
            added, so no tick ever routes it at the default (see the command's
            own note in DocumentCommands.cpp). */
        std::vector<std::pair<std::string_view, std::string>> attributes { { "bus", busId } };

        if (! level.empty())
            attributes.push_back ({ "level", level });

        return insertObject (cue, endOfSequence, "Send", id, attributes);
    }

    EditResult ShowDocument::createFx (const std::string& cueId,
                                       const std::string& pluginId,
                                       const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        /*  AN ENTRY OF THE SET, by its id: a plugin the show declared, whether
            or not tonight's machine has it. */
        const auto entry = findById (pluginId);

        if (! entry.isValid() || entry.getType().toString() != "Plugin")
            return EditResult::failed (reason::unknownId);

        /*  ONE FX PER ENTRY PER CUE, for createSend's reason: the entry is on
            the voice once, and two children switching it in would be two
            sets of values for one instance. */
        for (const auto& child : cue)
            if (child.hasType ("Fx")
                 && child.getProperty ("plugin").toString().toStdString() == pluginId)
                return EditResult::failed (reason::badValue);

        return insertObject (cue, endOfSequence, "Fx", id,
                             { { "plugin", pluginId } });
    }

    EditResult ShowDocument::createRange (const std::string& cueId, double in, double out,
                                          const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        /*  ONLY A MEDIA CUE, because a range is a region of the cue's own file
            (§3.24) and a cue that plays nothing has no file to cut up. The
            element name is asked directly here rather than through the owner
            word, unlike a trigger: `cue` is the answer for every kind, and this
            is the one kind. */
        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        /*  A RANGE THAT ENDS BEFORE IT BEGINS is not a range, and this is the
            one thing about one that can be judged without reading the file.
            Whether `out` is past the end of the media is answered when the cue
            is armed, which is when the file is opened - a show whose media has
            not arrived yet still has to open. */
        if (in < 0.0 || ! (out > in))
            return EditResult::failed (reason::badValue);

        return insertObject (cue, endOfSequence, "Range", id,
                             { { "in", osc::formatDouble (in) },
                               { "out", osc::formatDouble (out) } });
    }

    EditResult ShowDocument::splitRange (const std::string& cueId, double at,
                                         const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        /*  A MILLISECOND, which is the resolution a range is placed at by a
            hand on a bar and the same one the client calls "the same instant".
            Inside this of either edge, there is already a cut here and a
            second one would make a range of no length - so nothing happens,
            and that one rule answers for a cut, for the top of the file and
            for its end alike. */
        constexpr auto sameInstant = 0.001;

        for (int index = 0; index < cue.getNumChildren(); ++index)
        {
            auto child = cue.getChild (index);

            if (! child.hasType ("Range"))
                continue;

            const auto in = static_cast<double> (child.getProperty ("in"));
            const auto out = static_cast<double> (child.getProperty ("out"));

            if (! (at > in + sameInstant) || ! (at < out - sameInstant))
                continue;

            const auto first = child[idProperty].toString().toStdString();

            /*  THE SECOND HALF GOES DIRECTLY AFTER THE FIRST, because document
                order is playlist order (§3.24): a half appended to the end of
                the list would play after everything else, which is not what
                cutting something in two means.

                It inherits the loop count, so cutting a bed that goes round
                for ever gives two pieces that each go round for ever rather
                than one that stops. That is the reading of "split" a designer
                slicing a loop has; a piece that quietly stopped looping would
                be the gesture changing what the cue does. */
            const auto made = insertObject (cue, index + 1, "Range", id,
                                             { { "in", osc::formatDouble (at) },
                                               { "out", osc::formatDouble (out) } });

            if (! made.ok)
                return made;

            if (const auto loops = static_cast<int> (child.getProperty ("loops", 1)); loops != 1)
                if (const auto kept = setAttribute ("/godot/range/" + made.id + "/loops",
                                                    std::to_string (loops)); ! kept.ok)
                    return kept;

            /*  AND THE FIRST HALF STOPS WHERE THE SECOND BEGINS. Written last,
                so that at no point in this edit does the cue hold two ranges
                claiming the same seconds. */
            if (const auto shortened = setAttribute ("/godot/range/" + first + "/out",
                                                     osc::formatDouble (at)); ! shortened.ok)
                return shortened;

            return made;
        }

        return EditResult::failed (reason::badValue);
    }

    EditResult ShowDocument::createTrigger (const std::string& cueId, const std::string& kind,
                                            const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        /*  ON ANY CUE, which is what §3.7 says: "a cue or a group carries a
            trigger list". So the question asked is whether the parent IS a cue,
            through the owner word the parameter table already answers it with -
            not whether its element name is one of a list. That list has grown
            twice since Phase 1 and both times something was forgotten. */
        if (ownerForElement (cue.getType().toString().toStdString()) != "cue")
            return EditResult::failed (reason::typeMismatch);

        if (kind != "osc" && kind != "midi" && kind != "clock")
            return EditResult::failed (reason::badValue);

        return insertObject (cue, endOfSequence, "Trigger", id,
                             { { "kind", kind } });
    }

    EditResult ShowDocument::createRole (const std::string& groupId, const std::string& role,
                                         const std::string& id)
    {
        auto group = findById (groupId);

        if (! group.isValid())
            return EditResult::failed (reason::unknownId);

        /*  Only a group has members to run before or after. A header on a media
            cue would be a statement about an order that does not exist. */
        if (group.getType().toString() != "Group")
            return EditResult::failed (reason::typeMismatch);

        const auto element = role == "header" ? "Header"
                           : role == "footer" ? "Footer"
                                              : "";

        if (*element == '\0')
            return EditResult::failed (reason::typeMismatch);

        /*  ASKING TWICE ANSWERS WITH THE FIRST. A group has at most one of
            each, so a second is not a thing to refuse OR to create - the caller
            wanted the group's footer and there it is. It also makes the command
            idempotent, which is what a replay needs from anything that can
            arrive more than once. */
        if (const auto existing = group.getChildWithName (element); existing.isValid())
            return EditResult::succeeded (existing[idProperty].toString().toStdString());

        /*  AT THE END, whatever it is. Where a header sits among the members is
            not what makes it a header - the element is - and it HAS no member
            position to ask for: `order` does not name it, which is the whole
            reason `document/Sequence.h` exists. So it asks for none, and
            `endOfSequence` lands it after the last child, where `createRole`
            has always put it. */
        return insertObject (group, endOfSequence, element, id, {});
    }

    EditResult ShowDocument::createPersistent (const std::string& listId, const std::string& id)
    {
        auto list = findById (listId);

        if (! list.isValid())
            return EditResult::failed (reason::unknownId);

        if (list.getType().toString() != "List")
            return EditResult::failed (reason::typeMismatch);

        /*  One per list, and the second ask is the first's answer - the same
            idempotence `createRole` has, for the same replay reason. */
        if (const auto existing = list.getChildWithName ("Persistent"); existing.isValid())
            return EditResult::succeeded (existing[idProperty].toString().toStdString());

        /*  At the end and with no member position, for the reason the header
            above gives: `order` does not name the section either. */
        return insertObject (list, endOfSequence, "Persistent", id, {});
    }

    EditResult ShowDocument::createPort (const std::string& name, const std::string& id)
    {
        /*  `<MidiPorts>` is one of the containers `ensureContainers` makes, so
            it is here whether the show was opened or just made - which is why
            this create needs none of `createRackChannel`'s on-demand lines. */
        return insertObject (showNode.getChildWithName ("MidiPorts"),
                             endOfSequence, "Port", id, { { "name", name } });
    }

    EditResult ShowDocument::createMount (const std::string& prefix,
                                          const std::string& namespaceFile,
                                          const std::string& id)
    {
        auto mounts = showNode.getChildWithName ("Mounts");

        /*  AN EMPTY NAMESPACE IS NOT AN ATTRIBUTE, it is the absence of one.

            A device somebody typed an address into has no description file,
            and since 2026-09-22 that is the ordinary case rather than a
            degenerate one (PRD 3.22): the show says where the box is, and the
            cues say what to write. Writing namespace="" would put a value in
            the file that the canonical writer drops again anyway, since it
            equals the row's default - so it is left out here, where the reason
            is visible, rather than in the writer, where it would look like a
            coincidence. */
        std::vector<std::pair<std::string_view, std::string>> attributes {
            { "prefix", prefix } };

        if (! namespaceFile.empty())
            attributes.push_back ({ "namespace", namespaceFile });

        return insertObject (mounts, endOfSequence, "Mount", id, attributes);
    }

    //==============================================================================
    bool ShowDocument::dcaChainReaches (const std::string& start, const std::string& self) const
    {
        const auto dcas = showNode.getChildWithName ("Dcas");

        if (! dcas.isValid())
            return false;

        const juce::Identifier parentProperty { "dca" };
        auto current = start;

        for (int steps = 0; steps <= dcas.getNumChildren() && ! current.empty(); ++steps)
        {
            if (current == self)
                return true;

            const auto node = dcas.getChildWithProperty (idProperty, juce::String (current));

            if (! node.isValid())
                return false;

            current = node[parentProperty].toString().toStdString();
        }

        return false;
    }

    int ShowDocument::stripsForProfile (std::string_view profile)
    {
        /*  WHAT A FRESH SURFACE OF EACH KIND IS MADE WITH, and nothing more:
            strips are objects, so a Mackie unit with an extender gains its
            second eight with `strip.create`, and a pad controller with nine
            pads loses seven with `object.delete`. The D700 is two banks of
            eight on two port pairs (docs/D700_CONTROL_GUIDE.md 1.1); a pad
            controller is most often sixteen; a virtual panel starts where a
            Mackie unit does. */
        if (profile == "virtual")  return 8;
        if (profile == "mcu")      return 8;
        if (profile == "d700")     return 16;
        if (profile == "midiPads") return 16;
        return -1;
    }

    EditResult ShowDocument::createSurface (const std::string& profile, const std::string& name,
                                            const std::string& id,
                                            const std::vector<std::string>& stripIds,
                                            std::vector<std::string>& madeStrips)
    {
        madeStrips.clear();

        const auto count = stripsForProfile (profile);

        if (count < 0)
            return EditResult::failed (reason::badValue);

        /*  EVERY SUPPLIED IDENTIFIER IS ASKED ABOUT BEFORE ANYTHING IS MADE.
            A surface is one command and several objects, and a replay hands
            back the identifiers the live session drew; one of them malformed
            or already taken would otherwise leave a surface with half its
            strips - a document the refusal had changed. So the whole list is
            checked first, duplicates within it included, and a refusal leaves
            nothing behind. More identifiers than the profile makes is the
            same refusal: the record describes a different surface. */
        if (stripIds.size() > static_cast<std::size_t> (count))
            return EditResult::failed (reason::unknownId);

        for (std::size_t i = 0; i < stripIds.size(); ++i)
        {
            if (! Id::isValid (stripIds[i]) || registry.isTaken (stripIds[i]) || stripIds[i] == id)
                return EditResult::failed (reason::unknownId);

            for (std::size_t j = 0; j < i; ++j)
                if (stripIds[j] == stripIds[i])
                    return EditResult::failed (reason::unknownId);
        }

        std::vector<std::pair<std::string_view, std::string>> attributes {
            { "profile", profile } };

        if (! name.empty())
            attributes.push_back ({ "name", name });

        auto made = insertObject (showNode.getChildWithName ("Surfaces"), endOfSequence,
                                  "Surface", id, attributes);

        if (! made.ok)
            return made;

        auto surface = findById (made.id);

        for (int i = 0; i < count; ++i)
        {
            const auto wanted = static_cast<std::size_t> (i) < stripIds.size()
                                  ? stripIds[static_cast<std::size_t> (i)]
                                  : std::string {};

            const auto strip = insertObject (surface, endOfSequence, "Strip", wanted, {});

            /*  Cannot fail once the identifiers above passed and the surface
                exists - but a create that could fail must say so rather than
                answer with a surface short of strips. */
            if (! strip.ok)
                return strip;

            madeStrips.push_back (strip.id);
        }

        return made;
    }

    EditResult ShowDocument::createStrip (const std::string& surfaceId, const std::string& id)
    {
        auto surface = findById (surfaceId);

        if (! surface.isValid())
            return EditResult::failed (reason::unknownId);

        if (surface.getType().toString() != "Surface")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (surface, endOfSequence, "Strip", id, {});
    }

    EditResult ShowDocument::createDca (const std::string& name, const std::string& id)
    {
        std::vector<std::pair<std::string_view, std::string>> attributes;

        if (! name.empty())
            attributes.push_back ({ "name", name });

        return insertObject (showNode.getChildWithName ("Dcas"), endOfSequence, "Dca", id,
                             attributes);
    }

    EditResult ShowDocument::createPlugin (const std::string& name, const std::string& identifier,
                                          const std::string& format, const std::string& path,
                                          const std::string& id)
    {
        /*  ASKED HERE AS WELL AS AT THE DOOR, for createRackChannel's reason:
            the container below is made on demand before the door is reached,
            and a locked show must not gain an empty one from a refusal. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

        /*  THE SHOW'S THREE WORDS FOR A FORMAT, and nothing else (2026-09-26):
            the schema allows `VST3 | AU | LV2`, and JUCE calls an AU
            `AudioUnit` - a client passing the scan's own name through would
            write a show that no longer validates. The known list publishes
            the show's words; this is the door that holds them to it. */
        if (! format.empty() && format != "VST3" && format != "AU" && format != "LV2")
            return EditResult::failed (reason::badValue);

        auto audio = showNode.getChildWithName ("Audio");

        if (! audio.isValid())
            return EditResult::failed (reason::unknownId);

        auto plugins = audio.getChildWithName ("Plugins");

        if (! plugins.isValid())
        {
            /*  AT A FIXED PLACE - after the last bus and the named inputs,
                before the rack - whichever container was asked for first, so
                the canonical bytes of a show do not depend on the order two
                creates happened in. The rack appends itself at the end; a bus
                made afterwards lands among the buses through its own create. */
            int at = 0;

            for (int i = 0; i < audio.getNumChildren(); ++i)
                if (audio.getChild (i).hasType ("Bus") || audio.getChild (i).hasType ("Inputs"))
                    at = i + 1;

            plugins = juce::ValueTree ("Plugins");
            audio.addChild (plugins, at, nullptr);
        }

        std::vector<std::pair<std::string_view, std::string>> attributes;

        if (! name.empty())
            attributes.push_back ({ "name", name });

        attributes.push_back ({ "identifier", identifier });

        if (! format.empty())
            attributes.push_back ({ "format", format });

        if (! path.empty())
            attributes.push_back ({ "path", path });

        return insertObject (plugins, endOfSequence, "Plugin", id, attributes);
    }

    //==============================================================================
    void ShowDocument::collectIds (const juce::ValueTree& node, std::vector<std::string>& out) const
    {
        if (node.hasProperty (idProperty))
            out.push_back (node[idProperty].toString().toStdString());

        for (const auto& child : node)
            collectIds (child, out);
    }

    EditResult ShowDocument::remove (const std::string& id)
    {
        /*  First, before the identifier is looked up. A locked show refuses
            the corrected command as well, so of the two things that could be
            wrong with a delete on a locked show the lock is the one worth
            reading first. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

        auto node = findById (id);

        if (! node.isValid())
            return EditResult::failed (reason::unknownId);

        auto parent = node.getParent();

        if (! parent.isValid())
            return EditResult::failed (reason::unknownId);

        /*  Every identifier under it comes back, not just its own — deleting a
            group deletes its cues, and leaving their identifiers reserved would
            slowly poison the registry over a long editing session. */
        std::vector<std::string> released;
        collectIds (node, released);

        /*  If the list this cue belongs to is parked on it, work out where the
            standby goes BEFORE the cue disappears - afterwards there is no
            sequence left to ask.

            It advances to the next remaining sibling rather than clearing
            (author, 2026-09-06): during tech, deleting the cue you are parked
            on should leave you parked on the next one. Done inside the applied
            command rather than as a second event, so a replay reproduces it for
            free and the log does not need a repair record nobody sent. */
        /*  DEEPER THAN A LIST'S TOP LEVEL, since PR 3.4. The pointer can stand
            inside a manual sequence group, so the list that is parked on this
            cue may be several levels above it - and asking only the immediate
            parent would have left a standby pointing at a cue that had gone.

            Where it goes is the next remaining SIBLING, which is the same
            answer one level down as it was at the top: during tech, deleting
            the cue you are parked on should leave you parked on the next one.
            When it was the last member of a group, that is empty and the
            walk below falls back to the group's own successor - which is where
            `next` would have taken the pointer anyway.

            Worked out BEFORE the cue disappears, because afterwards there is no
            sequence left to ask, and done inside the applied command so a
            replay reproduces it with no repair record in the log. */
        std::string repairList, repairStandby;

        const auto list = listContaining (parent);
        const auto standby = list.isValid()
                               ? list[standbyProperty].toString().toStdString()
                               : std::string {};

        /*  PARKED ON IT, OR ON SOMETHING INSIDE IT - and the second is the half
            that was missing.

            PR 3.4 widened the LOOKUP (a list several levels above the cue) and
            left the MATCH asking only whether the deleted node WAS the standby.
            Deleting a group the pointer was standing inside therefore released
            the pointer's cue and left the list still naming it, which is the
            invariant broken by the very route the widening was for.

            It does not heal: `standby.next` finds nothing on the path, answers
            with the identifier it was given, and the write-back is then refused
            because that cue is no longer in the list - so the pointer is frozen.
            GO is worse. The standby is not empty, so it passes the early return;
            the advance fails and its result is discarded; and firing a cue that
            does not exist does nothing. The operator's GO key goes quietly dead
            until somebody thinks to move standby by hand.

            `released` is already every identifier under the node, collected
            above because they all go back to the registry - so asking whether
            the pointer is one of them is the same question in one test, and
            cannot come apart from the release the way a second walk could. */
        const auto parked = ! standby.empty()
                              && std::find (released.begin(), released.end(), standby)
                                   != released.end();

        if (parked)
        {
            repairList = list[idProperty].toString().toStdString();

            /*  WHERE THE POINTER GOES IS MEASURED FROM THE NODE BEING DELETED,
                not from the cue it was on: the cue may be three levels inside
                the thing that is disappearing, and its own siblings are going
                with it. What survives is whatever follows the deleted node,
                which is where `next` would have carried the pointer once the
                subtree was gone. */
            repairStandby = siblingAfter (parent, id);

            if (repairStandby.empty())
                repairStandby = cue::nextStandby (list, id);

            /*  `nextStandby` answers with the identifier it was given when
                there is nowhere to go, and that one is about to stop existing.
                It can also answer with something else inside the doomed
                subtree, so the whole released set is the test rather than just
                the node's own identifier. */
            if (std::find (released.begin(), released.end(), repairStandby) != released.end())
                repairStandby.clear();
        }

        /*  THE HISTORY IS THE SECOND ARGUMENT TO THIS OVERLOAD, not the third:
            `removeChild (const ValueTree&, UndoManager*)`. The action it makes
            holds a ref-counted handle on the whole subtree, which is what lets
            an undo put the cues back with the identifiers they had - and is why
            the registry has to be told afterwards rather than the document. */
        parent.removeChild (node, structuralHistory());

        for (const auto& released_id : released)
            registry.release (released_id);

        if (! repairList.empty())
            setAttribute ("/godot/list/" + repairList + "/standby", repairStandby);

        return EditResult::succeeded (id);
    }

    EditResult ShowDocument::move (const std::string& id, const std::string& newParentId, int newIndex)
    {
        /*  First, for the reason `remove` gives. A reorder is an edit to the
            show like any other - the order of the rows IS the show - so a
            locked show refuses it, and the standby repair below, which writes
            a state row, is never reached. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

        auto node = findById (id);

        if (! node.isValid())
            return EditResult::failed (reason::unknownId);

        auto newParent = findById (newParentId);

        if (! newParent.isValid())
            return EditResult::failed (reason::unknownId);

        if (newIndex < 0)
            return EditResult::failed (reason::badAddress);

        const auto* parentElement =
            Schema::instance().element (newParent.getType().toString().toStdString());

        if (parentElement == nullptr
            || ! parentElement->mayContain (node.getType().toString().toStdString()))
            return EditResult::failed (reason::badAddress);

        /*  A group cannot be moved inside itself. Without this the tree stops
            being a tree: the subtree detaches with the node and is never seen
            again, and the identifiers in it stay reserved forever. */
        for (auto ancestor = newParent; ancestor.isValid(); ancestor = ancestor.getParent())
            if (ancestor == node)
                return EditResult::failed (reason::badAddress);

        auto oldParent = node.getParent();

        /*  A move WITHIN one list's top level is a reorder, and a reorder never
            moves the standby: it still names the same cue, which is still a
            top-level child, and PRD §3.5 is explicit that the pointer does not
            follow the shape of the list around.

            A move OUT of that top level is different. The cue is no longer
            somewhere the standby is allowed to point - into a group, or into
            another list entirely - so the list it left is cleared rather than
            advanced. Advancing would be guessing that the operator meant to
            stay where they were; clearing says plainly that what they were
            parked on has gone somewhere else. */
        /*  WIDENED TWICE, AND THE TEST IS ALWAYS THE SAME ONE: does the pointer
            still have anywhere to be. PR 3.4 took it from "leaving a list's top
            level" to "leaving the manual path"; 2026-09-16 took it to "leaving
            the places the pointer may stand", which is now any enabled cue the
            list holds at any depth.

            Moving a cue from one of those places to another leaves the pointer
            alone - it stores an identifier, and §3.5 is explicit that it does
            not follow the shape of the list around. What clears it is the cue
            landing somewhere the pointer is not allowed to be: in a header, in a
            footer, in a persistent section, or in another list.

            The test below asks `cue::mayStandOn` rather than listing those four,
            which is why the line itself has come through both widenings
            unedited: whatever the rule becomes, it asks the rule. */
        const auto vacated = listContaining (oldParent);

        const auto wasParkedOnIt = vacated.isValid()
                                     && vacated[standbyProperty].toString().toStdString() == id;

        const auto vacatedList = wasParkedOnIt
                                   ? vacated[idProperty].toString().toStdString()
                                   : std::string {};

        /*  BOTH LIMBS TAKE THE HISTORY, or an undo of a cross-parent move would
            put the cue back without taking it out of where it went.

            A within-parent move is one action, which JUCE coalesces with the
            next move of the same parent on its own - so it is the transaction
            rule above, and not this line, that keeps two ▲ presses from
            collapsing into one step. A cross-parent move is two actions in one
            transaction, undone in reverse.

            AND BOTH LIMBS READ `newIndex` AS A MEMBER POSITION, translated
            through the one rule in `document/Sequence.h`. A raw child index is
            a number no client has ever been able to see; this is the number
            they all send.

            THE SAME TRANSLATION SERVES BOTH, which is worth the paragraph it
            takes, because the two JUCE calls underneath want different things
            and it is not obvious that one answer satisfies them.

            `addChild (child, index)` INSERTS: the child lands at `index` and
            everything from there on shifts along. `rawIndexForPosition` answers
            the raw index of the member holding that position, so the newcomer
            takes its place and pushes it down - which is what an insert at a
            position means. The cross-parent limb has also taken the child out
            of its old parent by then, and the new parent never held it, so the
            children it walks are the ones that will be there.

            `moveChild (from, to)` does NOT insert: JUCE takes the child out and
            puts it back so that it ENDS UP at index `to` in a list of the same
            length (juce_ArrayBase.h, moveInternal - a memmove either way, then
            the element written at `to`). The answer is the same all the same,
            and here is why. Moving EARLIER, to a member at raw index m below
            the child: m is below the removal point, so nothing before it
            shifts, and the child lands exactly where that member was - in front
            of it. Moving LATER, to a member at raw index m above the child:
            taking the child out drops that member to m-1, so ending up at m
            puts the child directly after it - and after the member that holds
            position p is where position p is, once the child itself is no
            longer counted below it. Both give the member the caller asked for.

            THE CLAMP IS STILL HERE for the move, and only for the move: a
            position past the end answers `getNumChildren()`, which is a legal
            insertion point but one index past the last slot a move can end at.
            JUCE clamps it too, with and without an undo manager - this says so
            where it is read rather than leaving it to a library detail. */
        if (oldParent == newParent)
        {
            const auto from = newParent.indexOf (node);
            const auto to = std::min (rawIndexForPosition (newParent, newIndex),
                                      newParent.getNumChildren() - 1);
            newParent.moveChild (from, to, structuralHistory());
        }
        else
        {
            oldParent.removeChild (node, structuralHistory());
            newParent.addChild (node, rawIndexForPosition (newParent, newIndex),
                                structuralHistory());
        }

        /*  Asked AFTER the move, because whether the cue is still somewhere the
            pointer may be is a question about where it has landed. A cue that
            moved from one stop of the same list to another keeps the pointer -
            including, since 2026-09-16, a cue dragged into a timeline group,
            which is a move that used to clear it. */
        if (! vacatedList.empty()
              && ! cue::mayStandOn (findById (vacatedList), id))
            setAttribute ("/godot/list/" + vacatedList + "/standby", "");

        return EditResult::succeeded (id);
    }

    //==============================================================================
    std::string ShowDocument::fragmentOf (const std::vector<std::string>& ids) const
    {
        /*  COPIES OF CUES, AND ONLY CUES: a list, a header or a footer is a
            place cues live and not a thing to paste somewhere else, so an id
            that names one of those is passed over. Copies, because the writer
            reads the node and the fragment must not hold a handle on the show. */
        std::vector<juce::ValueTree> nodes;

        for (const auto& id : ids)
        {
            const auto node = findById (id);

            if (! node.isValid())
                continue;

            const auto* element = Schema::instance().element (node.getType().toString().toStdString());
            const auto* holder = Schema::instance().element (std::string (Schema::rootElement));

            //  A cue is what a list may hold; a list is what the root may.
            const auto* list = Schema::instance().element ("List");

            if (element == nullptr || list == nullptr || holder == nullptr
                  || ! list->mayContain (node.getType().toString().toStdString()))
                continue;

            nodes.push_back (node.createCopy());
        }

        return nodes.empty() ? std::string {} : CanonicalXml::writeFragment (nodes);
    }

    void ShowDocument::copyToClipboard (const std::vector<std::string>& ids)
    {
        clipboard = fragmentOf (ids);
    }

    EditResult ShowDocument::paste (const std::string& parentId, int index, const std::string& fragment,
                                    const std::vector<std::string>& ids)
    {
        if (auto refusal = refuseIfLocked())
            return *refusal;

        auto parent = findById (parentId);

        if (! parent.isValid() || index < 0)
            return EditResult::failed (reason::badAddress);

        const auto* parentElement = Schema::instance().element (parent.getType().toString().toStdString());

        if (parentElement == nullptr)
            return EditResult::failed (reason::badAddress);

        /*  READ UNDER NEW NAMES FIRST, INTO NOTHING: the fragment is built
            whole, its identities reserved, before the show is touched, so a
            fragment that is refused leaves the document as it was. */
        auto read = CanonicalXml::readFragment (fragment, registry, ids);

        if (! read.ok)
            return EditResult::failed (reason::badValue);

        for (const auto& node : read.nodes)
        {
            if (! parentElement->mayContain (node.getType().toString().toStdString()))
            {
                for (const auto& id : read.ids)
                    registry.release (id);

                return EditResult::failed (reason::badAddress);
            }
        }

        /*  IN ORDER, EACH ONE FURTHER ALONG, under the structural history so
            the whole paste is one step of undo. The position is a member
            position, translated as every insert's is. */
        auto at = index;

        for (auto& node : read.nodes)
            parent.addChild (node, rawIndexForPosition (parent, at++), structuralHistory());

        std::string drawn;

        for (std::size_t n = 0; n < read.ids.size(); ++n)
            drawn += (n == 0 ? "" : " ") + read.ids[n];

        return EditResult::succeeded (drawn);
    }

    //==============================================================================
    std::vector<std::string> ShowDocument::validate() const
    {
        std::vector<std::string> problems;
        if (! audio::validAudioSettings (audio::audioSettingsOf (*this)))
            problems.push_back ("/Show/Audio: invalid audio patch or buffer size");
        std::unordered_set<std::string> seenIds;

        const auto& schema = Schema::instance();

        struct Walk
        {
            const Schema& schema;
            std::vector<std::string>& problems;
            std::unordered_set<std::string>& seenIds;

            void visit (const juce::ValueTree& node, const std::string& path)
            {
                const auto elementName = node.getType().toString().toStdString();
                const auto* element = schema.element (elementName);

                if (element == nullptr)
                {
                    problems.push_back (path + ": unknown element <" + elementName + ">");
                    return;
                }

                std::string here = path + "/" + elementName;

                if (element->hasIdentity)
                {
                    if (! node.hasProperty (idProperty))
                    {
                        problems.push_back (here + ": missing id");
                    }
                    else
                    {
                        const auto id = node[idProperty].toString().toStdString();
                        here += "[" + id + "]";

                        if (! Id::isValid (id))
                            problems.push_back (here + ": malformed id");
                        else if (! seenIds.insert (id).second)
                            problems.push_back (here + ": duplicate id");
                    }
                }
                else if (node.hasProperty (idProperty))
                {
                    problems.push_back (here + ": <" + elementName + "> may not carry an id");
                }

                for (int i = 0; i < node.getNumProperties(); ++i)
                {
                    const auto name = node.getPropertyName (i);

                    if (name == idProperty)
                        continue;

                    const auto attributeName = name.toString().toStdString();
                    const auto* attribute = element->attribute (attributeName);

                    if (attribute == nullptr)
                    {
                        problems.push_back (here + ": unknown attribute \"" + attributeName + "\"");
                        continue;
                    }

                    /*  Re-parse the value as text. That catches a property whose
                        var carries the wrong type as well as one out of range -
                        which is the whole point, because a string-typed "1" is
                        exactly what a careless loader leaves behind. */
                    const auto text = toText (*attribute, node[name]);

                    /*  A list element by element, as the reader takes one - and
                        a fade's points as a curve besides, by the same function
                        the write door and the Runner ask (FadePoints.h). */
                    if (attribute->isList())
                    {
                        std::string canonical;
                        const auto list = Schema::parseList (*attribute, text, canonical);

                        if (! list.ok)
                        {
                            problems.push_back (here + ": \"" + attributeName + "\" " + list.error);
                        }
                        else if (elementName == "Fade" && attributeName == "points")
                        {
                            const auto curve = readFadePoints (canonical);

                            if (! curve.problem.empty())
                                problems.push_back (here + ": \"points\" " + curve.problem);
                        }

                        continue;
                    }

                    Value parsedValue;
                    const auto parsed = Schema::parseValue (*attribute, text, parsedValue);

                    if (! parsed.ok)
                        problems.push_back (here + ": \"" + attributeName + "\" " + parsed.error);
                }

                for (const auto& child : node)
                {
                    const auto childName = child.getType().toString().toStdString();

                    if (! element->mayContain (childName))
                        problems.push_back (here + ": <" + elementName + "> may not contain <"
                                            + childName + ">");

                    visit (child, here);
                }
            }
        };

        if (showNode.getType().toString() != juce::String (std::string (Schema::rootElement)))
        {
            problems.push_back ("the root element is <" + showNode.getType().toString().toStdString()
                                + ">, expected <" + std::string (Schema::rootElement) + ">");
            return problems;
        }

        Walk { schema, problems, seenIds }.visit (showNode, "");

        /*  AN OSC TRIGGER MAY NOT LISTEN WHERE THE ENGINE ANSWERS.

            §3.7's triggers arrive on Go.dot's own OSC port, which is the same
            port `/godot/...` is written on and the same port a mounted
            namespace is proxied through. An address under either is a message
            that would both write a value and fire a cue, and nobody reading the
            log afterwards could say which had been meant - nor which the sender
            intended, because the sender wrote one message.

            REFUSED WHEN THE SHOW IS READ rather than discovered during it,
            which is the rule the whole document layer follows for things that
            cannot be honoured at all. A dangling reference is a warning, and
            this is not one: there is no reading of the file under which such a
            trigger does what it says.

            A SECOND PASS, because it needs the mounts and the walk above meets
            them in document order - mounts are written after the lists. */
        std::vector<std::string> prefixes;

        /*  The same pass also collects what each mount can be ASKED, which the
            verified-cue check below needs. One traversal, because the two
            questions are about the same elements and a second walk would be a
            second thing to keep in step. */
        struct MountFacts
        {
            std::string id;
            std::string prefix;
            bool canBeAsked = false;
        };

        std::vector<MountFacts> mountFacts;

        for (const auto& mounts : showNode)
        {
            if (mounts.getType().toString() != "Mounts")
                continue;

            for (const auto& mount : mounts)
            {
                if (! mount.hasProperty (juce::Identifier ("prefix")))
                    continue;

                const auto prefix = mount[juce::Identifier ("prefix")].toString().toStdString();
                prefixes.push_back (prefix);

                /*  `canBeAsked`, restated in document terms: readback names the
                    MECHANISM and a query port says where to use it. The same
                    rule lives on `tree::MountDeclaration`, which is what the
                    engine asks at run time; this is the read-time half, and the
                    two say the same sentence because question K's answer is one
                    sentence.

                    AND A DEVICE THAT DESCRIBES NOTHING CAN NEVER BE ASKED
                    (2026-09-22), whatever else it declares. There is no node
                    to ask about, so a verified cue aimed at an opaque device
                    would wait for an answer with nowhere to come from - which
                    is precisely the failure this check was written to move
                    from half past seven to the moment the file is read. */
                const auto readback = mount[juce::Identifier ("readback")].toString();
                const auto queryPort = static_cast<int> (mount[juce::Identifier ("queryPort")]);
                const auto described = mount[juce::Identifier ("namespace")]
                                         .toString().isNotEmpty();

                mountFacts.push_back ({ mount[juce::Identifier ("id")].toString().toStdString(),
                                        prefix,
                                        described && readback == "oscquery" && queryPort > 0 });
            }
        }

        struct Triggers
        {
            std::vector<std::string>& problems;
            const std::vector<std::string>& prefixes;

            static bool under (const std::string& address, const std::string& prefix)
            {
                return address == prefix
                         || address.rfind (prefix + "/", 0) == 0;
            }

            void visit (const juce::ValueTree& node)
            {
                if (node.getType().toString() == "Trigger")
                {
                    const auto kind = node[juce::Identifier ("kind")].toString().toStdString();
                    const auto address = node[juce::Identifier ("address")]
                                           .toString().toStdString();

                    if ((kind.empty() || kind == "osc") && ! address.empty())
                    {
                        const auto here = "/Show/.../Trigger[" + node[juce::Identifier ("id")]
                                            .toString().toStdString() + "]";

                        if (under (address, "/godot"))
                            problems.push_back (here + ": an OSC trigger may not listen at \""
                                                + address + "\", which is inside the engine's own"
                                                " namespace");

                        for (const auto& prefix : prefixes)
                            if (! prefix.empty() && under (address, prefix))
                                problems.push_back (here + ": an OSC trigger may not listen at \""
                                                    + address + "\", which is inside the mount"
                                                    " prefix \"" + prefix + "\"");
                    }
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        Triggers { problems, prefixes }.visit (showNode);

        /*  A START OFFSET AND A LIST OF RANGES ARE TWO ANSWERS TO ONE QUESTION.

            `startOffset` says where in the file playback begins. A range says
            the same thing and says where it ends and how many times, and a cue
            with ranges plays the list rather than the file (§3.24). Honouring
            both would mean choosing between them - offsetting the first range,
            or ignoring the offset - and either choice is a rule nobody wrote
            down that somebody would find out about during a show.

            REFUSED WHEN THE SHOW IS READ, like the trigger address above and
            for the same reason: there is no reading of the file under which the
            cue does what both attributes say. Zero is the resting value and
            says nothing, so it is only a non-zero offset that collides. */
        struct Offsets
        {
            std::vector<std::string>& problems;

            void visit (const juce::ValueTree& node)
            {
                if (node.getType().toString() == "Media")
                {
                    const auto offset = static_cast<double> (
                        node[juce::Identifier ("startOffset")]);

                    bool hasRange = false;

                    for (const auto& child : node)
                        if (child.getType().toString() == "Range")
                            hasRange = true;

                    /*  `> 0` rather than `!= 0`: the row's range is 0.. so a
                        negative offset is the Walk pass's problem, and an
                        equality on a double is a warning in the strict build. */
                    if (hasRange && offset > 0.0)
                        problems.push_back ("/Show/.../Media["
                                             + node[juce::Identifier ("id")].toString().toStdString()
                                             + "]: a cue with ranges plays its ranges, so it cannot"
                                               " also have a startOffset - the offset belongs in the"
                                               " first range's `in`");
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        Offsets { problems }.visit (showNode);

        /*  A MIDI CUE CANNOT WAIT TO BE VERIFIED, because nothing will ever
            answer.

            §3.11's `verified` asks the target for the value back and compares
            it, which is a thing an OSCQuery node can do and a MIDI cable
            cannot: there is no read-back, no address to ask about, and no
            protocol to ask in. A cue that asked for one would wait for its
            timeout and then fail, every time, at half past seven.

            REFUSED WHEN THE SHOW IS READ, like the trigger address inside
            /godot and the start offset beside a range, and for the same reason:
            there is no reading of the file under which the cue does what it
            says. The row's own enum already excludes it, so this catches the
            hand-edited file rather than the one a client wrote. */
        struct MidiWaits
        {
            std::vector<std::string>& problems;

            void visit (const juce::ValueTree& node)
            {
                if (node.getType().toString() == "Midi"
                      && node[juce::Identifier ("wait")].toString() == "verified")
                    problems.push_back ("/Show/.../Midi["
                                         + node[juce::Identifier ("id")].toString().toStdString()
                                         + "]: a MIDI cue cannot wait to be verified - there is"
                                           " no read-back on a MIDI cable, so nothing would ever"
                                           " answer");

                for (const auto& child : node)
                    visit (child);
            }
        };

        MidiWaits { problems }.visit (showNode);

        /*  A CUE THAT WAITS FOR AN ANSWER NOBODY CAN GIVE - question K, and
            the refusal decision K actually asked for.

            §3.11's `verified` writes a value and then asks the target what it
            holds. That needs a target that can be asked, which a mount says by
            declaring `readback` and a `queryPort` (§9, decision K). Aimed at a
            mount that declares neither - or at an address under no mount at all
            - the cue writes, waits its whole timeout, and fails. Every time,
            and only ever during the show.

            REFUSED WHEN THE SHOW IS READ, which is what decision K settled and
            what was NOT built: PR 2.6 put the check in the mount loader, where
            every verb printed it to stderr and opened the show anyway, so the
            only thing it changed was `wfg validate`'s exit code. It sits here
            now, beside the MIDI rule it is the twin of, so that the refusal
            arrives the way the other refusals do - a trigger listening inside
            /godot, a start offset beside a range, a MIDI cue asking to be
            verified. The check is on the document alone and needs no mount
            table, no socket and no device, so it still runs on a laptop with
            nothing plugged in, which is the machine somebody is sitting at when
            they have time to fix it. */
        struct VerifiedCues
        {
            std::vector<std::string>& problems;
            const std::vector<MountFacts>& mounts;

            void visit (const juce::ValueTree& node)
            {
                for (const auto& child : node)
                    visit (child);

                if (node.getType().toString() != "Osc"
                      || node[juce::Identifier ("wait")].toString() != "verified")
                    return;

                const auto id = node[juce::Identifier ("id")].toString().toStdString();
                const auto address = node[juce::Identifier ("address")].toString().toStdString();

                const MountFacts* owner = nullptr;

                for (const auto& mount : mounts)
                    if (address.size() > mount.prefix.size()
                          && address.compare (0, mount.prefix.size(), mount.prefix) == 0
                          && address[mount.prefix.size()] == '/')
                        owner = &mount;

                if (owner == nullptr)
                {
                    problems.push_back ("/Show/.../Osc[" + id + "]: \"" + address
                                          + "\" is under no mounted namespace, so nothing can be"
                                            " asked about it");
                    return;
                }

                if (! owner->canBeAsked)
                    problems.push_back ("/Show/.../Osc[" + id + "]: waits for verification from "
                                          + owner->id + ", which declares no readback. A cue that"
                                            " cannot succeed is worse than one that fails, because"
                                            " it holds the list");
            }
        };

        VerifiedCues { problems, mountFacts }.visit (showNode);

        //----------------------------------------------------------------------
        /*  PHASE 4'S SLOTS, and the destinations that name them (PRD §3.9b,
            §3.9e). Four rules, all of them about numbers the document can
            check itself.

            The widths first, so the walks below are lookups rather than a
            traversal per destination. */
        std::map<std::string, int> busWidth;
        std::map<std::string, int> slotWidth;

        const auto intAttribute = [] (const juce::ValueTree& node, const char* name, int fallback)
        {
            const juce::Identifier key { name };
            return node.hasProperty (key) ? static_cast<int> (node[key]) : fallback;
        };

        for (const auto& audio : showNode)
        {
            if (audio.getType().toString() != "Audio")
                continue;

            for (const auto& bus : audio)
                if (bus.getType().toString() == "Bus")
                    busWidth[bus[idProperty].toString().toStdString()]
                        = intAttribute (bus, "width", 1);
        }

        for (const auto& mounts : showNode)
        {
            if (mounts.getType().toString() != "Mounts")
                continue;

            for (const auto& mount : mounts)
            {
                const auto prefix = mount[juce::Identifier ("prefix")].toString().toStdString();

                for (const auto& slot : mount)
                {
                    if (slot.getType().toString() != "Slot")
                        continue;

                    const auto slotId = slot[idProperty].toString().toStdString();
                    const auto width = intAttribute (slot, "width", 1);
                    const auto firstChannel = intAttribute (slot, "firstChannel", 0);
                    const auto address = slot[juce::Identifier ("address")].toString().toStdString();
                    const auto busId = slot[juce::Identifier ("bus")].toString().toStdString();

                    slotWidth[slotId] = width;

                    const auto where = "/Show/.../Slot[" + slotId + "]: ";

                    /*  AN INPUT OF THAT PROCESSOR, and not of some other one. A
                        slot whose address falls outside its own mount's prefix
                        would claim exclusivity over parameters the mount does
                        not carry - and the osc cues that write those parameters
                        would go somewhere else entirely. */
                    if (! prefix.empty()
                          && ! (address.size() > prefix.size()
                                  && address.compare (0, prefix.size(), prefix) == 0
                                  && address[prefix.size()] == '/'))
                        problems.push_back (where + "\"" + address + "\" is not under \"" + prefix
                                              + "\", which is the prefix of the mount that"
                                                " declares it");

                    /*  AND IT HAS TO FIT IN ITS BUS. `firstChannel` is an offset
                        into the bus, exactly as a bus's own is an offset into
                        the hardware outputs, so a slot that ran off the end
                        would send part of a source into channels nobody
                        declared. A bus that is not there at all is the `refers`
                        column's warning rather than this refusal - deleting a
                        bus must not stop yesterday's show opening. */
                    if (const auto bus = busWidth.find (busId); bus != busWidth.end())
                        if (firstChannel + width > bus->second)
                            problems.push_back (where + "channels " + std::to_string (firstChannel)
                                                  + " to " + std::to_string (firstChannel + width - 1)
                                                  + " do not fit in a bus " + std::to_string (bus->second)
                                                  + " channels wide");
                }
            }
        }

        /*  AND THE COEFFICIENTS, which the parameter table has said were
            refused when the show loads since Phase 2 and which nothing has ever
            checked: the only check was at arm, where it failed the run.

            WHAT IS CHECKABLE HERE is that the list is a whole number of input
            channels wide. How many channels the cue HAS is the file's, and the
            file arrives on a different machine from the one the show was
            written on - so `gains.size() % width` is the question the document
            can answer, and "eight gains into a bus three wide" is a matrix that
            is not a matrix whoever plays it. */
        struct Destinations
        {
            std::vector<std::string>& problems;
            const std::map<std::string, int>& busWidth;
            const std::map<std::string, int>& slotWidth;

            void check (const juce::ValueTree& node, const char* element, const char* target,
                        const std::map<std::string, int>& widths)
            {
                const auto id = node[idProperty].toString().toStdString();
                const auto targetId = node[juce::Identifier (target)].toString().toStdString();
                const auto found = widths.find (targetId);

                if (found == widths.end())
                    return;                 // dangling: the `refers` column's warning

                const auto width = std::max (1, found->second);
                const auto gains = countTokens (node[juce::Identifier ("gains")]
                                                  .toString().toStdString());

                /*  The rule itself is `coefficientsFit`, which the write door
                    asks too - an empty list included, which fits. */
                if (! coefficientsFit (gains, width))
                    problems.push_back (std::string ("/Show/.../") + element + "[" + id + "]: "
                                          + std::to_string (gains) + " coefficients do not divide"
                                            " into a destination " + std::to_string (width)
                                          + " channels wide - a routing matrix is one row per"
                                            " channel the cue has");
            }

            void visit (const juce::ValueTree& node)
            {
                const auto element = node.getType().toString();

                if (element == "Route")       check (node, "Route", "bus", busWidth);
                else if (element == "Feed")   check (node, "Feed", "slot", slotWidth);

                for (const auto& child : node)
                    visit (child);
            }
        };

        Destinations { problems, busWidth, slotWidth }.visit (showNode);

        /*  A DCA INSIDE ITSELF (PRD §3.28). The write door refuses to make one,
            so a file carrying one was edited by hand - and no reading of it
            says what the cues marked with those DCAs should play at, because
            the sum has no end. Refused at load for the reason an OSC trigger
            under /godot is: there is no reading of the file under which it
            does what it says. */
        if (const auto dcas = showNode.getChildWithName ("Dcas"); dcas.isValid())
            for (const auto& dca : dcas)
            {
                const auto id = dca[idProperty].toString().toStdString();
                const auto parent = dca[juce::Identifier ("dca")].toString().toStdString();

                if (! id.empty() && dcaChainReaches (parent, id))
                    problems.push_back ("/Show/Dcas/Dca[" + id + "]: sits inside itself - a DCA"
                                        " may sit inside another, never in a circle, or the level"
                                        " of every cue marked with it would be a sum with no end");
            }

        return problems;
    }

    std::vector<std::string> ShowDocument::warnings() const
    {
        std::vector<std::string> problems;

        /*  EVERY REFERENCE THE TABLE DECLARES, checked in one place.

            An identifier in a document is a pointer at another object, and
            until the `refers` column there was no one place that knew it: the
            standby pointer had a function of its own, `fade/@target` and
            `stop/@target` had nothing at all, and this file carried a note
            saying the generalisation would be worth writing when there was a
            second case. Phase 3 brought four more - a range's cue, a MIDI
            cue's port, a run's parent, a trigger's cue - and this is it.

            A WARNING AND NEVER A LOAD REFUSAL, which is the whole shape of it.
            §3.8 makes a stop aimed at a cue that is not there a silent no-op
            during tech; `object.delete` repairs nothing referential, by
            design, because repairing it would mean deciding what somebody
            meant; and yesterday's saved show has to open tomorrow. So the file
            loads, `wfg validate` says which pointer is dangling, and a cue
            that is actually fired fails its run.

            THE TARGET IS LOOKED UP BY IDENTIFIER AND THEN BY KIND, both,
            because half a check is worse than none: a fade whose target is a
            BUS would otherwise pass, and would fail at half past seven with a
            message about a run rather than about a show. */
        struct References
        {
            std::vector<std::string>& problems;
            const ShowDocument& document;

            void visit (const juce::ValueTree& node)
            {
                const auto element = node.getType().toString().toStdString();

                if (const auto* described = Schema::instance().element (element))
                {
                    for (const auto& attribute : described->attributes)
                    {
                        const auto refers = attribute.refers();

                        if (refers.empty())
                            continue;

                        const juce::Identifier name { juce::String (std::string (attribute.name())) };

                        if (! node.hasProperty (name))
                            continue;

                        const auto whole = node[name].toString().toStdString();

                        /*  Empty is a pointer at nothing on purpose - a list
                            with no standby, a cue with no target yet - and is a
                            resting state rather than a dangling reference. */
                        if (whole.empty())
                            continue;

                        const auto here = "/Show/.../" + element + "["
                                            + node[idProperty].toString().toStdString() + "]/@"
                                            + std::string (attribute.name());

                        /*  SEVERAL IDENTIFIERS, SPACE-SEPARATED, ARE SEVERAL
                            REFERENCES (2026-09-23). A surface names its ports
                            in bank order, one per bank, in one row - and an
                            identifier is eight characters of Crockford base32
                            with no space in it, so a value with spaces was
                            never one identifier and splitting it loses nothing
                            a single-valued row could have said. */
                        std::vector<std::string> values;

                        for (std::size_t at = 0; at < whole.size();)
                        {
                            const auto start = whole.find_first_not_of (' ', at);

                            if (start == std::string::npos)
                                break;

                            const auto end = whole.find (' ', start);
                            values.push_back (whole.substr (start, end == std::string::npos
                                                                     ? std::string::npos
                                                                     : end - start));
                            at = end == std::string::npos ? whole.size() : end;
                        }

                        for (const auto& value : values)
                        {
                            const auto target = document.findById (value);

                            if (! target.isValid())
                            {
                                problems.push_back (here + ": names \"" + value
                                                     + "\", which is not in this show");
                                continue;
                            }

                            const auto found = ownerForElement (
                                target.getType().toString().toStdString());

                            if (found != refers)
                                problems.push_back (here + ": names \"" + value + "\", which is a "
                                                     + (found.empty() ? std::string ("thing of no kind")
                                                                      : std::string (found))
                                                     + " and not a " + std::string (refers));
                        }
                    }
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        References { problems, *this }.visit (showNode);

        /*  A PRESET THAT NAMES A GROUP THE CUE IS NOT INSIDE.

            §13.7: `preset` names the ANCESTOR group whose header prepares this
            cue, and naming an ancestor is what the gesture means - dragging a
            cue onto the header of one of the groups it is in. A value naming
            anything else is a mark nothing can act on: the horizon prepares a
            block by walking its own subtree, so a cue outside that subtree is
            never reached however early the pointer arrives.

            A WARNING AND NOT A REFUSAL, for the reason every referential
            mistake in this file is one: the repair is somebody dragging it
            somewhere sensible, and yesterday's saved show has to open tomorrow.
            The `refers` column above has already said whether the identifier
            names a cue at all; this says whether it names one that could ever
            do the preparing. */
        struct Persistents
        {
            std::vector<std::string>& problems;

            void visit (const juce::ValueTree& node)
            {
                if (node.getType().toString() == "Persistent")
                {
                    /*  MEDIA, OSC AND MIDI ARE WHAT A SECTION CAN ASSERT (§13.11).
                        A fade asserts nothing, a stop is the thing that
                        SUSPENDS an assertion, and a group is a lifetime rather
                        than a state. Each is left where it is and ignored, and
                        this is where somebody finds out why nothing happens. */
                    for (const auto& child : node)
                    {
                        const auto element = child.getType().toString().toStdString();

                        if (element != "Fade" && element != "Transport" && element != "Group"
                             && element != "Start")
                            continue;

                        problems.push_back (
                            "/Show/.../Persistent/" + element + "["
                              + child[idProperty].toString().toStdString()
                              + "]: a persistent section asserts media, osc and midi cues and"
                                " nothing else - a fade asserts nothing, a stop is what suspends"
                                " an assertion, a group is a lifetime rather than a state - so"
                                " this one is ignored");
                    }
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        Persistents { problems }.visit (showNode);

        /*  A DESTINATION THAT NAMES THE WRONG KIND OF OUTPUT.

            The `refers` column above has already said whether the identifier
            names a bus at all; this says whether it names one that could be
            what it is being used as. A direct out is where ONE cue's own
            channels land and a mix channel is where MANY cues arrive at a
            level, and the word on the bus is what puts it in one menu or the
            other - so a cue aimed at a mix channel, or a send into a direct
            out, is a routing somebody will not find in the window they go
            looking in.

            A WARNING AND NOT A REFUSAL, and here the reason is sharper than
            usual: BOTH STILL SOUND. `resolveRouting` reads a bus's channels
            and its width and neither depends on the word, so the cue plays out
            of exactly the channels the designer pointed it at. What is wrong
            is the bookkeeping, not the sound, and refusing to open a show over
            bookkeeping would be refusing to open a show that works. */
        struct Kinds
        {
            std::vector<std::string>& problems;
            const std::map<std::string, std::string>& busKind;

            void say (const std::string& where, const std::string& busId,
                      const char* used, const char* is)
            {
                const auto found = busKind.find (busId);

                //  An identifier naming no bus is the References visitor's.
                if (found == busKind.end() || found->second != is)
                    return;

                problems.push_back (where + ": names \"" + busId + "\", which is a " + is
                                      + " channel and not a " + used + " - it will sound out of"
                                        " that output all the same, but it is not what either"
                                        " menu offers");
            }

            void visit (const juce::ValueTree& node)
            {
                const auto element = node.getType().toString().toStdString();
                const auto id = node[idProperty].toString().toStdString();

                if (element == "Media")
                {
                    const auto out = node[juce::Identifier ("directOut")].toString().toStdString();

                    if (! out.empty())
                        say ("/Show/.../Media[" + id + "]/@directOut", out, "direct out", "mix");
                }
                else if (element == "Send")
                {
                    const auto bus = node[juce::Identifier ("bus")].toString().toStdString();

                    if (! bus.empty())
                        say ("/Show/.../Send[" + id + "]/@bus", bus, "mix channel", "direct");
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        /*  THE WORD EACH BUS CARRIES, gathered once. `Rack` holds channels and
            not buses, so it is skipped the way every other walk of `<Audio>`
            skips it. */
        std::map<std::string, std::string> busKind;

        if (const auto audioNode = showNode.getChildWithName (juce::Identifier ("Audio"));
            audioNode.isValid())
            for (const auto& bus : audioNode)
                if (bus.hasType ("Bus"))
                {
                    const auto word = bus[juce::Identifier ("kind")].toString().toStdString();

                    busKind[bus[idProperty].toString().toStdString()]
                        = word.empty() ? std::string ("direct") : word;
                }

        Kinds { problems, busKind }.visit (showNode);

        /*  AND A CUE THAT ARRIVES AT ONE OUTPUT TWICE.

            A cue's destinations are a list and not a choice (PRD 3.9b), so
            holding a direct out AND a route is ordinary - a source into the
            processor plus a feed to foldback is the example the section gives.
            Holding both onto the SAME bus is not: the coefficients are summed,
            so the cue arrives there at roughly double, and the designer who
            set a level on one of them will hear something else. Said rather
            than refused, because which of the two to drop is theirs to pick. */
        struct Doubles
        {
            std::vector<std::string>& problems;

            void visit (const juce::ValueTree& node)
            {
                if (node.hasType ("Media"))
                {
                    const auto out = node[juce::Identifier ("directOut")].toString().toStdString();

                    if (! out.empty())
                        for (const auto& child : node)
                            if ((child.hasType ("Route") || child.hasType ("Send"))
                                  && child[juce::Identifier ("bus")].toString().toStdString() == out)
                                problems.push_back (
                                    "/Show/.../Media[" + node[idProperty].toString().toStdString()
                                      + "]: its direct out and its "
                                      + child.getType().toString().toStdString()
                                      + " both name \"" + out + "\", so the cue arrives there"
                                        " twice and sums with itself");
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        Doubles { problems }.visit (showNode);

        struct Presets
        {
            std::vector<std::string>& problems;
            std::vector<std::string> ancestors;

            void visit (const juce::ValueTree& node)
            {
                const auto element = node.getType().toString().toStdString();
                const auto isGroup = element == "Group";

                if (ownerForElement (element) == "cue")
                {
                    const auto preset = node[juce::Identifier ("preset")]
                                            .toString().toStdString();

                    if (! preset.empty()
                         && std::find (ancestors.begin(), ancestors.end(), preset)
                              == ancestors.end())
                        problems.push_back (
                            "/Show/.../" + element + "["
                              + node[idProperty].toString().toStdString()
                              + "]/@preset: names \"" + preset + "\", which is not a group this"
                                " cue is inside - so no header will prepare it, and it will run"
                                " at its own moment as if the mark were not there");
                }

                if (isGroup)
                    ancestors.push_back (node[idProperty].toString().toStdString());

                for (const auto& child : node)
                    visit (child);

                if (isGroup)
                    ancestors.pop_back();
            }
        };

        Presets { problems, {} }.visit (showNode);

        /*  AND A MOUNT THAT SAYS ITS NODES MAY BE WRITTEN EARLY BUT CANNOT BE
            ASKED WHAT THEY HELD.

            §13.1 makes anticipation conditional on being able to take it back,
            and taking a write back means putting the old value there - which
            needs the old value, which needs a read. A mount marked
            `anticipatable` with `readback` of `none` has told Go.dot that an
            early write is safe and given it no way to undo one, so nothing on
            it is ever pre-sent and every cue aimed at it runs at entry.

            A WARNING AND NOT A REFUSAL, because the show is complete and
            correct: what it loses is a saved moment, not a sound. It is worth
            saying out loud because the two attributes are set in different
            places and one of them is doing nothing. */
        const auto mounts = showNode.getChildWithName (juce::Identifier ("Mounts"));

        for (const auto& mount : mounts)
        {
            if (mount.getType().toString() != "Mount")
                continue;

            const auto anticipatable =
                mount.hasProperty (juce::Identifier ("anticipatable"))
                  && static_cast<bool> (mount[juce::Identifier ("anticipatable")]);

            if (! anticipatable)
                continue;

            const auto readback = mount.hasProperty (juce::Identifier ("readback"))
                                    ? mount[juce::Identifier ("readback")].toString().toStdString()
                                    : std::string ("none");

            if (readback == "oscquery")
                continue;

            problems.push_back ("/Show/.../Mount["
                                  + mount[idProperty].toString().toStdString()
                                  + "]/@anticipatable: says its nodes may be written ahead of a"
                                    " GO, but @readback is \"" + readback
                                  + "\" - so nothing can be read back to restore, and nothing"
                                    " will be pre-sent");
        }

        /*  AND TWO OUTPUTS SHARING AN INTERFACE CHANNEL.

            The four layout commands keep `Bus/@firstChannel` packed, so a show
            written through them never reaches this. A show written by hand
            can, and an overlap is the one that matters: the same interface
            channel summing two different mixes, which nobody hears until the
            night and nothing else in the file would ever mention.

            A GAP IS NOT REPORTED, deliberately. Outputs that start above where
            the ones before them end are a RIG, not a fault -
            `tests/fixtures/bundles/slots` feeds a processor from channel 9
            upward and a foldback from 1, because that is how the box is wired
            - and the layout commands preserve exactly that by moving the
            channels into the interface patch before anything is repacked
            (`document/OutputLayout.h`). Warning about it would put a line in
            front of every designer who ever left room on their interface.

            A WARNING AND NOT A REFUSAL, for the reason every other one here is
            one: yesterday's saved show has to open tomorrow, and an overlap
            still plays - loudly. */
        {
            /*  IN CHANNEL ORDER, not document order - `busNodes` sorts, and
                asking this of the file's own order would report an overlap
                wherever somebody had simply written the outputs out of
                sequence, which is not a fault at all. */
            const auto outputs = busNodes();

            for (std::size_t at = 1; at < outputs.size(); ++at)
            {
                const auto previousId = outputs[at - 1][idProperty].toString().toStdString();
                const auto id = outputs[at][idProperty].toString().toStdString();

                const auto startOf = [this] (const std::string& busId)
                {
                    const auto text = getAttribute ("/godot/bus/" + busId + "/firstChannel");
                    return text.has_value() ? juce::String (*text).getIntValue() : 0;
                };

                const auto widthOf = [this] (const std::string& busId)
                {
                    const auto text = getAttribute ("/godot/bus/" + busId + "/width");
                    return text.has_value() ? juce::String (*text).getIntValue() : 1;
                };

                const auto ends = startOf (previousId) + widthOf (previousId);

                if (startOf (id) < ends)
                    problems.push_back (
                        "/Show/Audio/Bus[" + id + "]/@firstChannel: starts at "
                          + std::to_string (startOf (id)) + ", inside \"" + previousId
                          + "\", which runs to " + std::to_string (ends - 1)
                          + " - so both are summed onto the same interface channels. The output"
                            " list keeps outputs packed; this one was written by hand");
            }
        }

        return problems;
    }
}
