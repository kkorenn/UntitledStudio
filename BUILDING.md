# Building untitledstudio

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
./build/UntitledStudio_artefacts/Debug/untitledstudio.app/Contents/MacOS/untitledstudio --selftest
```

For profiling or the stress test, use a separate Release folder:

```
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/UntitledStudio_artefacts/Release/untitledstudio.app/Contents/MacOS/untitledstudio --stress
```

## Windows (prepared, not tested yet)

Needs Visual Studio 2022 with "Desktop development with C++".

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
build\UntitledStudio_artefacts\Release\untitledstudio.exe --selftest
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

## Dev flags

| Flag | What it does |
|------|--------------|
| `--selftest` | Headless checks, exit code 0 = pass |
| `--stress` | 100 channels / 50 effects / 16 bars: prints render speed and per-block load at 256 samples |
| `--snapshot file.png` | Renders the main window to a PNG and quits (`--snapshot-scale 2` for Retina) |
| `--open-mixer`, `--open-pianoroll`, `--demo-song`, `--open-channel`, `--open-drumpad`, `--open-instrument`, `--open-fm`, `--open-synth`, `--open-audio-editor`, `--open-picker`, `--template <name>` | Set up a window before `--snapshot` |
| `--dump-params` | Lists every effect's parameters |
