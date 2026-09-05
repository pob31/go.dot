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

#include <wfg/engine/tree/TreeCommands.h>

#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/osc/OscValue.h>

#include <string>

namespace wfg::tree
{
    namespace
    {
        /*  The origin the transport stamped on the event.

            An event with no origin at all is an engine-internal one, and those
            do not touch anything - so a touch without an origin is refused
            rather than filed under the empty string, where it would silence a
            node for every client that also has no origin. */
        const std::string* originOf (const CommandContext& context)
        {
            if (context.origin == nullptr || context.origin->empty())
                return nullptr;

            return context.origin;
        }
    }

    //==============================================================================
    std::vector<std::string> declaredMountIds (const doc::ShowDocument& document)
    {
        std::vector<std::string> ids;

        const juce::Identifier idProperty { "id" };

        for (const auto& container : document.root())
        {
            if (container.getType().toString() != "Mounts")
                continue;

            for (const auto& mount : container)
                if (mount.hasProperty (idProperty))
                    ids.push_back (mount[idProperty].toString().toStdString());
        }

        return ids;
    }

    std::optional<MountDeclaration> mountDeclarationFor (const doc::ShowDocument& document,
                                                         const std::string& mountId)
    {
        const auto base = "/godot/mount/" + mountId + "/";
        const auto prefix = document.getAttribute (base + "prefix");

        if (! prefix.has_value())
            return std::nullopt;

        MountDeclaration declaration;
        declaration.id = mountId;
        declaration.prefix = *prefix;
        declaration.namespaceFile = document.getAttribute (base + "namespace").value_or (std::string {});
        declaration.panic = document.getAttribute (base + "panic").value_or (std::string ("park"));

        if (const auto rateCap = document.getAttribute (base + "rateCap"))
            if (const auto parsed = osc::parseDouble (*rateCap))
                declaration.rateCap = *parsed;

        declaration.anticipatable =
            document.getAttribute (base + "anticipatable").value_or (std::string ("false")) == "true";

        return declaration;
    }

    //==============================================================================
    MountResult loadMountFromBundle (const doc::ShowDocument& document, MountTable& mounts,
                                     const juce::File& bundleFolder, const std::string& mountId)
    {
        const auto declaration = mountDeclarationFor (document, mountId);

        if (! declaration.has_value())
            return MountResult::failed ("no mount " + mountId + " in this show");

        if (declaration->namespaceFile.empty())
            return MountResult::failed (mountId + " declares no namespace file");

        /*  Bundle-relative, and it has to STAY inside the bundle. A namespace
            path of "../../etc/passwd" is not a threat model Phase 1 has, but a
            show that reads a file from outside its own folder is not a show
            anybody can hand to somebody else and expect to work. */
        const auto file = bundleFolder.getChildFile (juce::String (declaration->namespaceFile));

        if (! file.isAChildOf (bundleFolder))
            return MountResult::failed (mountId + ": \"" + declaration->namespaceFile
                                        + "\" points outside the bundle");

        if (! file.existsAsFile())
            return MountResult::failed (mountId + ": no " + declaration->namespaceFile
                                        + " in this bundle");

        juce::MemoryBlock bytes;

        if (! file.loadFileAsData (bytes))
            return MountResult::failed (mountId + ": cannot read " + declaration->namespaceFile);

        return mounts.load (*declaration,
                            std::string_view (static_cast<const char*> (bytes.getData()),
                                              bytes.getSize()));
    }

    std::vector<std::string> loadAllMountsFromBundle (const doc::ShowDocument& document,
                                                      MountTable& mounts,
                                                      const juce::File& bundleFolder)
    {
        std::vector<std::string> problems;

        for (const auto& id : declaredMountIds (document))
        {
            const auto result = loadMountFromBundle (document, mounts, bundleFolder, id);

            for (const auto& problem : result.problems)
                problems.push_back (problem);
        }

        return problems;
    }

    //==============================================================================
    void registerMountCommands (CommandRegistry& registry, const doc::ShowDocument& document,
                                MountTable& mounts, const juce::File& bundleFolder)
    {
        registry.add ({ "mount.load",
                        "Re-reads a mount's OSCQuery description from the bundle.",
                        { { "id", 's', false } },
                        true,
                        [&document, &mounts, &bundleFolder]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto& id = args[0].getString();
                            const auto result = loadMountFromBundle (document, mounts,
                                                                     bundleFolder, id);

                            if (result.ok)
                                return Outcome::ok (args);

                            /*  bad-namespace rather than unknown-id, because the
                                mount was named correctly and what failed is the
                                file it points at - which is somebody else's, and
                                is the thing to go and look at. */
                            return Outcome::rejected (mountDeclarationFor (document, id).has_value()
                                                        ? reason::badNamespace
                                                        : reason::unknownId);
                        } });
    }

    //==============================================================================
    void registerTreeCommands (CommandRegistry& registry, TouchTable& touches)
    {
        //----------------------------------------------------------------------
        registry.add ({ "node.touch",
                        "Holds a node for this origin: it stops receiving pushes for it until"
                        " it releases.",
                        { { "address", 's', false } },
                        true,
                        [&touches] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto* origin = originOf (context);

                            if (origin == nullptr)
                                return Outcome::rejected (reason::badAddress);

                            const auto& address = args[0].getString();

                            if (address.empty())
                                return Outcome::rejected (reason::badAddress);

                            /*  Touching something already held is applied, not
                                rejected. A surface that sends touch on every
                                movement rather than only on the first is doing
                                something reasonable, and an R record per frame
                                would bury the log in noise about nothing. */
                            touches.touch (*origin, address);
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "node.release",
                        "Gives a node back: this origin receives its current value once, and"
                        " pushes for it resume.",
                        { { "address", 's', false } },
                        true,
                        [&touches] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto* origin = originOf (context);

                            if (origin == nullptr)
                                return Outcome::rejected (reason::badAddress);

                            const auto& address = args[0].getString();

                            if (address.empty())
                                return Outcome::rejected (reason::badAddress);

                            /*  Releasing something not held is applied too, and
                                for a sturdier reason than the one above: a
                                surface reconnecting after a drop has no idea
                                what the engine still thinks it holds, and the
                                honest thing for it to do is release everything
                                it might have. Refusing would punish exactly the
                                client that is trying to get back in step. */
                            touches.release (*origin, address);
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  Everything this origin holds, in one command, because a DISCONNECT
            is one event and not a list of them.

            PRD 3.16 requires it: a surface that crashed mid-gesture must not
            leave a node gated against everybody for the rest of the show. The
            server calls it when a WebSocket closes.

            IT IS A NAMED COMMAND rather than the server reaching into the touch
            table, and for two reasons that both matter. The table belongs to
            the tick thread and a disconnect arrives on a socket thread, so a
            direct call would be the one place in the engine where a server
            thread writes to the model - and it would race the flush that reads
            the same table. And PRD 3.15 wants the log to record what happened:
            a surface dropping mid-show IS what happened, and a replay that
            skipped it would diverge from the session it claims to reproduce.

            Applied even when the origin held nothing. A client reconnecting
            after a drop has no idea what the engine still thinks it holds, and
            refusing would punish exactly the client trying to get back in
            step - the same reasoning as node.release above. */
        registry.add ({ "node.releaseAll",
                        "Gives back everything one origin is holding. Sent when it disconnects.",
                        {},
                        true,
                        [&touches] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto* origin = originOf (context);

                            if (origin == nullptr)
                                return Outcome::rejected (reason::badAddress);

                            touches.releaseAll (*origin);
                            return Outcome::ok (args);
                        } });
    }
}
