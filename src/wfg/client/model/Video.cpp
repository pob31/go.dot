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

#include <wfg/client/model/Video.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view canvasPrefix = "/godot/canvas/";
        constexpr std::string_view canvasOrderAddress = "/godot/canvas/order";
        constexpr std::string_view outputPrefix = "/godot/videoOutput/";
        constexpr std::string_view outputOrderAddress = "/godot/videoOutput/order";
        constexpr std::string_view displaysAddress = "/godot/videoOutput/displays";

        std::int64_t integerOf (const tree::Node* node, std::int64_t fallback)
        {
            const auto sole = node != nullptr ? node->soleValue() : std::nullopt;

            if (! sole.has_value())
                return fallback;

            if (sole->isInt32())
                return sole->getInt32();

            if (sole->isInt64())
                return sole->getInt64();

            return fallback;
        }

        bool truthOf (const tree::Node* node, bool fallback)
        {
            const auto sole = node != nullptr ? node->soleValue() : std::nullopt;
            return sole.has_value() && sole->isBool() ? sole->getBool() : fallback;
        }

        /*  The identifier in the middle and the row's own name after it; false
            for the container's own rows and anything a level deeper - the
            reading `Surfaces.cpp` does for the DCAs. */
        bool splitAddress (const std::string& address, std::string_view prefix,
                           std::string& id, std::string& name)
        {
            if (address.rfind (prefix, 0) != 0)
                return false;

            const auto rest = address.substr (prefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos)
                return false;

            name = rest.substr (slash + 1);

            if (name.find ('/') != std::string::npos)
                return false;

            id = rest.substr (0, slash);
            return true;
        }

        /*  In the order the show declares them, and anything the order does
            not name last rather than gone: a row nobody can see is a row
            nobody can delete. */
        template <typename Row>
        std::vector<Row> inOrder (std::map<std::string, Row>& found, const std::vector<std::string>& order)
        {
            std::vector<Row> rows;
            rows.reserve (found.size());

            for (const auto& id : order)
            {
                const auto at = found.find (id);

                if (at == found.end())
                    continue;

                rows.push_back (std::move (at->second));
                found.erase (at);
            }

            for (auto& entry : found)
                rows.push_back (std::move (entry.second));

            return rows;
        }
    }

    //==============================================================================
    std::string CanvasRow::label() const
    {
        return name.empty() ? id : name;
    }

    std::string CanvasRow::sizeWord() const
    {
        return std::to_string (width) + " \xc3\x97 " + std::to_string (height);
    }

    std::string VideoOutputRow::label() const
    {
        return name.empty() ? id : name;
    }

    //==============================================================================
    std::vector<CanvasRow> readCanvases (const tree::TreeSnapshot& snapshot)
    {
        std::map<std::string, CanvasRow> found;
        std::string order;

        for (const auto* node : snapshot.all())
        {
            if (node->address == canvasOrderAddress)
            {
                order = text (node);
                continue;
            }

            std::string id, name;

            if (! splitAddress (node->address, canvasPrefix, id, name))
                continue;

            auto& row = found[id];
            row.id = id;

            if (name == "name")        row.name = text (node);
            else if (name == "width")  row.width = static_cast<int> (integerOf (node, 1920));
            else if (name == "height") row.height = static_cast<int> (integerOf (node, 1080));
        }

        return inOrder (found, words (order));
    }

    std::vector<VideoOutputRow> readVideoOutputs (const tree::TreeSnapshot& snapshot)
    {
        std::map<std::string, VideoOutputRow> found;
        std::string order;

        for (const auto* node : snapshot.all())
        {
            if (node->address == outputOrderAddress)
            {
                order = text (node);
                continue;
            }

            std::string id, name;

            if (! splitAddress (node->address, outputPrefix, id, name))
                continue;

            auto& row = found[id];
            row.id = id;

            if (name == "name")                 row.name = text (node);
            else if (name == "canvas")          row.canvas = text (node);
            else if (name == "display")         row.display = text (node);
            else if (name == "enabled")         row.enabled = truthOf (node, true);
            else if (name == "bound")           row.bound = truthOf (node, false);
            else if (name == "problem")         row.problem = text (node);
            else if (name == "testPattern")     row.testPattern = truthOf (node, false);
            else if (name == "framesPresented") row.framesPresented = integerOf (node, 0);
            else if (name == "framesLate")      row.framesLate = integerOf (node, 0);
        }

        return inOrder (found, words (order));
    }

    std::vector<std::string> readDisplays (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PER LINE, as the MIDI ports are listed: a display's name has
            spaces in it, and a newline is the one thing it cannot hold. */
        std::vector<std::string> out;
        const auto all = text (snapshot, displaysAddress);

        std::size_t at = 0;

        while (at < all.size())
        {
            const auto end = all.find ('\n', at);
            const auto line = all.substr (at, end == std::string::npos ? std::string::npos : end - at);

            if (! line.empty())
                out.push_back (line);

            if (end == std::string::npos)
                break;

            at = end + 1;
        }

        return out;
    }

    bool isPictureFile (const std::string& name)
    {
        const auto dot = name.find_last_of ('.');

        if (dot == std::string::npos)
            return false;

        auto extension = name.substr (dot + 1);

        for (auto& c : extension)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char> (c - 'A' + 'a');

        return extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "gif";
    }

    bool isMovieFile (const std::string& name)
    {
        const auto dot = name.find_last_of ('.');

        if (dot == std::string::npos)
            return false;

        auto extension = name.substr (dot + 1);

        for (auto& c : extension)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char> (c - 'A' + 'a');

        for (const auto* known : { "mov", "mp4", "m4v", "mkv", "avi", "mxf", "webm", "mpg", "mpeg" })
            if (extension == known)
                return true;

        return false;
    }

    std::vector<ConversionRow> readConversions (const tree::TreeSnapshot& snapshot)
    {
        std::vector<ConversionRow> out;

        const auto all = text (snapshot, "/godot/videoOutput/conversions");
        std::vector<std::string> lines;

        for (std::size_t at = 0; at < all.size();)
        {
            const auto end = all.find ('\n', at);
            lines.push_back (all.substr (at, end == std::string::npos ? std::string::npos : end - at));

            if (end == std::string::npos)
                break;

            at = end + 1;
        }

        for (const auto& line : lines)
        {
            std::vector<std::string> fields;
            std::string::size_type from = 0;

            for (;;)
            {
                const auto tab = line.find ('\t', from);
                fields.push_back (line.substr (from, tab == std::string::npos ? std::string::npos : tab - from));

                if (tab == std::string::npos)
                    break;

                from = tab + 1;
            }

            if (fields.size() < 2 || fields[0].empty())
                continue;

            ConversionRow row;
            row.file = fields[0];
            row.state = fields[1];
            row.percent = fields.size() > 2 ? static_cast<int> (osc::parseDouble (fields[2]).value_or (0.0)) : 0;
            row.problem = fields.size() > 3 ? fields[3] : std::string {};
            out.push_back (row);
        }

        return out;
    }

    std::string ffmpegPath (const tree::TreeSnapshot& snapshot)
    {
        return text (snapshot, "/godot/videoOutput/ffmpeg");
    }

    FfmpegInstallRow readFfmpegInstall (const tree::TreeSnapshot& snapshot)
    {
        const auto all = text (snapshot, "/godot/videoOutput/ffmpegInstall");
        std::vector<std::string> fields;
        std::string::size_type from = 0;

        while (! all.empty())
        {
            const auto tab = all.find ('\t', from);
            fields.push_back (all.substr (from, tab == std::string::npos ? std::string::npos : tab - from));

            if (tab == std::string::npos)
                break;

            from = tab + 1;
        }

        FfmpegInstallRow row;

        if (! fields.empty())
            row.state = fields[0];

        if (fields.size() > 1)
            row.percent = static_cast<int> (osc::parseDouble (fields[1]).value_or (0.0));

        if (fields.size() > 2)
            row.problem = fields[2];

        if (fields.size() > 3)
            row.source = fields[3];

        return row;
    }

    std::string installNews (const FfmpegInstallRow& before, const FfmpegInstallRow& now)
    {
        if (now.state == before.state && (now.state != "downloading" || now.percent / 10 == before.percent / 10))
            return {};

        if (now.state == "downloading")
            return "Downloading FFmpeg" + (now.source.empty() ? std::string {} : " from " + now.source)
                 + ": " + std::to_string (now.percent) + " %.";

        if (now.state == "unpacking" || now.state == "checking")
            return "FFmpeg is downloaded, and being made ready.";

        if (now.state == "done")
            return "FFmpeg is ready: movies can be converted to HAP and previewed.";

        if (now.state == "failed")
            return "FFmpeg could not be downloaded" + (now.problem.empty() ? std::string (".") : ": " + now.problem + ".");

        return {};
    }

    std::string conversionNews (const ConversionRow* before, const ConversionRow& now)
    {
        const auto named = now.file;

        if (before != nullptr && before->state == now.state
              && (now.state != "converting" || before->percent / 10 == now.percent / 10))
            return {};

        if (now.state == "waiting")
            return named + " will be converted to HAP after the one before it.";

        if (now.state == "converting")
            return "Converting " + named + " to HAP: " + std::to_string (now.percent) + " %.";

        if (now.state == "done")
            return named + " is converted to HAP, and its cues play the HAP file.";

        if (now.state == "failed")
            return named + " could not be converted to HAP" + (now.problem.empty() ? std::string (".") : ": " + now.problem + ".");

        if (now.state == "cancelled")
            return "The conversion of " + named + " to HAP was stopped.";

        return {};
    }

    const char* pictureWildcard()
    {
        return "*.png;*.jpg;*.jpeg;*.gif;*.mov;*.mp4;*.m4v;*.mkv;*.avi;*.mxf;*.webm;*.mpg;*.mpeg";
    }

    std::vector<std::pair<std::string, std::string>> canvasChoices (const std::vector<CanvasRow>& canvases)
    {
        std::vector<std::pair<std::string, std::string>> choices;
        choices.reserve (canvases.size() + 1);

        /*  EMPTY IS A CHOICE, as for a DCA: a cue on no canvas shows nothing,
            and it has to be possible to go back to that. */
        choices.push_back ({ "", "(none)" });

        for (const auto& canvas : canvases)
            choices.push_back ({ canvas.id, canvas.label() + " \xc2\xb7 " + canvas.sizeWord() });

        return choices;
    }
}
