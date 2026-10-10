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

#include <wfg/client/model/FxEditor.h>

#include <wfg/client/model/Fx.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/cue/FadeMoves.h>
#include <wfg/engine/cue/FxValues.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cstdlib>

namespace wfg::client::model
{
    namespace
    {
        int integer (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            return static_cast<int> (osc::parseDouble (text (snapshot, address)).value_or (0.0));
        }

        /** "3 Steady", or whichever of the two the cue has, or its identifier. */
        std::string labelOf (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            const auto base = "/godot/cue/" + cueId + "/";
            const auto number = text (snapshot, base + "number");
            const auto name = shownCueName (snapshot, cueId);

            if (! number.empty() && ! name.empty())
                return number + " " + name;

            if (! name.empty())
                return name;

            return number.empty() ? cueId : "cue " + number;
        }

        constexpr const char* dash = " \xe2\x80\x94 ";
    }

    EditorSubject readEditorSubject (const tree::TreeSnapshot& snapshot, const std::string& pickedCueId,
                                     const std::string& pluginId)
    {
        EditorSubject out;
        out.cueId = pickedCueId;

        const auto base = "/godot/plugin/" + pluginId + "/";
        auto name = text (snapshot, base + "name");

        if (name.empty())
            name = pluginId;

        if (pickedCueId.empty())
        {
            out.title = name;
            out.reason = "Nothing is picked: pick an audio or a mic cue to set its " + name + ".";
            return out;
        }

        const auto label = labelOf (snapshot, pickedCueId);
        out.title = name + dash + label;

        /*  ON A FADE (namespace draft §26, PH): the window shows the TARGET'S
            insert, its values with the fade's laid over them, and what a hand
            turns is the fade's to move - routed by an identifier no Fx can
            have, `fade:` and the fade. Never a whole state: a fade keeps
            values. */
        if (text (snapshot, "/godot/cue/" + pickedCueId + "/kind") == "fade")
        {
            const auto fade = "/godot/cue/" + pickedCueId + "/";
            const auto targetId = text (snapshot, fade + "target");

            if (! text (snapshot, fade + "dca").empty() || targetId.empty())
            {
                out.reason = label + " moves no cue's sound: aim it at an audio or a mic cue to move its "
                               + name + ".";
                return out;
            }

            const auto targetLabel = labelOf (snapshot, targetId);
            const auto inserts = insertsOf (snapshot, targetId);
            const auto found = inserts.find (pluginId);
            out.title = name + dash + label + " \xe2\x86\x92 " + targetLabel;

            if (found == inserts.end())
            {
                out.reason = name + " is not on " + targetLabel + ": switch it in on that cue first.";
                return out;
            }

            out.greyed = false;
            out.fxId = fadeEditorId (pickedCueId);
            out.reason = "what you turn here, the fade moves - untick it in the fade's mixer to leave it alone";

            auto stored = cue::parseFxValues (text (snapshot, "/godot/fx/" + found->second.fxId + "/values"));

            for (const auto& [key, value] : cue::parseMoveList (text (snapshot, fade + "fx")))
                if (const auto slash = key.rfind ('/'); slash != std::string::npos && key.substr (0, slash) == pluginId)
                    stored[std::atoi (key.c_str() + slash + 1)] = value;

            auto count = integer (snapshot, base + "paramCount");

            if (! stored.empty())
                count = std::max (count, stored.rbegin()->first + 1);

            out.values.assign (static_cast<std::size_t> (std::max (0, count)), -1.0f);

            for (const auto& [index, value] : stored)
                if (index >= 0 && index < count)
                    out.values[static_cast<std::size_t> (index)] = static_cast<float> (value);

            const auto stateFile = text (snapshot, "/godot/fx/" + found->second.fxId + "/stateFile");
            const auto bundle = text (snapshot, "/godot/document/path");

            if (! stateFile.empty() && ! bundle.empty())
                out.statePath = bundle + "/plugins/" + stateFile;

            return out;
        }

        /*  A MEDIA CUE'S INSERTS, OR A MIC CUE'S (Phase 9b): the helper follows
            the pick onto a mic cue whose channel carries this plugin. */
        if (const auto kind = text (snapshot, "/godot/cue/" + pickedCueId + "/kind");
            kind != "media" && kind != "mic")
        {
            out.reason = label + " plays nothing: inserts belong to media and mic cues.";
            return out;
        }

        const auto inserts = insertsOf (snapshot, pickedCueId);
        const auto found = inserts.find (pluginId);

        if (found == inserts.end())
        {
            out.reason = name + " is not on " + label + ": switch it in from the FX panel.";
            return out;
        }

        out.greyed = false;
        out.fxId = found->second.fxId;
        out.reason = found->second.enabled ? std::string ("in this cue's signal")
                                           : std::string ("switched out on this cue - what you set is kept");

        /*  EVERY PARAMETER, as many as the catalogue says - or as far as the
            cue's own row reaches, on a machine whose catalogue is late - with
            nought-to-one where the cue says and -1 where it rests. */
        const auto stored = cue::parseFxValues (text (snapshot, "/godot/fx/" + out.fxId + "/values"));
        auto count = integer (snapshot, base + "paramCount");

        if (! stored.empty())
            count = std::max (count, stored.rbegin()->first + 1);

        out.values.assign (static_cast<std::size_t> (std::max (0, count)), -1.0f);

        for (const auto& [index, value] : stored)
            if (index >= 0 && index < count)
                out.values[static_cast<std::size_t> (index)] = static_cast<float> (value);

        /*  AND ITS WHOLE STATE, a name under the bundle's plugins/ as the
            row holds it, joined to the open bundle's folder. */
        const auto stateFile = text (snapshot, "/godot/fx/" + out.fxId + "/stateFile");
        const auto bundle = text (snapshot, "/godot/document/path");

        if (! stateFile.empty() && ! bundle.empty())
            out.statePath = bundle + "/plugins/" + stateFile;

        return out;
    }

    std::string fadeEditorId (const std::string& fadeId)
    {
        return "fade:" + fadeId;
    }

    std::string fadeOfEditorId (const std::string& fxId)
    {
        return fxId.rfind ("fade:", 0) == 0 ? fxId.substr (5) : std::string();
    }

    EditorStart readEditorStart (const tree::TreeSnapshot& snapshot, const std::string& pluginId)
    {
        EditorStart out;
        out.pluginId = pluginId;

        const auto base = "/godot/plugin/" + pluginId + "/";
        out.identifier = text (snapshot, base + "identifier");
        out.name = text (snapshot, base + "name");

        if (out.name.empty())
            out.name = pluginId;

        /*  THE PRESET IS A NAME UNDER THE BUNDLE'S plugins/ (§17.7), and the
            helper wants a file: joined to the open bundle's folder. */
        const auto preset = text (snapshot, base + "preset");
        const auto bundle = text (snapshot, "/godot/document/path");

        if (! preset.empty() && ! bundle.empty())
            out.presetPath = bundle + "/plugins/" + preset;

        if (! bundle.empty())
            out.stateFolder = bundle + "/plugins/state";

        if (const auto rate = integer (snapshot, "/godot/audio/actualSampleRate"); rate > 0)
            out.sampleRate = rate;

        if (const auto block = integer (snapshot, "/godot/audio/actualBufferSize"); block > 0)
            out.blockSize = block;

        if (const auto width = integer (snapshot, "/godot/audio/channelsPerTrack"); width > 0)
            out.channels = width;

        return out;
    }
}
