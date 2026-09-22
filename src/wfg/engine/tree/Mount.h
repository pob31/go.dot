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

#pragma once

/*
    Somebody else's namespace, mounted into ours.

    Go.dot conducts other programs - WFS-DIY, XOA, S21-HiJack, whatever someone
    points it at - and PRD §3.22 settles how it learns what they can do: THE
    TEMPLATE FORMAT IS AN OSCQuery DESCRIPTION. So a captured `GET /` from a
    running processor and a description somebody wrote by hand in a text editor
    are the same kind of file, and the engine cannot tell which it got. That is
    not a convenience; it is what stops the hand-written case from being a
    second-class citizen with a second-class parser.

    A mount appears at its prefix - `/wfs`, `/xoa`, `/s21`, `/ext/<name>` - and
    not under `/godot/mount`, which holds the DECLARATION rather than the
    namespace. The two are different things: `/godot/mount/<id>/prefix` says
    where a target is mounted; `/wfs/input/1/positionX` is a node on the target.

    THE PREFIX IS WHERE THE DESCRIPTION'S ROOT LANDS, and a description may
    perfectly well be of a SUBTREE. That matters in practice: WFS-DIY publishes
    its whole namespace under a `/wfs` container of its own, so a capture of
    `GET /` mounted at `/wfs` would produce `/wfs/wfs/input/1/positionX`.
    Capturing `GET /wfs` instead gives a description whose root is `/wfs`, and
    mounting that at `/wfs` gives the addresses anybody would expect. The
    reader handles either: the mounted address is always the prefix plus the
    nesting, and the root's own `FULL_PATH` is used only to check that the
    file's paths agree with its shape.

    WHAT A MOUNT DOES: reads a description, publishes it as nodes, accepts
    writes to those nodes, logs them, and - from Phase 2 - sends them to the
    box the description belongs to.

    WHERE IT SENDS THEM is `host` and `port` on the declaration, and `port` is
    required with no default. That is the same shape as `audio/tracks` and it is
    required for a harder reason: UDP never reports that nobody was listening,
    so a mount that guessed a port would send into the dark and report success
    for the whole of a show. A number that cannot be checked at run time has to
    be checked in the document.

      * CAPTURED VALUES ARE IGNORED. A captured description carries whatever
        the target happened to be doing when somebody pointed a browser at it,
        and PRD §4.10 keeps that out of anything Go.dot treats as decided. A
        mounted node therefore starts with NO value at all, and acquires one
        only when something writes it. It is never persisted.

      * KIND IS INFERRED WHEN THE FILE DOES NOT SAY. Write-only access with no
        `VALUE` is an event; anything else is state. A file may carry its own
        `GODOT` key and override that, which is allowed precisely so that a
        hand-written template can declare what a captured one can only imply.

      * THE MOUNT'S DECLARATION SUPPLIES THE REST. Rate cap, anticipatability
        and panic come from `/godot/mount/<id>` unless a node overrides them.
        `anticipatable` is false for a third party by default (PRD §3.3): we do
        not know whether sending early is revocable on somebody else's box, and
        guessing yes is the guess that breaks a show.

      * A WRITE TO A READ-ONLY NODE IS REFUSED, like anywhere else, and an
        accepted one lands in the tree, in the log, and on the wire. This table
        does the first two and hands the third to a MountSender, which is what
        keeps a socket out of a class that is otherwise pure arithmetic over a
        JSON document.
*/

#include <wfg/engine/tree/Node.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree
{
    /*  THE ADDRESSES A DEVICE ANSWERS AT, split from its `prefix` row.

        ONE DEVICE, SEVERAL ROOTS, and the author's own desk is why. A DiGiCo
        S21 reached directly rather than through its sidecar answers at
        `/channel/…` for every strip control, `/console/…` for ping, pong and
        the channel counts, and `/digico/snapshots/fire` for snapshot recall -
        three roots with nothing above them, because the desk's addresses
        simply start at the root of its own world. Declaring it three times
        would be three rows, three names and three `sent` counts for one
        console; giving it one root that is not really its own would mean the
        address in the cue is not the address in the manual somebody is
        copying from.

        SPACE-SEPARATED IN ONE ROW, which is what `audio/inputPatch` already
        does and what an OSC address can never contain. A device with one
        prefix reads exactly as it always did, so every show written before
        this is unchanged and no attribute had to grow a type. */
    std::vector<std::string> prefixesOf (const std::string& prefixRow);

    /*  How much of an address one of these prefixes covers, or 0 for none.

        THE BOUNDARY IS A SEPARATOR: `/desktop/fader` is not under `/desk`.
        THE LONGEST WINS, which is what makes nesting mean something - with
        `/desk` and `/desk/aux` both declared, a cue under the second belongs
        to the second, the way a mount point works everywhere else.

        It answers a LENGTH rather than a bool so a caller comparing two
        devices can tell which matched more of the address, and so a client
        rewriting an address knows how much of it to replace. */
    std::size_t prefixMatchLength (const std::string& address, const std::string& prefixRow);

    /*  A mount as the document declares it, read off `/godot/mount/<id>`. The
        defaults here are the table's. */
    struct MountDeclaration
    {
        std::string id;

        /*  Where it lands: "/wfs", or "/channel /console /digico" for a device
            that answers at several roots. See `prefixesOf`. */
        std::string prefix;
        std::string namespaceFile;    ///< bundle-relative: "namespaces/wfs-diy.json"

        /*  WHAT A PERSON CALLS IT, and it is not the prefix.

            The prefix is an address and is what every cue aimed here carries;
            the name is for a menu and a list, and renaming a device must move
            no cue at all - the reason a MIDI port has both (PRD 4.10). Empty
            reads as the prefix, so a device is never a blank row. */
        std::string name;

        /*  WHETHER IT IS HEARD, AND WHETHER IT IS SPOKEN TO.

            `rx` is read by one thing today: with strict senders on, a datagram
            from a host no declared device holds with this set is dropped
            rather than obeyed. What is DONE with what a device sends is a
            later piece of work, and the flag is stored now because it is a
            decision about the rig rather than something the machine noticed.

            `tx` is the one that acts. Off, a cue aimed here still runs, still
            lands in the tree and still lands in the log - it finishes at once
            carrying `not-sent` - so a rehearsal in a room without the desk
            plays the show instead of printing a column of failures. */
        bool rx = false;
        bool tx = true;

        /*  NO INTERFACE HERE, AND THAT IS A DECISION (2026-09-22). Which
            network card an outgoing message leaves by is the operating
            system's to answer: it reads the destination address and picks the
            route. WFS-DIY has had an interface menu for years that is stored
            and fed to no socket at all, and the one socket in that program
            which does bind an interface is its PSN receiver - because
            multicast has no routing table answer, which unicast OSC does.

            Where the choice is real is the RECEIVING side: binding to every
            address hears every network, binding to one hears one. That lives
            on the show's own network settings, not on each device. */

        /*  Where the box is, and how to reach it.

            `host` is a literal address rather than a name on purpose: a socket
            re-resolves whenever the destination differs from the last one it
            saw, and a blocking name lookup on the tick thread is a frame
            nobody gets back. `port` has no sensible default and is required in
            the document. */
        std::string host = "127.0.0.1";
        int port = 0;
        std::string transport = "udp";

        /*  WHETHER IT CAN BE ASKED, and where.

            `transport` says how to send and says nothing about the other
            direction, which is the gap that made `wait: verified` a cue that
            could never succeed against most devices with nothing noticing until
            the show. `none` is the honest default: OSCQuery was never
            standardised, most boxes do not run a server, and a mounted
            namespace is usually hand-written for something that will never
            answer.

            `queryPort` is a different number from `port` - WFS-DIY takes
            messages on 8000 and describes itself on 5005 - which is why it is a
            separate attribute and not an assumption. */
        std::string readback = "none";
        int queryPort = 0;

        /*  WHETHER THIS DEVICE DESCRIBES ITSELF, which is the difference
            between the two kinds of target a show can hold.

            A DESCRIBED device has a namespace file: its nodes are known, so a
            cue aimed at one is checked before the show, the value is coerced
            to the type the node declared, and - if it says so - it can be
            asked what it holds afterwards. That is PRD 3.22's first half and
            the only kind Go.dot had until 2026-09-22.

            An OPAQUE device has no file, and is the ordinary case for a desk
            somebody typed an address into. Nothing is published under its
            prefix, nothing is checked, and the value goes out exactly as it
            was written. It is deliberately not a weaker version of the first:
            "I know what is there" and "somebody told me where to send it" are
            different claims, and the second is honest about being the second. */
        bool opaque() const noexcept { return namespaceFile.empty(); }

        /*  Whether a verified cue can be aimed at this target at all. An
            opaque device can never be asked, whatever it declares: there is
            no node to ask about, so a `verified` cue aimed at one would wait
            for an answer that has nowhere to come from. */
        bool canBeAsked() const noexcept
        {
            return ! opaque() && readback == "oscquery" && queryPort > 0;
        }

        double rateCap = 50.0;
        bool anticipatable = false;
        std::string panic = "park";

        /*  Whether two declarations say the same thing, which is how the
            after-tick refresh tells a document edit that touched this device
            from one that touched the cue next to it. Defaulted rather than
            written out, so a field added later cannot be forgotten here. */
        bool operator== (const MountDeclaration&) const = default;
    };

    struct MountResult
    {
        bool ok = false;

        /** One message per problem, in the order they were found. */
        std::vector<std::string> problems;

        /** The mounted nodes, sorted by address. Empty when it did not load. */
        std::vector<Node> nodes;

        static MountResult failed (std::string problem);
    };

    /*  Reads an OSCQuery description into nodes under the mount's prefix.

        Pure: no filesystem, no engine, no state. Hand it the text and it hands
        back the nodes, which is what makes every rule above testable against a
        string literal rather than against a bundle on disk. */
    MountResult readNamespace (const MountDeclaration& mount, std::string_view json);

    //==============================================================================
    /*  Every mount that has been loaded, and the values written to them.

        The values live here rather than in the document because they are
        runtime state: they are what somebody else's box is doing, which PRD
        §4.10 keeps out of the file. A reload forgets them, deliberately - the
        namespace may have changed shape underneath, and carrying a value across
        that would be asserting something nobody checked.
    */
    class MountTable
    {
    public:
        /** Loads or reloads one mount. Replaces whatever was there before. */
        MountResult load (const MountDeclaration& mount, std::string_view json);

        /*  DECLARES A DEVICE THAT DESCRIBES NOTHING - an opaque one, which has
            no namespace file to read.

            It is not `load` with an empty string, and the difference is worth
            the second method: `load` fails a namespace that describes no nodes,
            deliberately, because a description file with nothing in it is a
            file somebody got wrong. A device with no file at all has nothing
            to get wrong. The entry holds a declaration and an empty node list,
            which is exactly what `write` reads to decide that an unknown
            address under this prefix is a message rather than a mistake. */
        MountResult declare (const MountDeclaration& mount);

        /*  REPLACES WHAT A DEVICE SAYS ABOUT ITSELF, keeping its nodes.

            Every row but the prefix and the namespace file can be edited while
            the show is open - the host, the port, the ports it answers on, the
            rate cap, rx and tx - and none of them changes what is mounted.
            Re-reading the namespace for a changed host would throw away every
            value the tree holds for that device and every read-back in flight,
            to arrive at the same nodes. False when there is no such mount. */
        bool updateDeclaration (const MountDeclaration& mount);

        /*  WHY THIS DEVICE CANNOT BE USED AS DECLARED, in one sentence, and
            empty when it can.

            These refusals used to be a line on the terminal at startup and
            nothing else, so a device that would never work looked exactly like
            one that works, in every client, until a cue failed during the
            show. Kept here rather than in the document because it is what the
            machine found, not what anybody decided (PRD 4.10). */
        void setProblem (const std::string& mountId, std::string problem);
        std::string problemOf (const std::string& mountId) const;

        /** Forgets a mount and everything under it. */
        bool unload (const std::string& mountId);

        void clear();

        //======================================================================
        bool isLoaded (const std::string& mountId) const;

        /** How many nodes that mount contributed, or 0. */
        std::size_t nodeCount (const std::string& mountId) const;

        /** Every mounted node from every mount, in address order. */
        std::vector<Node> allNodes() const;

        std::size_t size() const noexcept { return mounts.size(); }

        /*  WHICH VERSION OF THIS TABLE THIS IS, bumped by every change to any
            mounted node - a load, an unload, a write, a read-back.

            IT EXISTS SO THAT NOBODY HAS TO REMEMBER. M9 measured a mounted
            WFS-DIY capture at 3.1 ms of every applied mutation, twenty-nine
            times the rest of the tree put together, so the mounted half is
            cached separately from the show's - and a cache is only as good as
            its invalidation. A second `markStale` somebody had to call from
            every mount path would have been forgotten the first time a path was
            added; a counter the table bumps itself cannot be.

            A number rather than a flag, because the reader is a cache asking
            "is what I have still current" rather than a consumer clearing a
            signal it has taken. */
        std::uint64_t revision() const noexcept { return version; }

        //======================================================================
        /*  A write to a mounted node.

            `unknown-id` when no mount holds that address, `read-only` when it
            does and the node refuses writes, `type-mismatch` when the value is
            not what the node declared. Otherwise the value lands and nothing is
            sent, because there is nothing to send it to yet. */
        struct WriteResult
        {
            bool ok = false;
            std::string reason;

            /*  Which mount took it, and the value AS COERCED - both so the
                caller can put the same thing on the wire that went into the
                tree. A sender that re-read the node would be reading a value
                somebody else might already have overwritten in the same tick. */
            std::string mountId;
            osc::Value value;
        };

        WriteResult write (const std::string& address, const osc::Value& value);

        /** The current value of a mounted node, if it has been written. */
        const osc::Value* valueOf (const std::string& address) const;

        /*  One mounted node, or nullptr. What a caller wants from it is
            almost always the declared TYPE - a value read back off a
            device has to be coerced to it before it can be compared with
            what was written. */
        const Node* nodeAt (const std::string& address) const;

        /*  WHAT THE TARGET SAID, which is a different question from what was
            written to it and is kept apart for exactly that reason.

            A verified cue compares the two. Merging them - letting a read-back
            overwrite the written value - would make every verification pass by
            construction, because the thing being compared would be the answer
            against itself. PRD §4.10 is the same rule one level up: what
            somebody decided and what the machine is doing are never one field.

            Cleared when the address is written, so a stale answer from an
            earlier cue cannot satisfy a later one without anybody being asked. */
        void noteReadback (const std::string& address, const osc::Value& value);
        const osc::Value* readbackOf (const std::string& address) const;
        void forgetReadback (const std::string& address);

        /*  WHAT THE TARGET SAID WHEN NOBODY WAS WAITING FOR IT - the periodic
            observation of §13.10, kept in a third place for the same reason the
            second one exists.

            A verify's read-back is consumed by the cue that asked for it and
            must not be satisfied by an old answer. An observation is not
            waited on by anybody: it is the freshest thing known about the
            target, and what a jump diffs against so that a fader somebody moved
            by hand is corrected rather than assumed.

            FORGOTTEN WHEN GO.DOT WRITES THE ADDRESS, which is what makes the
            question it answers exact: an observation that survives is one taken
            since our last write, so a caller that finds one is holding the
            target's own account of a node nobody here has touched since. Where
            there is none, what Go.dot wrote is the best it knows. */
        void noteObservation (const std::string& address, const osc::Value& value,
                              std::int64_t tick = 0);
        const osc::Value* observedOf (const std::string& address) const;
        void forgetObservation (const std::string& address);

        /*  WHEN the observation was taken, or -1 when there is none - so that a
            reader waiting for the sweep it just asked for can tell a fresh
            answer from last second's. */
        std::int64_t observedAtTick (const std::string& address) const;

        /** What a mount declared, or nullptr if it is not loaded. */
        const MountDeclaration* declarationOf (const std::string& mountId) const;

        /** The mount whose prefix covers an address, or an empty string. */
        std::string mountOf (const std::string& address) const;

        /*  Every mount the table holds, by identifier, in a stable order.
            What the refresh walks to find the devices the document no longer
            declares. */
        std::vector<std::string> ids() const;

    private:
        struct Entry
        {
            MountDeclaration declaration;
            std::vector<Node> nodes;      // sorted by address
        };

        Node* findNode (const std::string& address);

        /** Bumped by every mutation, whatever kind. See `revision`. */
        std::uint64_t version = 1;

        std::map<std::string, Entry> mounts;   // by mount id, so the order is stable

        /*  Read-backs, by address, separate from the nodes because they are a
            different fact about the same thing and because a reload of the
            namespace must not carry one across. */
        std::map<std::string, osc::Value> readbacks;

        /** Observations, by address, and the tick each was taken on. See `noteObservation`. */
        std::map<std::string, osc::Value> observations;
        std::map<std::string, std::int64_t> observedTicks;

        /*  By mount id, and kept for mounts that are not in `mounts` at all -
            a device refused for having no port never became an entry, and the
            sentence saying so is the only thing anybody can act on. */
        std::map<std::string, std::string> problems;
    };
}
