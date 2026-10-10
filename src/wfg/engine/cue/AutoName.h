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

/*  WHAT A CUE IS CALLED WHILE NOBODY HAS CALLED IT ANYTHING (namespace draft
    §53). The author, 2026-10-10: "Could cues with a target like fade, stop and
    the like have an automatic name when the target is assigned [...] Same for
    OSC and MIDI cues, they could have a reference to their content. Once the
    default name has been edited by the user and until it is clear the default
    name doesn't come back. Clearing the name puts back the default name that
    can change to a new default if the content or target changes."

    THE AUTOMATIC NAME IS NEVER STORED. `name` holds what somebody typed, and
    an empty `name` is the cue saying "call me by what I do" - so the rule the
    author asked for is not a flag to keep in step but what an empty string
    already means: typing a name keeps it whatever the target becomes, and
    clearing it lets the cue be called by its content again, today's content
    and not the content it had when it was named. §4.10: the document holds
    what someone decided, and this is the machine reading the decisions back.

    WORKED OUT FROM THE SHOW ALONE, so every client - the window, the web
    console, a D700's scribble strip - says the same words: `cue/autoName` is
    published beside `name`, and a client shows `name`, or `autoName` when
    `name` is empty.

    WHAT EACH KIND IS CALLED, in English as the rest of the window is:
    - a fade, after what it fades: "Fade Intro music", "Fade out Intro music"
      when it takes the level to silence and moves nothing else, "... and stop"
      in front when it stops what it faded; a DCA's fade "Fade DCA Band";
    - a transport cue, after its verb and its target: "Stop Intro music",
      "Stop Intro music after this round", "Advance Intro music to Verse",
      "Enable ...", "Jump to ... and Go", "Rec Voice";
    - a start cue: "Start Intro music";
    - an OSC cue, after its first message: "/mixer/ch/1/fader 0.5", with
      "(+2)" when it sends two more;
    - a MIDI cue, after its message and its port: "Program change 5, ch 1 on
      Desk";
    - a sound, a picture and a movie, after its file: "Intro music" for
      media/Intro music.wav; a capture after its video input, a fill and a mask
      by what they are; a mic cue after its input.
    A cue whose target or content is not set yet has no automatic name - a fade
    is named once its target is assigned. A group, a memo and a process cue
    have none: what they do is what somebody writes.

    THE TARGET IS NAMED AS IT IS SHOWN: its own name, or its automatic one -
    a fade of a sound nobody named reads as the sound's file - or, when it has
    neither, its number ("cue 12"), or its kind. A chain that loops back on
    itself (a fade of a fade of the first) stops after a few links and names
    the cue by its number.
*/

#pragma once

#include <map>
#include <string>

#include <juce_data_structures/juce_data_structures.h>

namespace wfg::cue
{
    class AutoNames
    {
    public:
        /*  Indexes every identified node under the show's root once, so naming
            every cue of a show is one walk and not one per target. */
        explicit AutoNames (const juce::ValueTree& showRoot);

        /** The cue's automatic name, or empty when it has none. */
        std::string of (const juce::ValueTree& cue) const;

        /** What the cue is shown as: its name, else its automatic name. */
        std::string shownAs (const juce::ValueTree& cue) const;

    private:
        std::string of (const juce::ValueTree& cue, int depth) const;
        std::string targetWords (const std::string& targetId, int depth) const;
        std::string nameOf (const std::string& id) const;
        juce::ValueTree find (const std::string& id) const;

        std::map<std::string, juce::ValueTree> byId;
    };
}
