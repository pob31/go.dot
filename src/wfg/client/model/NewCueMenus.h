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
    The lists four of the new-cue buttons open, and what each line makes.

    The author (2026-09-27): "When clicking on the 'Add group' button I would
    like to have the various group types displayed in a vertical list", and
    the same for "+ transport", with Start as one of its lines rather than a
    button of its own; then "+ midi" and "+ mic", the other two buttons that
    stand for several things. A line makes its cue BORN WITH ITS SETTINGS -
    `cue.create`'s pairs - so one click is one record and one Undo.

    THE GROUP LIST OFFERS TO TAKE THE PICKED CUES, and offers it in the list
    itself rather than in a question afterwards: PRD §3.6 says grouping a
    selection is one keystroke, "if it is a dialog, people will resent it by
    the second tech". Its first part makes the group around the picked cues,
    its second an empty one where any new cue goes. The sampler joins the
    first part only when a sampler could hold every picked cue - a sound or a
    picture, a sound locked to a movie only with its movie (namespace draft
    §49) - because a sampler plays those members and nothing else.

    A TRANSPORT OR START CUE IS BORN AIMED at the picked cue, when there is
    one: a stop with no target stops nothing, and the cue somebody has just
    picked is the one they mean far more often than not. "Fade out and stop"
    is not in the transport list - the author gave it to "+ fade" - though
    the verb stays in the engine and in the inspector.

    What is decided here is only what is OFFERED. Whether cues may be grouped
    is the engine's to judge (ShowDocument::groupSelection); `wrapOf` mirrors
    its refusals so the list does not offer a click it could have known would
    be refused. std only, like the rest of model/.
*/

#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /** What a cue is born with: each row's name and its value as text. */
    using Settings = std::vector<std::pair<std::string, std::string>>;

    /*  One line a list offers: the words, the cue it makes, and the heading
        it stands under - a change of heading starts a new part of the list. */
    struct Choice
    {
        std::string label;          ///< "Timeline group"
        std::string explanation;    ///< after a dash, or empty
        std::string kind;           ///< the word `cue.create` takes
        Settings settings;          ///< what the cue is born with
        std::string section;        ///< the heading it stands under, or empty

        /*  Offered around picked cues only when a sampler could hold every one
            of them: a sound or a picture, a locked sound with its movie
            (namespace draft §49). */
        bool playableOnly = false;

        /** Born aimed at the picked cue: `target` is added when one is picked. */
        bool aimed = false;

        /*  BORN FROM A CUE TEMPLATE (namespace draft §38), by its identifier:
            `cue.createFrom` rather than `cue.create`, and for a media cue the
            files chosen first. Empty for every other line. */
        std::string cueTemplate;
    };

    /*  THE SHOW'S CUE TEMPLATES (namespace draft §38), in the show's order:
        each one's name, its kind - "media", or a picture's source: "movie",
        "picture", "fill", "mask" - and in words what it carries. */
    struct CueTemplateRow
    {
        std::string id;
        std::string name;
        std::string kind;
        std::string parts;      ///< the fragment, as the engine keeps it

        std::string label() const;          ///< the name, else "Template"
        std::string kindWord() const;       ///< "media cue", "movie", "fill"...
        std::string carriesWords() const;   ///< "level, DCA, routing, EQ, 2 sends, 1 effect, speed"
    };

    std::vector<CueTemplateRow> readCueTemplates (const tree::TreeSnapshot&);

    /*  THE TEMPLATES A PICKED CUE OF THIS KIND MAY TAKE: a media cue's the
        media templates, a video cue's those of its own source. */
    std::vector<CueTemplateRow> templatesFor (const std::vector<CueTemplateRow>&, const std::string& cueKind,
                                              const std::string& source);

    /*  THE "+ media" LIST (namespace draft §38): files chosen from the disk, as
        the button always did, then each media template - the files chosen
        next, each cue born from it. */
    std::vector<Choice> mediaChoices (const tree::TreeSnapshot&);

    /** Whether the button for this kind opens a list rather than making a cue. */
    bool opensList (const std::string& kind);

    const std::vector<Choice>& groupChoices();
    const std::vector<Choice>& transportChoices();
    const std::vector<Choice>& midiChoices();

    /*  One line per named input and rack channel that can take it - a mono
        input through a mono or a mono-to-stereo channel, a stereo one through
        a stereo channel; a shared channel is never claimed, so never offered -
        and a last line that makes a mic cue with neither, as the button did. */
    std::vector<Choice> micChoices (const tree::TreeSnapshot&);

    /*  Phase 8a: a fill on each canvas the show has, and a last line on no
        canvas. A mask and a picture join the list when they draw. */
    std::vector<Choice> videoChoices (const tree::TreeSnapshot&);

    /*  WHETHER THE PICKED CUES CAN BE PUT IN A NEW GROUP, and how many the
        group would hold: a picked group carries its picked members along, so
        they count once. `why` is the sentence the list shows in place of the
        offer when they cannot - picked cues in two lists are the case a hand
        meets. */
    struct Wrap
    {
        std::vector<std::string> cues;  ///< the picked ids, as picked
        int count = 0;
        bool allPlayable = false;       ///< every one a sampler could hold (§49)
        std::string why;

        bool possible() const noexcept  { return count > 0 && why.empty(); }
    };

    Wrap wrapOf (const tree::TreeSnapshot&, const std::vector<std::string>& picked);

    /*  A LINE OF A LIST AS IT IS DRAWN: a heading, a line to click, a
        greyed sentence, or a rule. A line to click names the choice it makes
        by its index in the choices the list was built from, and whether it
        makes the group around the picked cues. */
    struct MenuLine
    {
        enum class Kind { header, item, note, separator };

        Kind kind = Kind::item;
        std::string text;
        int choice = -1;
        bool wrap = false;
    };

    /*  `destination` is where an empty new cue lands, in the words the
        buttons' tooltips use: "after Rain", "at the end of the list". */
    std::vector<MenuLine> groupMenu (const Wrap&, const std::string& destination);

    /*  `aimName` is the picked cue's name, or empty when nothing is picked,
        in which case the cue lands at `destination` with no target. */
    std::vector<MenuLine> transportMenu (const std::string& aimName, const std::string& destination);

    std::vector<MenuLine> midiMenu (const std::string& destination);

    /*  The lines for `choices` as `micChoices` made them, with a greyed
        sentence saying where inputs and channels are made when the show has
        none - the list is never only the last line with no word why. */
    std::vector<MenuLine> micMenu (const tree::TreeSnapshot&, const std::vector<Choice>& choices,
                                   const std::string& destination);

    /*  And for `videoChoices`, saying where a canvas is made when the show
        has none. */
    std::vector<MenuLine> mediaMenu (const std::vector<Choice>& choices, const std::string& destination);

    std::vector<MenuLine> videoMenu (const tree::TreeSnapshot&, const std::vector<Choice>& choices,
                                     const std::string& destination);

    /*  The word a transport row shows in the list's kind column, as a group
        row shows its mode (the author, 2026-09-27): stop, fade out, member,
        round, advance, rec, loop, overdub, clear, enable, disable, jump, and
        jump+go for a jump with "and Go" on (2026-10-05). Eight characters at
        most - the column's width. */
    std::string verbWord (const std::string& verb, bool andGo = false);
}
