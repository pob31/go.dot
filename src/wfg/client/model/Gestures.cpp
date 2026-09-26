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

#include <wfg/client/model/Gestures.h>

#include <wfg/engine/osc/OscValue.h>

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace wfg::client::gesture
{
    namespace
    {
        Event plain (const char* command)
        {
            return { origin::window, command, {} };
        }
    }

    Event go()   { return plain ("go"); }

    Event stopAll() { return plain ("run.stopAll"); }
    Event killAll() { return plain ("run.killAll"); }

    Event standbyNext()     { return plain ("standby.next"); }
    Event standbyPrevious() { return plain ("standby.previous"); }

    Event park (const std::string& cueId)
    {
        return { origin::window, "standby.set", { osc::Value::string (cueId) } };
    }

    /*  NO ARGUMENTS, though both take an optional domain: one domain exists
        (`document`), and naming it would be this client deciding what the
        default is rather than the engine. When Phase 6 adds the parameter
        domain, the window will have somewhere to say which - and until it
        does, saying nothing is the honest way to mean "the one there is". */
    Event undo() { return plain ("undo"); }
    Event redo() { return plain ("redo"); }

    Event save()            { return plain ("document.save"); }
    Event revert()          { return plain ("document.revert"); }
    Event recover()         { return plain ("document.recover"); }
    Event discardRecovery() { return plain ("document.discardRecovery"); }

    Event createCue (const std::string& parent, int index,
                     const std::string& kind, const std::string& name)
    {
        return { origin::window, "cue.create",
                 { osc::Value::string (parent), osc::Value::int32 (index),
                   osc::Value::string (kind), osc::Value::string (name) } };
    }

    Event createSend (const std::string& cueId, const std::string& busId,
                      std::optional<double> level)
    {
        if (! level.has_value())
            return { origin::window, "send.create",
                     { osc::Value::string (cueId), osc::Value::string (busId) } };

        //  The identifier left empty for the engine to draw; the level after it.
        return { origin::window, "send.create",
                 { osc::Value::string (cueId), osc::Value::string (busId), osc::Value::string ({}),
                   osc::Value::string (osc::formatDouble (*level)) } };
    }

    Event createFx (const std::string& cueId, const std::string& pluginId)
    {
        return { origin::window, "fx.create",
                 { osc::Value::string (cueId), osc::Value::string (pluginId) } };
    }

    Event captureFx (const std::string& fxId, const std::string& stateFile, const std::string& values)
    {
        return { origin::window, "fx.capture",
                 { osc::Value::string (fxId), osc::Value::string (stateFile), osc::Value::string (values) } };
    }

    Event createPlugin (const std::string& name, const std::string& identifier,
                        const std::string& format, const std::string& path)
    {
        return { origin::window, "plugin.create",
                 { osc::Value::string (name), osc::Value::string (identifier),
                   osc::Value::string (format), osc::Value::string (path) } };
    }

    Event restartPlugin (const std::string& pluginId)
    {
        return { origin::window, "plugin.restart", { osc::Value::string (pluginId) } };
    }

    Event scanPlugins (const std::string& formatWord, const std::string& folder)
    {
        if (folder.empty())
            return { origin::window, "plugin.scan", { osc::Value::string (formatWord) } };

        return { origin::window, "plugin.scan", { osc::Value::string (formatWord), osc::Value::string (folder) } };
    }

    Event retryScan (const std::string& file)
    {
        return { origin::window, "plugin.scanRetry", { osc::Value::string (file) } };
    }

    Event loadPlugins()
    {
        return { origin::window, "plugin.load", {} };
    }

    Event createRackChannel (const std::string& channelClass)
    {
        return { origin::window, "channel.create", { osc::Value::string (channelClass) } };
    }

    Event createChannelPlugin (const std::string& channelId, const std::string& name,
                               const std::string& identifier, const std::string& format,
                               const std::string& path)
    {
        return { origin::window, "channel.plugin",
                 { osc::Value::string (channelId), osc::Value::string (name),
                   osc::Value::string (identifier), osc::Value::string (format),
                   osc::Value::string (path) } };
    }

    Event eqReset (const std::string& cueId)
    {
        return { origin::window, "eq.reset", { osc::Value::string (cueId) } };
    }

    Event aimSurfaces (const std::string& cueId)
    {
        return { origin::window, "surface.aim", { osc::Value::string (cueId) } };
    }

    Event dial (const std::string& address)
    {
        return { origin::window, "surface.dial", { osc::Value::string (address) } };
    }

    Event keepLive()
    {
        return { origin::window, "live.keep", {} };
    }

    Event dropLive()
    {
        return { origin::window, "live.drop", {} };
    }

    Event splitRange (const std::string& cueId, double at)
    {
        return { origin::window, "range.split",
                 { osc::Value::string (cueId), osc::Value::float64 (at) } };
    }

    Event fireCue (const std::string& cueId)
    {
        return { origin::window, "cue.fire", { osc::Value::string (cueId) } };
    }

    Event createRange (const std::string& cueId, double in, double out)
    {
        return { origin::window, "range.create",
                 { osc::Value::string (cueId), osc::Value::float64 (in),
                   osc::Value::float64 (out) } };
    }

    Event moveObject (const std::string& id, const std::string& parent, int index)
    {
        return { origin::window, "object.move",
                 { osc::Value::string (id), osc::Value::string (parent),
                   osc::Value::int32 (index == -1 ? std::numeric_limits<std::int32_t>::max() : index) } };
    }

    Event saveAs (const std::string& folder)
    {
        return { origin::window, "document.saveAs", { osc::Value::string (folder) } };
    }

    Event copyCues (const std::vector<std::string>& ids)
    {
        std::string joined;

        for (std::size_t at = 0; at < ids.size(); ++at)
            joined += (at == 0 ? "" : " ") + ids[at];

        return { origin::window, "document.copy", { osc::Value::string (joined) } };
    }

    Event pasteCues (const std::string& parent, int index, const std::string& fragment)
    {
        return { origin::window, "document.paste",
                 { osc::Value::string (parent), osc::Value::int32 (index), osc::Value::string (fragment) } };
    }

    Event groupRole (const std::string& group, const std::string& role)
    {
        return { origin::window, "group.role", { osc::Value::string (group), osc::Value::string (role) } };
    }

    Event deleteObject (const std::string& id)
    {
        return { origin::window, "object.delete", { osc::Value::string (id) } };
    }

    Event setNode (const std::string& address, const std::string& text)
    {
        /*  A STRING, WHATEVER THE ROW'S TYPE IS. `node.set` declares its value
            parameter as `*` and the engine coerces to the row's declared tag,
            refusing what will not go - which is the right place for that
            decision: a client that parsed "12" into an int here would be a
            second copy of the type rules, and the one that disagreed. */
        return { origin::window, "node.set",
                 { osc::Value::string (address), osc::Value::string (text) } };
    }

    Event kill (const std::string& runId)
    {
        return { origin::window, "run.kill", { osc::Value::string (runId) } };
    }

    Event seek (const std::string& runId, double seconds)
    {
        return { origin::window, "run.seek",
                 { osc::Value::string (runId), osc::Value::float64 (seconds) } };
    }

    Event aim (const std::string& listId, const std::string& cueId, double offset)
    {
        return { origin::window, "list.aim",
                 { osc::Value::string (listId), osc::Value::string (cueId),
                   osc::Value::float64 (offset) } };
    }

    Event loadToTime (const std::string& listId)
    {
        return { origin::window, "list.loadToTime", { osc::Value::string (listId) } };
    }

    Event recordStart() { return plain ("record.start"); }
    Event recordStop()  { return plain ("record.stop"); }

    Event createDevice (const std::string& prefix)
    {
        /*  The empty second argument is the namespace file, and it is what
            makes this an opaque device. See the header. */
        return { origin::window, "mount.create",
                 { osc::Value::string (prefix), osc::Value::string ({}) } };
    }

    Event createPort (const std::string& name)
    {
        return { origin::window, "port.create", { osc::Value::string (name) } };
    }

    Event createSurface (const std::string& profile, const std::string& name)
    {
        /*  NO NAME IS NO ARGUMENT, rather than an empty one: the engine's
            record carries the name in a fixed position either way, and a
            gesture that said "" would be this window deciding what a surface
            nobody named is called. */
        if (name.empty())
            return { origin::window, "surface.create", { osc::Value::string (profile) } };

        return { origin::window, "surface.create",
                 { osc::Value::string (profile), osc::Value::string (name) } };
    }

    Event createStrip (const std::string& surfaceId)
    {
        return { origin::window, "strip.create", { osc::Value::string (surfaceId) } };
    }

    Event createDca (const std::string& name)
    {
        return { origin::window, "dca.create", { osc::Value::string (name) } };
    }

    Event pressStrip (const std::string& stripId, int velocity)
    {
        /*  A HAND WITH NO VELOCITY SAYS NONE. The argument is optional because
            not every hand has one - a key, a fader lifted from the bottom - and
            a record carrying a number nobody struck would tell a reader of the
            log that a pad was hit that hard. Left out, the engine starts the
            clip where its strip puts it - unity under a pad or a fader at the
            bottom, the fader's own level once it is lifted - as it does for
            any press without one. */
        if (velocity < 1)
            return { origin::window, "strip.press", { osc::Value::string (stripId) } };

        return { origin::window, "strip.press",
                 { osc::Value::string (stripId), osc::Value::int32 (velocity) } };
    }

    Event releaseStrip (const std::string& stripId)
    {
        return { origin::window, "strip.release", { osc::Value::string (stripId) } };
    }

    Event touchNode (const std::string& address)
    {
        return { origin::window, "node.touch", { osc::Value::string (address) } };
    }

    Event releaseNode (const std::string& address)
    {
        return { origin::window, "node.release", { osc::Value::string (address) } };
    }

    Event createBus (const std::string& kind, int width, int index)
    {
        return { origin::window, "bus.create",
                 { osc::Value::string (kind), osc::Value::int32 (width),
                   osc::Value::int32 (index) } };
    }

    Event deleteBus (const std::string& busId)
    {
        return { origin::window, "bus.delete", { osc::Value::string (busId) } };
    }

    Event moveBus (const std::string& busId, int index)
    {
        return { origin::window, "bus.move",
                 { osc::Value::string (busId), osc::Value::int32 (index) } };
    }

    Event setBusWidth (const std::string& busId, int width)
    {
        return { origin::window, "bus.width",
                 { osc::Value::string (busId), osc::Value::int32 (width) } };
    }

    Event createInput (int width, int index)
    {
        return { origin::window, "input.create",
                 { osc::Value::int32 (width), osc::Value::int32 (index) } };
    }

    Event deleteInput (const std::string& inputId)
    {
        return { origin::window, "input.delete", { osc::Value::string (inputId) } };
    }

    Event moveInput (const std::string& inputId, int index)
    {
        return { origin::window, "input.move",
                 { osc::Value::string (inputId), osc::Value::int32 (index) } };
    }

    Event setInputPatchSettled (bool settled)
    {
        return { origin::window, "node.set",
                 { osc::Value::string ("/godot/audio/inputPatchSettled"),
                   osc::Value::boolean (settled) } };
    }

    Event setPatchSettled (bool settled)
    {
        /*  A boolean rather than the word, as `setLocked` explains: the row is
            a `T` and the value carries its own type. */
        return { origin::window, "node.set",
                 { osc::Value::string ("/godot/audio/patchSettled"),
                   osc::Value::boolean (settled) } };
    }

    Event setLocked (bool locked)
    {
        /*  A BOOLEAN, not the word "true": `node.set` takes its value as a
            wildcard and the row is a `T`, so the value carries its own type
            and nothing has to parse a string back into one. The page sends
            the same two OSC tags. */
        return { origin::window, "node.set",
                 { osc::Value::string ("/godot/document/locked"), osc::Value::boolean (locked) } };
    }
}
