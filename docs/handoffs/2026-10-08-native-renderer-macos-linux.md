# Handoff: the new video renderer, NDI, Spout and Syphon on macOS and Linux

*Written 2026-10-08 by the Windows session that built them, for the author and for whichever Claude session
runs on the **Mac mini** and on the **Linux NUC**. Everything below is on `main` at `a1c9f1a` (N.5) or
later. Namespace draft §44 is the design; this is how to try it.*

## In one paragraph

The video renderer (`wfg video-render`) no longer draws with JUCE's OpenGL. It draws through **sokol_gfx**
on each system's own graphics: Direct3D 11 on Windows, **Metal on macOS**, **OpenGL through EGL on Linux**.
One device draws each canvas once a frame for every projector, and each display is paced by its own
refresh. On that renderer, an output can now **send** its canvas over NDI, Spout (Windows) or Syphon (Mac).
A show can declare **video inputs**, which a **capture** cue shows, and **inserts**, which send a cue's
picture to another program and show what comes back. Everything was built and tested on Windows. The Mac
and Linux halves **compile on CI but have never run**. That is what this handoff is for.

## Already done and seen (do not redo)

| what | where seen |
|---|---|
| The renderer's pixels held to the reference compositor (`Compositor.h`) | WARP and an NVIDIA RTX PRO 2000, Windows |
| Projectors on two displays (GeChic 1303 HDMI, a 3840×2400 USB-C): 237–239 frames in 4 s, 2–3 late at the start, jitter 0.02–0.08 ms | The author's laptop; came up clean after `f0ac3d7` |
| Spout output, input, capture and insert round trip, in-process and across processes | Windows tests (`VideoSpoutTests`, `VideoHostTests`) |
| NDI output loopback against NDI 6.3.2 | Windows, NDI 6 Tools installed |

## Before building

- **Linux:** run `bash scripts/install-linux-deps.sh` again. It now installs `libegl-dev`,
  `libopengl-dev`, `libegl-mesa0` and `libgl1-mesa-dri`; CMake fails at `find_package(OpenGL ... EGL)`
  without them.
- **Both:** `git submodule update --init` picks up two new submodules: `ThirdParty/sokol` and, used on
  the Mac only, `ThirdParty/Syphon`. Spout's sources and NDI's headers are vendored in
  `ThirdParty/spout` and `ThirdParty/ndi`.
- Build as usual: `cmake --preset dev`, then `cmake --build --preset dev-debug`.

## 1. The automated tests (no window opens)

```
wfg_tests "--test-case=video gpu:*"                      # software rasteriser on Linux; Metal on the Mac
WFG_GPU_HARDWARE=1 wfg_tests "--test-case=video gpu:*"   # Linux: the NUC's own Intel GPU instead
wfg_tests "--test-case=video*" "--test-case-exclude=video gpu:*"
```

- Each GPU case says which device it drew on (`drawing on Metal on Apple M…` /
  `OpenGL on Mesa Intel(R) …`).
- `video gpu: each program and each way of laying draws alone` writes every step to stderr before trying
  it. If something crashes, **the last line names the program it crashed on**.
- **Known CI problem:** on the GitHub Linux runner, Mesa's software rasteriser (llvmpipe) ran out of memory
  compiling these shaders ("LLVM ERROR: out of memory", then SIGABRT). It may not happen on the NUC's real
  GPU. Please report what both commands above do there.
- **Known CI problem, Mac:** on the GitHub macOS runner, whose GPU is a virtual machine's ("Apple
  Paravirtual device"), Metal would not compile the shaders (`METAL_SHADER_COMPILATION_FAILED`). The
  Metal source looks ordinary, so this is most likely the virtual device, but nothing has proved it.
  **Run the first command before anything else on the Mac mini.** If it fails, the message now names the
  program ("the fill shader would not compile on Metal on Apple M…") and gives the Metal compiler's own
  words: send those back. Since `8984614` both runners skip these cases (`GITHUB_ACTIONS`), so CI no
  longer checks the GPU's pixels on Linux or macOS. The Mac mini and the NUC are where it is checked.
- **The Mac's NDI case** runs only if an NDI runtime is installed (NDI Tools). Without one it says so and
  checks that the refusal is in words.

## 2. Projectors on the new renderer (opens windows)

The bench opens one window on a display you name for about 6 seconds: a blue fill fades up, holds, then
goes. It prints frames, late frames and jitter.

```
WFG_VIDEO_BENCH=1 wfg_tests "--test-case=video bench*"                                   # lists displays, opens nothing
WFG_VIDEO_BENCH=1 WFG_VIDEO_BENCH_DISPLAY="<name>" wfg_tests "--test-case=video bench*"  # opens the window
```

To look for:
- **No rectangle in a corner, no flash.** Black until the first frame, then the whole display.
- About one frame per refresh: 240 in 4 s at 60 Hz. A few late frames at the start are normal; a graphics
  chip waking takes up to two seconds.
- **Mac:** a CAMetalLayer view inside the JUCE window, with a display link per display. `coverDisplay` is
  Windows-only, so window placement is JUCE's, as it was with the old renderer.
- **Linux:** an X11 child window and an EGL surface on Go.dot's own X connection. **Paced by a clock**, not
  each display's refresh (Windows and Mac wait on the display), so a frame may tear without a compositor.
  Under Wayland it goes through XWayland.

Then in Go.dot itself: a show with an output on a projector, and fill, picture, HAP movie, mask cues;
zones and a warp (Warp...); Identify; *Show > Hide the projectors while unlocked*. **Everything should
look as it did before.**

**If the new renderer misbehaves:** start Go.dot with `WFG_VIDEO_RENDERER=gl` in its environment and the
old OpenGL renderer draws instead. Say what differed. Removing the old renderer (R.4) waits for the
author's verdict.

## 3. Sending, taking in, inserts (Video tab, Show settings)

- **An output's Display cell** offers *Sent over Syphon* (Mac), *Sent over NDI*, then *Sent as...* and
  *Frames a second...*. Check the picture in a receiver:
  - Mac: Syphon Simple Client (github.com/Syphon/Simple, releases);
  - NDI: NDI Studio Monitor (NDI Tools), on Mac or another machine.
- **+ input:** kind Syphon (Mac) or NDI; the Sender cell lists what other programs offer now
  (`/godot/videoInput/available`). Syphon names read as *application - server*, e.g.
  *Simple Server - Simple Server*. Then *+ video > Capture of <input>* on a canvas.
- **+ insert:** kind, *Sent as*, *Comes back from*. Switch it in on a video cue (inspector, *insert*). The
  cue shows **black** until something comes back, then what came back, placed, faded and blended as the
  cue says. Of two cues through one insert, the later holds it and the other is black. A real round trip
  needs a program that takes a picture in and sends one out (TouchDesigner, Resolume, Isadora, MadMapper);
  After Effects only sends.
- **Linux:** NDI only - there is no Spout or Syphon. NDI on Linux needs `libndi.so.6` from the NDI SDK for
  Linux, in `/usr/lib`, `/usr/local/lib` or where `NDI_RUNTIME_DIR_V6` says.

## Known limits, written down (namespace draft §44.6)

- **Spout and Syphon share only on one graphics card** (YM). The renderer draws on the card driving the first
  projector's display, otherwise the fastest one.
- **Live input is not on the audio clock** (YD). The newest frame is shown as it comes.
- **A capture and an insert's return are drawn opaque** (YO). Many programs send BGRX, with junk in the
  fourth byte.
- **NDI sending reads the picture back synchronously** (YN). A one-frame-late ring is left for later.
- **Not built:** `wfg validate`'s warning where two cues may meet on one insert; a readout naming the NDI
  runtime found, or the graphics card in use.
- **Syphon's own `SyphonMetalServer` is not used.** It loads shaders from Syphon.framework's bundle, which
  a static build hasn't got. `render/GoDotSyphonServer.m` publishes by a plain blit on Syphon's base class
  instead. If a Syphon receiver shows nothing, look there first.

## What to send back

- the output of the two `video gpu:*` commands, on each machine;
- the bench's numbers and what the screen did;
- for each of: projectors, Syphon out / in / insert (Mac), NDI out / in (both), whether it worked and
  what was wrong;
- any crash. The renderer is a child process (`wfg video-render`); its fault shows in Go.dot as
  *videoOutput/renderer* `failed` and a sentence in the Video tab's summary.

## Where the code is

`src/wfg/engine/video/render/`:
- `Gpu*`: the device - `_metal.mm`, `_gl.cpp` (EGL), `_d3d11.cpp`;
- `Painter`: all drawing, ported from the old renderer;
- `Projector*`: windows, surfaces and pacing per system;
- `Sender*` and `Receiver*`: Spout, Syphon, NDI;
- `Ndi`: finding the runtime the user installed;
- `shaders/video.glsl`: the shaders, translated by `scripts/generate-shaders.py` into the committed
  `video.glsl.h`.

The render thread and the projector windows live in `VideoRenderChild.cpp` (`RenderLoop`,
`ProjectorWindow`).
