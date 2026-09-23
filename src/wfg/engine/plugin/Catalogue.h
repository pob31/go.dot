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

#pragma once

/*
    WHAT A PLUGIN'S PARAMETERS ARE, without an instance in the room (Phase 9a,
    namespace draft §17.7).

    A juce::PluginDescription carries no parameters; only an instance does. So
    the child that hosts a plugin reports them once - name, short name, unit,
    default, whether it is stepped and its step texts, a bipolar guess, and the
    plugin's own text for the value at a hundred and one points - and the
    engine caches that per identifier on this machine, beside Tracktion's
    known list and never in a bundle. That is what lets a page label a cue that
    is not sounding, and even show its value text, which is the fourth thing
    the surface pages draft's §8 asks for.

    NAMES NO JUCE TYPE IN THIS HEADER, so the tree, a test and a client model
    can hold one. The .cpp reads and writes JSON with JUCE.

    THE TEST CATALOGUE. `godot:test-gain` is the child mode CI uses in place of
    a plugin it cannot install: two parameters, written here by hand, so the
    whole surface a real plugin would present is exercised on every runner.
*/

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace wfg::plugin
{
    struct Parameter
    {
        std::string name;
        std::string shortName;
        std::string unit;

        /** Normalised, 0..1, as the plugin takes it. */
        float defaultValue = 0.0f;

        bool discrete = false;
        int steps = 0;

        /** The step texts of a discrete parameter, `steps` of them. */
        std::vector<std::string> stepText;

        /** Whether the middle is the rest and the ends are opposite signs - a
            ring fills from the centre for one of these. A guess from the
            default and the texts at the ends; a curated map may overrule it. */
        bool bipolar = false;

        /** The plugin's own text for the value at 0, 0.01, … 1. */
        std::array<std::string, 101> text;

        /** The text for a normalised value: the nearest sample, or the exact
            step of a discrete one. */
        const std::string& textFor (float normalised) const noexcept;
    };

    struct Catalogue
    {
        std::string identifier;
        std::string name;
        int latencySamples = 0;
        std::vector<Parameter> params;

        /** The identifier of the built-in test child, and its catalogue. */
        static const char* testGainIdentifier() noexcept { return "godot:test-gain"; }
        static Catalogue testGain();

        /** A bipolar guess from what the texts at the ends say. */
        static bool guessBipolar (float defaultValue, const std::string& atZero,
                                  const std::string& atOne);

        std::string toJson() const;
        static bool fromJson (const std::string& text, Catalogue& out, std::string& problem);
    };

    /*  The catalogues this machine has, by identifier, kept as one JSON file
        each under a folder the engine owns.

        THREADING: a mutex of its own, and a revision (PluginTable's idiom).
        The serve loop loads files on the tick thread when the show changes;
        the proxy host puts a child's report in from the message thread
        (PR 9a.7); the tree reads on the tick thread when it rebuilds and
        compares the revision at every publish. find() hands out a
        shared_ptr rather than a pointer, so a catalogue replaced while a
        rebuild is reading it stays alive until the rebuild is done. */
    class CatalogueStore
    {
    public:
        explicit CatalogueStore (std::string folder);

        /** The folder every file goes under. */
        const std::string& folder() const noexcept { return root; }

        /** What is known for an identifier, or null. Never reads the disk. */
        std::shared_ptr<const Catalogue> find (const std::string& identifier) const noexcept;

        /** Moves with every put that changed something, so a reader that
            cached what it read can tell. */
        std::uint64_t revision() const noexcept;

        /** Reads the identifier's file if one exists and it is not held yet.
            The built-in test catalogue is always known. */
        bool ensureLoaded (const std::string& identifier);

        /** Holds a catalogue and writes it to disk. Answers whether anything a
            reader could see changed. */
        bool put (const Catalogue& catalogue);

        /** Where the identifier's file is, whether or not it exists. */
        std::string fileFor (const std::string& identifier) const;

        std::size_t size() const noexcept;

    private:
        std::string root;
        mutable std::mutex mutex;
        std::map<std::string, std::shared_ptr<const Catalogue>> held;
        std::uint64_t revisionCount = 0;
    };
}
