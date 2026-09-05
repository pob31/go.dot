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

    WHAT PHASE 1 DOES WITH A MOUNT: reads it, publishes it, accepts writes to
    it, logs them, and sends nothing. There is no transport yet. That makes it a
    stub, and the rules below are what keep the stub HONEST rather than merely
    quiet - a mount that lied about what it knew would be worse than no mount.

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
        accepted one lands in the tree and in the log. Phase 2 replaces the sink
        with a transport; nothing above this has to change when it does.
*/

#include <wfg/engine/tree/Node.h>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree
{
    /*  A mount as the document declares it, read off `/godot/mount/<id>`. The
        defaults here are the table's. */
    struct MountDeclaration
    {
        std::string id;
        std::string prefix;           ///< where it lands: "/wfs"
        std::string namespaceFile;    ///< bundle-relative: "namespaces/wfs-diy.json"

        double rateCap = 50.0;
        bool anticipatable = false;
        std::string panic = "park";
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
        };

        WriteResult write (const std::string& address, const osc::Value& value);

        /** The current value of a mounted node, if it has been written. */
        const osc::Value* valueOf (const std::string& address) const;

    private:
        struct Entry
        {
            MountDeclaration declaration;
            std::vector<Node> nodes;      // sorted by address
        };

        Node* findNode (const std::string& address);

        std::map<std::string, Entry> mounts;   // by mount id, so the order is stable
    };
}
