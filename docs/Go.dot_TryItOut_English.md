# Go.dot — Try it out

Oct 15, 2026 · @Pierre-Olivier Boulant

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

This is an **early release** (0.1). It is ready for trying things out, in rehearsal rooms and on your own machine, and not yet for opening night.

## Getting it running

Download the latest release for your system from the [releases page](https://github.com/pob31/go.dot/releases), install it, and launch Go.dot. It opens an empty show called *Untitled* on your default audio interface, with the show settings open so you can choose another.

| System | Download | First launch | Log for bug reports |
| --- | --- | --- | --- |
| Windows 10/11 | `-setup.exe` (installer, adds the Start menu entry and opens `.wfg` files) or a `.zip` (unzip anywhere, run `Go.dot.exe`) | Not signed yet: SmartScreen warns, choose *More info*, then *Run anyway* | `%APPDATA%\Go.dot\logs` |
| macOS 13.3+ (Apple silicon and Intel) | `.dmg`, drag Go.dot to Applications | Signed and notarized, opens without a warning. Allow microphone access for mic cues | `~/Library/Logs/Go.dot` |
| Linux (built on Ubuntu 24.04) | `.deb` (`sudo apt install ./go.dot-…deb`) or a tarball (run `./go.dot.sh`) | Needs ALSA, FreeType, fontconfig, X11, D-Bus (any desktop has them), and curl or wget to look for a newer version | `~/.local/state/Go.dot/logs` |

**On Linux with a multichannel interface**, PipeWire only offers two channels through ALSA. Install `pipewire-jack`, set the interface profile to *Pro Audio* in pavucontrol, and choose JACK in Show settings.

**On Windows**, ASIO drivers are supported and are the best choice for a pro interface.

Useful checks from a terminal (on Windows, `wfg.exe` in the install folder; on macOS, inside `Go.dot.app/Contents/MacOS/`):

- `wfg --version` — which build you have
- `wfg devices` — the audio interfaces Go.dot can play through
- `wfg midi` — the MIDI ports it sees
- `wfg plugins --scan` — find your VST3, AU and LV2 plugins
- `wfg validate <show>` — check a show folder and list every problem

**Newer versions.** Once a day, when it starts, Go.dot looks on GitHub for a newer release and says so on its title line: press it to see what is new, or to be told only of the next one. Nothing is downloaded: install the new version as you installed this one. *Help > About Go.dot* says which version you have, *Help > Look for a newer Go.dot now* looks at once, and *Help > Look for a newer Go.dot at launch* turns the daily look off, for a show computer kept offline.

*Show settings* lists its tabs down the left in five sections: Show, Audio, Video, Controllers and Getting started. The last, **Getting started**, lists every other tab under its section, with one sentence on what each is for.

## Your first show in ten minutes

A show is a folder that contains one or more `.wfg` files plus a `media/` folder that Go.dot copies your sounds into. Save it anywhere, carry it to another machine, and only the audio settings need a second look.

1. **Pick the audio interface.** *Show > Show settings… > Audio interface*. The GO button turns yellow once audio is running.
2. **Create and name your outputs** (mix channels or direct outs, mono or stereo). On *Outputs*, create the show's outputs ("Main L/R", "Sub", "Foldback"). On *Output patch*, say which physical channel each one lands on. Cues are routed by name, so a new venue is a new patch and no cue changes.
3. **Add sounds.** Drag WAV, AIFF, FLAC or Ogg files onto the cue list. Each becomes a media cue named after its file. Drop one on an existing cue to replace its file.
4. **Add other cues** from the *Add* row above the list: `memo`, `media`, `mic`, `video`, `fade`, `transport`, `osc`, `midi`, `process`, `script`, `group`. The ones with a ▾ open a short menu of kinds.
5. **Shape each cue** in the inspector on the right: name, pre-wait, post-wait, follow, level. The buttons at the top of the inspector open the panels at the foot of the window: *Waveform*, *EQ*, *FX*, *Sends*, *Curve*, *Timeline*.
6. **Run it.** The grey column at the far left of the cue list holds the pointer: the cue marked ▶ there is the standby, the one GO fires next. Its row is lit amber and its name stands beside the GO button. Click that column on any cue to move the ▶ there, or use ↑ / ↓. While the show is unlocked, a click on a cue's row picks it for editing and ↑ / ↓ then step from cue to cue; a click in the grey column gives them back to the ▶. Until a cue holds it, the top of the list says "no standby" and GO has nothing to fire. Hover or click the ▶ at the top of the column for a reminder. Press **Space** for GO.
7. **Save** with Ctrl+S (⌘S on a Mac). Autosave runs anyway.

### The keys that matter during a show

| Key | What it does |
| --- | --- |
| Space | GO — fire the standby cue |
| ↑ / ↓ | Move standby to the previous / next cue; unlocked, after a click on a cue's row, pick the previous / next cue |
| Esc | Stop everything gracefully: fade out over the panic time (1 s by default), run the groups' endings |
| Esc twice (within 0.75 s) | Drop everything at once, no endings |
| F9 | **Doh!** — take back the last GO |
| F | Flag the picked cues to look at after the run (F again takes the flag off) |
| Shift+F | Flag the cue the last GO fired |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo an edit (⌘ on a Mac) |
| Ctrl+L | Lock the show so nothing can be edited by accident |
| Ctrl+T | Load to time: start the show from the middle of a scene |

**Doh!** exists for the mistake that happens every so often: a GO pressed a beat early. Within a short window (set on the *Playback* tab), F9 stops what that GO started, puts standby back, and returns devices to the state they were in. What that GO stopped comes back where it would be now, fading in: a cue, or a sampler bank with the clips it was playing on their faders and pads. Sound that already reached the speakers cannot be taken back, and Go.dot does not pretend otherwise.

Two GOs closer than half a second are treated as one bounce of the finger and the second is refused. You can change this on the *Playback* tab.

## What you can try today

Everything below is built. Sections marked new arrived after v0.1 and need the newest build. Each row says where to find it.

### Playing sound

| Feature | What it does | Where |
| --- | --- | --- |
| Media cues | Play a file to named outputs at set levels, with pre-wait, post-wait and follow | Drag a file onto the list |
| In and out points, loops | Trim a file, loop a region, leave the loop on cue with an *Advance*; when the last out point stops short of the end of the file, a button under the range list makes the rest of the file a new range. A range taken to the end of the file plays to the end of whatever file the cue plays, a frozen edit or a replaced file included; one that begins past the end is marked *outside the file* on its row | *Waveform* panel; *transport > Advance* |
| Level lane | Draw a volume curve over the waveform, or record it from a fader | *Waveform* panel |
| Speed | 0 to 20×, as varispeed (pitch follows) or timestretch (pitch held), fades of speed, and playback backwards or bouncing between its in and out points | Inspector, *what it does* |
| EQ | Four bands and two filters on every cue | *EQ* panel |
| Plugins | A VST3, AU or LV2 chain on every cue. Plugins run in a separate process, so a crashing plugin silences its cue, not the show. Add one from the cue's own *FX* panel (new): it is switched in on that cue and loaded at once when nothing is playing. *Edit...* switches a plugin in as it opens its window, and *Hide unused plugins* shows only the ones the cue uses | *FX* panel; *Show settings > Plugins* to scan |
| Sends | Levels from a cue into the show's mix channels | *Sends* panel |
| Send lanes | Draw each send's level over the file, as the level lane does for volume | Waveform panel, lane picker |
| Editing a sound (new) | Cut a sound into sections at the playhead, drag them into a new order or remove them, drag a join to set its crossfade, type a trim for a section, then *Freeze* to bounce the edit to a new file the cue plays; *Unfreeze* brings the sections back. Lanes and ranges stay on the sound they sit on | *Waveform* panel, the sections row |
| Handles on a sound (new) | On the waveform each section has handles: the square in its middle is its volume; at each end the bottom handle moves the edge, the top one the fade's length (Shift: this side alone). The wheel over a fade bends its curve (Shift: this side alone). Drag in the top half to select time; press in the lower half to pick a section, drag there to slide it along the timeline. **x** splits at the playhead or the selection's ends, **Backspace** deletes leaving silence, **Shift+Backspace** closes up | *Waveform* panel, click it first |
| Right-click on the waveform (new) | Right-click the playhead, or anywhere and it comes there, for *Set to start*, *Set to end*, *Add marker*, *Add new slice* and *Split*. Double-click between two marks - markers, scene changes, slice edges, edit cuts, the start and the end - to select what lies between; Shift+click another interval to take it in too. Right-click the selection for *Set start and end*, *Make new slices…* (type how many), *Trim* (cuts the edit at both ends) and *Remove* (takes it out and closes the gap). A marker is a diamond at the top: drag it to move it, right-click or double-click it to rename or remove it | *Waveform* panel |
| Cue templates | Keep a media or video cue's settings under a name and start new cues from it | media ▾ menu; Edit > Save as template…; Show settings > Templates |
| The wheel over a number (new) | Turn any number in the inspector with the mouse wheel, no click needed: a level along the fader's travel, a time a tenth of a second a notch; Shift for finer steps | Inspector |

### Shaping time

| Feature | What it does | Where |
| --- | --- | --- |
| Groups | *Timeline* (members start together), *Sequential on GO*, *Sequential automatic*, *Shuffle* (new order each round), *Sampler* (sounds and pictures fired from pads and faders, a picture's fader setting its opacity), *Time group* (its members start when LTC, MTC or the clock reaches their time: see *Timecode in*) | *Add > group* |
| Joining a sequence's members (new) | Choose how one member leads into the next, once for the whole group: *Let play* (each member as it is), *Gapless* (the next starts exactly as the previous ends) or *Crossfade*, with a length and a curve. In an automatic group the next member is placed on the end of the one before; in a group stepped with GO, each GO cuts or crossfades whatever of the group is still playing. Pictures dissolve into each other. A sound's looping range can crossfade its wraps the same way | A sequential group's *Join* in the inspector; a sound's *range wraps* |
| Fades | Fade level, sends (into auxes too), EQ and plugin values of any cue, group or aux, with a drawn curve. A fade on a video cue moves its opacity, place, size and turn, a movie's speed or a blur. Each fade has its target's panels with the fade's changes shown on them: *Mixer*, *EQ* and *FX* for a sound or an aux, *Picture* for a video cue, where you drag the frame to where the fade takes it. Doh! puts back what the GO's fade changed (new) | *Add > fade*, then *Mixer*, *EQ*, *FX*, *Picture*, *Curve*. The same buttons are at the top of the inspector and in the panel's own head (new) |
| Transport cues | Stop, stop after this member or round, start another cue; enable or disable a cue for this run; jump standby to a cue, or jump and GO | *Add > transport* |
| Automatic names (new) | A cue nobody has named is called by what it does: a fade or a stop by its target (*Fade out Intro music*), an OSC or MIDI cue by its message, a sound, a picture or a mic by its file or input. It follows when the target, file or message changes. Type a name and it stays; clear it and the automatic one comes back | Name column; inspector, *Name* |
| Triggers | Fire cues from incoming OSC, MIDI or the wall clock; the standby never moves when one fires. Or give a cue a key - of the computer's keyboard, any but the show's own (Space, Esc, F9 and the window's shortcuts), or of a Stream Deck: pressed, it is a GO from that cue - the cue starts and the standby moves on to the next one in its list, and Doh! takes it back | Cue inspector, *when*; a cue's *Key* line, then press the key on the keyboard or the Stream Deck |
| Carts (new) | A list that GO never meets, for cues fired out of order: by their triggers, a key, or the Android remote's *Cart* tab. Any kind of cue - a sound, an OSC or MIDI message, a script, a fade - and a group as a macro: a timeline group fires everything in it at once, an automatic sequence one after the other with its waits. A sampler group in a cart is armed all show, and its pads hold any kind of cue | The **+** beside the list tabs, *New cart*. The tabs over the list show each list; ▸ marks the one GO acts on, and a double click renames one |
| Load to time | Jump into the middle of a scene with the right cues playing at the right offsets, and the desk and the MIDI gear as the scene had left them by that second | Ctrl+T. On the web console, the *aim* bar: pick a cue, slide to the moment and read what would be playing, then press *load to time*. Scrubbing a running scene in the running pane does the same for its messages |
| Anticipation | The cue on standby is loaded and its devices prepared before you press GO | Automatic |

### Live sound

| Feature | What it does | Where |
| --- | --- | --- |
| Mic cues | Bring a named input up through a rack channel with its own plugins, picked on the cue's *FX rack* line | *Add > mic*; *Show settings > Inputs*, *Rack* |
| Aux channels (new) | A reverb or delay return the cues send into: its own plugins, level, output and sends, kept with the show; a fade can move them for the night. Click an aux's name in a cue's sends panel to open the aux itself | *Show settings > Rack*: + Mono aux, + Stereo aux; a cue's *sends* panel |
| Live sampling | Record sound during the show, then loop it, overdub it or clear it. A take can be as long as you type, up to a day (new). *Pad from loop* makes a cue that plays a slice of the take, even while it records: put it in a sampler bank, a cart or on a key; *Keep* later moves it onto the kept file (new) | *Take* panel; *transport > On a take*; *Show settings > Rack* for its length |
| Live recorder | Keep the moment of every cue you fire during a run, as a take | Ctrl+Shift+R |

### Talking to the rest of the rig

| Feature | What it does | Where |
| --- | --- | --- |
| OSC cues | Send to a desk, a processor or a video server, from a list of the device's addresses: several messages or bundles in one cue, and curves played on the cue's clock, recorded from the device's own reports or a SpaceMouse. A cue of thousands of messages - a processor's whole snapshot - leaves at the pace the device takes, set in its *Per tick* cell. In the messages table and the grid window, drag the line between two column headings to make a column wider or narrower; a double click on the line puts it back. The widths stay on this computer, not in the show | *Add > osc*; *Show settings > Network* |
| Cues written by WFS-DIY and S21_HiJack (new) | Store a snapshot in WFS-DIY or S21_HiJack and it lands in Go.dot as one cue after the cue picked in the list, or at the end of the list when nothing is picked; store it again and the same cue is updated. Each declares itself as a device, and its own addresses and snapshot names become menus, one per part of the address, under the cue's *target* | WFS-DIY: *Network* tab, protocol Go.dot, then *Write to Go.dot*; S21_HiJack: *Setup > Cueing system: Go.dot* |
| Address and value menus (new) | On a device that describes itself, pick the address part by part and the value from its list | Cue inspector, under *target* |
| Device presets (new) | Make a device from a preset for your desk or processor - Yamaha, DiGiCo, Allen & Heath, Behringer and Midas, SSL Live, ETC Eos, grandMA, Holophonix, L-ISA, d&b Soundscape, Spat Revolution, ADM-OSC, MIDI Show Control. Every protocol the device speaks is a preset of its own, the cue's menus follow it, and the message leaves on the device's own wire: OSC over UDP or TCP, Yamaha's RCP, a telnet line or MIDI. The device's rows say where it is (the Where cell: by datagram, over a connection, a serial port, a MIDI port or an MQTT broker) and how it is heard back. | *Show settings > Network > PRESET* |
| Your own presets (new) | Keep presets of your own beside the shipped ones: a file in your presets folder is offered under *Yours* when you open *PRESET*, and one named like a shipped preset is used in its place. Save any device that describes itself as a preset in one click - one found on the network, one that declared itself, or one made from a preset you want to change - and make it again in any show, on this computer or another once the file is copied there. | *Show settings > Network*: *Save as a preset…* in the device's *Kind* cell; *Show presets folder* |
| Devices found on the network (new) | Go.dot lists the devices on the network that describe themselves over OSCQuery - another Go.dot, Chataigne, ossia score, anything that announces itself - and one pick makes one a device of the show, its addresses offered as menus. A device that does not announce itself is given by its address. Go.dot announces itself the same way, so those programs find it too. | *Show settings > Network > FOUND* |
| MQTT (new) | A sensor, a controller or a hub that speaks MQTT is a device of the show: its topics are addresses under the device's prefix, so a patch, a script or an OSC cue hears it and publishes to it as with any device. Go.dot connects to any broker, and runs a broker of its own for a device that has nowhere else to publish. | *Show settings > Network* (the address cell's menu; the *MQTT broker* switch under the table) |
| Doh! rollback | When a GO is taken back, send each device the command that undoes it | Cue inspector; *Network* tab |
| MIDI cues | Program change, control change, notes, pitch bend, SysEx | *Add > midi* |
| Bindings (new) | Wire a MIDI fader, knob, note or pitch bend, or an OSC address, to any number in the show - a cue's level, a send, an EQ gain, a device's parameter - scaled into its range, for the whole show or only while a cue runs. *Learn*: move the control and it is bound. Faders and knobs set the value, endless encoders turn it a step at a time, pedals and joysticks move it faster the further they are pushed; a knob without a motor picks the value up before moving it, and a motor fader is sent the value back | Right-click a number in the inspector, *Bind a control to it*; *Show settings > Bindings* |
| Timecode out (new) | MTC on a MIDI port, or SMPTE LTC on an audio output, from a cue. It starts at the label you give it (01:00:00:00 unless you say otherwise) and runs until it is stopped, or for a length. Put it in a group with a song and both start on the same sample; a second timecode cue takes the line over; Esc stops it on a frame boundary; Doh! pauses it and the next GO picks it up where it stopped; drag it in the running pane to jump. Pick the frame rate, the MIDI port and the output once for the show, and give LTC an output of its own: it carries nothing else. | *Add > midi > MTC*, *Add > audio > LTC timecode*; *Show settings > Timecode* |
| Timecode in (new) | Start sounds, pictures, timeline groups and automatic sequences when incoming timecode reaches their time. Pick the cues, choose *Add > group > Time group*, say what it *follows* (*LTC in*, *MTC in* or *the clock* of the computer) and give each member the time it *starts at*. GO arms the group and it stays armed until stopped, as a sampler bank does; in the Persistent section it stays armed all show, and the standby never moves when a member fires. A member started by LTC starts on the very sample the code reaches its time. Each member also says how it keeps up with the code, its *chase*: *free run* plays at its own speed once started; *jump* jumps back into step over a 10 ms crossfade when it is more than 10 ms out; *catch up, varispeed* and *catch up, timestretch* play up to 1 % faster or slower until back in step, the first bending the pitch and the second keeping it, and jump instead when more than a quarter of a second out. When the code stops, members keep playing; when it jumps, the chasing members are put where it says and the free runners are left alone. Go.dot keeps its own clock and measures the code against it, so the rest of the show keeps its own time whatever the code does. Pick the LTC input and the MIDI port the code comes in on, and how closely members chase, once for the show; the tab shows what is arriving. | *Add > group > Time group*; *Show settings > Timecode > Coming in* |
| Network monitor | See every message in and out | *Show > Network monitor…* |
| Control surfaces | Faders, pads, dials and DCAs, on a Mackie Control surface or a virtual panel on screen. Full support for the Asparion D700: faders, EQ and plugin pages on the rotaries, a master dial, standby on the arrows | *Show > Surfaces…* |
| Stream Deck (new) | Every model, plugged into this computer. A deck of keys plays a cart: its cues on the keys with their numbers, names and colours, lit while they play, and a key to turn the page when the cart is longer than the deck. A key given to a cue from its *Key* line keeps that cue on every page, and the cart fills the other keys. Switch on *Show keys* for GO, Esc, Doh! and the standby's arrows at the end of the last row. On a Stream Deck+, the top keys pick *Send*, *EQ*, *FX* or *Loop* and the four dials turn the picked cue's, with what each one turns written above it; the keys below step to the previous or next cue. The settings show a picture of the deck's keys, plugged in or not, and pressing a key there presses it. When Elgato's own Stream Deck app is running, Go.dot offers to close it | *Show settings > Surfaces > Add surface > Stream Deck* |
| Web console | The same show in a browser, on any machine on the network | `http://<this machine>:5010/ui` |
| The web console on a tablet (new) | Worked by touch, an iPad's included, as the Android remote is: **PANIC** stops everything, and pressed again within 0.75 s drops everything, with **Doh!** beside it; swipe a running cue far either way to kill it, tap it for its stops; tap a member of a running group to skip it this round, hold it to skip it until the group ends; drag a playhead, finer the further your finger is above or below it; hold a cue for *Fire now*, *Park the standby here* and *Flag*; *pick several* to pick cues without a keyboard; drag a number sideways to move it from where it is, finer further from the box. A mouse does all of it the same way | `http://<this machine>:5010/ui` on the tablet |
| Serial ports | Hear an Arduino's lines in a process cue or a script cue and send lines back | Show settings > Serial |

### Video (new)

| Feature | What it does | Where |
| --- | --- | --- |
| Video cues | Show a movie, a still, a colour fill, a feathered mask or a live input on a canvas, with GO, Esc and Doh! as for sound | *Add > video*, then choose the canvas |
| Sound or picture delay (new) | A projector shows its picture a little late, and a long audio chain plays its sound a little late. Tell a cue which one to hold back and by how much: *Delay* set to *Sound* or *Picture*, and *Delay by* in milliseconds. The other starts when the cue does; the one held back starts that much later and ends that much later, and the cue ends when both have. A sound cue can hold back its sound to match a picture fired beside it, and a movie's own sound follows the movie | Inspector, *Delay* and *Delay by* |
| HAP playback | HAP, HAP Alpha and HAP Q play at full speed. Other movies play as a preview and convert to HAP in the background (FFmpeg is downloaded on first use) | Automatic; inspector |
| Analysis cache | The colours, levels and movie strips Go.dot works out are kept beside the media and reused next time; those of files no longer in the media folder are cleaned up at every launch | Automatic; *Show > Clean up the analysis cache* |
| Movie strip | Thumbnails along the strip with scene cuts marked; the playhead and the in and out points snap to a cut (Alt lets go); the *Scene changes* and *Snap to scene changes* ticks turn either off; a movie's locked sound is edited beside it | Top of the inspector |
| Editing a movie (new) | Cut a HAP movie into sections at the playhead, on its frame grid; drag them into a new order or remove them; drag a join to set its dissolve. Its locked sound is cut in step and keeps its lanes on the sound. *Freeze* bounces movie and sound to new files the cues play; *Unfreeze* brings the sections back. A movie that is not HAP says so: convert it first | *Strip* panel, the sections row |
| Picture panel | Fit, fill or stretch; scale, move, turn, flip; contrast, saturation, gamma, hue and four curves; normal, add, screen and multiply blending | *Picture* panel |
| Keys (new) | A picture, a movie or a camera made see-through by its own alpha, by its brightness, by its brightness as a shape in a colour, or by a blue or green screen with the screen's colour taken off the edges | *Picture* panel, *Key* |
| Text cues (new) | Titles and slides: words in a font, a size and a colour, with an outline that can glow. The words can hold fields like `{score}`, which any program fills by sending `/godot/text/score` over OSC - at once, even while the show is locked, and never saved | *Add > video > Text*; *Picture* panel, *Fields* |
| Fonts in the show (new) | A show can carry its own fonts, in a `fonts/` folder beside its media, put there from the font menu: an installed font carried, or a font file added. A title then looks the same on every machine, Mac or PC, main or backup, and *Save as* takes the fonts along. A font's size is the letters' size, as in any layout program | *Picture* panel, the font menu: *Carry it in the show*, *Add a font file...* |
| Blur cues (new) | Blurs everything beneath it on its canvas, inside or outside an outline; a fade moves how far | *Add > video > Blur*; *Picture* panel |
| Canvases and mapping | Several canvases on one output, each bent onto the wall with a mesh warp, and an ASC CDL per output to match projectors. **Grid** shows a grid on the projector, bent by the warp, to set the geometry by | *Show settings > Video*; warp editor; the Android remote's *Mapping* tab |
| NDI, Spout, Syphon | Send an output to another program, take a program's picture in as a live input, or pass a cue's picture through another program and back | *Show settings > Video* |
| Video cards and cameras (new) | Send a canvas down a Blackmagic DeckLink or AJA card's SDI or HDMI cable, at the format the projector or mixer downstream expects, with the show's sound inside the same cable; on a card with a keyer, send fill and key so a vision mixer lays your pictures over another. Take a picture in as a live picture: from a card, or from a camera - a webcam, a USB capture dongle, an ATEM Mini or a Web Presenter in webcam mode - each named with its maker, at the format you pick or the best it offers. A card needs its maker's software (Blackmagic's Desktop Video, AJA's Desktop Software), and `wfg video-probe` lists what your cards and cameras can do. On a Mac, the first camera you use asks your leave | *Show settings > Video*, an output's *Display* menu, *Sent to a video card*; *+ input*, kind *Video capture*, then the device and its *Format* |
| Read ahead | The next GO's stills and movies are loaded before you press it, and every cue row says whether its cue is ready | Cue list |
| DCAs on pictures | A DCA rides pictures as opacity; the knob above a DCA strip sets the curve a picture comes in on and the sound's offset. On a DCA that rides outputs or canvases (new), the knob moves each output's trim and each canvas's curve by the same amount, keeping their differences, even with nothing playing; a canvas's curve can also be typed | Surfaces; virtual panel; *Show settings > Video*, *Curve* column |
| Video monitor | The picked cue alone on screen while you adjust it | Opens with the strip or the picture panel |

Pictures are drawn by a separate process that Go.dot watches, so a slow movie cannot hold up a sound cue.

### Process cues: Pure Data inside (new)

A **process cue** holds a Pure Data patch and runs it while the cue runs, at control rate, never in the audio path. It hears what arrives (device reports, OSC, MIDI, serial lines) and sends to devices, to MIDI and to Go.dot's own commands. Edit the patch on Go.dot's canvas at the foot of the window, where values move live and toggles, bangs, sliders and number boxes play, or open it in plugdata or Pure Data and each save comes back. Ready-made abstractions cover the usual chores: `go.avg`, `go.minmax`, `go.smooth`, `go.scale`, `go.deadband`, `go.change`, `go.edge`, `go.hold` and `go.ratelimit`, each with a help patch, plus an example show. *Add > process*.

The **?** at the canvas's corner opens a help patch that shows, with boxes to copy, how a patch hears and sends:

| To... | In the patch |
|---|---|
| Hear a device's OSC (Show settings > Network, Rx on) | `[r /its/address]` |
| Send to a device, here or on another computer | `[s /its/address]` |
| Fire a cue | `[; /godot/cmd/cue/fire 3.1(` by its number, or by its id |
| Run any of Go.dot's commands | `[s /godot/cmd/go]`, `standby/set`, `run/stopAll`... |
| Hear or set a row of the show | `[r /godot/...]`, `[s /godot/...]` |
| MIDI (the cue's MIDI in and MIDI out) | `[notein]`, `[ctlin]`, `[midiin]`... and `[noteout]`, `[ctlout]`... |
| An Arduino (Show settings > Serial) | `[r /godot/serial/<id>/in]`, `[s /godot/serial/<id>/out]` |
| A sensor on an MQTT broker (Show settings > Network, an MQTT device whose prefix is `/house`) | `[r /house/room1/temp]`, `[s /house/lights/1]` |
| The SpaceMouse | `[r /godot/puck]` |

Right-click a cue in the list, or a port in the Serial tab, and choose *Copy for a patch* to get its box with its id; paste it on the canvas, or into a box in Pd. A cue dragged from the list onto the canvas becomes a box that fires it. Every patch opened in Pure Data shares one Pd, its window comes to the front, and on a high-resolution screen Pd's menus and patches are drawn at a readable size.

A device that speaks MQTT is added in the Network tab like any other: click its address cell and choose *On an MQTT broker, at an address...* (the venue's Mosquitto, a Home Assistant) or *On Go.dot's MQTT broker*, which Go.dot runs itself when the *MQTT broker* switch under the table is on. The same menu takes the topics it listens to (`#` for everything). Each topic is then an address under the device's prefix: `room1/temp` on a device whose prefix is `/house` is `/house/room1/temp`, and a payload is read as a serial line is, numbers as numbers.

### Script cues: JavaScript and Lua (new)

A **script cue** does what a process cue does, written as text instead of drawn: a few lines of JavaScript or Lua that run while the cue runs, at control rate, never in the audio path. A script hears what arrives (device reports, OSC, MIDI, serial lines, rows of the show) and sends to devices, to MIDI and to Go.dot's own commands. Pick the language in the inspector; write the script in the editor at the foot of the window, where a mistake is marked on its line and what the script prints shows under the code. *Open in your editor* hands it to your computer's own text editor, and each save comes back. *Add > script*.

```js
// Doubles what the console reports for source 1's x, and fires cue 3.1 past 0.8.
godot.hear('/wfs/source/1/x', (x) => {
    godot.send('/wfs/source/2/x', x * 2);
    if (x > 0.8) godot.fire('3.1');
});
```

The **?** beside the editor opens a help card in the script's language, with a line to copy for each way in and out, and ready-made scripts to start from (an average, a minimum and maximum, smoothing, a line crossed):

| To... | In JavaScript | In Lua |
|---|---|---|
| Hear a device's OSC (Show settings > Network, Rx on) | `godot.hear('/its/address', (x) => ...)` | `godot.hear('/its/address', function (x) ... end)` |
| Send to a device | `godot.send('/its/address', 0.5)` | the same |
| Fire a cue, by its number or its id | `godot.fire('3.1')` | the same |
| Run any of Go.dot's commands | `godot.command('go')`, `standby/set`, `run/stopAll`... | the same |
| Hear or set a row of the show | `godot.hear('/godot/...', ...)`, `godot.set('cue/<id>/name', 'Rain')` | the same |
| MIDI (the cue's MIDI in and MIDI out) | `godot.midi.hear(...)`, `godot.midi.send(0x90, 60, 100)` | the same |
| An Arduino (Show settings > Serial) | `godot.hear('/godot/serial/<id>/in', ...)`, `godot.send('/godot/serial/<id>/out', ...)` | the same |
| A sensor on an MQTT broker (Show settings > Network, an MQTT device whose prefix is `/house`) | `godot.hear('/house/room1/temp', ...)`, `godot.send('/house/lights/1', 1)` | the same |
| The SpaceMouse | `godot.hear('/godot/puck', ...)` | the same |
| Later, every so often, every tick | `godot.after(1.5, ...)`, `godot.every(0.25, ...)`, `godot.tick(...)` | the same |

Right-click a cue in the list, or a port in the Serial tab, and choose *Copy for a script* or *Copy for a Lua script* to get its line with its id; a cue dragged from the list onto the editor types the line that fires it. A script has no files, network or modules of its own: it reaches the world through Go.dot, so the show's log holds what it did. A script caught in a loop that never ends is stopped after a second, and the show runs on. The example show *Process examples* has the same light sensor as a patch, in JavaScript and in Lua. An MQTT device is heard and published to by its addresses, as in the table; the ready-made script *relay* hears a sensor on a broker, fires a cue past a line and publishes a light's state.

### Importing QLab and Ableton Live shows (new)

- **QLab**: File > Import QLab workspace… reads a QLab 4 or 5 workspace file directly, with no QLab needed. Tick the cue lists you want; Go.dot writes a new show (cues, groups, levels, fades, routing and targets) and a report of what it could not carry over. Text and video cues come over too (new): each QLab stage becomes a canvas of its size, its outputs waiting for a projector to be picked, and movies, stills and titles are placed and faded as in QLab. Some of QLab's own numbers are still being checked against a test workspace, and the report names the fonts a title uses that this machine has not got. MIDI cues come over too (new), fades and all, as do hotkeys and MIDI triggers, network cues sent to an Eos or to another QLab, camera cues and carts. If the sounds are not where the workspace says - a show carried off its computer - Add a folder to search… in the import window looks for them by name, in as many folders as you point it at; File > Find missing files in a folder… does the same for a show that is already open. A QLab devamp that starts the next cue as its loop ends comes over as such: on any advance cue, *then GO next* fires the cue after it on the very sample the loop completes.
- **Ableton Live**: the importer unzips the .als file (a gzipped XML document) and reads the XML inside to turn the Live set into cues and groups, so a show built in a session can move to the cue list. One scene is one GO, and the silent "track in" clips become the fades they stand for. It reads Live 10 to 12, turns a tour into one show with a performance per venue, and lives under File > Import Ableton Live set… A return with effects on it now comes over as an aux, its sends heard: Live's own effects (Reverb, Delay…) cannot be loaded outside Live, so the aux is left empty for a plugin of yours, and the report says which ones were there.

### Keeping the show safe

- **Undo history** for every edit (Ctrl+Shift+U), and continuous autosave.
- **Lock** the show during performance (Ctrl+L): GO, Esc and Doh! still work, editing does not.
- **What this computer lacks, shown (new).** A cue that needs something missing tonight - the audio interface, a screen, a MIDI device, a plugin, a file - wears a warning and a word after its name (*no audio*, *no screen*...); hover for why. A button on the title line opens the *Show settings* tab concerned.
- **A Mac held back, said (new).** With Low Power Mode on, or the Mac hot enough that macOS has slowed it, a warning on the title line says so before anything falls behind; press it to open the Battery settings. Go.dot also tells macOS it is latency-critical, so the system never naps it or lets the computer sleep while a show is open.
- **A Windows laptop held back, said (new).** Running on the battery, in Battery saver, with the power mode slider at *Best power efficiency*, on the *Power saver* plan, or with the processor limited below its full speed: a warning on the title line names the setting; press it to open *Power & battery*. Go.dot also tells Windows it is never to be throttled, which on a hybrid processor keeps it on the performance cores, keeps the computer awake while a show is open (`powercfg /requests` lists it), and runs its clock on a fine timer so a tick is no longer at the mercy of Windows' 15.6 ms one.
- **A Linux machine held back, said (new).** Go.dot holds the *performance* power profile and keeps the computer from suspending while a show is open (`powerprofilesctl list-holds` and `systemd-inhibit --list` show it under Go.dot's name), and the title line warns when the *Power Saver* profile or a capped clock holds the processor back, when a laptop runs on its battery, or when the machine is hot enough that the kernel has slowed it; press it to open the power settings. Real-time scheduling, which a stock desktop user is refused, is now asked for through RealtimeKit the way PipeWire does, so the show's clock, the plugin host and the ALSA callback run in the real-time class with no group or admin step; if even that is refused, a warning says what to do.
- **Flags (new).** Something to fix? Press F on the picked cue, or Shift+F for the cue the last GO fired, even with the show locked. A flag stands just left of the cue's type until F takes it off. Flags are kept when the show is saved.
- **Show and performances.** A show is the piece; each night is a performance, a copy you can change without touching the original. *File > New performance…*. At the end, Go.dot lists what changed, cue by cue, so you can bring the good changes back into the show.
- **Interface trouble.** If the audio interface disappears, the show pauses and resumes when it returns. If its sample rate changes, Go.dot follows it.
- **Double-click a `.wfg`** to open the show (Windows installer, Linux .deb, macOS).
- **Several shows at once (new).** Each show opens in its own window. *File > Open recent* lists the ten shows this computer opened last. *File > Close other shows* closes every other window in one go: a show with unsaved changes asks first in its own window, and a locked show stays open.

### Two computers: a main and a backup (new)

Two computers can play one show together. The **main** plays it and talks to the rig; the **backup** follows it a moment behind, starting each sound on the same sample, and takes over if the main stops.

**What each computer needs.** Its own copy of the show folder, sounds and movies included: nothing is copied between the two over the network. Its own audio interface, and its own video outputs if the show has pictures. The sound and the pictures are changed over on your console or matrix: Go.dot does not switch them. The two on one network, or on two for a spare path.

**Setting it up.** On each computer, *Show settings > Redundancy*: say whether this computer is the *Main* or the *Backup*, give the other computer's address (its IP address; port 5010 is assumed; two addresses separated by a space for two networks), and how long a silence counts as losing the link (1 s by default). These settings belong to the computer, not to the show: copying the show to the other computer does not carry them.

**What it says.** Beside the show's name at the top of the window each computer says what it is: *Main - sending*, *Main - alone* (no backup with it), *Backup - following*, *Backup - waiting* (it has not found its main since it started), *Backup - sending* (it took over), *Main - following* (a main started again after its backup took over). When something needs a hand, a banner under that line says so, with the button that answers it:

| The banner | What it means | The button |
| --- | --- | --- |
| *Backup - waiting* | The backup has not found its main; it sends nothing | **Take over**: it sends from now on |
| *Two mains!* | Both computers are sending, usually after the link came back from a takeover: every command reaches the devices twice | **Make this the main**, on the computer that should keep the show; the other one follows it |
| Following, but not in step | Something differs between the two at the same moment, such as a cue running on one only | **Realign**: the backup takes the main's position again |
| Following, but the show differs | The two shows are not the same, for example an edit made while the backup was off | **Take the main's show**: the backup takes the main's cues and settings, keeps its own interface and ports, and sets its old files aside in a dated folder of the show. Sounds are never copied: a file missing on one computer is named, and is yours to copy |

The main is told too: when the backup is not in step or its show differs, the main's banner says so in the backup's words, and its *Redundancy* tab shows whether the backup is in step and has the same show - the buttons that mend it are on the backup.

The same commands, with **Hand over** (the main lets the backup send), are under *Show > Redundancy* in the menu, which works while the show is locked, on the *Redundancy* tab with what each one would do, and in the web console's line under the transport.

**When the main stops.** The backup stops hearing it and, after the link timeout, takes over: it sends to the devices from then on and says *The main was lost and this machine took over*. An Esc or a double Esc pressed on the backup in that moment is applied; anything else the main had not confirmed is dropped. Change your console or matrix over to the backup. When the main is started again, it finds the backup sending and follows it.

**Timing.** While it follows, the backup sends nothing to devices (OSC, MIDI, serial); a command you give it is passed on to the main. The two start each sound on the same sample: on two computers over Dante, within about a tenth of a millisecond. The *Redundancy* tab shows how closely they keep together, and each interface's output delay.

### The Android remote (new)

Go.dot has an Android app for a tablet or a phone. It stays connected in the background, so it is live the moment you look at it, and you can switch to another app and back without waiting.

**Getting it.** Install the APK from the latest release at [github.com/pob31/go.dot-remote/releases](https://github.com/pob31/go.dot-remote/releases) (Android 8 or later; let your browser install it when Android asks). On the *Connections* tab, add the computer running Go.dot: its IP address and port 5010, the address the web console uses. The tablet and the computer must be on the same network.

**Always on screen.** **GO**, **Doh!** and **PANIC** (twice within 0.75 s kills everything), the current cue and the next one with its notes, arrows to move the standby, and, in words, a locked show or a command that was refused. At the end of the tab bar, a mark for each connection: a green dot when it is live, an orange ring while it connects, a red cross when it is lost. Tap them to open *Connections*.

| Tab | What you can do |
| --- | --- |
| *Cues* | Every list. Tap a cue to adjust it: level, trim while it plays, sends, the 16 EQ controls, its DCA, waits, speed, on or off, flag and notes, with its waveform. Tap the left edge to park the standby there; hold for *Fire now* |
| *Running* | Everything that is playing: stop, kill (a long swipe or the menu), advance, skip a member of a round, drag the playhead, and a large trim fader for each sound |
| *Surface* | The show's surfaces, strips and all: a D700 that is not plugged in works from the tablet. *This tablet* gives a fader for each DCA. *EQ* and *Sends* pages for the selected sound |
| *Cart* | Every cart's cues as pads: touch one to fire it, as hard as where your finger lands; a cue that plays while held stops when you let go. There while the show has a cart |
| *Mapping* | Each video output's warp and zones: drag a point or nudge it, change the grid of points, a zone's opacity and blend, **Identify** the projector, and **Grid**, which shows a grid on the projector bent by the warp |
| *Script* | Choufleur's script page, once its address is set (below) |
| *Connections* | The computers, a main and its backup together with their *Take over* buttons; the tablet's name; Find this tablet; Choufleur and its wrist buzzer; *Quit the remote* |

**Riding levels in a locked show.** A cue's own level is part of the show, so the lock keeps it. Ride the trim of the sound playing, or a DCA: both are tonight's and never saved. EQ and sends ride live, as on the D700.

**Find this tablet.** In Go.dot, *Show settings > Network* lists the connected tablets with a **Find** button: the tablet flashes its screen and its camera light, sounds its alarm and vibrates until it is touched.

**Choufleur on the tablet.** On *Connections*, type the operator address Choufleur gives you, or scan its code. The *Script* tab then shows the script following the show, under GO. A Choufleur wrist buzzer is linked on the same tab and taps your wrist before your cues.

**Stopping it.** *Connections > Quit the remote* unlinks everything and stops it running in the background. The next launch links the same computers again.

## Coming next

These are designed in the specification and not built yet. The order may change with what testers ask for, which is one more reason to send feedback.

### Integration with software

| System | What Go.dot will do with it |
| --- | --- |
| **WFS-DIY** (wave field synthesis) | Command its sources, positions and LFOs, claimed per cue so two cues never fight over one input. Writing its snapshots into Go.dot as cues already works (above) |
| **XOA / Tight-WFS** | The same, for ambisonic and WFS rendering |
| **S21-HiJack** | Drive it as a sidecar for the DiGiCo S21, under its own branch of addresses. Writing its snapshots into Go.dot and being driven by Go.dot's GO already work (above) |
| **Millumin** | The escape hatch for shows whose video outgrows Go.dot |

WFS-DIY, XOA/Tight-WFS and S21-HiJack come from the same author as Go.dot, as does Choufleur, the script follower the Android remote already shows (above). They stay separate applications, possibly on other machines; Go.dot directs them over the network.

### Integration with hardware

- **Sound consoles**: snapshot recall, fader and channel moves, and Doh! rollback, as presets (above): DiGiCo, Yamaha, Allen & Heath, Behringer and Midas, SSL Live; any desk that speaks OSC or MIDI also works as an opaque device.
- **Lighting consoles**: GO and cue recall over OSC, MIDI and MIDI Show Control - ETC Eos and grandMA presets (above) - and sACN as an input source.
- **Control surfaces**: HUI, Icon, Behringer and PreSonus, after Mackie Control, the D700 and the Stream Deck (already built, above). A SpaceMouse already moves OSC cue curves.
- **Device templates**: a shared library of the presets users make, for the desks and processors that cannot describe themselves. The shipped presets are its first entries, and yours can join them (above).

## Sending feedback

This release exists so that people other than the author can say what they found. Tell us what you liked, what you would change, and what went wrong: all three shape what comes next. Every message helps, including "I could not work out how to…".

**Where:** [github.com/pob31/go.dot/issues](https://github.com/pob31/go.dot/issues). A free GitHub account is enough. Write in English or French.

**A bug report is most useful with:**

1. The build's name (the download's file name, *Help > About Go.dot*, or the first line of `wfg --version`).
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
- Your desk, processor or timecode source with Go.dot: the device presets and timecode in and out were built from the published documents, so tell us what yours did.

**Code contributions** are welcome too. Build instructions and the repository's conventions are in the README, under *Building* and *Contributing*. Go.dot is GPL-3: what you contribute stays free.
