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
#include <optional>
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

        /*  A flag sent as a number - leaveGap, alone, ripple: set when it is one,
            or anything further from nought than a half. */
        bool flagAt (const std::vector<osc::Value>& args, std::size_t index)
        {
            return std::abs (numberAt (args, index)) > 0.5;
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
                        " up - or, with leaveGap 1, its time stays as silence and everything after it stays"
                        " where it was (namespace draft 55.9, AEE); the silence before it stays either way."
                        " With the last section gone the cue plays its whole file again, and every"
                        " point is carried back to the file's own time.",
                        { { "section", 's', false }, { "leaveGap", 'i', true } },
                        true,
                        [&document, facts] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (const auto section = document.findById (stringAt (args, 0));
                                section.isValid() && section.hasType ("Section"))
                                if (const auto movie = movieFactsOf (document, section.getParent(), facts, 0.0);
                                    ! movie.refusal.empty())
                                    return refusedMovie (movie);

                            return fromEdit (document.removeSection (stringAt (args, 0), flagAt (args, 1)), args);
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
        /*  THE HANDLES (namespace draft §55.9-55.11): what the waveform's
            handles and keys send, each one step. A side is "in" or "out". */
        const auto sideAt = [] (const std::vector<osc::Value>& args, std::size_t index) -> std::optional<bool>
        {
            const auto side = stringAt (args, index);

            if (side == "in")
                return true;

            if (side == "out")
                return false;

            return std::nullopt;
        };

        const auto refusedMovieOf = [&document, facts] (const std::string& sectionId, double recordedFps)
        {
            MovieFacts movie;

            if (const auto section = document.findById (sectionId); section.isValid() && section.hasType ("Section"))
                movie = movieFactsOf (document, section.getParent(), facts, recordedFps);

            return movie;
        };

        registry.add ({ "section.edge",
                        "Moves a section's in or out point to a second of the file with its material in place"
                        " (namespace draft 55.9, AEC): where it touches the next or the one before, the cut"
                        " between the two moves and the neighbour's edge with it; beside silence, the silence"
                        " gives or takes. Nothing else moves; what sat on material cut away goes with it."
                        " Refused where a neighbour would be pushed or the section left nothing. On a movie the"
                        " second lands on its frame grid, written back with the rate (AEG).",
                        { { "section", 's', false }, { "side", 's', false }, { "seconds", 'd', false },
                          { "frameRate", 'd', true } },
                        true,
                        [&document, sideAt, refusedMovieOf] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto sectionId = stringAt (args, 0);
                            const auto side = sideAt (args, 1);
                            auto seconds = numberAt (args, 2);

                            if (! side.has_value())
                                return Outcome::rejected (reason::badValue);

                            const auto movie = refusedMovieOf (sectionId, numberAt (args, 3));

                            if (! movie.refusal.empty())
                                return refusedMovie (movie);

                            if (movie.movie)
                                seconds = snappedToFrames (seconds, movie.fps);

                            auto applied = withValue (args, 2, osc::Value::float64 (seconds));

                            if (movie.movie)
                                applied = withValue (std::move (applied), 3, osc::Value::float64 (movie.fps));

                            return fromEdit (document.edgeSection (sectionId, *side, seconds), std::move (applied));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.place",
                        "Slides a section along its cue's edited timeline to begin at a second of it, its"
                        " material with it and everything else where it was - the silence before it and"
                        " after it giving and taking (namespace draft 55.13). It stops at its neighbours:"
                        " refused where it would cover one; under a millisecond from one it meets it. The lane"
                        " points over it go with it. On a movie the second lands on its frame grid, written"
                        " back with the rate.",
                        { { "section", 's', false }, { "seconds", 'd', false }, { "frameRate", 'd', true } },
                        true,
                        [&document, refusedMovieOf] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto sectionId = stringAt (args, 0);
                            auto seconds = numberAt (args, 1);
                            const auto movie = refusedMovieOf (sectionId, numberAt (args, 2));

                            if (! movie.refusal.empty())
                                return refusedMovie (movie);

                            if (movie.movie)
                                seconds = snappedToFrames (seconds, movie.fps);

                            auto applied = withValue (args, 1, osc::Value::float64 (seconds));

                            if (movie.movie)
                                applied = withValue (std::move (applied), 2, osc::Value::float64 (movie.fps));

                            return fromEdit (document.placeSection (sectionId, seconds), std::move (applied));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.fade",
                        "Sets how long a section fades in or out, in seconds (namespace draft 55.9, AED): its"
                        " partner across a join - the fade out of the section before, the fade in of the one"
                        " after - moves by as much, keeping any difference, unless alone is 1. Both held to the"
                        " material: twice the in point for a fade in centred on a join, and what the section's"
                        " length leaves beside its other fade.",
                        { { "section", 's', false }, { "side", 's', false }, { "seconds", 'd', false },
                          { "alone", 'i', true } },
                        true,
                        [&document, sideAt, refusedMovieOf] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto side = sideAt (args, 1);

                            if (! side.has_value())
                                return Outcome::rejected (reason::badValue);

                            if (const auto movie = refusedMovieOf (stringAt (args, 0), 0.0); ! movie.refusal.empty())
                                return refusedMovie (movie);

                            return fromEdit (document.fadeSection (stringAt (args, 0), *side, numberAt (args, 2),
                                                                   flagAt (args, 3)),
                                             args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.curve",
                        "Sets the curve of a section's fade in or out, -1..1 (namespace draft 55.9, AEB): the"
                        " fade's shape raised to two to the minus this; its partner across a join moves by as"
                        " much unless alone is 1.",
                        { { "section", 's', false }, { "side", 's', false }, { "curve", 'd', false },
                          { "alone", 'i', true } },
                        true,
                        [&document, sideAt, refusedMovieOf] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto side = sideAt (args, 1);

                            if (! side.has_value())
                                return Outcome::rejected (reason::badValue);

                            if (const auto movie = refusedMovieOf (stringAt (args, 0), 0.0); ! movie.refusal.empty())
                                return refusedMovie (movie);

                            return fromEdit (document.curveSection (stringAt (args, 0), *side, numberAt (args, 2),
                                                                    flagAt (args, 3)),
                                             args);
                        } });

        //----------------------------------------------------------------------
        /*  A SELECTION'S TWO ENDS, and the length a cue with no sections yet is
            cut from: the ends on a movie's grid, everything written back so a
            replay with no facts makes the same show. */
        struct Span
        {
            std::string cueId;
            double from = 0.0, to = 0.0, length = 0.0;
            MovieFacts movie;
        };

        const auto spanOf = [&document, facts] (const std::vector<osc::Value>& args, std::size_t lengthAt, std::size_t rateAt)
        {
            Span span;
            span.cueId = stringAt (args, 0);
            span.from = numberAt (args, 1);
            span.to = numberAt (args, 2);
            span.length = numberAt (args, lengthAt);

            const auto cue = document.findById (span.cueId);

            if (! (span.length > 0.0) && facts.lengthOf && cue.isValid())
                span.length = facts.lengthOf (cue["file"].toString().toStdString());

            span.movie = movieFactsOf (document, cue, facts, numberAt (args, rateAt));

            if (span.movie.movie)
            {
                span.from = snappedToFrames (span.from, span.movie.fps);
                span.to = snappedToFrames (span.to, span.movie.fps);
            }

            return span;
        };

        const auto spanApplied = [] (std::vector<osc::Value> args, const Span& span, std::size_t idsAt,
                                     const std::string& first, const std::string& second, std::size_t lengthAt,
                                     std::size_t rateAt)
        {
            args = withValue (std::move (args), 1, osc::Value::float64 (span.from));
            args = withValue (std::move (args), 2, osc::Value::float64 (span.to));
            args = withValue (std::move (args), idsAt, osc::Value::string (first));
            args = withValue (std::move (args), idsAt + 1, osc::Value::string (second));
            args = withValue (std::move (args), lengthAt, osc::Value::float64 (span.length));

            if (span.movie.movie)
                args = withValue (std::move (args), rateAt, osc::Value::float64 (span.movie.fps));

            return args;
        };

        registry.add ({ "section.splitSpan",
                        "Cuts a cue's edited timeline at both ends of a selection, in one step (namespace draft"
                        " 55.9): x with a selection. An end on a cut or in silence is passed over; with neither"
                        " to cut, refused. The identifiers drawn and the length a cue with no sections is cut"
                        " from are written back on the record.",
                        { { "cue", 's', false }, { "from", 'd', false }, { "to", 'd', false },
                          { "id", 's', true }, { "id2", 's', true }, { "length", 'd', true }, { "frameRate", 'd', true } },
                        true,
                        [&document, spanOf, spanApplied] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto span = spanOf (args, 5, 6);

                            if (! span.movie.refusal.empty())
                                return refusedMovie (span.movie);

                            auto first = stringAt (args, 3);
                            auto second = stringAt (args, 4);
                            const auto edit = document.splitSpan (span.cueId, span.from, span.to, span.length, first, second);

                            return fromEdit (edit, spanApplied (args, span, 3, first, second, 5, 6));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.deleteSpan",
                        "Takes the material between two seconds of a cue's edited timeline out (namespace draft"
                        " 55.9, AEE): as silence, everything after staying where it was - Backspace on a"
                        " selection - or closed up with ripple 1, everything after moving earlier by the"
                        " selection's length - Shift+Backspace. The ends are cut first; the lane points, the"
                        " ranges and the start offset are carried. Refused when nothing would be left, and"
                        " without ripple when the selection holds only silence.",
                        { { "cue", 's', false }, { "from", 'd', false }, { "to", 'd', false }, { "ripple", 'i', true },
                          { "id", 's', true }, { "id2", 's', true }, { "length", 'd', true }, { "frameRate", 'd', true } },
                        true,
                        [&document, spanOf, spanApplied] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto span = spanOf (args, 6, 7);

                            if (! span.movie.refusal.empty())
                                return refusedMovie (span.movie);

                            auto first = stringAt (args, 4);
                            auto second = stringAt (args, 5);
                            const auto edit = document.deleteSpan (span.cueId, span.from, span.to, flagAt (args, 3),
                                                                   span.length, first, second);

                            auto applied = withValue (args, 3, osc::Value::int32 (flagAt (args, 3) ? 1 : 0));
                            return fromEdit (edit, spanApplied (std::move (applied), span, 4, first, second, 6, 7));
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
