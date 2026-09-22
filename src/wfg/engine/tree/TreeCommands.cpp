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

#include <algorithm>
#include <functional>

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

        /*  WHAT A PERSON CALLS IT, AND WHETHER IT IS HEARD OR SPOKEN TO
            (2026-09-22). The name moves no cue and the two flags are read
            elsewhere - `rx` by the sender gate, `tx` by the runner - but they
            arrive here because this is the one place the document becomes a
            declaration, and a second reader would be a second chance to
            disagree with it. */
        declaration.name = document.getAttribute (base + "name").value_or (std::string {});

        declaration.rx =
            document.getAttribute (base + "rx").value_or (std::string ("false")) == "true";
        declaration.tx =
            document.getAttribute (base + "tx").value_or (std::string ("true")) == "true";

        /*  WHERE IT SENDS. `transport` was declared in Phase 1 and read by
            nobody; from Phase 2 it decides whether anything can be sent at all,
            and a mount naming a transport Go.dot cannot speak is refused when
            the show loads (loadMountFromBundle) rather than going quiet during
            it.

            `port` has no default, so an absent one stays 0 and is refused the
            same way. That is the whole of what makes a mistyped destination a
            load-time problem instead of a show-time mystery. */
        declaration.host = document.getAttribute (base + "host").value_or (std::string ("127.0.0.1"));
        declaration.transport = document.getAttribute (base + "transport").value_or (std::string ("udp"));

        if (const auto port = document.getAttribute (base + "port"))
            if (const auto parsed = osc::parseDouble (*port))
                declaration.port = static_cast<int> (*parsed);

        /*  And whether anything can be asked of it. Read here rather than
            inferred from the presence of a query port, because "it has a port"
            and "it will answer" are different claims and the second is the one
            a verified cue rests on. */
        declaration.readback = document.getAttribute (base + "readback")
                                 .value_or (std::string ("none"));

        if (const auto queryPort = document.getAttribute (base + "queryPort"))
            if (const auto parsed = osc::parseDouble (*queryPort))
                declaration.queryPort = static_cast<int> (*parsed);

        return declaration;
    }

    //==============================================================================
    MountResult loadMountFromBundle (const doc::ShowDocument& document, MountTable& mounts,
                                     const juce::File& bundleFolder, const std::string& mountId)
    {
        const auto declaration = mountDeclarationFor (document, mountId);

        if (! declaration.has_value())
            return MountResult::failed ("no mount " + mountId + " in this show");


        /*  WHERE IT SENDS, CHECKED WHEN THE SHOW OPENS, and refusing the whole
            mount rather than letting it load and fail one cue at a time.

            The reasoning is about which morning somebody finds out. A mount
            with no port loads perfectly well and then every network cue aimed
            at it does nothing, silently, because UDP has no way of telling
            anybody that nobody was listening. Refusing here puts the problem in
            front of whoever opened the file, alongside every other thing wrong
            with the bundle, which is the moment it costs least. */
        const auto refuse = [&mounts, &mountId] (std::string why)
        {
            /*  THE SENTENCE GOES WHERE A CLIENT CAN READ IT, not only to the
                terminal the caller prints on. Until 2026-09-22 these refusals
                were a line at startup and nothing else, so a device that could
                never work looked exactly like one that works - in the page, in
                the window, everywhere - until a cue failed during the show. */
            mounts.setProblem (mountId, why);
            return MountResult::failed (mountId + ": " + why);
        };

        if (declaration->transport != "udp")
            return refuse ("transport \"" + declaration->transport
                           + "\" is declared but not implemented -"
                             " Go.dot speaks udp to a mount today");

        if (declaration->port <= 0 || declaration->port > 65535)
            return refuse ("no usable port. A device has to say which port it listens"
                           " on; nothing can be inferred and UDP will never tell you it"
                           " was wrong");

        /*  A DEVICE THAT DESCRIBES NOTHING IS DECLARED AND NOT READ.

            This used to be the first refusal in this function, and it made the
            description file the price of having a device at all. Most desks
            have no such file and never will (PRD 3.22): what a show knows
            about an X32 is where it is and what to send it. So an empty
            namespace now means an OPAQUE device - see `MountDeclaration::
            opaque` for what that costs - and the checks above still apply,
            because a device with no port is useless whether or not anybody
            described it. */
        if (declaration->opaque())
        {
            mounts.setProblem (mountId, {});
            return mounts.declare (*declaration);
        }

        /*  Bundle-relative, and it has to STAY inside the bundle. A namespace
            path of "../../etc/passwd" is not a threat model Phase 1 has, but a
            show that reads a file from outside its own folder is not a show
            anybody can hand to somebody else and expect to work. */
        const auto file = bundleFolder.getChildFile (juce::String (declaration->namespaceFile));

        if (! file.isAChildOf (bundleFolder))
            return refuse ("\"" + declaration->namespaceFile
                           + "\" points outside the bundle");

        if (! file.existsAsFile())
            return refuse ("no " + declaration->namespaceFile + " in this bundle");

        juce::MemoryBlock bytes;

        if (! file.loadFileAsData (bytes))
            return refuse ("cannot read " + declaration->namespaceFile);

        auto result = mounts.load (*declaration,
                                   std::string_view (static_cast<const char*> (bytes.getData()),
                                                     bytes.getSize()));

        /*  The problems a namespace has are already sentences naming this
            mount; the first is what a one-line readout shows. */
        mounts.setProblem (mountId, result.ok || result.problems.empty()
                                      ? std::string {} : result.problems.front());

        return result;
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
    void refreshMountDeclarations (const doc::ShowDocument& document, MountTable& mounts,
                                   const juce::File& bundleFolder)
    {
        const auto declared = declaredMountIds (document);

        for (const auto& id : declared)
        {
            const auto wanted = mountDeclarationFor (document, id);

            if (! wanted.has_value())
                continue;

            const auto* held = mounts.declarationOf (id);

            /*  NEW, OR MOVED TO ANOTHER ADDRESS SPACE: read it properly. The
                prefix decides every mounted node's address and the namespace
                file decides which nodes there are, so either one changing
                means what is loaded is about a different thing. */
            if (held == nullptr
                  || held->prefix != wanted->prefix
                  || held->namespaceFile != wanted->namespaceFile)
            {
                loadMountFromBundle (document, mounts, bundleFolder, id);
                continue;
            }

            /*  EVERYTHING ELSE KEEPS THE NODES. A retyped port is a different
                destination for the same device, not a different device. */
            if (! (*held == *wanted))
                mounts.updateDeclaration (*wanted);
        }

        /*  AND A DEVICE SOMEBODY DELETED STOPS BEING ONE. Without this its
            nodes would answer for the rest of the session and its prefix would
            go on claiming addresses, so a cue re-aimed at its replacement
            would still be matched to the ghost. */
        for (const auto& id : mounts.ids())
            if (std::find (declared.begin(), declared.end(), id) == declared.end())
                mounts.unload (id);
    }

    //==============================================================================
    void registerMountCommands (CommandRegistry& registry, const doc::ShowDocument& document,
                                MountTable& mounts, const juce::File& bundleFolder)
    {
        /*  WHAT A TARGET SAID, arriving as a command like everything else the
            machine learns (§3.15). Origin `mount:<id>`, applied on the tick it
            reached the tick thread, and in the log - which is what lets a
            verified cue replay on a laptop with no network: the answer is
            re-injected from the record and the comparison comes out the same.

            It is written by MountProbe's thread and by nothing else in
            production. There is no origin check on it, for the same reason
            there is none on `run.started` or `audio.armed` - the namespace
            draft calls for one and no engine-origin command has ever had it, so
            adding it to this one alone would be a rule with one member. */
        registry.add ({ "mount.readback",
                        "What a mounted target said one of its nodes currently holds.",
                        { { "mount", 's', false }, { "address", 's', false },
                          { "value", '*', false }, { "observed", 'T', true } },
                        true,
                        [&mounts] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            /*  ONE RECORD, TWO STORES. A cue waiting on this
                                node consumes a read-back; an observation is
                                what the target says when nobody asked on a
                                cue's behalf, and the two are kept apart so that
                                a periodic sweep cannot make a verification pass
                                by construction (Mount.h says it at length).

                                The flag is trailing and optional, so every log
                                written before observations existed replays as
                                what it was: a verify. */
                            if (args.size() > 3 && args[3].getBool())
                                mounts.noteObservation (args[1].getString(), args[2],
                                                        context.tick);
                            else
                                mounts.noteReadback (args[1].getString(), args[2]);

                            return Outcome::ok (args);
                        } });

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
