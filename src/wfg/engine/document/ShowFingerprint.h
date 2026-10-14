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
    THE SAME SHOW, CHECKED AND NOT ASSUMED (namespace draft §58, RD.10; the
    author's AFU, "Compare, warn, offer the text").

    A main and its backup each open their own copy of the show folder, and the
    link between them carries what the main APPLIES, never the show itself
    (§58.1). Nothing therefore says the two copies were the same to begin with,
    and an edit the main made while the backup was away - switched off, or in
    the moment between its seat turning `following` and its first request
    reaching the main (§58.7, "Found 2026-10-12") - is never streamed to it. So
    each machine works out a FINGERPRINT of its show and of its media, and the
    two are compared.

    WHAT SOMEONE DECIDED, AND ONLY THAT (PRD §4.10). The fingerprint hashes the
    canonical show text with every row the parameter table marks `machine` left
    out (`CanonicalXml::writeDecided`; RD.3, AGC) - two machines with different
    interfaces, MIDI devices, serial paths and displays, playing the same show,
    agree - and state.xml left out with it: a standby is where the operator
    stands, and a GO moves it on one machine a delay before the other. The
    device descriptions in the bundle's `namespaces/` are IN, as they are in the
    log header's `Bundle::contentHash`: a session that read one description
    behaves differently from one that read another.

    THE SAME WORDS AS THE FILE, so the same numbers. Canonical XML writes every
    number through `osc::formatDouble` and sorts every element's attributes by
    name, so a French machine and an English one, and a show.xml somebody wrote
    by hand with its attributes in another order, hash alike.

    UNITS, so a difference can be said. A fingerprint says only "the same" or
    "not"; a backup told its show differs needs to know WHERE, in a sentence -
    "cue 4 Rain changed". So the show is also cut into units - the root, each
    container under it, each cue, and each other object with an identity that is
    not part of a cue (a list, a device, a bus) - each with a hash of its own
    rows and of whatever it holds that is not a unit of its own (a cue's routes,
    feeds and sections are the cue's), and the order of the units directly
    inside it. Two machines' units, laid side by side, name what changed, what
    was added, what was removed, and what was moved. They are sent on request,
    identifiers and a hash each - never the show (§58.6's "per-cue hashes").

    THE MEDIA. Every file the show names - a sound cue's, a movie's, a still's
    - by the path the show writes, with whether it is there, its size, when it
    was last written, and its content hash where this machine's analyser has one
    (`audio::MediaRecord::contentHash`, a sound's). Sounds never travel (§58.1);
    a file one machine has and the other lacks, or has otherwise, is said. A
    digest of the list rides beside the fingerprint, and the list itself is
    sent on request.

    PURE AND ANY-THREAD. Everything here reads what it is given - a tree nobody
    else is writing, a folder, a snapshot of the analyser's records - and
    nothing else, so it runs on a thread of its own from a copy of the show the
    tick thread hands over (pair/ShowCheck.h's `Fingerprinter`), never on the
    tick: the canonical text of a five-hundred-cue show is more than a tick's
    worth of work in the Debug build.
*/

#include <wfg/engine/audio/MediaInfo.h>

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc
{
    /*  ONE UNIT OF A SHOW (above): `key` is its identifier, the element's name
        for a container of the root (`Audio`, `Lists`), `Show` for the root, and
        `namespaces/<file>` for a device description; `hash` covers its own rows
        and what it holds that is no unit of its own; `order` is the units
        directly inside it, each `<path>/<key>` with the path of the elements
        between (`Header/B3N8R5TW`), space-separated; `label` is how an operator
        reads it - `4 Rain`, `list Show`, `device Desk`, `Audio`, `desk.json`. */
    struct ShowUnit
    {
        std::string key;
        std::string element;
        std::string hash;
        std::string order;
        std::string label;

        bool operator== (const ShowUnit& other) const noexcept
        {
            return key == other.key && element == other.element && hash == other.hash && order == other.order
                && label == other.label;
        }
    };

    /*  ONE FILE THE SHOW NAMES: the path as the show writes it (relative to
        `media/`), what it is - `sound`, `movie` or `picture` - whether it is on
        this machine where the show would look for it (`audio::resolveMediaPath`),
        its size in bytes and when it was last written, in milliseconds since
        1970, and the analyser's content hash when it has one. */
    struct MediaEntry
    {
        std::string path;
        std::string kind;
        bool present = false;
        std::int64_t size = 0;
        std::int64_t modified = 0;
        std::string hash;

        bool operator== (const MediaEntry& other) const noexcept
        {
            return path == other.path && kind == other.kind && present == other.present && size == other.size
                && modified == other.modified && hash == other.hash;
        }
    };

    /*  A DEVICE DESCRIPTION, a file of the bundle's `namespaces/`: its name and
        its bytes. */
    struct Description
    {
        std::string name;
        std::string bytes;

        bool operator== (const Description& other) const noexcept
        {
            return name == other.name && bytes == other.bytes;
        }
    };

    //==========================================================================
    /*  A SHOW'S FINGERPRINT AND EVERYTHING IT WAS MADE FROM, as one machine
        worked it out: `revision` is the show revision of the tree it was made
        from (`ShowDocument::showRevision`), which is how a reader knows it is
        still the show's; `decided` the text it hashes, which is also what a
        backup is handed when it takes this show (RD.10's `pair.adoptShow`). */
    struct ShowFingerprint
    {
        std::uint64_t revision = 0;
        std::string fingerprint;
        std::string decided;
        std::vector<Description> descriptions;
        std::vector<ShowUnit> units;
        std::vector<MediaEntry> media;
        std::string mediaDigest;
    };

    /*  THE WHOLE OF IT, from a tree (a copy, nobody else writing it), the
        bundle folder its descriptions are read from (none: no descriptions),
        the `media/` folder its files are found in, and the analyser's records
        (null: no hash for anything). */
    ShowFingerprint fingerprintShow (const juce::ValueTree& show, std::uint64_t revision,
                                     const juce::File& bundle, const std::string& mediaFolder,
                                     const audio::MediaRecords* analysed);

    /*  THE FINGERPRINT OF A DECIDED TEXT AND ITS DESCRIPTIONS: lower-case hex
        SHA-256 over a format line, the text, and each description - its name,
        its length and its bytes, in the order given (`descriptionsIn` sorts
        them). The name is in so a renamed description changes it, the length
        so no two files can be run together into another pair. */
    std::string fingerprintOf (std::string_view decided, const std::vector<Description>& descriptions);

    /*  The files of `<bundle>/namespaces/`, not its folders, sorted by name -
        exactly what `Bundle::contentHash` reads of it. None when there is no
        such folder. */
    std::vector<Description> descriptionsIn (const juce::File& bundle);

    /*  THE UNITS OF A SHOW TREE, root first and then in document order,
        followed by one unit for each description. */
    std::vector<ShowUnit> showUnitsOf (const juce::ValueTree& show, const std::vector<Description>& descriptions);

    /*  EVERY FILE THE TREE NAMES, once each, sorted by path: a `Media` cue's
        `file` is a sound, a `Video` cue's a movie or a still by its name. */
    std::vector<MediaEntry> mediaListOf (const juce::ValueTree& show, const std::string& mediaFolder,
                                         const audio::MediaRecords* analysed);

    /*  The list's digest, what rides beside the fingerprint: lower-case hex
        SHA-256 over each entry - its path, kind, presence and size, and its
        content hash, or when there is none the time it was last written. */
    std::string mediaDigestOf (const std::vector<MediaEntry>& media);

    //==========================================================================
    /*  THIS MACHINE'S ROWS PUT BACK INTO ANOTHER MACHINE'S SHOW (RD.10,
        `pair.adoptShow`): every row the table marks `machine` on every node of
        `taken` is set to what the same node of `own` holds - the node with the
        same identifier, or for one without, the one of the same element at the
        same place among its siblings under the same parent - and taken away
        where `own` has no such node, or the node does not hold it. So a backup
        that takes its main's show keeps its own interface, its MIDI ports'
        devices, its serial paths and its displays, and a port the main added
        while the backup was away arrives bound to no device here, as it would
        have had the backup followed the edit (a machine row never travels). */
    void putMachineRowsBack (juce::ValueTree taken, const juce::ValueTree& own);
}
