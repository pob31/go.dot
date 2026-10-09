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

#include <wfg/engine/process/PatchText.h>

#include <algorithm>
#include <charconv>
#include <system_error>

namespace wfg::process
{
    namespace
    {
        bool isSpace (char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        int intOf (const std::string& word)
        {
            int value = 0;
            const auto* first = word.data();
            const auto* last = word.data() + word.size();
            // Pd writes a place as a whole number; an older file may carry a
            // fraction, which Pd itself truncates.
            const auto [end, error] = std::from_chars (first, last, value);
            (void) end;
            return error == std::errc() ? value : 0;
        }

        std::string joined (const std::vector<std::string>& words, std::size_t from, std::size_t to)
        {
            std::string out;
            for (auto i = from; i < to && i < words.size(); ++i)
            {
                if (! out.empty() && words[i] != ",")
                    out += ' ';
                out += words[i];
            }
            return out;
        }

        std::optional<BoxKind> boxKindOf (const std::string& word)
        {
            if (word == "obj")         return BoxKind::object;
            if (word == "msg")         return BoxKind::message;
            if (word == "floatatom")   return BoxKind::number;
            if (word == "symbolatom")  return BoxKind::symbol;
            if (word == "listbox")     return BoxKind::list;
            if (word == "text")        return BoxKind::comment;
            if (word == "scalar")      return BoxKind::other;
            if (word == "array")       return BoxKind::other;
            return std::nullopt;
        }
    }

    std::vector<std::size_t> Patch::boxesOn (std::size_t canvas) const
    {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < boxes.size(); ++i)
            if (boxes[i].canvas == canvas)
                out.push_back (i);
        return out;
    }

    std::vector<std::string> splitWords (std::string_view recordText)
    {
        std::vector<std::string> words;
        std::string word;
        bool inWord = false;

        const auto finish = [&]
        {
            if (inWord)
                words.push_back (std::move (word));
            word.clear();
            inWord = false;
        };

        for (std::size_t i = 0; i < recordText.size(); ++i)
        {
            const char c = recordText[i];
            if (c == '\\' && i + 1 < recordText.size())
            {
                word += c;
                word += recordText[++i];
                inWord = true;
            }
            else if (isSpace (c))
            {
                finish();
            }
            else if (c == ',')
            {
                finish();
                words.emplace_back (",");
            }
            else
            {
                word += c;
                inWord = true;
            }
        }
        finish();
        return words;
    }

    std::string unescaped (std::string_view word)
    {
        std::string out;
        out.reserve (word.size());
        for (std::size_t i = 0; i < word.size(); ++i)
        {
            if (word[i] == '\\' && i + 1 < word.size())
                ++i;
            out += word[i];
        }
        return out;
    }

    std::string escaped (std::string_view word)
    {
        std::string out;
        out.reserve (word.size());
        for (const char c : word)
        {
            if (c == ' ' || c == ';' || c == ',' || c == '$' || c == '\\')
                out += '\\';
            out += c;
        }
        return out;
    }

    PatchRecord recordOf (std::vector<std::string> words)
    {
        PatchRecord record;
        for (const auto& word : words)
        {
            // Pd writes a comma straight after the word before it: "100, f 20".
            if (! record.raw.empty() && word != ",")
                record.raw += ' ';
            record.raw += word;
        }
        record.raw += ";\n";
        record.words = std::move (words);
        return record;
    }

    Patch parsePatch (std::string_view text)
    {
        Patch patch;

        // The records: each ends at a semicolon Pd did not escape, and takes the
        // new line after it with it.
        std::size_t at = 0;
        while (at < text.size())
        {
            std::size_t end = at;
            bool found = false;
            for (; end < text.size(); ++end)
            {
                if (text[end] == '\\')
                {
                    ++end;
                    continue;
                }
                if (text[end] == ';')
                {
                    found = true;
                    break;
                }
            }
            if (! found)
            {
                patch.trailing = std::string (text.substr (at));
                break;
            }

            const auto body = text.substr (at, end - at);
            auto next = end + 1;
            if (next + 1 < text.size() && text[next] == '\r' && text[next + 1] == '\n')
                next += 2;
            else if (next < text.size() && text[next] == '\n')
                next += 1;

            PatchRecord record;
            record.raw = std::string (text.substr (at, next - at));
            record.words = splitWords (body);
            patch.records.push_back (std::move (record));
            at = next;
        }

        // What the records make: canvases, boxes and lines.
        std::vector<std::size_t> open;
        const auto fail = [&] (std::string why)
        {
            if (patch.problem.empty())
                patch.problem = std::move (why);
        };

        for (std::size_t r = 0; r < patch.records.size(); ++r)
        {
            const auto& w = patch.records[r].words;
            if (w.size() < 2)
                continue;

            if (w[0] == "#N" && w[1] == "canvas")
            {
                PatchCanvas canvas;
                canvas.record = r;
                if (! open.empty())
                    canvas.parent = open.back();
                patch.canvases.push_back (canvas);
                open.push_back (patch.canvases.size() - 1);
                continue;
            }

            if (w[0] != "#X")
                continue;

            if (open.empty())
            {
                fail ("a box comes before any canvas");
                continue;
            }

            if (const auto kind = boxKindOf (w[1]))
            {
                PatchBox box;
                box.kind = *kind;
                box.record = r;
                box.canvas = open.back();
                if (*kind != BoxKind::other)
                {
                    box.x = w.size() > 2 ? intOf (w[2]) : 0;
                    box.y = w.size() > 3 ? intOf (w[3]) : 0;
                }

                // ", f <width>" closes a box Pd was told how wide to draw.
                auto textEnd = w.size();
                if (w.size() >= 3 && w[w.size() - 3] == "," && w[w.size() - 2] == "f")
                {
                    box.width = intOf (w.back());
                    textEnd = w.size() - 3;
                }
                box.text = *kind == BoxKind::other && w[1] == "array" ? joined (w, 2, textEnd)
                                                                      : joined (w, 4, textEnd);
                patch.boxes.push_back (std::move (box));
                continue;
            }

            if (w[1] == "restore")
            {
                if (open.size() < 2)
                {
                    fail ("a subpatch is closed that was never opened");
                    continue;
                }
                const auto closing = open.back();
                open.pop_back();

                PatchBox box;
                box.kind = BoxKind::subpatch;
                box.record = r;
                box.canvas = open.back();
                box.x = w.size() > 2 ? intOf (w[2]) : 0;
                box.y = w.size() > 3 ? intOf (w[3]) : 0;
                auto textEnd = w.size();
                if (w.size() >= 3 && w[w.size() - 3] == "," && w[w.size() - 2] == "f")
                {
                    box.width = intOf (w.back());
                    textEnd = w.size() - 3;
                }
                box.text = joined (w, 4, textEnd);

                auto& canvas = patch.canvases[closing];
                canvas.boxInParent = patch.boxesOn (open.back()).size();
                canvas.name = box.text;
                patch.boxes.push_back (std::move (box));
                continue;
            }

            if (w[1] == "connect")
            {
                if (w.size() < 6)
                {
                    fail ("a line names fewer than two boxes");
                    continue;
                }
                PatchLine line;
                line.canvas = open.back();
                line.fromBox = intOf (w[2]);
                line.outlet = intOf (w[3]);
                line.toBox = intOf (w[4]);
                line.inlet = intOf (w[5]);
                line.record = r;
                patch.lines.push_back (line);
            }
        }

        if (patch.canvases.empty() && ! text.empty())
            fail ("the text holds no canvas");
        else if (open.size() > 1)
            fail ("a subpatch is opened and never closed");

        return patch;
    }

    std::string writePatch (const Patch& patch)
    {
        std::string out;
        for (const auto& record : patch.records)
            out += record.raw;
        out += patch.trailing;
        return out;
    }

    PatchNames namesIn (const Patch& patch)
    {
        PatchNames names;

        const auto wanted = [] (const std::string& name, const char* catchAll)
        {
            return (! name.empty() && name.front() == '/') || name == catchAll;
        };
        const auto add = [] (std::vector<std::string>& to, std::string name)
        {
            if (std::find (to.begin(), to.end(), name) == to.end())
                to.push_back (std::move (name));
        };
        const auto send = [&] (const std::string& word)
        {
            auto name = unescaped (word);
            if (wanted (name, "out"))
                add (names.sends, std::move (name));
        };
        const auto receive = [&] (const std::string& word)
        {
            auto name = unescaped (word);
            if (wanted (name, "in"))
                add (names.receives, std::move (name));
        };

        // Where Pd's GUI boxes keep their send and receive names, after the
        // class: tgl size init SEND RECEIVE ..., and so on.
        struct GuiNames { const char* name; std::size_t send; std::size_t receive; };
        static constexpr GuiNames guis[] = {
            { "tgl", 2, 3 },     { "toggle", 2, 3 },
            { "bng", 4, 5 },     { "bang", 4, 5 },
            { "nbx", 6, 7 },     { "hsl", 6, 7 },     { "vsl", 6, 7 },
            { "hslider", 6, 7 }, { "vslider", 6, 7 },
            { "hradio", 4, 5 },  { "vradio", 4, 5 },  { "hdl", 4, 5 }, { "vdl", 4, 5 },
            { "cnv", 3, 4 },     { "my_canvas", 3, 4 },
        };

        for (const auto& box : patch.boxes)
        {
            const auto words = splitWords (box.text);
            if (words.empty())
                continue;

            if (box.kind == BoxKind::object)
            {
                const auto cls = unescaped (words[0]);
                if ((cls == "s" || cls == "send") && words.size() > 1)
                    send (words[1]);
                else if ((cls == "r" || cls == "receive") && words.size() > 1)
                    receive (words[1]);
                else if (cls == "vu" && words.size() > 3)
                {
                    if (unescaped (words[3]) != "empty")
                        receive (words[3]);
                }
                else
                {
                    for (const auto& gui : guis)
                    {
                        if (cls != gui.name)
                            continue;
                        if (words.size() > gui.send + 1 && unescaped (words[gui.send + 1]) != "empty")
                            send (words[gui.send + 1]);
                        if (words.size() > gui.receive + 1 && unescaped (words[gui.receive + 1]) != "empty")
                            receive (words[gui.receive + 1]);
                        break;
                    }
                }
            }
            else if (box.kind == BoxKind::message)
            {
                // "\; name message" sends the message to the receiver `name`.
                for (std::size_t i = 0; i + 1 < words.size(); ++i)
                    if (words[i] == "\\;")
                        send (words[i + 1]);
            }
            else if (box.kind == BoxKind::number || box.kind == BoxKind::symbol || box.kind == BoxKind::list)
            {
                // width min max flag label RECEIVE SEND; "-" is none.
                if (words.size() > 5 && unescaped (words[5]) != "-")
                    receive (words[5]);
                if (words.size() > 6 && unescaped (words[6]) != "-")
                    send (words[6]);
            }
        }
        return names;
    }

    std::string starterPatch()
    {
        return "#N canvas 0 50 640 400 12;\n"
               "#X text 20 20 [r /address] hears a device or a row of the show - [s /address] writes to it"
               " - [s /godot/cmd/cue/fire] fires a cue;\n";
    }
}
