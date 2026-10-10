# Go.dot — Try it out

Oct 9, 2026 · @Pierre-Olivier Boulant

## What Go.dot is

Go.dot is a free, open-source (GPL-3) show-control application for theatre, dance and live music. You build a cue list, press GO, and it plays sound and video, fades them, loops them, routes them and talks to the rest of the rig. It runs on Windows, macOS and Linux.

It borrows from three tools many of you already know:

- **QLab** for the cue list, fades, chaining and the operator's habits.
- **Ableton Live** for keeping hands on parameters while sound is running.
- **Chataigne** for turning incoming control data into actions.

It is not a DAW, not a lighting console and not a spatial renderer. It plays the show and directs the machines that do those jobs.

Three promises shape everything in it:

- **GO never blocks.** Whatever else is happening, the next cue fires when you press it.
- **Three ways to stop.** Esc stops gracefully and tidies up as if the cue had ended normally. Esc twice stops everything Go.dot is doing, immediately. **Doh!** recovers from the most common mistake: a cue fired too early.
- **Nothing is lost.** Every edit can be undone, and the show is saved continuously so a crash costs you nothing.

This is a **test build** (v0.1). It is meant for trying things out, in rehearsal rooms and on your own machine, not yet for opening night.

## Getting it running

Download the test build for your system from the [releases page](https://github.com/pob31/go.dot/releases), install it, and launch Go.dot. It opens an empty show called *Untitled* on your default audio interface, with the show settings open so you can choose another.

| System | Download | First launch | Log for bug reports |
| --- | --- | --- | --- |
| Windows 10/11 | `-setup.exe` (installer, adds the Start menu entry and opens `.wfg` files) or a `.zip` (unzip anywhere, run `Go.dot.exe`) | Not signed yet: SmartScreen warns, choose *More info*, then *Run anyway* | `%APPDATA%\Go.dot\logs` |
| macOS 13.3+ (Apple silicon and Intel) | `.dmg`, drag Go.dot to Applications | Signed and notarized, opens without a warning. Allow microphone access for mic cues | `~/Library/Logs/Go.dot` |
| Linux (built on Ubuntu 24.04) | `.deb` (`sudo apt install ./go.dot-…deb`) or a tarball (run `./go.dot.sh`) | Needs ALSA, FreeType, fontconfig, X11 (any desktop has them) | `~/.local/state/Go.dot/logs` |

**On Linux with a multichannel interface**, PipeWire only offers two channels through ALSA. Install `pipewire-jack`, set the interface profile to *Pro Audio* in pavucontrol, and choose JACK in Show settings.

**On Windows**, ASIO drivers are supported and are the best choice for a pro interface.

Useful checks from a terminal (on Windows, `wfg.exe` in the install folder; on macOS, inside `Go.dot.app/Contents/MacOS/`):

- `wfg --version` — which build you have
- `wfg devices` — the audio interfaces Go.dot can play through
- `wfg midi` — the MIDI ports it sees
- `wfg plugins --scan` — find your VST3, AU and LV2 plugins
- `wfg validate <show>` — check a show folder and list every problem

The last tab of *Show settings*, **Getting started**, lists every settings tab in the order a show is set up, with one sentence on what each is for.

## Your first show in ten minutes

A show is a folder that contains one or more `.wfg` files plus a `media/` folder that Go.dot copies your sounds into. Save it anywhere, carry it to another machine, and only the audio settings need a second look.

1. **Pick the audio interface.** *Show > Show settings… > Audio*. The GO button turns yellow once audio is running.
2. **Create and name your outputs** (mix channels or direct outs, mono or stereo). On *Outputs*, create the show's outputs ("Main L/R", "Sub", "Foldback"). On *Output patch*, say which physical channel each one lands on. Cues are routed by name, so a new venue is a new patch and no cue changes.
3. **Add sounds.** Drag WAV, AIFF, FLAC or Ogg files onto the cue list. Each becomes a media cue named after its file. Drop one on an existing cue to replace its file.
4. **Add other cues** from the *Add* row above the list: `memo`, `media`, `mic`, `video`, `fade`, `transport`, `osc`, `midi`, `process`, `group`. The ones with a ▾ open a short menu of kinds.
5. **Shape each cue** in the inspector on the right: name, pre-wait, post-wait, follow, level. The buttons at the top of the inspector open the panels at the foot of the window: *Waveform*, *EQ*, *FX*, *Sends*, *Curve*, *Timeline*.
6. **Run it.** Click the far left of any cue to move the playback pointer. The standby cue is highlighted with a yellow mark. Press **Space** for GO.
7. **Save** with Ctrl+S (⌘S on a Mac). Autosave runs anyway.

### The keys that matter during a show

| Key | What it does |
| --- | --- |
| Space | GO — fire the standby cue |
| ↑ / ↓ | Move standby to the previous / next cue |
| Esc | Stop everything gracefully: fade out over the panic time (1 s by default), run the groups' endings |
| Esc twice (within 0.75 s) | Drop everything at once, no endings |
| F9 | **Doh!** — take back the last GO |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo an edit (⌘ on a Mac) |
| Ctrl+L | Lock the show so nothing can be edited by accident |
| Ctrl+T | Load to time: start the show from the middle of a scene |

**Doh!** exists for the mistake that happens every so often: a GO pressed a beat early. Within a short window (set on the *Playback* tab), F9 stops what that GO started, puts standby back, and returns devices to the state they were in. Sound that already reached the speakers cannot be taken back, and Go.dot does not pretend otherwise.

Two GOs closer than half a second are treated as one bounce of the finger and the second is refused. You can change this on the *Playback* tab.

## What you can try today

Everything below is built. Sections marked new arrived after v0.1 and need the newest build. Each row says where to find it.

### Playing sound

| Feature | What it does | Where |
| --- | --- | --- |
| Media cues | Play a file to named outputs at set levels, with pre-wait, post-wait and follow | Drag a file onto the list |
| In and out points, loops | Trim a file, loop a region, leave the loop on cue with an *Advance*; when the last out point stops short of the end of the file, a button under the range list makes the rest of the file a new range | *Waveform* panel; *transport > Advance* |
| Level lane | Draw a volume curve over the waveform, or record it from a fader | *Waveform* panel |
| Speed | 0 to 20×, as varispeed (pitch follows) or timestretch (pitch held), fades of speed, and playback backwards or bouncing between its in and out points | Inspector, *what it does* |
| EQ | Four bands and two filters on every cue | *EQ* panel |
| Plugins | A VST3, AU or LV2 chain on every cue. Plugins run in a separate process, so a crashing plugin silences its cue, not the show | *FX* panel; *Show settings > Plugins* to scan |
| Sends | Levels from a cue into the show's mix channels | *Sends* panel |
| Send lanes | Draw each send's level over the file, as the level lane does for volume | Waveform panel, lane picker |
| Editing a sound (new) | Cut a sound into sections at the playhead, drag them into a new order or remove them, drag a join to set its crossfade, type a trim for a section, then *Freeze* to bounce the edit to a new file the cue plays; *Unfreeze* brings the sections back. Lanes and ranges stay on the sound they sit on | *Waveform* panel, the sections row |
| Handles on a sound (new) | On the waveform each section has handles: the square in its middle is its volume; at each end the bottom handle moves the edge, the top one the fade's length (Shift: this side alone). The wheel over a fade bends its curve (Shift: this side alone). Drag in the top half to select time; press in the lower half to pick a section, drag there to slide it along the timeline. **x** splits at the playhead or the selection's ends, **Backspace** deletes leaving silence, **Shift+Backspace** closes up | *Waveform* panel, click it first |
| Cue templates | Keep a media or video cue's settings under a name and start new cues from it | media ▾ menu; Edit > Save as template…; Show settings > Templates |

### Shaping time

| Feature | What it does | Where |
| --- | --- | --- |
| Groups | *Timeline* (members start together), *Sequential on GO*, *Sequential automatic*, *Shuffle* (new order each round), *Sampler* (sounds and pictures fired from pads and faders, a picture's fader setting its opacity) | *Add > group* |
| Fades | Fade level, sends, EQ and plugin values of any cue or group, with a drawn curve | *Add > fade*, then *Mixer* and *Curve* |
| Transport cues | Stop, stop after this member or round, start another cue; enable or disable a cue for this run; jump standby to a cue, or jump and GO | *Add > transport* |
| Automatic names (new) | A cue nobody has named is called by what it does: a fade or a stop by its target (*Fade out Intro music*), an OSC or MIDI cue by its message, a sound, a picture or a mic by its file or input. It follows when the target, file or message changes. Type a name and it stays; clear it and the automatic one comes back | Name column; inspector, *Name* |
| Triggers | Fire cues from incoming OSC, MIDI or the wall clock | Cue inspector, *when* |
| Load to time | Jump into the middle of a scene with the right cues playing at the right offsets | Ctrl+T |
| Anticipation | The cue on standby is loaded and its devices prepared before you press GO | Automatic |

### Live sound

| Feature | What it does | Where |
| --- | --- | --- |
| Mic cues | Bring a named input up through a rack channel with its own plugins | *Add > mic*; *Show settings > Inputs*, *Rack* |
| Live sampling | Record sound during the show, then loop it, overdub it or clear it | *Take* panel; *transport > On a take* |
| Live recorder | Keep the moment of every cue you fire during a run, as a take | Ctrl+Shift+R |

### Talking to the rest of the rig

| Feature | What it does | Where |
| --- | --- | --- |
| OSC cues | Send to a desk, a processor or a video server, from a list of the device's addresses: several messages or bundles in one cue, and curves played on the cue's clock, recorded from the device's own reports or a SpaceMouse | *Add > osc*; *Show settings > Network* |
| Cues written by WFS-DIY and S21_HiJack (new) | Store a snapshot in WFS-DIY or S21_HiJack and it lands in Go.dot as one cue after the standby; store it again and the same cue is updated. Each declares itself as a device, and its own addresses and snapshot names become menus, one per part of the address, under the cue's *target* | WFS-DIY: *Network* tab, protocol Go.dot, then *Write to Go.dot*; S21_HiJack: *Setup > Cueing system: Go.dot* |
| Address and value menus (new) | On a device that describes itself, pick the address part by part and the value from its list | Cue inspector, under *target* |
| Doh! rollback | When a GO is taken back, send each device the command that undoes it | Cue inspector; *Network* tab |
| MIDI cues | Program change, control change, notes, pitch bend, SysEx | *Add > midi* |
| Network monitor | See every message in and out | *Show > Network monitor…* |
| Control surfaces | Faders, pads, dials and DCAs. Full support for the Asparion D700: faders, EQ and plugin pages on the rotaries, a master dial, standby on the arrows | *Show > Surfaces…* |
| Web console | The same show in a browser, on any machine on the network | `http://<this machine>:5010/ui` |
| Serial ports | Hear an Arduino's lines in a process cue and send lines back | Show settings > Serial |

### Video (new)

| Feature | What it does | Where |
| --- | --- | --- |
| Video cues | Show a movie, a still, a colour fill, a feathered mask or a live input on a canvas, with GO, Esc and Doh! as for sound | *Add > video*, then choose the canvas |
| HAP playback | HAP, HAP Alpha and HAP Q play at full speed. Other movies play as a preview and convert to HAP in the background (FFmpeg is downloaded on first use) | Automatic; inspector |
| Analysis cache | The colours, levels and movie strips Go.dot works out are kept beside the media and reused next time; those of files no longer in the media folder are cleaned up at every launch | Automatic; *Show > Clean up the analysis cache* |
| Movie strip | Thumbnails along the strip with scene cuts marked; the playhead and the in and out points snap to a cut (Alt lets go); the *Scene changes* and *Snap to scene changes* ticks turn either off; a movie's locked sound is edited beside it | Top of the inspector |
| Editing a movie (new) | Cut a HAP movie into sections at the playhead, on its frame grid; drag them into a new order or remove them; drag a join to set its dissolve. Its locked sound is cut in step and keeps its lanes on the sound. *Freeze* bounces movie and sound to new files the cues play; *Unfreeze* brings the sections back. A movie that is not HAP says so: convert it first | *Strip* panel, the sections row |
| Picture panel | Fit, fill or stretch; scale, move, turn, flip; contrast, saturation, gamma, hue and four curves; normal, add, screen and multiply blending | *Picture* panel |
| Canvases and mapping | Several canvases on one output, each bent onto the wall with a mesh warp, and an ASC CDL per output to match projectors | *Show settings > Video*; warp editor |
| NDI, Spout, Syphon | Send an output to another program, take a program's picture in as a live input, or pass a cue's picture through another program and back | *Show settings > Video* |
| Read ahead | The next GO's stills and movies are loaded before you press it, and every cue row says whether its cue is ready | Cue list |
| DCAs on pictures | A DCA rides pictures as opacity; the knob above a DCA strip sets the curve a picture comes in on and the sound's offset | Surfaces; virtual panel |
| Video monitor | The picked cue alone on screen while you adjust it | Opens with the strip or the picture panel |

Pictures are drawn by a separate process that Go.dot watches, so a slow movie cannot hold up a sound cue.

### Process cues: Pure Data inside (new, being finished)

A **process cue** holds a Pure Data patch and runs it while the cue runs, at control rate, never in the audio path. It hears what arrives (device reports, OSC, MIDI, serial lines) and sends to devices, to MIDI and to Go.dot's own commands. Edit the patch on Go.dot's canvas at the foot of the window, where values move live and toggles, bangs, sliders and number boxes play, or open it in plugdata or Pure Data and each save comes back. Ready-made abstractions cover the usual chores: `go.avg`, `go.minmax`, `go.smooth`, `go.scale`, `go.deadband`, `go.change`, `go.edge`, `go.hold` and `go.ratelimit`, each with a help patch, plus an example show. *Add > process*.

### Importing QLab and Ableton Live shows (new)

- **QLab**: File > Import QLab workspace… reads a QLab 4 or 5 workspace file directly, with no QLab needed. Tick the cue lists you want; Go.dot writes a new show (cues, groups, levels, fades, routing and targets) and a report of what it could not carry over.
- **Ableton Live**: the importer unzips the .als file (a gzipped XML document) and reads the XML inside to turn the Live set into cues and groups, so a show built in a session can move to the cue list. One scene is one GO, and the silent "track in" clips become the fades they stand for. It reads Live 10 to 12, turns a tour into one show with a performance per venue, and lives under File > Import Ableton Live set…

### Keeping the show safe

- **Undo history** for every edit (Ctrl+Shift+U), and continuous autosave.
- **Lock** the show during performance (Ctrl+L): GO, Esc and Doh! still work, editing does not.
- **Show and performances.** A show is the piece; each night is a performance, a copy you can change without touching the original. *File > New performance…*. At the end, Go.dot lists what changed, cue by cue, so you can bring the good changes back into the show.
- **Interface trouble.** If the audio interface disappears, the show pauses and resumes when it returns. If its sample rate changes, Go.dot follows it.
- **Double-click a `.wfg`** to open the show (Windows installer, Linux .deb, macOS).

## Coming next

These are designed in the specification and not built yet. The order may change with what testers ask for, which is one more reason to send feedback.

### Tablet app and HTML remote

The web console already runs in any browser on the network. It will grow into a full **remote**: a running pane to kill, advance and scrub what is playing; parameters to adjust while you walk the house; GO from a designer's seat; and a complete fallback if a control surface fails. Touch controls are relative from where your finger lands, so nothing jumps, and dangerous actions need a deliberate gesture rather than a tap. A **native tablet companion** will come on top of it, for one reason: it wakes instantly when you reach for it mid-cue.

### Video latency compensation

Set a projector's delay once, and sound is held back to line up with the picture.

### Timecode

**LTC and MTC, both chase and generate.** Go.dot will follow a timecode source or be one.

### Joining a sequence's members

**Gap, gapless or crossfade**, set once on a sequential group: a silence between its members, none at all, or a crossfade whose length you choose.

### Integration with software

| System | What Go.dot will do with it |
| --- | --- |
| **WFS-DIY** (wave field synthesis) | Command its sources, positions and LFOs, claimed per cue so two cues never fight over one input. Writing its snapshots into Go.dot as cues already works (above) |
| **XOA / Tight-WFS** | The same, for ambisonic and WFS rendering |
| **S21-HiJack** | Drive it as a sidecar for the DiGiCo S21, under its own branch of addresses. Writing its snapshots into Go.dot and being driven by Go.dot's GO already work (above) |
| **Choufleur** (script following) | Show the script position and upcoming cues next to the cue list. Choufleur never fires anything, by design |
| **ADM-OSC** processors | Built-in template for any object-based spatial processor that speaks ADM-OSC |
| **Millumin** | The escape hatch for shows whose video outgrows Go.dot |
| Any OSCQuery device | Discovered automatically, with its parameters listed and checked |

WFS-DIY, XOA/Tight-WFS, S21-HiJack and Choufleur come from the same author as Go.dot. They stay separate applications, possibly on other machines; Go.dot directs them over the network.

### Integration with hardware

- **Sound consoles**: snapshot recall, fader and channel moves, and Doh! rollback. A DiGiCo profile is first in line, then Yamaha, Allen&Heath and Behringer/Midas; any desk that speaks OSC or MIDI already works as an opaque device today.
- **Lighting consoles**: GO and cue recall over OSC and MIDI, and sACN as an input source.
- **Control surfaces**: Mackie Control in v1; HUI, Icon, Behringer, PreSonus and Stream Deck after. A SpaceMouse already moves OSC cue curves.
- **Device templates**: a shared library of device descriptions, written by users, for the desks and processors that cannot describe themselves.

## Sending feedback

A test build exists so that people other than the author can say what they found. Every report helps, including "I could not work out how to…".

**Where:** [github.com/pob31/go.dot/issues](https://github.com/pob31/go.dot/issues). A free GitHub account is enough. Write in English or French.

**A bug report is most useful with:**

1. The build's name (the download's file name, or the first line of `wfg --version`).
2. Your system and your audio interface.
3. What you did, what you expected, and what happened.
4. The newest log file (see the table in *Getting it running*).
5. If you can, the show folder, zipped. Leave out media you cannot share.

**A suggestion is most useful with** the situation it comes from: the show, the moment in it, and what you do today in QLab, Live or on the desk to get round it. "During the curtain call I need to…" leads to a better feature than "please add X".

**What is especially wanted right now:**

- First impressions of the window, from people who have never seen it.
- Interfaces, drivers and operating systems we have not tried.
- Control surfaces and desks you would like Go.dot to talk to, with a link to their OSC or MIDI documentation.
- QLab workspaces and Live sets you would be willing to share for testing the importers.

**Code contributions** are welcome too. Build instructions and the repository's conventions are in the README, under *Building* and *Contributing*. Go.dot is GPL-3: what you contribute stays free.
