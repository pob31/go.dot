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

#include "SectionCommands.h"

#include <cmath>
#include <utility>

namespace wfg::doc
{
    namespace
    {
        Outcome fromEdit (const EditResult& edit, std::vector<osc::Value> appliedArgs)
        {
            if (! edit.ok)
                return Outcome::rejected (edit.reason);

            return Outcome::ok (std::move (appliedArgs));
        }

        /*  The record carries what the session drew or knew, at `index`, so a
            replay draws nothing and reads it there. */
        std::vector<osc::Value> withValue (std::vector<osc::Value> args, std::size_t index, osc::Value value)
        {
            while (args.size() < index)
                args.push_back (osc::Value::string ({}));

            if (args.size() > index)
                args[index] = std::move (value);
            else
                args.push_back (std::move (value));

            return args;
        }

        std::string stringAt (const std::vector<osc::Value>& args, std::size_t index)
        {
            return args.size() > index && args[index].isString() ? args[index].getString() : std::string {};
        }

        double numberAt (const std::vector<osc::Value>& args, std::size_t index)
        {
            return args.size() > index && args[index].isNumber() ? args[index].asDouble() : 0.0;
        }

        /*  A SECOND ON THE MOVIE'S FRAME GRID (55.5, ADT): the nearest frame. */
        double snappedToFrames (double seconds, double fps)
        {
            return fps > 0.0 ? std::round (seconds * fps) / fps : seconds;
        }

        /*  WHAT A MOVIE'S VERBS NEED TO KNOW, and the words when it is not there
            (ADX): the frame rate off the record when a replay has no facts,
            else the session's; a movie whose codec is not HAP, or not read yet,
            is refused with the words that say what to do. */
        struct MovieFacts
        {
            bool movie = false;
            double fps = 0.0;
            std::string refusal;   // empty when the verb may go on
        };

        MovieFacts movieFactsOf (const ShowDocument& document, const juce::ValueTree& cue,
                                 const MediaFacts& facts, double recordedFps)
        {
            MovieFacts out;

            if (! document.isMovieCue (cue))
                return out;

            out.movie = true;
            out.fps = recordedFps;
            const auto file = cue["file"].toString().toStdString();

            /*  Only a session that knows files refuses: a replay hands no
                facts, reads the rate off the record and snaps nothing a
                record does not say to. */
            if (facts.codecOf)
            {
                const auto codec = facts.codecOf (file);

                if (codec.empty())
                    out.refusal = "the movie is not read yet";
                else if (codec != "Hap1" && codec != "Hap5" && codec != "HapY")
                    out.refusal = "convert the movie to HAP first (Show > Convert the movie to HAP)";
            }

            if (! (out.fps > 0.0) && facts.frameRateOf)
                out.fps = facts.frameRateOf (file);

            if (out.refusal.empty() && facts.codecOf && ! (out.fps > 0.0))
                out.refusal = "the movie is not read yet";

            return out;
        }

        Outcome refusedMovie (const MovieFacts& facts)
        {
            auto outcome = Outcome::rejected (reason::badValue);
            outcome.detail = facts.refusal;
            return outcome;
        }
    }

    void registerSectionCommands (CommandRegistry& registry, ShowDocument& document, MediaFacts facts)
    {
        //----------------------------------------------------------------------
        registry.add ({ "section.split",
                        "Cuts a sound's edited timeline at a second of it (namespace draft 55): the piece"
                        " that second falls in becomes two, the second half a new section directly after"
                        " the first, with the first's trim. A cue with no sections yet first gets one over"
                        " the whole file, as long as the session knows the file to be - written back on the"
                        " record, so a replay reads it there. On a cut, at the top or at the end there is"
                        " nothing to divide.",
                        { { "cue", 's', false }, { "at", 'd', false }, { "id", 's', true }, { "length", 'd', true },
                          { "frameRate", 'd', true } },
                        true,
                        [&document, facts] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = stringAt (args, 0);
                            auto at = numberAt (args, 1);
                            const auto id = stringAt (args, 2);
                            auto length = numberAt (args, 3);
                            const auto cue = document.findById (cueId);

                            if (! (length > 0.0) && facts.lengthOf && cue.isValid())
                                length = facts.lengthOf (cue["file"].toString().toStdString());

                            /*  A MOVIE'S CUT ON ITS FRAME GRID (55.5, ADT), the snapped second
                                and the rate written back so a replay with no facts snaps the
                                same; a movie that is not HAP refused in words (ADX). */
                            const auto movie = movieFactsOf (document, cue, facts, numberAt (args, 4));

                            if (! movie.refusal.empty())
                                return refusedMovie (movie);

                            if (movie.movie)
                                at = snappedToFrames (at, movie.fps);

                            const auto edit = document.splitSection (cueId, at, length, id);

                            auto applied = withValue (args, 1, osc::Value::float64 (at));
                            applied = withValue (std::move (applied), 2, osc::Value::string (edit.id));
                            applied = withValue (std::move (applied), 3, osc::Value::float64 (length));

                            if (movie.movie)
                                applied = withValue (std::move (applied), 4, osc::Value::float64 (movie.fps));

                            return fromEdit (edit, std::move (applied));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.join",
                        "Takes a cut back: the section and the one after it made one again, when the two"
                        " are still one in the file. The survivor keeps its trim and its crossfade.",
                        { { "section", 's', false } },
                        true,
                        [&document, facts] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (const auto section = document.findById (stringAt (args, 0));
                                section.isValid() && section.hasType ("Section"))
                                if (const auto movie = movieFactsOf (document, section.getParent(), facts, 0.0);
                                    ! movie.refusal.empty())
                                    return refusedMovie (movie);

                            return fromEdit (document.joinSection (stringAt (args, 0)), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.trim",
                        "Moves a section's edges, in seconds of the file. Everything after it on the"
                        " edited timeline shifts, and the cue's lane points, its ranges and its start"
                        " offset are carried along in the same edit; what sat on a sliver cut away goes"
                        " with it.",
                        { { "section", 's', false }, { "in", 'd', false }, { "out", 'd', false },
                          { "frameRate", 'd', true } },
                        true,
                        [&document, facts] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto sectionId = stringAt (args, 0);
                            auto in = numberAt (args, 1);
                            auto out = numberAt (args, 2);
                            const auto section = document.findById (sectionId);
                            const auto cue = section.isValid() && section.hasType ("Section") ? section.getParent() : juce::ValueTree();
                            const auto movie = movieFactsOf (document, cue, facts, numberAt (args, 3));

                            if (! movie.refusal.empty())
                                return refusedMovie (movie);

                            if (movie.movie)
                            {
                                in = snappedToFrames (in, movie.fps);
                                out = snappedToFrames (out, movie.fps);
                            }

                            const auto edit = document.trimSection (sectionId, in, out);
                            auto applied = args;

                            if (movie.movie)
                            {
                                applied = withValue (std::move (applied), 1, osc::Value::float64 (in));
                                applied = withValue (std::move (applied), 2, osc::Value::float64 (out));
                                applied = withValue (std::move (applied), 3, osc::Value::float64 (movie.fps));
                            }

                            return fromEdit (edit, std::move (applied));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.move",
                        "Moves a section to a place among its cue's sections, counting from nought; the"
                        " lane points, the ranges and the start offset go with the sound. A range left"
                        " ending before it begins - the two sides of the move - is taken out.",
                        { { "section", 's', false }, { "index", 'i', false } },
                        true,
                        [&document, facts] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (const auto section = document.findById (stringAt (args, 0));
                                section.isValid() && section.hasType ("Section"))
                                if (const auto movie = movieFactsOf (document, section.getParent(), facts, 0.0);
                                    ! movie.refusal.empty())
                                    return refusedMovie (movie);

                            return fromEdit (document.moveSection (stringAt (args, 0),
                                                                   static_cast<int> (std::lround (numberAt (args, 1)))),
                                             args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.remove",
                        "Takes a section out of the edit: what sat on it goes with it and the rest closes"
                        " up. With the last section gone the cue plays its whole file again, and every"
                        " point is carried back to the file's own time.",
                        { { "section", 's', false } },
                        true,
                        [&document, facts] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (const auto section = document.findById (stringAt (args, 0));
                                section.isValid() && section.hasType ("Section"))
                                if (const auto movie = movieFactsOf (document, section.getParent(), facts, 0.0);
                                    ! movie.refusal.empty())
                                    return refusedMovie (movie);

                            return fromEdit (document.removeSection (stringAt (args, 0)), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.clear",
                        "The edit gone: every section taken out, the cue playing its whole file again,"
                        " its lane points, ranges and start offset carried back to the file's own time.",
                        { { "cue", 's', false } },
                        true,
                        [&document, facts] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (const auto movie = movieFactsOf (document, document.findById (stringAt (args, 0)), facts, 0.0);
                                ! movie.refusal.empty())
                                return refusedMovie (movie);

                            return fromEdit (document.clearSections (stringAt (args, 0)), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "media.frozen",
                        "The bounce is there (namespace draft 55, ADN): the cue plays it - file becomes the"
                        " bounce's name, editSource keeps the file the edit was made from - and its"
                        " sections wait for media.unfreeze. Submitted by the renderer once media.freeze's"
                        " copy has landed; one undoable edit, and the bounce stays on disk when it is undone."
                        " A movie's locked sound is frozen with it, in the three arguments after (55.5, ADW).",
                        { { "cue", 's', false }, { "source", 's', false }, { "bounce", 's', false },
                          { "sound", 's', true }, { "soundSource", 's', true }, { "soundBounce", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            /*  A MOVIE'S PAIR (55.5, ADW): its locked sound frozen in the
                                same record, onto its own bounce. */
                            return fromEdit (document.freezeEdit (stringAt (args, 0), stringAt (args, 1), stringAt (args, 2),
                                                                  stringAt (args, 3), stringAt (args, 4), stringAt (args, 5)),
                                             args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "media.unfreeze",
                        "The cue pointed back at the file its edit was made from; the sections are live"
                        " again and the render follows them.",
                        { { "cue", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.unfreezeEdit (stringAt (args, 0)), args);
                        } });
    }
}
