# Go.dot — Multichannel files and outputs, Ambisonics first: notes before the work starts

**Draft 0.2**, 2026-11-24 (0.1 the same day; 0.2 follows the author's ruling quoted in §4,
which set aside 0.1's format word on outputs). Written from a conversation with the author and a read of the code
that day (Go.dot at `c3e0263a`, XOA at `e4c3fbd`, spatcore at `v0.4.0-50-g7bdf4ab`). Nothing
here is built and nothing was run: every claim comes from reading the source. Line numbers are
those of that day. **Check a claim against the code before relying on it.**

What is the author's and what is not:

- **Asked for by the author:** multichannel playback and outputs, prompted by a request for
  Ambisonic files, "in a flexible way without redoing everything from scratch".
- **Decided by the author** (2026-11-24): Go.dot does not become a spatial renderer, **but**
  "we should at least give a simple way to mix down a full Ambisonics stream to mono and
  stereo."
- **Decided by the author** (2026-11-24, later): handled at the patch as in QLab — a diagonal
  default the user adjusts, no output reserved for a use, matching the format left to the user
  at the receiving end. His words are in §4.
- Everything else is a finding or a recommendation. §6 lists what is his to decide.
- The PRD is not amended by this file. When the work starts: a namespace-draft section (the
  next free is **§150**, the next code **BKE**; stage codes **MC.** are unused), a PRD §3.9b
  amendment, never §4.

---

## 1. The short of it

The playback core is already written for any number of channels: an output of any width, a
full matrix at the end of every voice, a matrix of any size on a cue. What is missing is
small: a default that lays a wide file diagonally across the outputs, a grid in which the
user moves the crosspoints, and a preset that writes the fold of an Ambisonic stream to mono
or stereo into that grid. No output is given a meaning and nothing is added to the document.

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

Read here as:

- **Outputs carry no format.** No "Ambisonic output", no run of channels set aside for a bed.
  An output is what it is today.
- **A wide file arrives with a diagonal**: file channel 1 to output channel 1, 2 to 2, and so
  on. The user then moves the crosspoints.
- **Go.dot checks nothing about meaning.** Whether the sixteen channels that arrive at XOA are
  the order XOA expects is the user's to match there.

This sets aside draft 0.1's format word on outputs and slots, and its table of matching rules
(same order, lower order, refused…). They are kept in §9 for the record only.

### What the ruling needs *(proposed)*

1. **A diagonal that runs across outputs.** A `<Route>` belongs to one bus, and today's
   default (`ShowDocument::defaultMediaRoute`, `ShowDocument.cpp:2673-2703`; the command
   `route.default`, `DocumentCommands.cpp:427`) writes the identity onto the *lowest bus
   only*. For a file wider than that bus the diagonal has to continue onto the next outputs:
   one Route per bus touched, each holding its piece. With two stereo outputs and a 4-channel
   file: channels 1-2 on the first, 3-4 on the second. Nothing new in the document — the
   Routes are what already exists. Channels that fall beyond the last output get no
   crosspoint, and the window says so.
2. **The window's import does the same.** `routeImportedMedia` (`Client.cpp:4920-4975`) gives
   a new cue a direct out to the lowest output, else a send. For a file wider than that
   output a direct out is refused at GO ("the cue is wider than its direct out"), so a wide
   file should get the diagonal instead. Mono and stereo files: unchanged.
3. **A grid to adjust it.** QLab shows cue channels against outputs. On 2026-11-24 no such
   grid was found on a cue in the desktop window (the only matrix component in
   `src/wfg/client` is the hardware patch in the show settings, spatcore's
   `PatchMatrixComponent`). **To check before building**: what the inspector or the foot
   offers today for a Route's coefficients. If nothing, this is the stage that makes the rest
   usable — a foot panel, cue channels down, output channels across with the outputs' names
   as groups, a level per crosspoint.
4. **Where the diagonal starts.** At the first output, as QLab; moving the whole diagonal
   along (start at output 9) as one gesture is worth having, since "no channel reserved"
   means the bed lands wherever the user says tonight.

### The fold to mono and stereo, without a format word

The author's earlier ruling of the same day stands: "a simple way to mix down a full
Ambisonics stream to mono and stereo." Under the later one it becomes **a preset in the
grid**: the user picks an output and "Ambisonic to stereo" (or mono), says AmbiX or FuMa in
the same menu, and Go.dot writes the coefficients. What is stored is the coefficients — what
someone decided (constraint 10) — and no word on the cue.

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

What this gives up against draft 0.1, knowingly:

- The fold is Route coefficients, so the send's width, balance and swap do not act on it, and
  a fade does not move it (`FadeMoves.cpp:218-264` moves sends, not Routes). The cue's level
  still fades the whole.
- With no word saying "this is a field", the waveform, the timbre colour and the dynamics
  detector keep folding every channel together (§3, items 6 and 7).
- A wider stereo image or another aim is the user's own coefficients.

### What stays outside

Decoding to a loudspeaker rig, binaural, rotation, distance: XOA, a plugin, or a decoder
elsewhere.

---

## 5. Stages

Each is small enough to build and test alone, in both locales. MC.1 to MC.4 need no change to
the voices: they work by setting `channelsPerTrack` to the widest file. None adds an attribute
to the document, so no existing show and no golden file changes.

**MC.0 — Pins and small repairs, no feature.**
Tests beside M1: a 5.1 WAV with a speaker mask plays channel i to output i; a 16-channel and a
36-channel file (the second fails on the 32 cap until it is lifted); a negative coefficient
through `CueMatrix`. The 32 cap: a fourth Tracktion patch raising it to 64
(`patches/tracktion_engine/series`, `scripts/te-patches.py`), after checking the array on the
stack is safe on the reader's path — or wide files stop at 32 channels and the window says so.
The silent placeholder named by its width.

**MC.1 — The diagonal.**
`defaultMediaRoute` continues across buses for a file wider than the lowest one; the window's
import (`routeImportedMedia`) uses it for a file wider than the output it would have given.
Existing assignments are preserved, as `route.default` already promises. Tests: a 4-channel
file over two stereo outputs, over one 8-wide output, over a rig too narrow (the remainder
without crosspoints, and said). Must stay green: `ImportRoutingTests.cpp:149, 179`,
`GoTests.cpp:1414-1480`.

**MC.2 — The grid.**
After the check of §4, item 3. Every crosspoint edit is a named command (constraint 11) and
one undo step; a run of the same crosspoint coalesces as `node.set` does. Sliding the whole
diagonal is one step. Levels are words and numbers, not colour alone (constraint 8). The words
are shown to the author before they ship.

**MC.3 — The fold presets.**
A pure file beside `SendPlacement.h` that returns the coefficients (AmbiX and FuMa, mono and
stereo); a command that writes them as a Route onto the chosen output, one undo step. Tests:
the numbers, `L + R = W`, FuMa against XOA's table; a blackbox driver with a 4-channel file
carrying a tone hard left — heard on the left only through the stereo preset, at unity through
the mono one.

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

**Not staged:** the 64 ceiling; BHZ; a wide rack, aux, take or key; a format word.

---

## 6. Decisions

**Decided by the author** (2026-11-24): no spatial renderer; a simple fold of an Ambisonic
stream to mono and stereo; a diagonal default the user adjusts, as QLab; no output reserved
for a use; matching the format is the user's, at the receiving end.

**Still the author's**, each with a recommendation:

| Question | Recommendation |
|---|---|
| The fold as a preset that writes coefficients, with no word on the cue | Yes: it is what the later ruling implies. The cost is listed in §4 |
| The 32-channel cap in Tracktion | Patch it to 64 |
| The stereo law | Two cardioids back to back: plain, sums to the mono fold, never above unity |
| A centre source is 6 dB down per side in the stereo fold | Accept, and say so in the guides; a make-up is one number in the grid |
| FuMa among the presets | Yes, order 1 is enough for a fold |
| File channels beyond the rig's last output | No crosspoint, and said in the window; never wrapped |
| The diagonal for a mono or stereo file | Unchanged from today (direct out, or a send) |
| A narrow cue on a free wide voice when the ordinary ones are busy (MC.5) | Allow |
| A VST3 above 8 channels (BHZ) | Leave open until a plugin asks |
| Whether MC.5 is built at all | After the rest has been used on a real bed |

**The implementer's:** file layout, command names, the placeholder's name, the shape of the
tests. Any word that reaches the screen is shown to the author before it ships.
---

## 7. Risks

- The 32 cap truncates **silently**, and it has only been read in the source.
- The grid of MC.2 is the largest piece and has not been looked for thoroughly: check what
  the window already offers for a Route before designing it.
- A diagonal across several outputs is several Routes; an output deleted or narrowed later
  must leave the others standing (the load already checks each Route's length).
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
