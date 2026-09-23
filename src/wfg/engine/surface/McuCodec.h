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

/*  A MACKIE CONTROL SURFACE, AS BYTES. What a surface sends becomes a typed
    event; what Go.dot wants a surface to show becomes the bytes that leave.

    THE PURE LAYER UNDER THE SURFACE BRIDGE. No thread, no state, no port and
    no JUCE: every function here is a function of its arguments and nothing
    else, so every message a surface can send, and every message Go.dot sends
    one, is checked byte for byte on a machine with no MIDI interface - and
    every CI runner is one. The bridge above owns the ports, the banks and what
    a button means; this file owns the bytes.

    STANDARD MACKIE CONTROL, WITH THE ASPARION D700'S EXTENSIONS ON TOP. The
    D700 is a textbook MCU implementation (docs/godot-asparion-d700-protocol-0.1.md
    §2.1), and its colour, its three-row display and its fine rings are more
    messages on the same cable rather than a second protocol (§5) - so they are
    more functions in the same file, each named for the D700. The byte tables
    are docs/D700_CONTROL_GUIDE.md's, and tests/McuCodecTests.cpp writes its
    fixtures from there rather than from what this file happens to produce.

    A DECODER READS SOMEBODY ELSE'S BYTES, so it refuses rather than guesses.
    A wrong length, a data byte with its top bit set, a channel MCU does not
    use, or a SysEx that is not Mackie's decodes to nothing - never to the
    nearest plausible event, because a fader that moved on a misread byte is a
    real change nobody made.

    AN ENCODER CLAMPS A VALUE AND REFUSES AN ADDRESS. A level past the top of a
    fader's travel still means "as far as it goes", and these values are
    computed from the show, never typed, so the nearest legal one is what was
    meant. A strip, a note or a row that does not exist is another matter:
    clamping it would paint the element next door, which is how a surface goes
    wrong quietly. So an address out of range answers an EMPTY Bytes, and an
    empty Bytes sends nothing. (MidiMessages.h refuses a cue's out-of-range
    values instead, for the opposite reason: a person typed those.)

    THE BANK IS THE PORT. A sixteen-fader D700 is two banks of eight, each on
    its own MIDI port pair and each numbering its strips 0-7 (control guide
    §1.1, §2). Nothing inside a message says which bank it belongs to, so
    nothing here does either: a strip is always 0-7, and which eight is the
    bridge's business.
*/

#pragma once

#include <wfg/engine/midi/MidiSink.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wfg::surface
{
    using midi::Bytes;

    //==========================================================================
    /*  MACKIE'S BUTTONS, BY THE PROTOCOL'S OWN NOTE NUMBERS, and named for the
        standard layout rather than for what a given surface prints on a cap.

        What a button MEANS belongs to a profile, written later. On the D700,
        note 0x38 - MCU's F3 - is the master dial's press, and the `*` button
        is 0x36 (F1) under the Mackie preset but 0x5A under Reaper's (control
        guide §1.2). A D700 meaning written in here would make the codec wrong
        for every other Mackie surface, and wrong for the D700 itself the day
        somebody changes its preset.

        `touch` is the automation-mode button. A fader being touched is not a
        button at all: it arrives as McuEvent::Kind::touch.
    */
    enum class Button
    {
        none,

        //  One of each on every strip, at 0x00, 0x08, 0x10, 0x18 and 0x20
        //  plus the strip.
        rec, solo, mute, select, vpotPress,

        assignTrack, assignSend, assignPan,             // 0x28..0x2A
        assignPlugin, assignEq, assignInstrument,       // 0x2B..0x2D
        bankLeft, bankRight, channelLeft, channelRight, // 0x2E..0x31
        flip, globalView, nameValue, smpteBeats,        // 0x32..0x35

        //  F1..F8, 0x36..0x3D, `index` 0..7.
        function,

        //  0x3E..0x45, `index` 0..7: MIDI tracks, inputs, audio tracks, audio
        //  instruments, aux, busses, outputs, user.
        view,

        shift, option, control, alt,                    // 0x46..0x49
        readOff, write, trim, touch, latch, group,      // 0x4A..0x4F
        save, undo, cancel, enter,                      // 0x50..0x53
        marker, nudge, cycle, drop, replace, click,     // 0x54..0x59
        globalSolo,                                     // 0x5A
        rewind, forward, stop, play, record,            // 0x5B..0x5F
        up, down, left, right, zoom, scrub,             // 0x60..0x65
        userA, userB                                    // 0x66, 0x67
    };

    /*  One button, by its standard name: which, and for the kinds that come
        eight wide, which of the eight - the strip for the five per-strip
        buttons, the position for `function` and `view`. -1 for every button
        that is one of a kind. It is what a profile's own table is keyed on. */
    struct ButtonId
    {
        Button button = Button::none;
        int index = -1;

        bool operator== (const ButtonId&) const = default;
    };

    /** The button a note is, or `none` for a note that is not one. A fader's
        touch notes, 0x68..0x70, are not buttons: they arrive as touches. */
    ButtonId buttonForNote (int note) noexcept;

    /** The note a button is. -1 for `none`, for an index outside 0..7, and for
        an index on a button that is one of a kind - a ButtonId `buttonForNote`
        never makes, so it names no note. */
    int noteForButton (ButtonId id) noexcept;

    //==========================================================================
    /*  WHAT ARRIVES FROM A MACKIE SURFACE, one message at a time. */
    struct McuEvent
    {
        enum class Kind
        {
            fader,                          // `strip` 0..7, `value` 0..16383: pitch bend E0..E7
            masterFader,                    // `value` 0..16383: pitch bend E8, the D700's volume knob
            touch,                          // `strip` 0..7, `down`: note 0x68 + strip
            masterTouch,                    // `down`: note 0x70, which the D700 never sends
            encoder,                        // `strip` 0..7, `value` a signed step: CC 0x10 + strip
            jog,                            // `value` a signed step: CC 0x3C
            button,                         // `id`, `down`
            hostConnectionQuery,            // `deviceId`, `serial`, `challenge`: SysEx command 01
            hostConnectionConfirmation,     // `deviceId`, `serial`: SysEx command 03
            hostConnectionError             // `deviceId`, `serial`: SysEx command 04
        };

        Kind kind = Kind::fader;

        /*  The strip a control belongs to: a fader, its touch, an encoder, or
            one of the five per-strip buttons. 8 for the master fader and its
            touch - the number `faderPosition` takes for the master, so echoing
            a fader back is `faderPosition (event.strip, event.value)` whichever
            kind it was - and -1 for everything else. */
        int strip = -1;

        int value = 0;          // a fader's position 0..16383, or a signed step
        bool down = false;      // a touch or a button: pressed, or released
        ButtonId id;            // a button

        /*  THE HANDSHAKE'S KINDS. The device id is what the unit says it is -
            0x14 a Mackie Control, 0x15 an Extender, and the D700's two banks
            report one each - and the serial is its seven bytes as sent, a
            stable identifier that survives USB renumbering (protocol §2.1). */
        std::uint8_t deviceId = 0;
        std::string serial;
        std::array<std::uint8_t, 4> challenge {};   // hostConnectionQuery
    };

    /*  One message as one event, or nothing for a message this protocol does
        not define. JUCE delivers a message whole and never in running status,
        so each is judged on its own.

        A NOTE-OFF AND A NOTE-ON OF VELOCITY NOUGHT ARE BOTH A RELEASE, and any
        other velocity is a press: surfaces send 127, some send less, and none
        means anything by the difference (control guide §3.3).

        AN ENCODER STEP IS SIGN-MAGNITUDE, NOT TWO'S COMPLEMENT: bit 6 is the
        direction and bits 0-5 the size, so 65 is one step anticlockwise. A
        two's-complement reader takes 65 as -63, and it is the single most
        likely bug in a new MCU integration (control guide §3.2).

        NOTES AND CONTROLLERS ARRIVE ON CHANNEL 1 ONLY, and pitch bend on
        channels 1 to 9 only; anything else is refused. */
    std::optional<McuEvent> decodeMcu (const Bytes& message);

    //==========================================================================
    /*  WHAT ARRIVES FROM A PAD CONTROLLER - the `midiPads` profile: presses,
        releases and pressure, on any channel.

        CLASSIFIED BY THE STATUS BYTE, so a note-on of velocity nought stays a
        note-on with value 0 and the bridge decides it is a release. That is
        the rule `midi::eventFrom` follows (src/wfg/engine/midi/MidiInputs.cpp)
        and for the same reason: JUCE reports such a message as a note-off, a
        pad controller means a release by either spelling, and what the wire
        said is what is reported - what it MEANS is the bridge's. */
    struct PadEvent
    {
        enum class Kind { noteOn, noteOff, polyPressure, channelPressure };

        Kind kind = Kind::noteOn;
        int channel = 1;        // 1..16, as every device prints it
        int note = -1;          // noteOn, noteOff and polyPressure; -1 for channelPressure
        int value = 0;          // the velocity, or the pressure, 0..127
    };

    /** One message as one pad event, or nothing: anything that is not a note
        or a pressure, and anything malformed. */
    std::optional<PadEvent> decodePads (const Bytes& message);

    //==========================================================================
    /*  TEXT. A display takes seven-bit ASCII; a show's names are UTF-8, and
        are often French - "Écho", "Entrée à jardin".

        So every letter of Latin-1 and of Latin Extended-A folds to its base
        letter, and the few that are two letters fold to both: Æ AE, Œ OE,
        Ĳ IJ, Þ TH, and ß ss, the one expansion that matters. A letter typed
        decomposed - an "e" followed by a combining acute, which is how a name
        copied from a macOS file name arrives - keeps its letter and loses its
        accent. Curly quotes, guillemets and dashes become their ASCII twins,
        and so do the few Latin-1 signs that have one - the degree sign of
        "N°" is an o. Every kind of space becomes a space, and an ellipsis
        becomes "...".

        A control character becomes a space. One that is invisible anyway - a
        soft hyphen, a zero-width joiner, a byte-order mark - becomes nothing,
        rather than a '?' in the middle of a word that shows none. Anything
        else becomes '?', one per character and never one per byte.

        MALFORMED UTF-8 NEVER CRASHES AND NEVER SWALLOWS WHAT FOLLOWS IT: each
        byte that is not part of a well-formed character becomes one '?', and
        reading resumes at the next byte.
    */
    std::string asciiFold (std::string_view utf8);

    /*  Exactly `width` characters: folded, then cut, or padded on the right
        with spaces.

        A DISPLAY WRITE REPLACES ONLY THE BYTES SENT (control guide §4.6), so a
        label shorter than its field leaves the previous one's tail on screen:
        "PORT-1" written over "S21 HIJACK" rendered as "PORT-1J" (protocol §4).
        Every field-sized encoder below writes its text through this. */
    std::string fitText (std::string_view utf8, std::size_t width);

    /** The fields' widths: an MCU display cell, and the D700's three native
        rows. What a layout needs to know about what fits. */
    inline constexpr std::size_t mcuCellWidth = 7;
    inline constexpr std::size_t d700RowWidth = 12;
    inline constexpr std::size_t d700Row3Width = 8;

    //==========================================================================
    //  What goes to a Mackie surface.

    /*  A motor fader's position: E0 + strip, then the value, least significant
        seven bits first. `strip` 0..7, or 8 for the master (E8); the value is
        clamped to 0..16383.

        ONE MESSAGE IS ONE MOVE AT FULL SPEED. Commanding the whole travel at
        once drives the motor into the end stop with no deceleration, and the
        bottom of the travel is where a parked channel lives - so the bridge
        interpolates a large jump, about twenty steps (control guide §4.1). It
        never clamps short of the ends to spare the stop: a fader that cannot
        reach -inf misrepresents the desk, which is worse than the wear. */
    Bytes faderPosition (int strip, int value14);

    /** A button's LED, as MCU's three velocities: 00, 01 and 7F. */
    enum class Led { off, flash, on };

    /*  90 note velocity, on channel 1. THE SURFACE HAS NO LOCAL FEEDBACK: a
        press lights nothing until the host echoes it, so an unlit button after
        a press means the host did not answer (control guide §4.2). */
    Bytes led (int note, Led state);

    /*  MCU's own V-Pot ring: B0 (0x30 + strip), then one byte holding the
        centre LED in bit 6, the mode in bits 4-5 and the position in bits 0-3.
        Position 0..11, where 0 lights nothing; mode 0..3 - a dot, boost/cut,
        wrap, spread. Both clamped. A D700 also takes the finer `d700Ring`. */
    Bytes ringMcu (int strip, int position, int mode, bool centre);

    /*  A meter: D0 ((strip << 4) | level), the level clamped to 0..14. The
        D700 shows twelve levels, 0..11 (control guide §4.7), and 0x0F is not a
        level at all but the peak-hold reset, which is `meterClearPeak`'s. */
    Bytes meter (int strip, int level);
    Bytes meterClearPeak (int strip);

    /*  MCU's display: F0 00 00 66 id 12 offset text F7 - two rows of 56
        characters, the lower at offset 0x38 (control guide §4.6). `offset`
        0..111; the text is folded, and cut where the buffer ends. It is
        written as given and not padded, which is `lcdCell`'s job. */
    Bytes lcd (std::uint8_t deviceId, int offset, std::string_view utf8);

    /*  One strip's cell of that display: `row` 0..1, `strip` 0..7, at offset
        row * 0x38 + strip * 7, and ALWAYS exactly seven characters of text. */
    Bytes lcdCell (std::uint8_t deviceId, int row, int strip, std::string_view utf8);

    /*  F0 00 00 66 id 00 F7: asks a surface to identify itself, which a Mackie
        surface answers with the message that opens the connection handshake.
        Safe on the D700 (protocol §6). */
    Bytes deviceQuery (std::uint8_t deviceId);

    //==========================================================================
    /*  THE D700'S OWN EXTENSIONS (control guide §4), each read from Asparion's
        published Bitwig script and confirmed on the unit. */

    /*  The device id every D700 write carries. Both banks accept it, as they
        accept 0x10, 0x11 and 0x15 on a display write (protocol §2.1): the bank
        is the PORT a message is sent to, never the id inside it. */
    inline constexpr std::uint8_t d700DeviceId = 0x14;

    /*  An encoder's ring as a position 0..127: B<mode> (0x30 + strip) value.
        THE MIDI CHANNEL IS THE MODE - 0 none, 1 filling outward from the
        centre, for a pan, 2 filling from the left, for a level - and the value
        is 0..127, not MCU's eleven positions (control guide §4.3). Mode
        clamped to 0..2, value to 0..127. */
    Bytes d700Ring (int strip, int value, int mode);

    /*  The top two rows, twelve characters each: F0 00 00 66 14 1A
        (strip * 12) (row + 1) text F7. THE ROW BYTE IS ONE-BASED ON THE WIRE,
        01 being the top row, while `row` here is 0 or 1 like every other index
        in this file; any other row sends nothing (control guide §4.5). */
    Bytes d700DisplayRow (int strip, int row, std::string_view utf8);

    /*  The third row, eight characters: F0 00 00 66 14 19 (strip * 8) text F7. */
    Bytes d700DisplayRow3 (int strip, std::string_view utf8);

    /*  All eight strips' track-number fields in one message: F0 00 00 66 14 17
        00, then eight numbers each clamped to 0..127, then F7. */
    Bytes d700TrackNumbers (const std::array<int, 8>& numbers);

    /*  An element's colour, as three note-ons at the element's own button note
        on channels 2, 3 and 4, the velocity being red, green and blue, each
        clamped to 0..127 - so an 8-bit colour is halved first. BLUE IS LAST,
        because the element refreshes when blue arrives (control guide §4.4).

        ONLY SEVENTEEN ELEMENTS CARRY RGB: each bank's eight V-Pots, notes
        0x20..0x27, and the master dial, 0x38. Any other note answers an EMPTY
        Bytes rather than three messages that would paint nothing, silently.

        THREE MESSAGES IN ONE Bytes, in the order they must leave. A sender
        that hands a Bytes to a port as a single message - which is what a
        `juce::MidiMessage` is - has to split it into its three first. */
    Bytes d700Colour (int note, int red, int green, int blue);

    //==========================================================================
    /*  Whether a SysEx is one the D700 is known to survive: F0 00 00 66, a
        device id, one of six command bytes, data bytes only, F7. The six are
        0x00 device query, 0x02 host connection reply, 0x12 MCU display, 0x17
        track numbers, and 0x19 and 0x1A the native display rows.

        A SWEEP OF THE OTHER COMMAND BYTES PUT THE D700'S DISPLAYS INTO A
        LOGO-ONLY STATE THAT NEEDED THE CONTROLLER RESTARTED (control guide §6,
        the second gotcha; protocol §6), and during a show that is a dead
        surface. So the bridge asserts that every outgoing SysEx passes this
        before it leaves, and every SysEx this file writes is tested against it.

        0x72, MCU's coarse colour, was established as safe too and is left out
        on purpose: nothing here sends it, and a command nobody sends has no
        business being allowed. False for anything that is not F0 00 00 66
        <id> <cmd> ... F7. */
    bool isSafeSysEx (const Bytes& message) noexcept;
}
