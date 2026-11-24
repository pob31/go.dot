# Go.dot — Multichannel files and outputs, Ambisonics first: notes before the work starts

**Draft 0.7**, 2026-11-24 (0.1 to 0.6 the same day; 0.2 set aside 0.1's format word on
outputs after the author's ruling quoted in §4, and 0.3 corrects 0.2's reading of that
ruling by his own clarification; 0.4 added his answers on summing and on the grid; 0.5 withdrew 0.4's summing patch at his word; 0.6 makes the fold a decoder on the cue, after
his ruling on how a wide file reaches a direct out or a mix; 0.7 adds the list of decoders he
asked for). Written from a conversation with the author and a read of the code
that day (Go.dot at `c3e0263a`, XOA at `e4c3fbd`, spatcore at `v0.4.0-50-g7bdf4ab`). Nothing
here is built and nothing was run: every claim comes from reading the source. Line numbers are
those of that day. **Check a claim against the code before relying on it.**

What is the author's and what is not:

- **Asked for by the author:** multichannel playback and outputs, prompted by a request for
  Ambisonic files, "in a flexible way without redoing everything from scratch".
- **Decided by the author** (2026-11-24): Go.dot does not become a spatial renderer, **but**
  "we should at least give a simple way to mix down a full Ambisonics stream to mono and
  stereo."
- **Decided by the author** (2026-11-24, later): direct outs and mixes stay as they are, mono
  and stereo; beside them a rawer set of outputs at the end of the patch, reached through a
  matrix as in QLab — a diagonal default the user adjusts, no output reserved for a use,
  matching the format left to the user at the receiving end. His words are in §4.
- Everything else is a finding or a recommendation. §6 lists what is his to decide.
- The PRD is not amended by this file. When the work starts: a namespace-draft section (the
  next free is **§150**, the next code **BKE**; stage codes **MC.** are unused), a PRD §3.9b
  amendment, never §4.

---

## 1. The short of it

The playback core is already written for any number of channels: a full matrix at the end
of every voice, a matrix of any size on a cue. Direct outs and mixes stay what they are, for
mono and stereo. What is added is **a plain set of outputs after them, reached through a
matrix**: a wide file plays there at its full count, on a diagonal the user adjusts. And a
wide file reaches a direct out or a mix only **through a decoder to mono or stereo**, set on
the cue. No output is given a meaning.

The one real engine change — voices of two widths, so that one wide bed does not make every
voice wide — can come last, or never, because the rest works by raising a show setting.

---

## 2. What already carries any width

| What | How | Where |
|---|---|---|
| An output | a `<Bus>`: a named run of `width` consecutive logical channels, any width | `SchemaTable.generated.h:4841-4912`; CSV rows 589-597 |
| The rig | one wide Tracktion device; logical to hardware by `audio/outputPatch`, 512 at most | `AudioHost.cpp:88-124`, `AudioSettings.h:15` |
| The end of every voice | `CueMatrix`: cue channels by rig outputs, plus one cue level | `audio/CueMatrix.*`, `CueOutputPlugin.*` |
| A cue's own matrix | `<Route bus gains>`, cue channels × bus width, any size | `ShowDocument.cpp:7320-7347` |
| A processor's input | `<Feed slot gains>` onto a `Mount/Slot` with its own `width` | CSV rows 611-612 |
| A voice's width | `Audio/@channelsPerTrack`, 1 to 64, **one value for the whole show**, fixed when the graph is built | `Console.cpp:3362`, `AudioHost.cpp:470-930` |
| A file's width | `media/channels`, read from the file at import | `Client.cpp:4920-4975` |
| EQ, dynamics, plugin places | built at the voice's width, 64 at most | `CueEq.h:68`, `CueDyn.h:69`, `ProxyPlugin.cpp:34` |
| Conversions | compressed sound to WAV, a movie's sound, bounce: all keep the source's count | `CompressedSound.cpp:95`, `Conversion.cpp:231` |
| Sound into a video card | up to 32 logical channels | `CardSound.h:61` |

So a first-order file (4 channels) should play today into a 4-wide output, once
`channelsPerTrack` is 4. A test already plays 8 channels into 64 outputs (M1,
`tests/AudioTests.cpp:995-1010`). **This has not been tried by ear.**

### How a cue reaches its outputs

`Runner::resolveRouting` (`cue/Runner.cpp:6652-7080`) flattens four kinds of destination into
coefficients for the matrix (`emit`, 6687-6757):

1. **Direct out** (`sound/directOut`) — no matrix stored; `spreadOf` (6816-6889) decides: mono
   to every channel of the bus; N channels onto a bus at least N wide, channel to channel; two
   onto one, half each (§42); anything else **refused** ("the cue is wider than its direct
   out").
2. **Route** — the cue's own matrix, coefficient by coefficient.
3. **Feed** — the same onto a processor's slot.
4. **Send** — one level to a mix bus or an aux, spread by `spreadOf`, then shaped by
   `applyPlacement` (`cue/SendPlacement.cpp:60-114`). The published word `send/placement` says
   which controls mean something: `pan` (1 into 2), `stereo` (2 into 2), `fold` (2 into 1),
   `none` otherwise.

Fades move the cue level, send levels and a send's pan, width and balance
(`FadeMoves.cpp:218-264`). They do not move Route or Feed coefficients.

### What the PRD already says

- §1, non-goals: "Not a spatial renderer. That is WFS-DIY / XOA, addressed over the network."
- §3.9b: "Width is explicit, never automatic"; "refuse silent downmix or upmix"; the routing
  matrix is "the most common way to handle multichannel outputs".
- Namespace draft §13.3 and §42 (XS): "A file wider than two onto a narrower output is still
  refused. Six channels onto two is a layout — which side a centre or an LFE goes to — and no
  rule here knows it; a Route says it coefficient by coefficient."
- §6.1, §6.2: file channel counts "closed… 64 channels".
- Rack classes: "wider comes later" (deferred).
- §142, BHZ, **open, the author's**: a VST3 above 8 channels fails the layout ladder and falls
  back to stereo; asking for "the Ambisonic order whose channel count it is" is proposed.

The fold asked for on 2026-11-24 does not break XS. XS refuses because *no rule knows the
layout*. A format word written by a person **is** that rule. Width stays explicit and nothing
is folded unasked.

---

## 3. Limits found

1. **Tracktion silences file channels above 32.**
   `AudioFileCache::Reader::readSamples` has `static constexpr int maxNumChannels = 32` and
   clears any destination whose source channel is 32 or more
   (`ThirdParty/tracktion_engine/.../audio_files/tracktion_AudioFileCache.cpp:931-957`). No
   patch of ours touches it. So the real ceiling today is **order 4** (25 channels), not
   order 7 (64), and the failure is silent. Read, not measured.
2. **64 is the ceiling everywhere else** (`CueEq.h:68`, `CueDyn.h:69`, `ProxyPlugin.cpp:34`,
   `ProxyLane.h:474`, `PluginHostChild.cpp:427`, the CSV range). Order 7. XOA runs order 10
   (121 channels). Raising it touches all of those: not recommended.
3. **A voice's width is show-wide.** One 16-channel bed makes every voice carry 16 buffers
   through EQ, dynamics and every plugin place. See MC.5.
4. **Edges that are one or two channels wide, and stay so:**

   | Edge | Where |
   |---|---|
   | Aux channels | `AudioHost.cpp:587, 1602, 1713`; `AuxPlugins.cpp` |
   | Rack channels and mic cues | `rackChannels = 2` (`AudioHost.cpp:1566`), class `mono \| monoToStereo \| stereo` |
   | Sampling takes | `Looper::maxChannels = 2` (`Looper.h:108`) |
   | Sidechain keys | `KeyReader` (`KeyReader.h:115-219`), `keyChannelsMax = 2` in the shared region |
   | The plugin window's own copy | first two channels (§146 BJK), `tapChannelsMax = 2` |
   | Pan, width, balance, swap on a send | one or two channels only |

   A wide cue cannot use these until each is widened, one at a time, when somebody needs it.
5. **Plugins on a wide voice.** A cue wider than its plugin passes dry
   (`plugin/LaneMapping.h`). Above 8 channels a VST3 is BHZ, open.
6. **One picture per file.** Peaks and timbre fold every channel into one
   (`Peaks.cpp:147-162`, `Timbre.cpp:510-530`); the strip meter is the loudest channel. For an
   Ambisonic file the sum of all channels is not a meaningful picture; the first channel is.
7. **The dynamics detector** folds more than two channels to two (`CueDyn.cpp:203-222`). The
   gain is one for all channels, which is right for a field.
8. **The silent placeholder** `silence.wav` is reused whatever width is asked
   (`AudioHost.cpp:2405-2440`): a 2-channel one left on disk serves a graph rebuilt wider.
9. **No fixture sets `channelsPerTrack`**: every test and the three byte-exact goldens rely on
   the default of 2.

Two worries that turned out not to be:

- A WAV with a speaker mask (5.1, 7.1) is **not** reordered: Tracktion numbers channels in
  file order (`tracktion_ChannelConfiguration.cpp:176-189`). Worth one test that pins it.
- The plugin region is **per child**, each with its own channel count, so a second voice width
  needs no change of the region's format.

---

## 4. The design

### The author's ruling (2026-11-24, after draft 0.1)

> "For multichannel, right now I'd say this be at the end of the patch and managed by the user
> in a similar fashion to what happens in QLab. There the user is presented with diagonal
> default patches and makes the adjustments. No channel is reserved for a specific use. This
> way we don't squat outputs for a single use. It's up to the user to match the format/patch
> on the receiving end."

And, correcting draft 0.2, which had laid the diagonal across the existing outputs:

> "What I meant is keep things as they are for the direct outs (mono/stereo) and mix
> (mono/stereo) and have a more raw set of outputs accessed through a matrix."

So:

- **Direct outs and mixes are untouched.** They stay the mono and stereo outputs they are,
  with their menus, sends, pan and width, fades and surface pages. A mono or stereo file is
  imported and routed exactly as today.
- **A raw set of outputs comes after them** in the patch: plain numbered channels, with no
  kind, no pairing and no meaning.
- **A cue reaches them through a matrix**, file channels against raw outputs, which opens on a
  diagonal (file channel 1 to raw output 1, 2 to 2…) and which the user adjusts.
- **Nothing is reserved.** Tonight's bed and tomorrow's 5.1 use the same raw outputs; no
  "Ambisonic output" squats sixteen channels.
- **Go.dot checks nothing about meaning.** Whether what arrives at XOA is the order XOA
  expects is the user's to match there.

This sets aside draft 0.1's format word on outputs and slots and its table of matching rules
(kept in §9 for the record only), and draft 0.2's diagonal across direct outs and mixes.

### What the ruling needs *(proposed)*

1. **The raw set, in the document.** The smallest way found: a third `bus/kind` beside
   `direct` and `mix` (CSV row 590) — one bus of that kind, as wide as the user says, packed
   last so that it sits at the end of the patch. Everything under it already exists: a bus of
   any width, its place in `audio/outputPatch` (CSV row 563), a `<Route>` holding a matrix of
   cue channels by bus width (`ShowDocument.cpp:7320-7347`), the engine flattening it
   (`Runner.cpp:6687-6757`). Whether it is one such bus or may be several, and its name on
   screen, are open (§6).
2. **Kept out of the mono and stereo machinery.** A raw bus appears in no direct-out menu and
   no send list, takes no pan or width, and has no page on a surface. Whether it has a trim
   and a DCA like other outputs is open (§6).
3. **The diagonal.** Today's default (`ShowDocument::defaultMediaRoute`,
   `ShowDocument.cpp:2673-2703`; the command `route.default`, `DocumentCommands.cpp:427`)
   writes an identity onto the lowest bus. For a file wider than two it writes the diagonal
   onto the raw set instead; channels beyond the raw set's width get no crosspoint, and the
   window says so. The window's own import (`routeImportedMedia`, `Client.cpp:4920-4975`)
   does the same for a wide file, where today it gives a direct out that is then refused at
   GO ("the cue is wider than its direct out"). A wide file in a show with no raw outputs:
   said plainly at import, with the way to add them.
4. **A grid to adjust it — to be built.** QLab shows cue channels against outputs. Go.dot has
   none: the author, 2026-11-24, "There are only the faders for the sends to the mix channels
   so far" (and the only matrix component in `src/wfg/client` is the hardware patch in the
   show settings, spatcore's `PatchMatrixComponent`). So this is the stage that makes the rest
   usable, and the largest: a foot panel, file channels down, raw outputs across, a level per
   crosspoint. Sliding the whole diagonal along (start at raw output 9) as one gesture is
   worth having.
5. **Summing: at the outputs, never at the patch.** The author, 2026-11-24, in order: "I would
   say the mixing is a possibility. Things should stay flexible."; "You have to trust the
   operator to be a responsible technician. Yes summing should be possible. It's up to the
   user to manage this."; and then, settling where: **"No summing at the patch level is
   best."**
   - *Cues are summed on an output, and that is already there.* Every output, direct or mix,
     is a summing point: the schema's own words for `bus/kind` are "a second cue sent to a
     direct out is mixed there like anywhere else rather than refused". His case for it: "if
     for instance a media cue is crossfading with another over the same spatial sound object
     then it makes sense", even if the wording of a direct out does not push towards it. The
     raw set sums cues the same way, the matrix at the end of every voice being additive.
     Nothing guards against it and nothing should: it is the operator's to manage.
   - *The hardware patch stays one to one.* `readPatch` (`audio/AudioSettings.cpp:20-43`)
     refuses a hardware channel named twice and `patchOutputs` (`:78-91`) copies; both stay
     as they are. (Draft 0.4 proposed making the patch sum. Withdrawn.)
   - *What follows:* each raw output takes a hardware channel of its own, beside those of the
     direct outs and mixes. "No channel reserved" then holds for the *use* of the raw outputs
     — any cue, any content — not for the hardware under them.
   - *Closed by the author the same day:* "Direct outs and mix channels stay as they are and
     only going through a decoder to mono or stereo will allow wider files to play through
     them. Wider files play at their full count in the matrix end of the patch." So the
     matrix runs over the raw set only, never over the channels of a direct out or a mix.
6. **A mono or stereo cue into the raw set** costs nothing more — a Route of 1 or 2 rows — and
   is how QLab users expect to reach any output. Open (§6).

### The decoder to mono and stereo

The author, 2026-11-24, first: "a simple way to mix down a full Ambisonics stream to mono and
stereo"; and then, saying where it sits: "Direct outs and mix channels stay as they are and
only going through a decoder to mono or stereo will allow wider files to play through them."

So a wide file has two ways out, and only two:

- **at its full count**, through the matrix, onto the raw set;
- **decoded to mono or stereo**, and from there onto direct outs and mixes like any mono or
  stereo cue.

*(Proposed, the mechanism.)* The decoder is **a choice on the cue**: none, or Ambisonic to
mono, or Ambisonic to stereo, each in AmbiX or FuMa. With a decoder chosen, the cue presents
one or two channels to everything that deals with direct outs and mixes: the direct-out menu
accepts it, a send's pan or width, balance and swap act on it, fades move them, the surfaces
show their usual pages. Nothing in that machinery learns anything new, because what it sees is
a mono or a stereo cue. In the engine it is one small matrix (file channels by one or two)
multiplied into the coefficients `spreadOf` already produces, before they reach `emit`
(`Runner.cpp:6687-6757`, `6816-6889`); the voice still carries the file's full width, so the
raw set can be fed from the same cue at the same time.

This is one new attribute on the media cue, default none and not written — and it is the
choice of a decoder, something someone decided (constraint 10), not a label on the file. The
same word also tells the waveform, the timbre colour and the dynamics detector that the first
channel is the one to look at (§3, items 6 and 7).

A wide file with no decoder and a direct out or a send: refused as today ("the cue is wider
than its direct out"), and the words should now name the two ways out.

AmbiX: channel 0 is W (everything, from every direction), channel 1 is Y (left minus right),
channel 2 is Z (up minus down), channel 3 is X (front minus back). In SN3D each of them peaks
at 1 as W does. A virtual microphone aimed along a direction is `p·W + (1−p)·(that
direction's component)`, and is at unity on its axis whatever `p`.

- **Mono: W alone, at unity.** An omnidirectional microphone: every source at the level it was
  encoded.
- **Stereo: two cardioids back to back**, aimed left and right:

  ```
  L = ½·W + ½·Y
  R = ½·W − ½·Y
  ```

  A source hard left is full on the left and silent on the right. A source in the centre is
  6 dB down on each side. Nothing exceeds unity. And `L + R = W`: the stereo fold summed is
  exactly the mono fold. One coefficient is negative; no test yet says the matrix passes one.

- **FuMa** — the table is XOA's (`D:\dev\XOA\Source\DSP\AmbiConventions.h`,
  `fumaToAmbixTable`): W is the file's first channel at −3 dB, Y its third. Mono is `√2·W`;
  stereo is `(√2/2)·W ± ½·Y`.

### Which decoders

The author, 2026-11-24: "Ambisonics or LCR, 4.0, 5.1, 7.1, ATMOS if there's an available codec
for it."

All but the last are a table of coefficients each, file channels by one or two, so they are
the same mechanism as the Ambisonic one and cost a row apiece. The usual downmix (ITU-R
BS.775, **to be checked against the text before it is built**) keeps left and right at unity
and brings centre and surrounds in 3 dB down:

| Decoder | File channels, in a WAV's order | To stereo | To mono |
|---|---|---|---|
| Ambisonic, AmbiX | W Y Z X … | above | W |
| Ambisonic, FuMa | W X Y Z … | above | √2·W |
| LCR | L R C | L + 0.707·C, R + 0.707·C | L + R + C, scaled |
| 4.0 (quad) | L R Ls Rs | L + 0.707·Ls, R + 0.707·Rs | the four, scaled |
| 5.1 | L R C LFE Ls Rs | L + 0.707·C + 0.707·Ls, the same on the right; LFE left out | the stereo pair summed, scaled |
| 7.1 | L R C LFE, back pair, side pair | as 5.1, with both surround pairs at 0.707 | the same |

Three things a table does not settle, each in §6:

- **Channel order.** A WAV's own order for 5.1 is L R C LFE Ls Rs; film stems are often
  L C R Ls Rs LFE; "4.0" is quad to some and L C R S to others. Tracktion keeps the file's
  order (§3), and Go.dot does not guess (constraint 10), so either each order is its own entry
  in the menu or the user says the order once on the cue.
- **Level.** These sums are not normalised: a 5.1 file loud in every channel comes out of the
  stereo decoder up to 7.6 dB above one channel. That is the standard's own behaviour, and
  the operator's to manage with the cue's level.
- **The LFE.** Left out by the standard downmix; some want it in at −10 dB or so.

**Atmos is a different kind of thing.** It is not a channel layout but a bed plus moving
objects, and it reaches a theatre in three forms:

- *As a delivery codec* (Dolby Digital Plus with JOC, TrueHD, AC-4). The decoders are
  Dolby's, under licence. FFmpeg — which Go.dot already uses to turn compressed sound into WAV
  — reads the 5.1 or 7.1 core of such a stream and, by what a search of 2026-11-24 found,
  recognises the Atmos part without decoding it (a patch for TrueHD object data was posted in
  March 2025; whether it was merged was not established). **No freely usable Atmos decoder was
  found; check again when the work starts.**
- *As a master* (ADM BWF, a wide WAV carrying its description). Its first channels are the
  bed, plain PCM in a known layout (7.1.2 as a rule); the rest are objects whose positions are
  in the file's metadata.
- *Rendered*, by Dolby's renderer or a processor, onto loudspeakers or to a stereo or
  binaural file.

What Go.dot can do without a codec, and without becoming a renderer: convert a delivery file
at import to its 5.1 or 7.1 core, which then plays through those decoders, with a plain word
that the objects were left behind; and play an ADM master's channels at their full count into
the raw set, for a renderer downstream, or decode its **bed only** to stereo. Placing the
objects is rendering, and stays outside (PRD §1).

What the decoder does not do:

- It decodes to mono or stereo only: no 7.1 to 5.1, no Ambisonic to 5.1. A wider image or
  another aim is the user's own coefficients in the matrix.
- The stereo law has one control, the send's existing width (1 is the pair of cardioids, 0 is
  W on both sides): applying the two-into-two law of `SendPlacement.cpp:94-102` to the pair
  gives `L = ½W + ½w·Y`, `R = ½W − ½w·Y` with nothing added.
- No turning of the field.

### What stays outside

Decoding to a loudspeaker rig, binaural, rotation, distance: XOA, a plugin, or a decoder
elsewhere.

---

## 5. Stages

Each is small enough to build and test alone, in both locales. MC.1 to MC.4 need no change to
the voices: they work by setting `channelsPerTrack` to the widest file. The document gains one
value of an existing attribute (a bus's kind) and one attribute whose default is not written
(the cue's decoder), so no existing show and no golden file changes.

**MC.0 — Pins and small repairs, no feature.**
Tests beside M1: a 5.1 WAV with a speaker mask plays channel i to output i; a 16-channel and a
36-channel file (the second fails on the 32 cap until it is lifted); a negative coefficient
through `CueMatrix`. The 32 cap: a fourth Tracktion patch raising it to 64
(`patches/tracktion_engine/series`, `scripts/te-patches.py`), after checking the array on the
stack is safe on the reader's path — or wide files stop at 32 channels and the window says so.
The silent placeholder named by its width.

**MC.1 — The raw set and the diagonal.**
The third bus kind in `docs/parameters/godot-parameters.csv` (row 590) and what follows from
it: packed last, absent from the direct-out menu and the send lists, added and resized in the
show settings' outputs page (`ShowSettingsWindow.cpp:6356-6370`). `defaultMediaRoute` writes
the diagonal onto it for a file wider than two; the window's import (`routeImportedMedia`)
uses it. The default kind is not written, so no existing show and no golden file changes.
Tests: a 4-channel and a 16-channel file onto a raw set wide enough, and onto one too narrow
(the remainder without crosspoints, and said); a wide file with no raw set; a show round trip
in both locales. Must stay green: `ImportRoutingTests.cpp:149, 179`, `GoTests.cpp:1414-1480`,
`OutputLayoutTests.cpp`.

**MC.2 — The grid.**
After the check of §4, item 3. Every crosspoint edit is a named command (constraint 11) and
one undo step; a run of the same crosspoint coalesces as `node.set` does. Sliding the whole
diagonal is one step. Levels are words and numbers, not colour alone (constraint 8). The words
are shown to the author before they ship.

**MC.3 — The decoder.**
A row in `docs/parameters/godot-parameters.csv` near 144 (the media cue), regenerated
(`scripts/generate-schema.py`). A pure file beside `SendPlacement.h` that returns the decoder's
matrices (AmbiX and FuMa first, then LCR, 4.0, 5.1 and 7.1, each to mono and to stereo).
`resolveRouting` multiplies it into what `spreadOf`
gives for a direct out and for each send (`Runner.cpp:6927, 6939, 7023`), `spreadOf` being
asked for one or two channels instead of the file's count; `send/placement`
(`ParameterTree.cpp:917-934`) is derived from the decoded count, so it says `pan`, `stereo` or
`fold` and no surface or window changes. The refusal's words. The detector, peaks and timbre
on the first channel when a decoder is set; the analysis cache's key takes it. The decoder in
the inspector. Tests: the numbers, `L + R = W`, FuMa against XOA's table, width 0 and 1,
balance, swap; a wide cue with no decoder still refused on a direct out; a blackbox driver
with a 4-channel file carrying a tone hard left — heard on the left only through the stereo
decoder into a stereo direct out, at unity through the mono one, and at its four channels on
the raw set from the same cue.

**MC.4 — Importers, settings, guides.**
QLab and Ableton imports already raise `channelsPerTrack` to the widest file
(`QlabImport.cpp:415`, `AlsImport.cpp:691`); the QLab import keeps crosspoints as Routes of
any width already (`QlabWalk.cpp:1584-1622`). The audio settings say what `channelsPerTrack`
costs. Both tester guides.

**MC.5 — Voices of two widths. Later.**
`Audio/@wideTracks` (default 0) and `@wideChannels`: a few wide voices after the ordinary
ones. `buildEdit` takes a width per track (`AudioHost.cpp:568, 677, 700, 723, 754, 780, 794`).
The voice is picked as the lowest free one wide enough, ordinary voices first
(`Runner.cpp:6310`, `RunTable::lowestFreeTrack` at `Run.cpp:389`) — this must stay
deterministic, replay depends on it. `Player` gains a width per track with a default, so the
twenty-odd test fakes compile untouched (`Runner.h:717`). The plugin pool stays at the
ordinary width, so a wide voice passes its plugin places dry by the existing rule. A new
refusal: no wide voice free.

**Not staged:** the 64 ceiling; BHZ; a wide rack, aux, take or key; a format word on outputs.

---

## 6. Decisions

**Decided by the author** (2026-11-24): no spatial renderer; a simple fold of an Ambisonic
stream to mono and stereo; direct outs and mixes kept as they are; a raw set of outputs at
the end of the patch reached through a matrix, with a diagonal default the user adjusts, as
QLab; no output reserved for a use; matching the format is the user's, at the receiving end;
a wide file reaches a direct out or a mix only through a decoder to mono or stereo, and plays
at its full count only in the matrix;
cues may be summed on any output and that is the operator's to manage; no summing at the
patch.

**Still the author's**, each with a recommendation:

| Question | Recommendation |
|---|---|
| The raw set as a third kind of bus, one per show | Yes: the smallest thing that does it; several raw sets only if a rig asks |
| A trim and a DCA on the raw set | Trim per raw output, no DCA: it is a patch, not a mix |
| A mono or stereo cue into the raw set | Allow, through the same grid |
| The decoder as a choice on the cue, after which it is a mono or stereo cue to every direct out and mix | Yes: sends, pan and width, fades and surfaces then work with nothing new. The other way, a preset writing coefficients, loses all of those |
| Channel order for LCR, 4.0, 5.1, 7.1 | One menu entry per order, named by its channels ("5.1 — L R C LFE Ls Rs"), rather than a second control |
| The LFE in a stereo or mono decode | Left out, as the standard; the matrix says otherwise for who wants it |
| Atmos | No decoder: none is free to use. At import, the 5.1 or 7.1 core of a delivery file, said plainly; an ADM master at full count into the raw set, or its bed decoded. Look again for a codec when the work starts |
| One decoder per cue, or one per destination | One per cue: a cue is decoded to mono or to stereo, and a stereo cue onto a mono output is already folded (§42) |
| The 32-channel cap in Tracktion | Patch it to 64 |
| The stereo law | Two cardioids back to back: plain, sums to the mono fold, never above unity |
| A centre source is 6 dB down per side in the stereo decoder | Accept, and say so in the guides; the cue's level makes it up |
| FuMa among the decoders | Yes; the first four channels are all a decoder to stereo reads |
| File channels beyond the raw set's last output | No crosspoint, and said in the window; never wrapped |
| A narrow cue on a free wide voice when the ordinary ones are busy (MC.5) | Allow |
| A VST3 above 8 channels (BHZ) | Leave open until a plugin asks |
| Whether MC.5 is built at all | After the rest has been used on a real bed |

**The implementer's:** file layout, command names, the placeholder's name, the shape of the
tests. Any word that reaches the screen is shown to the author before it ships.
---

## 7. Risks

- The 32 cap truncates **silently**, and it has only been read in the source.
- The grid of MC.2 is the largest piece, and is new: nothing on a cue shows crosspoints
  today.
- MC.5 touches which voice a cue gets, and with it replay.
- By ear: nothing here has been heard.

---

## 8. What the sibling projects give

| From | What | State |
|---|---|---|
| XOA `Source/DSP/AmbiConventions.h` | N3D ↔ SN3D at any order; FuMa → AmbiX to order 3, checked against two sources | lift the table |
| XOA `Source/Parameters/XoaParameterIDs.h:107`, `Source/Audio/PatchRouting.h` | a format per input (mono, or an order) and the flattening of groups onto channels | what the user matches at the receiving end |
| XOA `tests/XoaFileFormatTests.cpp` | 121- and 128-channel WAV through stock JUCE | says JUCE's reader is not the limit |
| XOA, removed at `a5e1062` | a multichannel file player (128 channels), dropped because "XOA is a processor; program material arrives via device inputs" | Go.dot is that player |
| XOA decoders, binaural, rotation | a whole renderer | not for Go.dot |
| spatcore | `MultiChannelEQBank`, `DynamicsModule::processKeyed` (N channels, linked) | the second is already in use |
| spatcore, WFS-DIY | no Ambisonics, no channel-layout type | nothing to take |

XOA is under construction: take the table and the idea, not a dependency.

---

## 9. Set aside: draft 0.1's format word on outputs

*Kept for the record. The author's ruling of 2026-11-24 (§4) replaced this: outputs carry
no format and nothing is matched by rule. The fold's mathematics moved to §4.*

### A word for what the channels mean

One attribute; the order is derived from the count, (n+1)², because a stored order would be a
second copy of the width.

| Where | Values | Default |
|---|---|---|
| the media cue: `media/format` | `discrete`, `ambix`, `fuma` | `discrete` |
| the output: `bus/format` | `discrete`, `ambix` | `discrete` |
| a processor's slot | `discrete`, `ambix` | `discrete` |

- `ambix` is ACN channel order with SN3D levels — what XOA speaks and what almost every tool
  writes now. `fuma` is the older convention (W X Y Z order, W at −3 dB), order 3 at most, on
  the cue only: it is turned into AmbiX by coefficients on the way out.
- `discrete` is today's behaviour, and the default is not written, so **no existing show and
  no golden file changes**.
- The enum leaves room for `5.1`, `7.1` and the like later without building them now.
- **Never guessed.** Import writes the channel count and nothing more (constraint 10: the
  document holds what someone decided). When a file has 4, 9, 16, 25… channels the window
  *offers* the word; pressing is a named command (constraint 11).

### The matching rules

In one new pure file beside `SendPlacement.h`, called from `spreadOf`, so direct outs and
sends follow the same rules:

| Cue | Destination | What is heard |
|---|---|---|
| ambix | ambix, same order | channel to channel |
| ambix | ambix, lower order | the upper channels dropped (legitimate: a lower order is the first channels of a higher one) |
| ambix | ambix, higher order | the missing channels silent |
| fuma | ambix | converted, then as above |
| ambix or fuma | discrete, 1 wide | **the mono fold** |
| ambix or fuma | discrete, 2 wide | **the stereo fold** |
| ambix | discrete, at least as wide as the cue | channel to channel, as now |
| ambix | discrete, 3 wide up to one less than the cue | refused, as now |
| discrete | ambix | refused: placing a point in a field is an encoder's job, XOA's |

A Route still says anything at all by hand.

### The fold

AmbiX: channel 0 is W (everything, from every direction), channel 1 is Y (left minus right),
channel 2 is Z (up minus down), channel 3 is X (front minus back). In SN3D each of them peaks
at 1 as W does. A virtual microphone aimed along a direction is `p·W + (1−p)·(that
direction's component)`, and is at unity on its axis whatever `p`.

- **Mono: W alone, at unity.** An omnidirectional microphone: every source at the level it was
  encoded.
- **Stereo: two cardioids back to back**, aimed left and right:

  ```
  L = ½·W + ½·Y
  R = ½·W − ½·Y
  ```

  A source hard left is full on the left and silent on the right. A source in the centre is
  6 dB down on each side. Nothing exceeds unity. And `L + R = W`: the stereo fold summed is
  exactly the mono fold.

- **Width comes free.** The existing two-into-two law (`SendPlacement.cpp:94-102`) applied to
  that pair gives `L = ½W + ½w·Y`, `R = ½W − ½w·Y`: `send/width` at 1 is the pair of
  cardioids, at 0 it is W on both sides. Balance and swap work unchanged (swap mirrors the
  field). So the send's existing controls, the fades that move them and the surface pages all
  keep working; the published placement word gains one value (`soundfield` as a placeholder)
  so that everything switching on it knows.
- **FuMa** — the table is XOA's (`D:\dev\XOA\Source\DSP\AmbiConventions.h`,
  `fumaToAmbixTable`): W to channel 0 times √2, X to 3, Y to 1, Z to 2, and so on to order 3.
  One non-zero per row, so it is pure coefficients. Mono is `√2·W`; stereo is
  `(√2/2)·W ± ½w·Y`, Y being the file's third channel.
- **Not now: turning the field** (aiming the pair elsewhere than straight ahead). It needs X,
  a new control, a fade move and a surface page, and it is the first step of a renderer.

### What stays outside

Decoding to a loudspeaker rig, binaural, rotation, distance: XOA, a plugin, or a decoder
elsewhere. Go.dot carries the field to an output or a slot of the right width and format.
XOA's inputs already have the same notion (`inputFormat`: mono, or order 1 to 10).
