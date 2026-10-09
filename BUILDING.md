# Building Untitled Studio

JUCE and Tracktion Engine are git submodules in `libs/`:

```
git submodule update --init --recursive
```

Needs CMake 3.22+ and a C++20 compiler.

## macOS

Needs Xcode (or the Command Line Tools) and Ninja (`brew install cmake ninja`).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
"./build/UntitledStudio_artefacts/Debug/Untitled Studio.app/Contents/MacOS/Untitled Studio" --selftest
```

For profiling or the stress test, use a separate Release folder:

```
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
"./build-release/UntitledStudio_artefacts/Release/Untitled Studio.app/Contents/MacOS/Untitled Studio" --stress
```

Release packaging (DMG in `dist/`): `scripts/package-macos.sh`, see [RELEASING.md](RELEASING.md).

## Windows (prepared, not tested yet)

Needs Visual Studio 2022 with "Desktop development with C++".

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
build\UntitledStudio_artefacts\Release\"Untitled Studio.exe" --selftest
```

- WASAPI and DirectSound work out of the box.
- ASIO: download the ASIO SDK from Steinberg (https://www.steinberg.net/developers/), unzip it,
  and point CMake at it. Steinberg's license doesn't let us ship the SDK in this repo.

  ```
  cmake -S . -B build -G "Visual Studio 17 2022" -DUNTITLEDSTUDIO_ASIO_SDK_DIR=C:/SDKs/asiosdk
  ```

- The app links the MSVC runtime statically, so no VC++ redistributable is needed.

## Optional dependencies

- **MP3 export** uses the `lame` command-line encoder if it's installed
  (macOS: `brew install lame`). WAV, FLAC and OGG need nothing extra.
- **SoundFont test**: the self-test also loads a SoundFont when `fluid-synth` is installed through Homebrew.
- **FL Studio import tests**: `scripts/fetch-flp-testdata.sh` copies PyFLP's test files into `testdata/flp`
  (gitignored, never commit them). Without them the self-test skips the real-file checks.

## Dev flags

| Flag | What it does |
|------|--------------|
| `--selftest` | Headless checks, exit code 0 = pass |
| `--stress` | 100 channels / 50 effects / 16 bars: prints render speed and per-block load at 256 samples |
| `--snapshot file.png` | Renders the main window to a PNG and quits (`--snapshot-scale 2` for Retina) |
| `--import-flp <file.flp>` | Imports an FL Studio project before `--snapshot` (put it last) |
| `--open-mixer`, `--open-pianoroll`, `--demo-song`, `--open-channel`, `--open-drumpad`, `--open-instrument`, `--open-fm`, `--open-synth`, `--open-audio-editor`, `--open-picker`, `--template <name>` | Set up a window before `--snapshot` |
| `--dump-params` | Lists every effect's parameters |
| `--open-about` | With `--snapshot`: renders the About window instead of the main window |
| `--crash-test` | Crashes on purpose; the report lands in `~/Library/Untitled Studio/CrashReports/` and is offered at the next launch |
| `--update-check <url>` | Fetches an update feed now and prints the result (works with `http://`, `https://`, `file://`) |

Any `--` flag also skips the splash screen, the crash/recovery prompt and the update check.

## Update check

Off unless the build has a feed URL:

```
cmake -S . -B build-release -DUNTITLEDSTUDIO_UPDATE_URL=https://raw.githubusercontent.com/kkorenn/UntitledStudio/master/update.json
```

The feed is a small JSON file (`update.json` in the repo root is the template). It's checked at most
once a day, in the background; OPTIONS > "Check for updates at startup" turns it off.

## App icon

`resources/icon/icon.svg` is the source; `resources/icon/render.sh` turns it into `icon_1024.png`
(needs `brew install librsvg`). CMake makes the `.icns` from the PNG.
