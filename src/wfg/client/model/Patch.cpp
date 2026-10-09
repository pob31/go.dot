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

#include <wfg/client/model/Patch.h>
#include <wfg/client/model/Text.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <string_view>
#include <system_error>

namespace wfg::client::model
{
    using process::BoxKind;

    namespace
    {
        //  Pd's s_main.c, sys_fontspec: point size, character width, line height.
        constexpr PdFont pdFonts[] = { { 8, 5.0, 11.0 }, { 10, 6.0, 13.0 }, { 12, 7.0, 16.0 },
                                       { 16, 10.0, 19.0 }, { 24, 14.0, 29.0 }, { 36, 22.0, 44.0 } };

        //  Pd's g_rtext.c margins, and the line width a box wraps at by default.
        constexpr double horizontalMargins = 4.0;   // LMARGIN + RMARGIN
        constexpr double verticalMargins = 5.0;     // TMARGIN + BMARGIN
        constexpr std::size_t defaultColumns = 60;

        int intOf (const std::string& word, int otherwise = 0)
        {
            int value = 0;
            const auto [end, error] = std::from_chars (word.data(), word.data() + word.size(), value);
            (void) end;
            return error == std::errc() ? value : otherwise;
        }

        /*  Characters, not bytes: a UTF-8 continuation byte starts nothing. */
        std::size_t columnsOf (std::string_view text)
        {
            return static_cast<std::size_t> (std::count_if (text.begin(), text.end(),
                                                            [] (char c) { return (static_cast<unsigned char> (c) & 0xC0) != 0x80; }));
        }

        /*  Words wrapped into lines of at most `columns` characters, as Pd breaks
            a box's text - at a space, a word longer than a line broken where it
            must be. */
        std::vector<std::string> wrapped (const std::vector<std::string>& words, std::size_t columns)
        {
            std::vector<std::string> lines;
            std::string line;

            for (const auto& word : words)
            {
                const auto shown = process::unescaped (word);
                const auto joined = line.empty() ? shown : line + (shown == "," || shown == ";" ? "" : " ") + shown;

                if (! line.empty() && columnsOf (joined) > columns)
                {
                    lines.push_back (line);
                    line = shown;
                }
                else
                {
                    line = joined;
                }

                //  A semicolon in a message box ends Pd's line there.
                if (shown == ";")
                {
                    lines.push_back (line);
                    line.clear();
                }
            }

            if (! line.empty() || lines.empty())
                lines.push_back (line);

            return lines;
        }

        struct Ports { int inlets = 1; int outlets = 1; };

        /*  HOW MANY INLETS AND OUTLETS a vanilla object has, by its name and its
            arguments - the common ones; nullopt for one this table does not know. */
        std::optional<Ports> knownPorts (const std::string& name, const std::vector<std::string>& args)
        {
            const auto n = static_cast<int> (args.size());
            const auto count = [n] (int least) { return std::max (n, least); };

            static const std::map<std::string, Ports> fixed {
                { "inlet", { 0, 1 } }, { "inlet~", { 0, 1 } }, { "outlet", { 1, 0 } }, { "outlet~", { 1, 0 } },
                { "loadbang", { 0, 1 } }, { "bang", { 1, 1 } }, { "b", { 1, 1 } },
                { "float", { 2, 1 } }, { "f", { 2, 1 } }, { "int", { 2, 1 } }, { "i", { 2, 1 } },
                { "symbol", { 2, 1 } }, { "value", { 1, 1 } }, { "v", { 1, 1 } },
                { "+", { 2, 1 } }, { "-", { 2, 1 } }, { "*", { 2, 1 } }, { "/", { 2, 1 } },
                { "pow", { 2, 1 } }, { "max", { 2, 1 } }, { "min", { 2, 1 } }, { "mod", { 2, 1 } },
                { "div", { 2, 1 } }, { "%", { 2, 1 } }, { "==", { 2, 1 } }, { "!=", { 2, 1 } },
                { ">", { 2, 1 } }, { "<", { 2, 1 } }, { ">=", { 2, 1 } }, { "<=", { 2, 1 } },
                { "&&", { 2, 1 } }, { "||", { 2, 1 } }, { "&", { 2, 1 } }, { "|", { 2, 1 } },
                { "<<", { 2, 1 } }, { ">>", { 2, 1 } }, { "atan2", { 2, 1 } },
                { "abs", { 1, 1 } }, { "sqrt", { 1, 1 } }, { "exp", { 1, 1 } }, { "log", { 2, 1 } },
                { "sin", { 1, 1 } }, { "cos", { 1, 1 } }, { "tan", { 1, 1 } }, { "atan", { 1, 1 } },
                { "wrap", { 1, 1 } }, { "mtof", { 1, 1 } }, { "ftom", { 1, 1 } }, { "dbtorms", { 1, 1 } },
                { "rmstodb", { 1, 1 } }, { "powtodb", { 1, 1 } }, { "dbtopow", { 1, 1 } },
                { "clip", { 3, 1 } }, { "moses", { 2, 2 } }, { "swap", { 2, 2 } }, { "spigot", { 2, 1 } },
                { "change", { 1, 1 } }, { "print", { 1, 0 } }, { "until", { 2, 1 } }, { "random", { 2, 1 } },
                { "metro", { 2, 1 } }, { "delay", { 2, 1 } }, { "del", { 2, 1 } }, { "timer", { 2, 1 } },
                { "pipe", { 2, 1 } }, { "line", { 3, 1 } }, { "line~", { 2, 1 } }, { "vline~", { 3, 1 } },
                { "snapshot~", { 1, 1 } }, { "sig~", { 1, 1 } },
                { "notein", { 0, 3 } }, { "ctlin", { 0, 3 } }, { "pgmin", { 0, 2 } }, { "bendin", { 0, 2 } },
                { "touchin", { 0, 2 } }, { "polytouchin", { 0, 3 } }, { "midiin", { 0, 2 } },
                { "noteout", { 3, 0 } }, { "ctlout", { 3, 0 } }, { "pgmout", { 2, 0 } }, { "bendout", { 2, 0 } },
                { "touchout", { 2, 0 } }, { "polytouchout", { 3, 0 } }, { "midiout", { 2, 0 } },
                { "makenote", { 3, 2 } }, { "stripnote", { 2, 2 } },
                { "netsend", { 1, 2 } }, { "netreceive", { 1, 2 } }, { "oscformat", { 1, 1 } }, { "oscparse", { 1, 1 } },
                { "list", { 2, 1 } }, { "float", { 2, 1 } }, { "tgl", { 1, 1 } }, { "toggle", { 1, 1 } },
                { "bng", { 1, 1 } }, { "nbx", { 1, 1 } }, { "hsl", { 1, 1 } }, { "vsl", { 1, 1 } },
                { "hslider", { 1, 1 } }, { "vslider", { 1, 1 } }, { "hradio", { 1, 1 } }, { "vradio", { 1, 1 } },
                { "cnv", { 1, 0 } }, { "vu", { 2, 2 } },
            };

            if (name == "r" || name == "receive")  return Ports { n > 0 ? 0 : 1, 1 };
            if (name == "s" || name == "send")     return Ports { n > 0 ? 1 : 2, 0 };
            if (name == "t" || name == "trigger")  return Ports { 1, n > 0 ? n : 2 };
            if (name == "route" || name == "select" || name == "sel")
                return Ports { n == 1 ? 2 : 1, count (1) + 1 };
            if (name == "pack")                    return Ports { count (2), 1 };
            if (name == "unpack")                  return Ports { 1, count (2) };

            if (name == "expr" || name == "expr~" || name == "fexpr~")
            {
                //  An inlet for each $f1.. it reads, an outlet for each expression.
                int inlets = 1;
                int outlets = 1;
                for (const auto& arg : args)
                {
                    const auto shown = process::unescaped (arg);
                    if (shown == ";")
                        ++outlets;
                    for (std::size_t at = shown.find ('$'); at != std::string::npos; at = shown.find ('$', at + 1))
                        if (at + 2 < shown.size() + 1 && at + 1 < shown.size())
                            inlets = std::max (inlets, intOf (shown.substr (at + 2), 1));
                }
                return Ports { inlets, outlets };
            }

            if (const auto found = fixed.find (name); found != fixed.end())
                return found->second;

            return std::nullopt;
        }

        /*  A GUI box's place on the canvas: its size from its arguments, as Pd
            draws it - a toggle a square, a slider its length by its breadth. */
        std::optional<std::pair<double, double>> guiSize (const std::string& name, const std::vector<std::string>& args,
                                                          const PdFont& font)
        {
            const auto arg = [&args] (std::size_t i, int otherwise)
            {
                return i < args.size() ? intOf (process::unescaped (args[i]), otherwise) : otherwise;
            };

            if (name == "tgl" || name == "toggle" || name == "bng" || name == "bang")
            {
                if (name == "bang" && args.empty())
                    return std::nullopt;     // [bang] the object, not the button
                const auto size = static_cast<double> (std::max (8, arg (0, 15)));
                return std::pair { size, size };
            }
            if (name == "hsl" || name == "hslider" || name == "vsl" || name == "vslider")
            {
                const bool horizontal = name[0] == 'h';
                return std::pair { static_cast<double> (std::max (8, arg (0, horizontal ? 128 : 15))),
                                   static_cast<double> (std::max (8, arg (1, horizontal ? 15 : 128))) };
            }
            if (name == "hradio" || name == "vradio" || name == "hdl" || name == "vdl")
            {
                const auto size = static_cast<double> (std::max (8, arg (0, 15)));
                const auto cells = static_cast<double> (std::max (1, arg (3, 8)));
                return name[0] == 'h' ? std::pair { size * cells, size } : std::pair { size, size * cells };
            }
            if (name == "nbx")
            {
                const auto digits = static_cast<double> (std::max (1, arg (0, 5)));
                return std::pair { digits * font.charWidth + font.lineHeight / 2.0 + 4.0,
                                   static_cast<double> (std::max (8, arg (1, 14))) };
            }
            if (name == "vu")
                return std::pair { static_cast<double> (arg (0, 15)), static_cast<double> (arg (1, 120)) };
            if (name == "cnv" || name == "my_canvas")
                return std::pair { static_cast<double> (std::max (1, arg (1, 100))),
                                   static_cast<double> (std::max (1, arg (2, 60))) };

            return std::nullopt;
        }

        /*  Whether a GUI box's send or receive name is set: Pd hides the outlet
            or the inlet it would have had. */
        bool named (const std::vector<std::string>& args, std::size_t at)
        {
            return at < args.size() && process::unescaped (args[at]) != "empty";
        }
    }

    PatchReading readPatchFoot (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        PatchReading out;
        const auto base = "/godot/cue/" + cueId + "/";

        if (cueId.empty() || text (snapshot, base + "kind") != "process")
            return out;

        out.cueId = cueId;
        out.text = text (snapshot, base + "patch");
        out.locked = isYes (flag (snapshot, "/godot/document/locked"));
        out.editor = text (snapshot, "/godot/engine/patchEditor");
        out.editorInstall = text (snapshot, "/godot/engine/patchEditorInstall");

        //  Its live run, the newest: a process cue has one at a time.
        for (const auto& run : words (text (snapshot, "/godot/runs/order")))
        {
            const auto at = "/godot/run/" + run + "/";
            const auto state = text (snapshot, at + "state");
            if (text (snapshot, at + "cue") != cueId || state == "done" || state == "failed")
                continue;
            out.runId = run;
            out.state = text (snapshot, at + "processState");
            out.said = text (snapshot, at + "said");
            out.ports = text (snapshot, at + "ports");
        }
        return out;
    }

    PdFont pdFontFor (int size)
    {
        //  Pd's sys_findfont: the last size the asked one reaches.
        std::size_t at = 0;
        while (at + 1 < std::size (pdFonts) && size >= pdFonts[at + 1].size)
            ++at;
        return pdFonts[at];
    }

    const PatchBoxView* PatchView::viewOf (std::size_t box) const
    {
        for (const auto& view : boxes)
            if (view.box == box)
                return &view;
        return nullptr;
    }

    double portX (const PatchBoxView& box, int index, int count)
    {
        //  Pd's onset: (width - IOWIDTH) * index / (count - 1), the first at the left edge.
        if (count <= 1)
            return box.x;
        return box.x + (box.w - portWidth) * static_cast<double> (index) / static_cast<double> (count - 1);
    }

    PatchView viewPatch (const process::Patch& patch, std::size_t canvas)
    {
        PatchView view;
        view.problem = patch.problem;
        view.canvas = canvas;

        //  The font is the patch's own: the last word of its first #N canvas.
        if (! patch.canvases.empty())
        {
            const auto& words = patch.records[patch.canvases.front().record].words;
            view.font = pdFontFor (words.size() >= 7 ? intOf (words[6], 12) : 12);
        }

        const auto onCanvas = patch.boxesOn (canvas);

        //  Each box's ports, first from what its lines use.
        std::vector<Ports> used (onCanvas.size(), Ports { 0, 0 });
        for (const auto& line : patch.lines)
        {
            if (line.canvas != canvas)
                continue;
            if (line.fromBox >= 0 && static_cast<std::size_t> (line.fromBox) < used.size())
                used[static_cast<std::size_t> (line.fromBox)].outlets
                    = std::max (used[static_cast<std::size_t> (line.fromBox)].outlets, line.outlet + 1);
            if (line.toBox >= 0 && static_cast<std::size_t> (line.toBox) < used.size())
                used[static_cast<std::size_t> (line.toBox)].inlets
                    = std::max (used[static_cast<std::size_t> (line.toBox)].inlets, line.inlet + 1);
        }

        for (std::size_t n = 0; n < onCanvas.size(); ++n)
        {
            const auto& box = patch.boxes[onCanvas[n]];
            PatchBoxView shown;
            shown.box = onCanvas[n];
            shown.kind = box.kind;
            shown.x = static_cast<double> (box.x);
            shown.y = static_cast<double> (box.y);

            const auto words = process::splitWords (box.text);
            const auto name = words.empty() ? std::string {} : process::unescaped (words[0]);
            const std::vector<std::string> args (words.empty() ? words.begin() : words.begin() + 1, words.end());

            Ports ports { 1, 1 };
            std::optional<std::pair<double, double>> gui;

            switch (box.kind)
            {
                case BoxKind::object:
                    gui = guiSize (name, args, view.font);
                    ports = knownPorts (name, args).value_or (Ports { 1, 1 });
                    if (gui.has_value())
                    {
                        //  A GUI box hides the inlet its receive replaces and the
                        //  outlet its send does (send and receive at Pd's places).
                        static const std::map<std::string, std::pair<std::size_t, std::size_t>> names {
                            { "tgl", { 2, 3 } }, { "toggle", { 2, 3 } }, { "bng", { 4, 5 } }, { "bang", { 4, 5 } },
                            { "nbx", { 6, 7 } }, { "hsl", { 6, 7 } }, { "vsl", { 6, 7 } }, { "hslider", { 6, 7 } },
                            { "vslider", { 6, 7 } }, { "hradio", { 4, 5 } }, { "vradio", { 4, 5 } } };
                        if (const auto at = names.find (name); at != names.end())
                        {
                            if (named (args, at->second.first))  ports.outlets = 0;
                            if (named (args, at->second.second)) ports.inlets = 0;
                        }
                    }
                    break;

                case BoxKind::message:
                case BoxKind::number:
                case BoxKind::symbol:
                case BoxKind::list:
                    ports = Ports { 1, 1 };
                    break;

                case BoxKind::comment:
                case BoxKind::other:
                    ports = Ports { 0, 0 };
                    break;

                case BoxKind::subpatch:
                {
                    //  As many as the [inlet]s and [outlet]s inside it.
                    ports = Ports { 0, 0 };
                    for (std::size_t c = 0; c < patch.canvases.size(); ++c)
                        if (patch.canvases[c].parent == std::optional<std::size_t> (canvas)
                             && patch.canvases[c].boxInParent == n)
                            for (const auto inner : patch.boxesOn (c))
                            {
                                const auto innerWords = process::splitWords (patch.boxes[inner].text);
                                const auto innerName = innerWords.empty() ? std::string {} : innerWords[0];
                                if (innerName == "inlet" || innerName == "inlet~")    ++ports.inlets;
                                if (innerName == "outlet" || innerName == "outlet~")  ++ports.outlets;
                            }
                    break;
                }
            }

            shown.inlets = std::max (ports.inlets, used[n].inlets);
            shown.outlets = std::max (ports.outlets, used[n].outlets);

            //  ITS SIZE, by Pd's rules.
            if (gui.has_value())
            {
                shown.w = gui->first;
                shown.h = gui->second;
            }
            else if (box.kind == BoxKind::number || box.kind == BoxKind::symbol || box.kind == BoxKind::list)
            {
                const auto width = words.empty() ? 0 : intOf (words[0]);
                shown.lines = { box.kind == BoxKind::number ? std::string ("0") : std::string {} };
                const auto columns = width > 0 ? static_cast<double> (width) : 3.0;
                shown.w = columns * view.font.charWidth + 2.0;
                shown.h = view.font.lineHeight + 4.0;
            }
            else if (box.kind == BoxKind::other)
            {
                shown.w = 0.0;
                shown.h = 0.0;
            }
            else
            {
                const auto columns = box.width > 0 ? static_cast<std::size_t> (box.width) : defaultColumns;
                shown.lines = wrapped (words, columns);
                std::size_t widest = 0;
                for (const auto& line : shown.lines)
                    widest = std::max (widest, columnsOf (line));
                if (box.width > 0)
                    widest = static_cast<std::size_t> (box.width);
                else if (box.kind != BoxKind::comment)
                    widest = std::max<std::size_t> (widest, 3);
                shown.w = static_cast<double> (widest) * view.font.charWidth + horizontalMargins;
                shown.h = static_cast<double> (shown.lines.size()) * view.font.lineHeight + verticalMargins;
            }

            view.right = std::max (view.right, shown.x + shown.w);
            view.bottom = std::max (view.bottom, shown.y + shown.h);
            view.boxes.push_back (std::move (shown));
        }

        //  THE LINES, from an outlet's foot to an inlet's head.
        for (std::size_t l = 0; l < patch.lines.size(); ++l)
        {
            const auto& line = patch.lines[l];
            if (line.canvas != canvas || line.fromBox < 0 || line.toBox < 0
                 || static_cast<std::size_t> (line.fromBox) >= view.boxes.size()
                 || static_cast<std::size_t> (line.toBox) >= view.boxes.size())
                continue;

            const auto& from = view.boxes[static_cast<std::size_t> (line.fromBox)];
            const auto& to = view.boxes[static_cast<std::size_t> (line.toBox)];

            PatchLineView drawn;
            drawn.line = l;
            drawn.fromView = static_cast<std::size_t> (line.fromBox);
            drawn.toView = static_cast<std::size_t> (line.toBox);
            drawn.outlet = line.outlet;
            drawn.inlet = line.inlet;
            drawn.x1 = portX (from, line.outlet, from.outlets) + (portWidth - 1.0) / 2.0;
            drawn.y1 = from.y + from.h;
            drawn.x2 = portX (to, line.inlet, to.inlets) + (portWidth - 1.0) / 2.0;
            drawn.y2 = to.y;
            view.lines.push_back (drawn);
        }

        return view;
    }

    PatchHit hitPatch (const PatchView& view, double x, double y)
    {
        constexpr double grab = 3.0;

        for (std::size_t n = view.boxes.size(); n-- > 0;)
        {
            const auto& box = view.boxes[n];
            if (x < box.x - 1.0 || x > box.x + box.w + 1.0 || y < box.y - grab || y > box.y + box.h + grab)
                continue;

            const auto onPort = [&] (int count, double edge, PatchHit::What what) -> std::optional<PatchHit>
            {
                if (std::abs (y - edge) > grab + 1.0)
                    return std::nullopt;
                for (int p = 0; p < count; ++p)
                {
                    const auto left = portX (box, p, count);
                    if (x >= left - 1.0 && x <= left + portWidth + 1.0)
                        return PatchHit { what, n, p };
                }
                return std::nullopt;
            };

            if (auto outlet = onPort (box.outlets, box.y + box.h - portHeight / 2.0, PatchHit::What::outlet))
                return *outlet;
            if (auto inlet = onPort (box.inlets, box.y + portHeight / 2.0, PatchHit::What::inlet))
                return *inlet;
            if (x >= box.x && x <= box.x + box.w && y >= box.y && y <= box.y + box.h)
                return PatchHit { PatchHit::What::box, n, 0 };
        }

        for (std::size_t l = view.lines.size(); l-- > 0;)
        {
            const auto& line = view.lines[l];
            const auto dx = line.x2 - line.x1, dy = line.y2 - line.y1;
            const auto length2 = dx * dx + dy * dy;
            const auto t = length2 > 0.0 ? std::clamp (((x - line.x1) * dx + (y - line.y1) * dy) / length2, 0.0, 1.0) : 0.0;
            const auto px = line.x1 + t * dx, py = line.y1 + t * dy;
            if ((x - px) * (x - px) + (y - py) * (y - py) <= grab * grab)
                return PatchHit { PatchHit::What::line, l, 0 };
        }

        return {};
    }

    std::vector<std::size_t> boxesTouched (const PatchView& view, double x1, double y1, double x2, double y2)
    {
        const auto left = std::min (x1, x2), right = std::max (x1, x2);
        const auto top = std::min (y1, y2), bottom = std::max (y1, y2);
        std::vector<std::size_t> out;
        for (std::size_t n = 0; n < view.boxes.size(); ++n)
        {
            const auto& box = view.boxes[n];
            if (box.w <= 0.0)
                continue;
            if (box.x <= right && box.x + box.w >= left && box.y <= bottom && box.y + box.h >= top)
                out.push_back (n);
        }
        return out;
    }

    std::string patchMoved (const std::string& text, const std::vector<std::size_t>& boxes, int dx, int dy)
    {
        auto patch = process::parsePatch (text);
        bool changed = false;

        for (const auto b : std::set<std::size_t> (boxes.begin(), boxes.end()))
        {
            if (b >= patch.boxes.size() || patch.boxes[b].kind == BoxKind::other)
                continue;

            auto words = patch.records[patch.boxes[b].record].words;
            if (words.size() < 4)
                continue;

            const auto x = std::max (0, intOf (words[2]) + dx);
            const auto y = std::max (0, intOf (words[3]) + dy);
            if (x == intOf (words[2]) && y == intOf (words[3]))
                continue;

            words[2] = std::to_string (x);
            words[3] = std::to_string (y);
            patch.records[patch.boxes[b].record] = process::recordOf (std::move (words));
            changed = true;
        }

        return changed ? process::writePatch (patch) : text;
    }

    namespace
    {
        /*  Typed words as Pd writes them: split on spaces, a semicolon or a comma
            its own word, each escaped. */
        std::vector<std::string> typedWords (const std::string& typed)
        {
            std::vector<std::string> out;
            std::string word;
            const auto finish = [&]
            {
                if (! word.empty())
                    out.push_back (process::escaped (word));
                word.clear();
            };
            for (const char c : typed)
            {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                    finish();
                else if (c == ';' || c == ',')
                {
                    finish();
                    out.push_back (process::escaped (std::string (1, c)));
                }
                else
                    word += c;
            }
            finish();
            return out;
        }

        const char* const emptyCanvas = "#N canvas 0 50 640 400 12;\n";
    }

    std::string patchTyped (const std::string& text, std::size_t box, const std::string& typed)
    {
        auto patch = process::parsePatch (text);
        if (box >= patch.boxes.size())
            return text;

        const auto kind = patch.boxes[box].kind;
        if (kind != BoxKind::object && kind != BoxKind::message && kind != BoxKind::comment)
            return text;

        auto words = typedWords (typed);
        if (words.empty() && kind != BoxKind::comment)
            return patchDeleted (text, { box }, {});
        if (words.empty())
            words.push_back ("comment");

        const auto& old = patch.records[patch.boxes[box].record].words;
        std::vector<std::string> record (old.begin(),
                                         old.begin() + static_cast<std::ptrdiff_t> (std::min<std::size_t> (4, old.size())));
        record.insert (record.end(), words.begin(), words.end());
        if (patch.boxes[box].width > 0)
        {
            record.push_back (",");
            record.push_back ("f");
            record.push_back (std::to_string (patch.boxes[box].width));
        }

        auto made = process::recordOf (std::move (record));
        if (made.raw == patch.records[patch.boxes[box].record].raw)
            return text;
        patch.records[patch.boxes[box].record] = std::move (made);
        return process::writePatch (patch);
    }

    std::string patchPlaced (const std::string& text, Placed what, int x, int y, const std::string& typed)
    {
        auto base = process::parsePatch (text).canvases.empty() ? std::string (emptyCanvas) + text : text;
        if (! base.empty() && base.back() != '\n')
            base += '\n';

        std::vector<std::string> words { "#X" };
        switch (what)
        {
            case Placed::object:  words.push_back ("obj"); break;
            case Placed::message: words.push_back ("msg"); break;
            case Placed::number:  words.push_back ("floatatom"); break;
            case Placed::symbol:  words.push_back ("symbolatom"); break;
            case Placed::comment: words.push_back ("text"); break;
        }
        words.push_back (std::to_string (std::max (0, x)));
        words.push_back (std::to_string (std::max (0, y)));

        if (what == Placed::number || what == Placed::symbol)
        {
            //  Pd's own: a width, no range, no label, no names.
            for (const auto* field : { what == Placed::number ? "5" : "10", "0", "0", "0", "-", "-", "-", "0" })
                words.push_back (field);
        }
        else
        {
            const auto typedOnes = typedWords (typed);
            words.insert (words.end(), typedOnes.begin(), typedOnes.end());
            if (what == Placed::comment && typedOnes.empty())
                words.push_back ("comment");
        }

        return base + process::recordOf (std::move (words)).raw;
    }

    std::string patchConnected (const PatchView& view, const std::string& text,
                                std::size_t fromView, int outlet, std::size_t toView, int inlet)
    {
        //  The patch's own canvas only: a line is added at the end of the text,
        //  which is that canvas's.
        if (view.canvas != 0 || fromView >= view.boxes.size() || toView >= view.boxes.size() || fromView == toView
             || outlet < 0 || inlet < 0
             || outlet >= view.boxes[fromView].outlets || inlet >= view.boxes[toView].inlets)
            return text;

        for (const auto& line : view.lines)
            if (line.fromView == fromView && line.toView == toView && line.outlet == outlet && line.inlet == inlet)
                return text;

        auto out = text;
        if (! out.empty() && out.back() != '\n')
            out += '\n';
        return out + process::recordOf ({ "#X", "connect", std::to_string (fromView), std::to_string (outlet),
                                          std::to_string (toView), std::to_string (inlet) }).raw;
    }

    std::string patchCopied (const std::string& text, const std::vector<std::size_t>& boxes)
    {
        const auto patch = process::parsePatch (text);
        const std::set<std::size_t> picked (boxes.begin(), boxes.end());

        //  Only boxes on the patch's own canvas, numbered in its order.
        const auto onTop = patch.boxesOn (0);
        std::map<int, int> renumber;
        std::string out;

        for (std::size_t n = 0; n < onTop.size(); ++n)
        {
            const auto b = onTop[n];
            if (picked.count (b) == 0)
                continue;
            renumber[static_cast<int> (n)] = static_cast<int> (renumber.size());

            const auto& box = patch.boxes[b];
            auto first = box.record;
            if (box.kind == BoxKind::subpatch)
                for (const auto& canvas : patch.canvases)
                    if (canvas.parent == std::optional<std::size_t> (0) && canvas.boxInParent == n)
                        first = canvas.record;
            for (auto r = first; r <= box.record; ++r)
                out += patch.records[r].raw;
        }

        for (const auto& line : patch.lines)
        {
            if (line.canvas != 0)
                continue;
            const auto from = renumber.find (line.fromBox), to = renumber.find (line.toBox);
            if (from == renumber.end() || to == renumber.end())
                continue;
            out += process::recordOf ({ "#X", "connect", std::to_string (from->second), std::to_string (line.outlet),
                                        std::to_string (to->second), std::to_string (line.inlet) }).raw;
        }
        return out;
    }

    Pasted patchPasted (const std::string& text, const std::string& piece, int dx, int dy)
    {
        Pasted out;
        auto base = process::parsePatch (text).canvases.empty() ? std::string (emptyCanvas) + text : text;
        if (! base.empty() && base.back() != '\n')
            base += '\n';

        //  The piece read under a canvas of its own, so its numbers are its own.
        const auto pieceAlone = process::parsePatch (std::string (emptyCanvas) + piece);
        const auto before = process::parsePatch (base);
        const auto firstNumber = static_cast<int> (before.boxesOn (0).size());

        std::string added;
        for (std::size_t r = 1; r < pieceAlone.records.size(); ++r)
        {
            auto words = pieceAlone.records[r].words;
            if (words.size() >= 2 && words[0] == "#X" && words[1] == "connect" && words.size() >= 6)
            {
                words[2] = std::to_string (intOf (words[2]) + firstNumber);
                words[4] = std::to_string (intOf (words[4]) + firstNumber);
                added += process::recordOf (std::move (words)).raw;
                continue;
            }

            //  A box of the piece's own canvas moves; a subpatch's inside does not.
            const bool topLevel = std::any_of (pieceAlone.boxes.begin(), pieceAlone.boxes.end(),
                                               [&] (const process::PatchBox& b) { return b.record == r && b.canvas == 0; });
            if (topLevel && words.size() >= 4 && words[0] == "#X" && words[1] != "array")
            {
                words[2] = std::to_string (std::max (0, intOf (words[2]) + dx));
                words[3] = std::to_string (std::max (0, intOf (words[3]) + dy));
                added += process::recordOf (std::move (words)).raw;
            }
            else
            {
                added += pieceAlone.records[r].raw;
            }
        }

        out.text = base + added;
        const auto after = process::parsePatch (out.text);
        const auto now = after.boxesOn (0);
        for (std::size_t n = static_cast<std::size_t> (firstNumber); n < now.size(); ++n)
            out.boxes.push_back (now[n]);
        return out;
    }

    std::string patchDeleted (const std::string& text, const std::vector<std::size_t>& boxes,
                              const std::vector<std::size_t>& lines)
    {
        auto patch = process::parsePatch (text);
        std::set<std::size_t> gone;          // records
        std::map<std::size_t, std::set<int>> goneNumbers;   // per canvas, the box numbers deleted

        for (const auto b : boxes)
        {
            if (b >= patch.boxes.size())
                continue;

            const auto& box = patch.boxes[b];
            const auto onCanvas = patch.boxesOn (box.canvas);
            const auto number = static_cast<int> (std::find (onCanvas.begin(), onCanvas.end(), b) - onCanvas.begin());
            goneNumbers[box.canvas].insert (number);

            if (box.kind == BoxKind::subpatch)
            {
                //  The subpatch goes whole: its #N canvas through its #X restore.
                for (const auto& canvas : patch.canvases)
                    if (canvas.parent == std::optional<std::size_t> (box.canvas)
                         && static_cast<int> (canvas.boxInParent) == number)
                        for (auto r = canvas.record; r <= box.record; ++r)
                            gone.insert (r);
            }
            gone.insert (box.record);
        }

        for (const auto l : lines)
            if (l < patch.lines.size())
                gone.insert (patch.lines[l].record);

        for (const auto& line : patch.lines)
        {
            if (gone.count (line.record) > 0)
                continue;

            const auto found = goneNumbers.find (line.canvas);
            if (found == goneNumbers.end())
                continue;

            const auto& deleted = found->second;
            if (deleted.count (line.fromBox) > 0 || deleted.count (line.toBox) > 0)
            {
                gone.insert (line.record);
                continue;
            }

            //  The boxes after a deleted one move up; so do the numbers that name them.
            const auto below = [&deleted] (int number)
            {
                return static_cast<int> (std::count_if (deleted.begin(), deleted.end(),
                                                        [number] (int d) { return d < number; }));
            };
            const auto from = line.fromBox - below (line.fromBox);
            const auto to = line.toBox - below (line.toBox);
            if (from != line.fromBox || to != line.toBox)
            {
                auto words = patch.records[line.record].words;
                words[2] = std::to_string (from);
                words[4] = std::to_string (to);
                patch.records[line.record] = process::recordOf (std::move (words));
            }
        }

        if (gone.empty())
            return text;

        process::Patch kept;
        for (std::size_t r = 0; r < patch.records.size(); ++r)
            if (gone.count (r) == 0)
                kept.records.push_back (patch.records[r]);
        kept.trailing = patch.trailing;
        return process::writePatch (kept);
    }
}
