# Untitled Studio — Road to FL Studio Quality

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
- [x] Time signature editing UI — "4/4" button next to the tempo: 2/4 … 12/4 (quarter-note bars only; x/8 would make Tracktion beats eighth notes, see 14f)
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
- [x] Sample looping — via our own multi-sampler: add the WAV as an "Instrument" channel (Loop on/off + start/end in its channel window). The old "Sampler" channel (Tracktion's) still has no loop.
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
- [x] 7 bands + live spectrum behind the curve (≈ Fruity Parametric EQ 2) — Untitled Parametric EQ: bell/shelf/cut/notch/band-pass per band, 12 dB/oct cuts only
- [ ] Graphic EQ
- [x] Compressor (≈ Fruity Compressor)
- [x] Multiband compressor / maximizer (≈ Maximus) — Untitled Maximizer: 3 bands (LR4 crossovers) + look-ahead limiter; no per-band solo yet
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
- [x] Tempo-synced + ping-pong delay (≈ Fruity Delay 3) — Untitled Delay: 1/64…4 bars incl. dotted/triplet or ms, filters in the feedback

**Color / creative**
- [x] Distortion / saturation / bitcrusher (≈ Fruity Blood Overdrive, Fast Dist)
- [x] Tape / tube / vinyl warmth
- [x] Chorus, ensemble, phaser, tremolo, vibrato
- [x] Flanger (Untitled Flanger; Untitled Chorus / Phaser added too, all with tempo-sync LFO)
- [x] Filters (low/high-pass)
- [x] Filter with LFO + envelope (≈ Fruity Love Philter) — Untitled Filter: one SVF (12/24 dB) with synced LFO + envelope follower; not Love Philter's multi-filter drawable envelopes
- [x] Volume/pitch/time gating effect, draw-your-own envelope (≈ Gross Beat) — Untitled Gate & Time FX: 16-point drawn volume shape + stutter / half speed / tape stop; no pitch, no per-step time envelope
- [x] Trance gate / stutter (same plugin, step mode)
- [x] Enhancer + Air (≈ Soundgoodizer-ish)
- [ ] Vocoder (≈ Vocodex)
- [ ] Pitch correction / autotune (≈ Pitcher, NewTone)
- [ ] Formant shifter

**Utility**
- [x] Spectrum analyzer + oscilloscope
- [x] Loudness meter (LUFS) — Untitled Loudness Meter: BS.1770 momentary / short-term / gated integrated + 4x true-peak estimate; no LRA
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
- [x] Audio input recording (mic / interface) to playlist — needs a test with a real mic
- [x] Input monitoring, latency compensation for recording (Tracktion)
- [x] Punch in/out (loop region)
- [ ] Loop recording takes + comping (takes stack on lanes; untested)
- [x] MIDI recording with count-in (pattern mode overdubs; song mode makes a new pattern)

**Done when:** record vocals over a beat and they line up perfectly.

## Phase 13 — Export
- [x] Render to WAV / MP3 / FLAC / OGG (MP3 uses LAME: `brew install lame`)
- [x] Sample rate + bit depth options, dither, normalize
- [x] Export stems (each mixer insert separately)
- [ ] Include send/bus effects in stems
- [x] Export MIDI
- [x] Faster-than-realtime offline render

**Done when:** exported WAV sounds identical to playback.

## Phase 14 — Built-in instruments, sound library & pro features

> ⚠️ **Sound licensing:** never copy FL's (or any DAW's) sounds. Use samples you record,
> buy with redistribution rights, or free libraries whose license allows bundling
> (CC0 / CC-BY). Keep a `SOUNDS_LICENSES.md` listing source + license for every pack.

### 14a — Sample-based instrument engine (do this first, it unlocks most sounds)
- [x] SFZ instrument player — own parser + engine (`Source/MultiSampler.h`), not sfizz. Supports <control>/<global>/<master>/<group>/<region>, default_path, #define, key ranges + note names, velocity layers, round robin (seq_*), tune/transpose/volume/pan, offset/end, loop modes, amp ADSR, group/off_by. No filters/LFOs/curves/CC modulation/keyswitches yet.
- [x] SoundFont (.sf2) player — TinySoundFont (MIT, `libs/TinySoundFont`) parses the file, presets become multi-sampler regions; preset selector in the channel window. SF2 modulators/filter/vibrato ignored.
- [x] Multi-sample + velocity layers + round robins (32 voices, voice stealing, sustain pedal, pitch bend ±2)
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
- [x] Simple 3-oscillator synth (≈ 3xOsc) — Tracktion's 4-OSC engine as the "4-OSC Synth" channel + 13 factory presets (Presets button in its channel window). Generic knob grid, no custom UI yet.
- [ ] Wavetable synth: 2–3 osc, unison, mod matrix, LFOs, filters (≈ Vital/Serum-lite) — not started
- [x] FM synth (≈ Sytrus) — own "FM Synth" (`Source/FMSynth.h`): 4 operators (ratio/fine/level/ADSR), feedback on op 4, 8 algorithms with a diagram, 16 voices + stealing, velocity, pitch bend ±2, sustain pedal, resonant low-pass, 13 presets. No LFOs/mod matrix, no oversampling.
- [x] 303-style bass synth (≈ Transistor Bass) — only as the "Acid Bass" 4-OSC preset (saw, 24 dB resonant filter env, legato slide). No accent/step sequencer.
- [x] 808 / sub-bass instrument with glide + distortion — "808 Sub" 4-OSC preset (sine, legato glide, distortion) + generated 808 samples. No pitch-drop envelope.
- [x] Supersaw / trance lead synth — "Supersaw Lead" 4-OSC preset (2×8 unison saws, reverb/delay)
- [x] Pad synth (≈ GMS) — "Soft Pad" / "String Pad" (4-OSC) and "Glass Pad" (FM) presets, not a dedicated engine
- [ ] Granular synth — not started

### 14d — Drums
- [x] Drum pad instrument, 16 pads, velocity, choke groups (≈ FPC) — "Drum pad" channel, notes 36..51, default kit, closed hat chokes open hat, click pad to play, drop a sample on a pad to replace it
- [ ] Drum synth (synthesized kicks/snares/hats)
- [x] Slicer: chop loops to pads/notes (≈ Fruity Slicer / Slicex) — energy-flux onset detection (8 equal slices fallback), slices on keys from C5, writes a pattern that replays the loop. No slice editing UI yet.

### 14e — Sound library (content, not code)
- [ ] Drum kits: trap, hip-hop, house, techno, lo-fi, rock, acoustic — only one generated electronic kit (Packs/Drums, 24 sounds); no genre kits or recorded acoustic drums
- [x] One-shots: 808s, kicks, snares, claps, hats, percs, FX, risers, impacts — all synthesised on first launch (`DrumKit::ensureLibrary`): 3 extra kicks, 2 snares, 2 claps, shaker, 3 toms, crash, ride, perc, cowbell, 2 more 808s; FX: riser, downlifter, impact, noise sweep
- [ ] Loops: drum, melody, bass, vocal (tagged with BPM + key) — 3 generated drum loops with BPM in the filename (120/124/140); no melody/bass/vocal loops, no key tags
- [ ] Synth presets: 50+ per synth (bass, lead, pad, pluck, keys, FX) — 13 per synth so far (4-OSC + FM), also in the channel rack "+" > Synth presets menu
- [ ] Effect presets + mixer chain presets
- [ ] Demo projects showing off features
- [ ] Browser: tags, search, favorites, auto BPM/key detection, preview in tempo

### 14f — Pro features
- [x] Audio editor (≈ Edison): our "Audio editor" window — waveform, drag-select, Ctrl+wheel zoom, play selection, cut/copy/paste/delete/trim, silence, fade in/out, normalize (-0.3 dB), reverse, ±3 dB, undo/redo (20 buffer snapshots). Opened from playlist audio clip title-bar right-click, sampler/instrument "Edit sample...", browser right-click. Apply writes ~/Music/Untitled Studio/Edits/"<name> (edit N).wav" and repoints the channel (one undo step); originals never touched. Not done: noise removal
- [x] Time-stretch + pitch-shift audio clips — per clip "Stretch to tempo", "Pitch..." (±24 st), "Set original BPM..." (clip title-bar right-click). Tracktion auto-tempo + SoundTouch, stretched in real time (no proxy files). Files named "... 120 BPM" stretch automatically when dropped (FL loop behaviour)
- [x] Preset browser + search — browser "Presets" root (factory + user presets per synth/effect), search box filters sounds (recursive), presets, plugins; double-click loads on the selected channel's instrument or selected insert's matching effect, else a hint. Not done: tags
- [x] Tempo + time-signature changes over the song — tempo markers in the playlist ruler (right-click: add / edit / remove), stored in the model (STUDIO/TEMPOS) and synced to the tempo sequence as jumps; clips keep their beat positions. Time signature is project-wide (no mid-song signature changes); toolbar tempo shows the start tempo
- [ ] Performance / live mode (clip launching)
- [x] Project templates (trap, house, lo-fi…) — FILE > New from template: Empty, Basic beat (default), Trap 140, House 124, Lo-fi 85 + swing; "Save as template..." → ~/Music/Untitled Studio/Templates
- [ ] Arpeggiator + chord generator (MIDI effects)
  - [x] Arpeggiator: per channel (ARP row in channel windows, channel rack right-click > Arpeggiator): off/up/down/up-down/random, 1/32…1/4, 1–4 octaves, gate. Applied when notes are generated, so it renders/exports. Live typing-keyboard arp not done
  - [ ] Chord generator

**Done when:** make a full song using ONLY built-in instruments, sounds & effects — no 3rd-party plugins.

## Phase 15 — Polish, UX & stability
- [x] Custom look & feel: dark theme, consistent colors/fonts (`LookAndFeel`) — base `ColourScheme` so every stock widget/dialog (alerts, combo boxes, popup menus, text editors, toggles, lists, scrollbars, callouts) uses Theme colours + Theme fonts; checked every window via `--snapshot`
- [x] Hi-DPI / Retina scaling, resizable everything — `--snapshot-scale 2` renders crisp (all vector drawing, no bitmaps); main window min 960x600, panels min 200x120, detached windows resizable
- [x] Keyboard shortcuts everywhere (match FL defaults) — F5/F6/F7/F9, F8 sound/plugin picker (type to search, Return adds a channel), F10 settings, F12 close all, Alt+F8 browser, Tab next window, numpad +/- or Ctrl+Up/Down next/previous pattern, Ctrl+N/O/S/Shift+S/R/Z/Y, Space, L; OPTIONS > Keyboard shortcuts lists them
- [x] Detachable / dockable windows — double-click title = maximize/restore, drag snaps to workspace edges + other windows (8 px), right-click title > Detach into a native window (closing it docks back); open windows + bounds remembered across launches
- [x] Hint bar (hover → explain control, like FL) — existed since Phase 1; now also shows non-blocking warnings (missing samples/plugins after loading)
- [x] CPU meter, voice count, RAM display — RAM = resident memory (mach task_info; Windows code path untested); voices = sounding voices of our Instrument/drum pad/FM Synth plugins (atomic per plugin); 4-OSC and the stock sampler don't expose a count yet
- [x] Profile CPU (Instruments on macOS), fix hotspots — `sample` on a Release `--stress` run. Top hotspot in our code: FM Synth `std::sin` (4 ops x voices x samples) → interpolated sine table, FM CPU -46% (5078 → 2735 samples). Model→engine regen rewrote every channel's notes on any change → one pass over notes + only rewrite channels whose notes changed (100-channel regen 6.7 → 4.0 ms; one-note edit 9.1 → 0.4 ms and no graph rebuild for the other 99 channels). UI: playlist/piano-roll playhead repaints only its columns (was full grid at 30 Hz), EQ view repaints only when a band moves (was 20 Hz always), analyzer skips FFT when hidden. Rest of the profile is Tracktion/JUCE DSP (reverb, 4-OSC, ButterComp2)
- [x] Stress test: 100 tracks, 50 plugins, no dropouts at 256 buffer — `--stress` (25 sampler, 25 4-OSC, 25 FM, 25 instrument channels, 50 effects on 20 inserts incl. EQ/compressor/reverb/delay/Airwindows, 1200 notes, 16 bars): live graph at 256 samples / 44.1 kHz averages 7.5% of the block budget, worst block 13–26%, 0 blocks over budget; offline render 9.4x realtime. Measured on this Mac with 12 audio threads; a 4-core machine will be heavier
- [x] Fuzz save/load, test corrupted project files — in `--selftest`: empty / truncated / garbage / foreign XML / no STUDIO / missing file are rejected; old project shape and a minimal hand-written project load; huge/NaN/negative values are clamped on load; missing samples + plugins are listed (hint bar: "N samples could not be found"); 24 seeded random byte/attribute/element mutations load or are rejected without crashing (400 mutations checked once by hand). Also fixed: a fresh project came up "unsaved" (Tracktion's async track/clip sorting landed in the undo history)
- [ ] Windows build (ASIO support) — prepared, not built or tested yet: CMake guards mac-only flags, static MSVC runtime, `UNTITLEDSTUDIO_ASIO_SDK_DIR` enables ASIO (WASAPI + DirectSound always on), see BUILDING.md
- [ ] Own virtual ASIO driver for Windows (like FL Studio ASIO / ASIO4ALL): low-latency ASIO on any sound card, wraps WASAPI, lets other apps share the device
  - [ ] Build as a separate COM DLL + installer registration; test with other DAWs too
  - [ ] Check ASIO SDK license terms (Steinberg) before shipping
  - [ ] macOS doesn't need it (CoreAudio is already low-latency)

## Phase 15b — FL Studio project import (.flp)
> Read-only import. .flp is undocumented; we wrote our own reader (Source/FlpReader.h), using the community
> project PyFLP (GPLv3) only as a format reference (event IDs, struct layouts). No PyFLP code is copied.
> Never copy FL code or bundle FL plugins. Saving back to .flp is out of scope.
- [ ] Collect real .flp test files (simple beat, full song w/ audio + automation, one with 3rd-party VSTs) — so far only PyFLP's test files (`scripts/fetch-flp-testdata.sh` -> testdata/flp, gitignored): one FL 20.8.4 project + presets. Still need FL 21 / 2024 songs with audio, linked automation and installed VSTs
- [x] FLP reader: FLhd header + FLdt event stream (byte/word/dword/varint-length events) — UTF-16 text from FL 11.5, rejects all corrupted fixtures, 50 seeded mutations per self-test run
- [x] Import tempo, time signature, project name — title/version/tempo shown in the import report (we have no project-title field); x/8 etc. become x/4 (reported); swing imported
- [x] Import channels: names, volume, pan, sampler sample paths (warn if missing) — also colours, mute (disabled), root note, insert routing; paths: absolute, next to the .flp, %FLStudioUserData% / %FLStudioFactoryData% (installed FL), then by file name under the .flp's folder. Channel volume is relative to FL's 78% default (= our 0 dB)
- [x] Import patterns + piano roll notes — names, colours, notes (fine pitch rounds to the nearest key; note pan / release / mod X/Y not imported, we have no per-note fields). Pattern length (FL's fixed length) not imported: ours is auto. Layer channels: their notes are copied to the layered channels
- [x] Import playlist: pattern clips, audio clips, lanes — current arrangement only (others reported), FL track -> lane (tracks above 64 go to the last lane), clip start/length/offset; muted flag (0x2000) and audio clip offsets are unverified guesses; track names not imported (lanes have no names)
- [x] Import mixer: routing, levels, mute/solo — names, volume, pan, disabled -> muted, insert->insert routes (a single route not to master = output to that insert, else sends; zero-level routes ignored). Solo, stereo separation, insert EQ, send levels not imported. We keep 20 inserts: FL inserts above 20 fold into insert 20 (reported) — raising Studio::numInserts would widen every project's mixer for rare cases
- [ ] Third-party VST plugins: reload plugin + state if installed — implemented, NOT verified (no plugins installed on the dev machine): matched by name (+vendor); VST3 state from FL's wrapper records restored via the VST3 component/controller state; VST2 chunks wrapped as a "VstW" bank for the plugin's VST3 build; AU targets and non-chunk VST2 params not restored. Not installed -> 4-OSC placeholder + listed as missing
- [x] FL native plugins (Sytrus, FLEX, Fruity effects...): replace with closest built-in, list in report — effects via FlStockMap; settings decoded for Balance, Soft Clipper, Stereo Enhancer, Fast Dist, Blood Overdrive, Soundgoodizer (layouts known); Parametric EQ 2 / Limiter / Delay 3 / Reeverb 2 / Compressor get default settings (layouts unknown). Generators: BooBass/Transistor Bass/Plucked! -> 4-OSC presets, Sytrus/GMS/Toxic/DX10/FL Keys -> FM Synth preset by channel-name heuristic, Harmor/Harmless/PoiZone/... -> 4-OSC preset, 3x Osc -> default 4-OSC (state not decoded), Fruit Kick -> sampler kick, FPC -> drum pad (pads from sample paths found in its data), Slicex/Fruity Slicer -> our slicer, FLEX/DirectWave -> instrument if a sample path is found in the data, else placeholder; MIDI Out etc. skipped
- [ ] Automation clips (map FL parameters we understand) — points (beats, 0..1) parsed and verified; the clip -> parameter link (remote-controller event) is decoded by guess (channel vol/pan, insert vol/pan, plugin param by index) and unverified: the only test clip is unlinked, so it's reported, not imported. Tempo automation not imported
- [x] Import report window ("Imported X channels, replaced Y plugins...") — themed panel + hint bar; time markers listed there (we have no markers)
- [ ] Support FL 20 / 21 / 2024 format differences — only FL 20.8.4 verified with a real file; FL 21's 60-byte playlist items handled from the format notes, untested
- [x] FILE > Import FL Studio project... + drag .flp onto the window
- [x] Untitled stock effects suite (substitutes for FL stock effects on import) — FlStockMap::untitledTypeFor() maps FL plugin names to our types; parameter mapping for the documented ones (see above)

## Phase 16 — Release
- [x] App icon, splash, about screen — own icon (`resources/icon/icon.svg` → `icon_1024.png` → `.icns`, an orange "u" + step row, nothing from FL); splash while the engine starts (normal launches only); OPTIONS > About shows version, build date, GPLv3 notice, source link and the third-party licenses
- [ ] macOS code signing + notarization (Apple Developer account, $99/yr) — `scripts/package-macos.sh` makes an ad-hoc signed DMG today (13 MB, mounts and launches); the Developer ID + notarytool path is written (`resources/entitlements.plist`) but untested until there's an account
- [ ] Windows installer + code signing — plan in RELEASING.md (Inno Setup / WiX, OV/EV certificate)
- [x] Crash reporting — local only: crash handler writes `~/Library/untitledstudio/CrashReports/*.txt` (version, OS, backtrace, last 200 log lines); next launch offers View / Copy / Reveal / Dismiss in the same dialog as autosave recovery. No upload yet (hook marked in `Release.h`)
- [x] Auto-update — update *check* only: JSON feed (`update.json`), once a day in the background, "Version X is available" + Download button; off until a build sets `UNTITLEDSTUDIO_UPDATE_URL` and the feed is hosted (raw GitHub URL works once pushed). No self-installing updates
- [ ] Website, manual, tutorial videos
- [ ] Beta testers → fix → v1.0
- [x] Check licenses: JUCE (paid above revenue cap, or GPL), Tracktion Engine (same), VST3 SDK (now MIT), any DSP libs — app is GPLv3 (`LICENSE`); JUCE 9 is actually AGPLv3 (compatible via GPLv3 §13); every bundled library in `THIRD_PARTY_LICENSES.md` (SoundTouch is LGPL, fine for a GPL app), license texts copied into the app bundle. Version 0.1.0

---

## Reality check
FL Studio = 25+ years, full team. Phases 0–8 already = usable beat-making app — a real achievement.
Phases 9–16 = what makes it "FL quality". Expect this to take a long time; finishing each phase matters more than speed.
