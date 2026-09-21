# Audio playback verification — 2026-09-20

The device callback is implemented in `DeviceLayer.cpp` and registered with
JUCE's device manager. It patches hardware inputs into the preallocated input
buffer, runs `AudioHost::processBlock` / Tracktion, and patches the resulting
logical outputs into the buffers supplied by the selected interface.

The verification found and fixed a launch bug in `Console.cpp`: only the
hosted branch initialized the cue runner's samples per tick. On hardware,
`Runner::launchIfDue` returned without launching because that value was zero.
It is now initialized for every clock source. The existing cue scheduler,
sample-based launches, and Tracktion graph remain in use.

Additional fixes rebase initial session time after device/UI startup, refresh
the reported clock source after an interface switch, stop every owned source
at shutdown, and default an absent input device to disconnected input patches.

## Evidence

- Strict Debug builds of `wfg` and `wfg_tests` pass, with Windows ASIO enabled.
- The new hardware test in `DeviceTests.cpp` plays a quiet -60 dBFS sine and
  observes the final buffers after output patching. With logical output 0
  patched to hardware output 1, measured peaks were approximately
  `[0, 0.00101]`. All 35 assertions passed across Windows Audio (shared,
  exclusive, low latency), DirectSound, and **ASIO MADIface USB**.
- The extended `device_serve.py` test passed 26 checks on DirectSound and 26
  on ASIO MADIface USB: fire media, observe the moving playhead, Kill, apply
  interface and swapped output patch while stopped, play again, save the show,
  reopen using its saved selection, and play again.
- The existing first-sound, phase 3, and phase 4 blackboxes pass in both C and
  French locales. These cover rendered sound, fades/stops, groups and preparation.
- All existing replay, schema, tree, client-boundary and source-comment checks
  pass. `docs/schema/show.rng` was regenerated for the added audio settings.
- Full unit suites: **798 tests passed in each locale** (114506 assertions in
  C, 114494 in French), including the hardware output verification.

The original device smoke test could return success after skipping every
device because it forced 44100 Hz. It now lets the device report its rate,
preserves device API names, and treats crashes as failures.

## Limits

This verifies samples submitted to device buffers, not a physical speaker or
listening test. The native settings panel was compiled; visual interaction was
not automated. Tests use shows with explicit audio tracks, a bus, and media
routes. A fresh show still declares zero tracks; selecting an interface alone
does not create tracks, buses or routes for imported media. Live-input
monitoring/rack processing is outside this playback check.

The shared spatcore submodule was not modified.

## Native settings hang found afterward

Opening the settings panel on ASIO could stop the active callback. The panel
constructed a second device to query capabilities; JUCE's ASIO constructor
initializes and briefly starts/stops the underlying driver. The engine then
waited for samples that no longer arrived, leaving Apply queued indefinitely.
The earlier headless Apply tests did not exercise this panel construction.

The panel now reads channel counts and supported buffer sizes published by the
engine's existing device. It never creates a device. A dedicated Windows UI
test opens and rescans the actual panel over a live ASIO callback and checks
that callbacks continue. It also checks that completion and refusal release
the Apply controls, and that completion sends Save only once.

All 12 targeted checks passed after this fix, including the new native tests
in both locales and the existing playback/save/reopen checks. The preview was
reopened with the user's imported cues recovered. A subsequent real-window
Apply completed, saved the eight-channel output patch, and left ticks advancing.

## Output test signals

The output patch now offers Test mode, Off/Pink noise/Tone/Sweep/Pulse, level,
tone frequency, and Hold. The user explicitly chose WFS-DIY's repeating pulse
instead of a frequency chirp. The shared spatcore generator is reused without
modifying the submodule. All four signals have its 500 ms protective ramp.

The UI generator is only a local interaction draft. Logged `audio.testSignal`
and `audio.testStop` commands transfer configuration via one lock-free atomic
message; only the audio callback touches the rendering generator. A preallocated
mono buffer replaces the selected physical output after patching. Other
outputs and cue processing continue unchanged. No test state is persisted.

Tab exit, window close/destruction, applying settings, and global stop/panic
clear tests. Momentary Space/mouse release stops the selected output; Hold
keeps it active. The settings window forwards Esc to the existing panic handler.

Offline tests verify all four ramps, hardware channel isolation, restart ramps,
tone frequency/level, untouched cue buffers after stop, real-time allocation
counts, invalid command refusal, and global stops. Native UI tests verify Hold,
tab/window cleanup and momentary release in C and fr-FR. Thirteen targeted
checks passed, including live ASIO panel/rescan, audio replay, first sound,
group playback and prepare/playback, plus schema and source/boundary checks.
These are rendered-buffer and interaction checks, not a listening test.

The final full unit run passed all 801 cases in both C and fr-FR.

## Drag/drop silence fixed

The native importer assigned files without creating output routes. Added
`route.default(cue, channels, optionalId)` and invoke it after importing or
linking media. It creates explicit gains into the lowest-channel output bus,
preserves existing routes/feeds, and supports undo, save/reopen and replay.
Mono maps to both stereo outputs; wider files map matching channels. Shows
without tracks/buses still require those resources; the importer does not
change graph capacity or invent a playback fallback.

Both locales passed routing unit tests, the new import-to-render-and-replay
blackbox, and existing first-sound tests. Preview Thunder and Sweep were
repaired through commands and saved, with the original quiet test unchanged.
The app was reopened on ASIO MADIface USB. No audible playback was triggered.

## Test selection reset fix

The UI incorrectly treated an idle hardware target as a signal reset. A late
active snapshot after releasing an output followed by the idle snapshot
cleared the selected signal. It now reflects explicit Off rather than channel
activity; received resets do not echo another stop command. The native
regression failed before this fix and passes afterward in C and fr-FR (53
assertions), covering message dispatch, late snapshots, idle/Hold persistence,
panic and page/window cleanup. Audio rendering and spatcore are unchanged.
