# untitledstudio — Road to FL Studio Quality

Stack: **C++ · JUCE · Tracktion Engine · CMake · Git**
Platform first: **macOS** (add Windows later, Phase 15).

Each phase ends with a **Done when** test. Don't start the next phase until it passes.
Tick boxes as you go: `- [ ]` → `- [x]`.

---

## ⭐ RULE #1 — UI & workflow must feel like FL Studio
**This rule beats every other one.** An FL user should open the app and know how to use it with zero tutorial.

**Copy the workflow, not the brand.** Workflow, layout and shortcuts are fine to match.
Do NOT copy FL's logo, name, "Fruity" names, icons, skins/artwork or exact graphics. Draw your own.
(The "≈ Fruity X" notes in this plan are references only. Give your plugins their own names.)

Every feature, every phase:
- [ ] Before building, open FL Studio and study how that feature looks + behaves. Screenshot it.
- [ ] Prompt the AI with: *"Make this behave exactly like FL Studio's [feature]: [describe clicks, drags, shortcuts]."*
- [ ] After building, do the same task in FL and in your app side by side. Any difference = bug.
- [ ] A phase isn't **done** until its UI test passes, not just its audio test.

### FL feel checklist (applies everywhere)
**Layout**
- [ ] Top toolbar: menus, transport, BPM, pattern selector, CPU/RAM, hint bar
- [ ] Browser docked left
- [ ] Channel Rack, Piano Roll, Mixer, Playlist = floating/dockable windows over the workspace
- [ ] Dark grey theme, colored patterns/channels, compact dense controls

**Window shortcuts**
- [ ] F5 Playlist · F6 Channel Rack · F7 Piano Roll · F9 Mixer · Alt+F8 Browser
- [ ] Space play/stop · L toggle pattern/song mode · Ctrl+S save · Ctrl+Z undo

**Mouse behavior**
- [ ] Left-click draws, right-click deletes (piano roll, playlist, step sequencer)
- [ ] Left-drag on a step button paints steps
- [ ] Knobs: drag up/down, scroll wheel, modifier key for fine adjust, right-click menu (reset, automate, link to controller)
- [ ] Right-click everything gives a context menu
- [ ] Hover any control and the hint bar explains it

**Piano roll tools**
- [ ] P pencil · B brush · D delete · E select · C slice · Z zoom
- [ ] Ctrl+B duplicate selection · Ctrl+Q quantize · Shift+scroll / Ctrl+scroll zoom
- [ ] Last-used note length remembered for the next note

**Patterns workflow**
- [ ] Pattern = notes for many channels; playlist places patterns; clone patterns
- [ ] Drag channel/sample from browser into rack or playlist
- [ ] Any knob → right-click → "Create automation clip"

---

## Vibe-coding rules (read before every session)

- [ ] Commit to git after every working step. Broken? `git restore .` or `git reset --hard`.
- [ ] One small feature per AI prompt. Build + listen after each.
- [ ] Paste the **full** error (compiler, linker, crash stack) to the AI, not a summary.
- [ ] Any code that runs inside `processBlock` / audio callback → tell the AI: *"audio thread: no allocation, no locks, no file I/O, no logging."*
- [ ] Run from Xcode debugger so crashes show the exact line.
- [ ] Ask the AI "explain this" whenever you don't understand. You're learning C++ along the way.
- [ ] Keep this file updated. Give it to the AI at the start of each session for context.
- [ ] Remind the AI every session: **Rule #1 — must look & behave like FL Studio.**

---

## Phase 0 — Tools & setup
- [x] Install Xcode + command line tools (`xcode-select --install`)
- [x] Install Homebrew, then `brew install cmake git`
- [ ] Install an editor (VS Code + C/C++ + CMake Tools extensions, or CLion)
- [x] `git init` this folder, add `.gitignore` (ignore `build/`)
- [x] Add JUCE as git submodule (`libs/JUCE`)
- [ ] Learn 5 git commands: `status`, `add`, `commit`, `log`, `restore`

**Done when:** `cmake -B build && cmake --build build` compiles an empty JUCE app window.

## Phase 1 — First sound
- [x] JUCE `AudioAppComponent` app that plays a 440 Hz sine wave
- [x] Volume slider (via `std::atomic<float>`, no locks)
- [x] Audio device settings dialog (pick output, buffer size, sample rate)
- [x] Play / stop button

**Done when:** sine plays, volume slider is smooth with no clicks, changing buffer size works.

## Phase 2 — Simple synth (learn DSP basics)
- [x] Oscillators: sine, saw, square, triangle
- [x] Play notes with computer keyboard, FL layout: Z-row = lower octave (Z S X D C V G B H N J M), Q-row = upper octave (Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P)
- [x] MIDI keyboard input (`MidiInput`)
- [x] Polyphony: 8–16 voices (`juce::Synthesiser`)
- [x] ADSR envelope
- [x] Low-pass filter with cutoff + resonance
- [x] Anti-click: smooth parameter changes (`SmoothedValue`)

**Done when:** you can play chords on a MIDI keyboard with no clicks, pops or stuck notes.

## Phase 3 — Transport & step sequencer (FL Channel Rack)
> Built directly on Tracktion Engine (switch moved here from Phase 4 to avoid a rewrite).
- [x] Transport: play, stop, BPM, song position display
- [ ] Time signature editing UI (engine supports it; no UI yet)
- [x] Sample-accurate clock (timing from sample count, not timers)
- [x] Channel rack UI: rows = channels, 16 step buttons each
- [x] Load drum samples (kick, snare, hat) per channel
- [x] Per-channel volume, pan, mute, solo
- [x] Swing
- [x] Metronome

**Done when:** 4-channel drum loop at 140 BPM runs 10 minutes with perfect timing (no drift).

## Phase 4 — Move to Tracktion Engine + project model
> The big switch. Tracktion gives you timeline, clips, tracks, plugin hosting, undo, save/load.
- [x] Add Tracktion Engine as submodule (`libs/tracktion_engine`)
- [x] Rebuild Phases 1–3 features on top of `tracktion::Edit` (Phase 2 synth replaced by Tracktion's 4-OSC)
- [x] Project save / load (`.usp` file)
- [x] **Undo / redo from day one** (`UndoManager`) — very hard to add later
- [x] Autosave every N minutes + crash recovery
- [x] Recent projects list

**Done when:** make a beat, save, quit, reopen → identical. Undo works on every edit.

## Phase 5 — Sampler & browser
- [x] Sampler channel: load WAV/MP3/FLAC/AIFF, pitch by note, root note, start/end
- [ ] Sample looping (Tracktion's sampler has no loop mode; needs custom sampler)
- [x] Drag-and-drop audio files onto the channel rack
- [x] File browser panel (left side, like FL) with folders + preview on click
- [x] Waveform display (`AudioThumbnail`)
- [x] Background disk streaming for long samples (no disk reads on audio thread)

**Done when:** browse a sample folder, preview, drag into rack, play it from step sequencer.

## Phase 6 — Piano roll
- [x] Grid: pitch (vertical) × time (horizontal), zoom + scroll
- [x] Draw, delete, move, resize notes (mouse)
- [x] Select box, multi-select, copy/paste, duplicate (Ctrl+B like FL)
- [x] Snap to grid (1/4, 1/8, 1/16, triplets, none)
- [x] Velocity lane
- [ ] Pan lane
- [x] Quantize, humanize
- [x] Ghost notes (show notes from other channels)
- [x] Scale highlighting, chord stamp tool
- [ ] Slide/glide notes
- [ ] Record MIDI keyboard into piano roll

**Done when:** you can write a full melody + chords comfortably without touching the step sequencer.

## Phase 7 — Patterns & Playlist (FL arrangement)
- [x] Patterns: each pattern holds notes for many channels (FL-style)
- [x] Pattern selector, create / clone / rename
- [ ] Pattern colour picker (auto colours for now)
- [x] Playlist: tracks × time, place pattern clips
- [x] Audio clips on playlist
- [x] Move, resize, split, slice, mute clips
- [x] Loop region
- [ ] Markers
- [x] Song mode vs pattern mode toggle
- [x] Zoom, scroll, follow playhead

**Done when:** arrange a full 3-minute song (intro, verse, drop, outro) from patterns.

## Phase 8 — Mixer
- [x] Mixer window: master + 20 insert tracks
- [ ] Grow to 100+ inserts (create on demand)
- [x] Route channels → mixer tracks
- [x] Per track: volume fader, pan, mute, solo, peak meter
- [x] 10 effect slots per track
- [x] Sends / routing between tracks, sidechain
- [x] Latency compensation (PDC) — handled by Tracktion
- [ ] Stereo separation, phase invert

**Done when:** route drums to bus with sidechain compression from kick, no phase/latency issues.

## Phase 9 — Built-in effects (stock plugins)
Every effect has a themed window, bypass, factory presets (main ones) and user presets. EQ + Analyzer have custom visuals.
186 Airwindows effects (MIT) are bundled under "More".

**Core mixing**
- [x] Parametric EQ with draggable curve (4 bands)
- [ ] 8 bands + live spectrum behind the curve (≈ Fruity Parametric EQ 2)
- [ ] Graphic EQ
- [x] Compressor (≈ Fruity Compressor)
- [ ] Multiband compressor / maximizer (≈ Maximus) — needs custom DSP
- [x] Bus Glue + Vari-Mu bus compressors
- [x] Limiter for master (≈ Fruity Limiter)
- [x] Gate
- [x] De-esser
- [x] Transient shaper
- [x] Stereo tools: width, mid/side (≈ Stereo Shaper)

**Space**
- [x] Algorithmic reverb (≈ Fruity Reeverb 2) + Small Rooms
- [x] Convolution reverb with generated room (≈ Fruity Convolver)
- [ ] Load your own impulse response files
- [x] Delay, Tape Delay, Echo
- [ ] Tempo-synced + ping-pong delay (≈ Fruity Delay 3)

**Color / creative**
- [x] Distortion / saturation / bitcrusher (≈ Fruity Blood Overdrive, Fast Dist)
- [x] Tape / tube / vinyl warmth
- [x] Chorus, ensemble, phaser, tremolo, vibrato
- [ ] Flanger
- [x] Filters (low/high-pass)
- [ ] Filter with LFO + envelope (≈ Fruity Love Philter)
- [ ] Volume/pitch/time gating effect, draw-your-own envelope (≈ Gross Beat)
- [ ] Trance gate / stutter
- [x] Enhancer + Air (≈ Soundgoodizer-ish)
- [ ] Vocoder (≈ Vocodex)
- [ ] Pitch correction / autotune (≈ Pitcher, NewTone)
- [ ] Formant shifter

**Utility**
- [x] Spectrum analyzer + oscilloscope
- [ ] Loudness meter (LUFS)
- [ ] Tuner
- [ ] Sidechain tool (volume ducking shape)

**Done when:** mix a song using only stock effects and it sounds clean.

## Phase 10 — Third-party plugin hosting
- [x] Scan VST3 + AU plugins (scan in separate process so bad plugins can't crash the DAW)
- [x] Plugin browser / favorites
- [x] Load instruments in channel rack, effects in mixer
- [x] Open plugin GUI windows
- [x] Save/restore plugin state in project (Tracktion)
- [ ] CLAP support (JUCE 9 has no CLAP hosting yet; needs clap-host library)
- [x] Scan-time crash protection / blacklist (out-of-process scan)
- [ ] Run plugins sandboxed so a crash during playback can't kill the app

**Done when:** Serum/Vital + FabFilter-type plugins load, save, reload correctly.

## Phase 11 — Automation
- [x] Automation clips on playlist (any knob → right-click → "create automation clip")
- [x] Curve editing: points, curve tension (mouse wheel)
- [ ] Preset curve shapes (saw, sine, steps...)
- [x] Automate plugin params too
- [x] Record knob movements live (record button, SONG mode)
- [x] MIDI controller mapping ("link to controller")

**Done when:** filter sweep automation into the drop works on both stock and 3rd-party plugins.

## Phase 12 — Recording
- [ ] Audio input recording (mic / interface) to playlist
- [ ] Input monitoring, latency compensation for recording
- [ ] Punch in/out, loop recording, takes
- [ ] MIDI recording with count-in

**Done when:** record vocals over a beat and they line up perfectly.

## Phase 13 — Export
- [ ] Render to WAV / MP3 / FLAC / OGG
- [ ] Sample rate + bit depth options, dither
- [ ] Export stems (each mixer track separately)
- [ ] Export MIDI
- [ ] Faster-than-realtime offline render

**Done when:** exported WAV sounds identical to playback.

## Phase 14 — Built-in instruments, sound library & pro features

> ⚠️ **Sound licensing:** never copy FL's (or any DAW's) sounds. Use samples you record,
> buy with redistribution rights, or free libraries whose license allows bundling
> (CC0 / CC-BY). Keep a `SOUNDS_LICENSES.md` listing source + license for every pack.

### 14a — Sample-based instrument engine (do this first, it unlocks most sounds)
- [ ] SFZ instrument player (use **sfizz** library, BSD license) → loads 1000s of free instruments
- [ ] SoundFont (.sf2) player (≈ Fruity Soundfont Player; TinySoundFont or FluidSynth)
- [ ] Multi-sample + velocity layers + round robins
- [ ] Rompler browser with categories (≈ FLEX): pick preset → plays instantly

### 14b — Bundled acoustic instruments (via 14a)
- [ ] **Grand piano** (Salamander Grand Piano, CC-BY)
- [ ] Upright / electric piano (Rhodes, Wurli)
- [ ] Organ
- [ ] Strings: violin, cello, ensemble (VSCO 2 Community Edition, CC0)
- [ ] Brass + woodwinds (VSCO 2 CE)
- [ ] Acoustic + electric guitar
- [ ] Bass guitar (≈ BooBass)
- [ ] Choir / vocal pads
- [ ] Mallets: marimba, vibes, bells, celesta
- [ ] Orchestral percussion, timpani
- [ ] World instruments (koto, sitar, kalimba…)

### 14c — Built-in synths
- [ ] Simple 3-oscillator synth (≈ 3xOsc) — upgrade of Phase 2
- [ ] Wavetable synth: 2–3 osc, unison, mod matrix, LFOs, filters (≈ Vital/Serum-lite)
- [ ] FM synth (≈ Sytrus)
- [ ] 303-style bass synth (≈ Transistor Bass)
- [ ] 808 / sub-bass instrument with glide + distortion
- [ ] Supersaw / trance lead synth
- [ ] Pad synth (≈ GMS)
- [ ] Granular synth

### 14d — Drums
- [ ] Drum pad instrument, 16 pads, velocity, choke groups (≈ FPC)
- [ ] Drum synth (synthesized kicks/snares/hats)
- [ ] Slicer: chop loops to pads/notes (≈ Fruity Slicer / Slicex)

### 14e — Sound library (content, not code)
- [ ] Drum kits: trap, hip-hop, house, techno, lo-fi, rock, acoustic
- [ ] One-shots: 808s, kicks, snares, claps, hats, percs, FX, risers, impacts
- [ ] Loops: drum, melody, bass, vocal (tagged with BPM + key)
- [ ] Synth presets: 50+ per synth (bass, lead, pad, pluck, keys, FX)
- [ ] Effect presets + mixer chain presets
- [ ] Demo projects showing off features
- [ ] Browser: tags, search, favorites, auto BPM/key detection, preview in tempo

### 14f — Pro features
- [ ] Audio editor (≈ Edison): cut, fade, normalize, reverse, noise removal
- [ ] Time-stretch + pitch-shift audio clips (Rubber Band or Signalsmith Stretch)
- [ ] Preset browser + tags + search (shared by all plugins)
- [ ] Tempo + time-signature changes over the song
- [ ] Performance / live mode (clip launching)
- [ ] Project templates (trap, house, lo-fi…)
- [ ] Arpeggiator + chord generator (MIDI effects)

**Done when:** make a full song using ONLY built-in instruments, sounds & effects — no 3rd-party plugins.

## Phase 15 — Polish, UX & stability
- [ ] Custom look & feel: dark theme, consistent colors/fonts (`LookAndFeel`)
- [ ] Hi-DPI / Retina scaling, resizable everything
- [ ] Keyboard shortcuts everywhere (match FL defaults)
- [ ] Detachable / dockable windows
- [ ] Hint bar (hover → explain control, like FL)
- [ ] CPU meter, voice count, RAM display
- [ ] Profile CPU (Instruments on macOS), fix hotspots
- [ ] Stress test: 100 tracks, 50 plugins, no dropouts at 256 buffer
- [ ] Fuzz save/load, test corrupted project files
- [ ] Windows build (ASIO support)
- [ ] Own virtual ASIO driver for Windows (like FL Studio ASIO / ASIO4ALL): low-latency ASIO on any sound card, wraps WASAPI, lets other apps share the device
  - [ ] Build as a separate COM DLL + installer registration; test with other DAWs too
  - [ ] Check ASIO SDK license terms (Steinberg) before shipping
  - [ ] macOS doesn't need it (CoreAudio is already low-latency)

## Phase 16 — Release
- [ ] App icon, splash, about screen
- [ ] macOS code signing + notarization (Apple Developer account, $99/yr)
- [ ] Windows installer + code signing
- [ ] Crash reporting
- [ ] Auto-update
- [ ] Website, manual, tutorial videos
- [ ] Beta testers → fix → v1.0
- [ ] Check licenses: JUCE (paid above revenue cap, or GPL), Tracktion Engine (same), VST3 SDK (now MIT), any DSP libs

---

## Reality check
FL Studio = 25+ years, full team. Phases 0–8 already = usable beat-making app — a real achievement.
Phases 9–16 = what makes it "FL quality". Expect this to take a long time; finishing each phase matters more than speed.
