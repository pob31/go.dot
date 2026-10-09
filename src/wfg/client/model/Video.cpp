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
#include <wfg/engine/video/SendNames.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/video/Mapping.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
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
        constexpr std::string_view inputPrefix = "/godot/videoInput/";
        constexpr std::string_view inputOrderAddress = "/godot/videoInput/order";
        constexpr std::string_view offeredAddress = "/godot/videoInput/available";
        constexpr std::string_view insertPrefix = "/godot/videoInsert/";
        constexpr std::string_view insertOrderAddress = "/godot/videoInsert/order";

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

        double numberOf (const tree::Node* node, double fallback)
        {
            const auto value = osc::parseDouble (text (node));
            return value.has_value() ? *value : fallback;
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

    std::string CanvasRow::levelWord() const
    {
        //  To the tenth, built from integers so no locale's comma gets in.
        const auto tenths = static_cast<long> (level * 10.0 + 0.5);
        auto out = std::to_string (tenths / 10);

        if (tenths % 10 != 0)
            out += "." + std::to_string (tenths % 10);

        return out + " %";
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
            else if (name == "level")  row.level = numberOf (node, 100.0);
            else if (name == "dca")    row.dca = text (node);
        }

        return inOrder (found, words (order));
    }

    //==============================================================================
    WarpPoints WarpPoints::whole (int columns, int rows)
    {
        WarpPoints out;
        out.columns = std::max (2, columns);
        out.rows = std::max (2, rows);

        for (int row = 0; row < out.rows; ++row)
            for (int column = 0; column < out.columns; ++column)
            {
                out.x.push_back (static_cast<double> (column) / (out.columns - 1));
                out.y.push_back (static_cast<double> (row) / (out.rows - 1));
            }

        return out;
    }

    bool WarpPoints::isWhole() const
    {
        const auto plain = whole (columns, rows);

        for (std::size_t n = 0; n < x.size() && n < plain.x.size(); ++n)
            if (std::abs (x[n] - plain.x[n]) > 1.0e-6 || std::abs (y[n] - plain.y[n]) > 1.0e-6)
                return false;

        return x.size() == plain.x.size();
    }

    WarpPoints readWarp (const tree::TreeSnapshot& snapshot, const std::string& base)
    {
        const auto whole = [&snapshot, &base] (const char* row, int fallback)
        {
            const auto value = osc::parseDouble (text (snapshot, base + row));
            return value.has_value() ? std::clamp (static_cast<int> (*value), 2, 16) : fallback;
        };

        auto out = WarpPoints::whole (whole ("meshColumns", 2), whole ("meshRows", 2));
        //  A list row: its values, not one text (the tree publishes `d*` as numbers).
        std::vector<double> numbers;

        if (const auto* node = snapshot.find (base + "mesh"))
            for (const auto& value : node->values)
                if (value.isNumber() && ! value.isNonFinite())
                    numbers.push_back (value.asDouble());

        if (numbers.size() != 2 * out.x.size())
            return out;

        for (std::size_t n = 0; n < out.x.size(); ++n)
        {
            out.x[n] = numbers[2 * n];
            out.y[n] = numbers[2 * n + 1];
        }

        return out;
    }

    std::string warpText (const WarpPoints& warp)
    {
        std::string out;

        for (std::size_t n = 0; n < warp.x.size() && n < warp.y.size(); ++n)
        {
            if (! out.empty())
                out += ' ';

            out += osc::formatDouble (std::round (warp.x[n] * 10000.0) / 10000.0);
            out += ' ';
            out += osc::formatDouble (std::round (warp.y[n] * 10000.0) / 10000.0);
        }

        return out;
    }

    WarpPoints regridded (const WarpPoints& warp, int columns, int rows)
    {
        auto out = WarpPoints::whole (columns, rows);

        video::Mesh mesh;
        mesh.columns = warp.columns;
        mesh.rows = warp.rows;

        for (std::size_t n = 0; n < warp.x.size(); ++n)
        {
            mesh.x.push_back (static_cast<float> (warp.x[n]));
            mesh.y.push_back (static_cast<float> (warp.y[n]));
        }

        if (! mesh.isValid())
            return out;

        for (int row = 0; row < out.rows; ++row)
            for (int column = 0; column < out.columns; ++column)
            {
                double x = 0.0, y = 0.0;
                video::meshAt (mesh, static_cast<double> (column) / (out.columns - 1),
                               static_cast<double> (row) / (out.rows - 1), x, y);
                out.x[out.index (column, row)] = x;
                out.y[out.index (column, row)] = y;
            }

        return out;
    }

    std::vector<WarpTarget> warpTargets (const tree::TreeSnapshot& snapshot)
    {
        const auto canvases = readCanvases (snapshot);
        const auto canvasName = [&canvases] (const std::string& id)
        {
            for (const auto& canvas : canvases)
                if (canvas.id == id)
                    return canvas.label();

            return id.empty() ? std::string ("no canvas") : id;
        };

        std::vector<WarpTarget> out;

        for (const auto& output : readVideoOutputs (snapshot))
        {
            const auto base = "/godot/videoOutput/" + output.id + "/";
            out.push_back ({ base, output.id, output.label(), canvasName (output.canvas) + " - the output's own" });

            auto number = 1;

            for (const auto& zone : readZones (snapshot, output.id))
                out.push_back ({ "/godot/zone/" + zone.id + "/", output.id, output.label(),
                                 "Zone " + std::to_string (number++) + ": "
                                   + (zone.name.empty() ? canvasName (zone.canvas) : zone.name) });
        }

        return out;
    }

    std::vector<std::pair<std::string, std::string>> warpCopyWrites (const WarpPoints& warp, const std::string& targetBase)
    {
        return { { targetBase + "meshColumns", std::to_string (warp.columns) },
                 { targetBase + "meshRows", std::to_string (warp.rows) },
                 { targetBase + "mesh", warpText (warp) } };
    }

    std::vector<ZoneRow> readZones (const tree::TreeSnapshot& snapshot, const std::string& outputId)
    {
        std::vector<ZoneRow> out;

        for (const auto& id : words (text (snapshot, std::string (outputPrefix) + outputId + "/zones")))
        {
            const auto base = "/godot/zone/" + id + "/";
            ZoneRow row;
            row.id = id;
            row.name = text (snapshot, base + "name");
            row.canvas = text (snapshot, base + "canvas");
            row.blend = text (snapshot, base + "blend");
            row.opacity = osc::parseDouble (text (snapshot, base + "opacity")).value_or (100.0);
            row.warp = readWarp (snapshot, base);

            if (row.blend.empty())
                row.blend = "normal";

            out.push_back (std::move (row));
        }

        return out;
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
            else if (name == "zones")           row.zones = static_cast<int> (words (text (node)).size());
            else if (name == "kind")            row.kind = text (node).empty() ? std::string ("display") : text (node);
            else if (name == "sendName")        row.sendName = text (node);
            else if (name == "frameRate")       row.frameRate = numberOf (node, 60.0);
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

    std::string VideoInputRow::label() const
    {
        return name.empty() ? id : name;
    }

    std::string VideoInsertRow::label() const
    {
        return name.empty() ? id : name;
    }

    std::vector<VideoInputRow> readVideoInputs (const tree::TreeSnapshot& snapshot)
    {
        std::map<std::string, VideoInputRow> found;
        std::string order;

        for (const auto* node : snapshot.all())
        {
            if (node->address == inputOrderAddress)
            {
                order = text (node);
                continue;
            }

            std::string id, name;

            if (! splitAddress (node->address, inputPrefix, id, name))
                continue;

            auto& row = found[id];
            row.id = id;

            if (name == "name")            row.name = text (node);
            else if (name == "kind")       row.kind = text (node).empty() ? std::string ("ndi") : text (node);
            else if (name == "sender")     row.sender = text (node);
            else if (name == "enabled")    row.enabled = truthOf (node, true);
            else if (name == "connected")  row.connected = truthOf (node, false);
            else if (name == "width")      row.width = static_cast<int> (integerOf (node, 0));
            else if (name == "height")     row.height = static_cast<int> (integerOf (node, 0));
            else if (name == "frameRate")  row.frameRate = numberOf (node, 0.0);
            else if (name == "problem")    row.problem = text (node);
        }

        return inOrder (found, words (order));
    }

    std::vector<VideoInsertRow> readVideoInserts (const tree::TreeSnapshot& snapshot)
    {
        std::map<std::string, VideoInsertRow> found;
        std::string order;

        for (const auto* node : snapshot.all())
        {
            if (node->address == insertOrderAddress)
            {
                order = text (node);
                continue;
            }

            std::string id, name;

            if (! splitAddress (node->address, insertPrefix, id, name))
                continue;

            auto& row = found[id];
            row.id = id;

            if (name == "name")               row.name = text (node);
            else if (name == "kind")          row.kind = text (node).empty() ? std::string ("spout") : text (node);
            else if (name == "sendName")      row.sendName = text (node);
            else if (name == "returnSender")  row.returnSender = text (node);
            else if (name == "connected")     row.connected = truthOf (node, false);
            else if (name == "frameRate")     row.frameRate = numberOf (node, 0.0);
            else if (name == "returnAge")     row.returnAge = numberOf (node, 0.0);
            else if (name == "problem")       row.problem = text (node);
        }

        return inOrder (found, words (order));
    }

    std::vector<OfferedSender> readOfferedSenders (const tree::TreeSnapshot& snapshot)
    {
        //  The kind, a tab, the name, a line each.
        std::vector<OfferedSender> out;
        const auto all = text (snapshot, offeredAddress);
        std::size_t at = 0;

        while (at < all.size())
        {
            const auto end = all.find ('\n', at);
            const auto line = all.substr (at, end == std::string::npos ? std::string::npos : end - at);
            const auto tab = line.find ('\t');

            if (tab != std::string::npos && tab + 1 < line.size())
                out.push_back ({ line.substr (0, tab), line.substr (tab + 1) });

            if (end == std::string::npos)
                break;

            at = end + 1;
        }

        return out;
    }

    std::vector<std::string> pictureKindsHere()
    {
       #if defined (_WIN32)
        return { "spout", "ndi" };
       #elif defined (__APPLE__)
        return { "syphon", "ndi" };
       #else
        return { "ndi" };
       #endif
    }

    std::string pictureKindWord (const std::string& kind)
    {
        if (kind == "ndi")     return "NDI";
        if (kind == "spout")   return "Spout";
        if (kind == "syphon")  return "Syphon";
        return "a display";
    }

    std::vector<std::pair<std::string, std::string>> videoInputChoices (const std::vector<VideoInputRow>& inputs)
    {
        std::vector<std::pair<std::string, std::string>> choices { { "", "(none)" } };

        for (const auto& input : inputs)
            choices.push_back ({ input.id, input.label() + " \xc2\xb7 " + pictureKindWord (input.kind) });

        return choices;
    }

    std::vector<std::pair<std::string, std::string>> videoInsertChoices (const std::vector<VideoInsertRow>& inserts)
    {
        std::vector<std::pair<std::string, std::string>> choices { { "", "(none)" } };

        for (const auto& insert : inserts)
            choices.push_back ({ insert.id, insert.label() + " \xc2\xb7 " + pictureKindWord (insert.kind) });

        return choices;
    }

    NewInsert newInsertOf (const std::vector<VideoInsertRow>& inserts, const std::string& kind)
    {
        NewInsert made;
        made.kind = kind;

        for (auto number = 1; ; ++number)
        {
            const auto name = pictureKindWord (kind) + " insert " + std::to_string (number);
            const auto taken = std::any_of (inserts.begin(), inserts.end(),
                                            [&name] (const VideoInsertRow& row) { return row.name == name; });

            if (! taken)
            {
                made.name = name;
                break;
            }
        }

        made.sendName = video::newInsertSendName (made.name);
        made.returnSender = video::newInsertReturnName (made.name);
        return made;
    }

    std::string sentAs (const VideoInsertRow& insert)
    {
        return video::insertSendName (insert.sendName, insert.name, insert.id);
    }

    std::string sentAs (const VideoOutputRow& output)
    {
        return video::outputSendName (output.sendName, output.name, output.id);
    }

    std::vector<std::string> ownSendNames (const std::vector<VideoOutputRow>& outputs,
                                           const std::vector<VideoInsertRow>& inserts)
    {
        std::vector<std::string> names;

        for (const auto& output : outputs)
            if (output.sends())
                names.push_back (sentAs (output));

        for (const auto& insert : inserts)
            names.push_back (sentAs (insert));

        return names;
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
