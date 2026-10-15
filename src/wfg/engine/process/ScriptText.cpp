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

#include <wfg/engine/process/ScriptText.h>

#include <juce_core/juce_core.h>

#include <algorithm>

namespace wfg::process
{
    bool isLua (std::string_view language) noexcept
    {
        return language == "lua";
    }

    std::string helpCardName (std::string_view language)
    {
        return isLua (language) ? "godot-help.lua" : "godot-help.js";
    }

    std::string readyMadeFolder (std::string_view language)
    {
        const auto name = juce::String (isLua (language) ? "lua" : "js");
        const auto program = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        for (const auto& folder : { program.getChildFile (name), program.getSiblingFile ("Resources").getChildFile (name) })
            if (folder.isDirectory())
                return folder.getFullPathName().toStdString();
        return {};
    }

    std::vector<std::string> readyMadeFiles (const std::string& folder, std::string_view language)
    {
        std::vector<std::string> names;
        if (folder.empty())
            return names;

        const auto ending = juce::String (isLua (language) ? "*.lua" : "*.js");
        for (const auto& file : juce::File (juce::String::fromUTF8 (folder.c_str())).findChildFiles (juce::File::findFiles, false, ending))
            names.push_back (file.getFileName().toStdString());

        const auto help = helpCardName (language);
        std::sort (names.begin(), names.end(), [&help] (const std::string& a, const std::string& b)
        {
            if ((a == help) != (b == help))
                return a == help;
            return a < b;
        });
        return names;
    }

    std::string starterScript (std::string_view language)
    {
        if (isLua (language))
            return "-- This script runs while its cue runs. Once a tick it is handed what Go.dot\n"
                   "-- hears - a device, a row of the show, MIDI, a serial port - and what it\n"
                   "-- sends goes to a device, a MIDI port or Go.dot itself. The ? beside the\n"
                   "-- editor opens every way in and out.\n"
                   "--\n"
                   "-- godot.hear('/wfs/source/1/x', function (x) godot.send('/wfs/source/2/x', x * 2) end)\n";

        return "// This script runs while its cue runs. Once a tick it is handed what Go.dot\n"
               "// hears - a device, a row of the show, MIDI, a serial port - and what it\n"
               "// sends goes to a device, a MIDI port or Go.dot itself. The ? beside the\n"
               "// editor opens every way in and out.\n"
               "//\n"
               "// godot.hear('/wfs/source/1/x', (x) => godot.send('/wfs/source/2/x', x * 2));\n";
    }

    std::string scriptLineToFireCue (std::string_view language, std::string_view cueId)
    {
        return "godot.fire('" + std::string (cueId) + "')" + (isLua (language) ? "" : ";");
    }

    std::string scriptLinesToHearSerial (std::string_view language, std::string_view portId)
    {
        const auto address = "'/godot/serial/" + std::string (portId) + "/in'";
        if (isLua (language))
            return "godot.hear(" + address + ", function (...)\n    print(...)\nend)";
        return "godot.hear(" + address + ", (...words) => {\n    print(...words);\n});";
    }

    std::string scriptLineToSendSerial (std::string_view language, std::string_view portId)
    {
        return "godot.send('/godot/serial/" + std::string (portId) + "/out', 'led', 1)" + (isLua (language) ? "" : ";");
    }
}
