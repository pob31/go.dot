# Device presets

A preset is a namespace file - the OSCQuery JSON a show keeps as `namespaces/<id>.json`, which
`tree::readNamespace` checks - shipped with Go.dot and copied into a show when a device is made from
it (`mount.createFromPreset`). One file per device family **and per protocol**, each in that
protocol's own spelling, so the address in a cue is the address in the device's manual (PRD §3.11,
§3.22). The design and the decisions are namespace draft §57.

## The file

- The root's `GODOT` key names it: `PRESET` (the slug, equal to the file name without `.json`),
  `VERSION` (an integer, bumped when the tree changes), `VENDOR`, `MODEL`, `TRANSPORT` (`udp`, `tcp`,
  `midi` or `https`), `WIRE` (`osc`, `rcp`, `line`, `midi`, `shure`, `ssc` or `sscv2`), `FRAMING` (`length` or `slip`, for OSC over
  TCP only), `PORT` (the device's usual port, `0` when the device has none), `QUERY_PORT` (where it
  answers OSCQuery, for a preset saved from such a device; §76) and `SOURCES`.
- A root of `/` with the device's roots as its first-level children mounts each child as a root
  (an X32's `/ch`, `/bus`, `/dca`); a root of `/eos` or `/dbaudio1` mounts at that one root.
- A node carries `TYPE`, `ACCESS` (2 a command, 3 a value), `RANGE` with `MIN`/`MAX` or `VALS`,
  `UNIT`, a `DESCRIPTION` that is a sentence, and under `GODOT`: `KIND`, `RATE_CAP`, `ANTICIPATABLE`,
  `PANIC` as `docs/godot-namespace-draft-0.1.md` §3 reads them; `ROLE` (§57, AFM); `BARE` (`true`: a
  command that takes its TYPE's arguments or none at all, so a cue with no value sends it bare - an Eos
  cue's `fire`, 2026-10-25); and the wire's own
  key where the address alone cannot say how to send it - `RCP`, `LINE`, `MIDI` (§57, AFJ), `SHURE` (§124,
  BER: `OFFSET` added to a number sent and taken off one heard, `BRACES` for a name, `PAD` the digits).
- At the root of a Shure receiver's file, `SAMPLE`: the fields of its meter line, a list of node names per
  layout, picked by how many fields came (§124, BER). At the root of a Sennheiser receiver's (`ssc`),
  `SUBSCRIBE` is the subscription message whole, JSON, sent as the device's socket opens and every nine
  seconds after (§124, BES). A base station's (`sscv2`) is a JSON array of the resources to watch, and `IDS`
  names the field each collection's elements are known by - `{"/api/mts/paired/all": "mtUid"}` (§124, BET).
- On a part of the tree, `OPEN` (`true`: an address under it that the file does not list is sent as
  typed - an Eos cue numbered 98.1) and `SKIP` (`true`, or the word the menus offer it by: the level under it
  may be left out, offered as *[skip]* or *[word]* - Eos's *[current]*, a cue in the console's current list);
  §110, §114. A level of numbers under an `OPEN` part is typed into its box rather than picked.

## Your presets

Beside this folder, Go.dot reads a folder of the user's own - `Documents/Go.dot/Device presets`
unless serve is told another with `--user-presets` (namespace draft §76). The same files, the same
rules: a file there is offered in the Network tab's PRESET menu under *Yours*, and one named like a
preset here is used in its place, which is how a shipped preset is corrected on one machine - copy
it there, change it, raise its `VERSION`. The folder is read at the start, again whenever the
Network tab is shown or PRESET opens, and after a save.

*Save as a preset…* in a device's Kind cell writes the device's description there with its `GODOT`
header made or kept: `PRESET`, `VERSION` one more than any of that name, `VENDOR`, `MODEL`, the
device's `TRANSPORT`, `WIRE`, `FRAMING`, `PORT`, `QUERY_PORT` (where it answers OSCQuery; a device
made from the preset is asked there) and `READBACK`, and one more line of `SOURCES` saying where it
was captured or what it was copied from. `GENERATED` is dropped: no script wrote the copy.

## Provenance

Every preset says where it came from, the way `tests/fixtures/README.md` asks of a fixture, because
a file written from memory of a protocol proves nothing. `SOURCES` is a list of strings, one per
document, naming it, its version or date, and the page or table the nodes were read from. A
preset is written from the vendor's own document where one is public, from the device's own
answers where it describes itself, and from a protocol specification (ADM-OSC, MIDI Show Control)
where that is the primary text. Companion and Chataigne modules are read for the command surface a
device has and never copied: they are JavaScript under their own licences, and Go.dot is GPL-3.

A preset bears `"VERIFIED": "<date>, <console>, <firmware>"` in its `GODOT` only once somebody has
sent its messages to the real device and watched them land. Until then it is documentation.

Yamaha's RCP document is under NDA and was not read (§57, AFA): `yamaha-rcp` is written from
Yamaha's public OSC specifications' parameter identifiers, the names the Companion module uses, and
what a console answers to `devinfo` and `get`.

## Generated files

A preset with a machine-readable source is generated by a script under `scripts/presets/` and the
script, not the file, is what gets edited; the file carries `"GENERATED": "scripts/presets/<name>.py"`.
Hand-written presets carry nothing of the kind.

`wfg presets` lists what is installed; `wfg presets --check` reads every file and exits 1 on the
first that does not load or names no source. `tests/PresetTests.cpp` does the same under ctest.
