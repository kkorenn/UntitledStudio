#!/bin/bash
# Builds Untitled Studio (macOS).
#
#   ./scripts/build.sh                 debug build  -> build/
#   ./scripts/build.sh release         release build -> build-release/
#   ./scripts/build.sh --test          build, then run the self-test
#   ./scripts/build.sh --run           build, then launch the app
#   ./scripts/build.sh release --clean wipe the build folder first
#
# Needs: Xcode command line tools, cmake, ninja (brew install cmake ninja).
set -euo pipefail
cd "$(dirname "$0")/.."

usage() { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

CONFIG=Debug RUN=0 TEST=0 CLEAN=0
for arg in "$@"; do
    case "$arg" in
        debug)      CONFIG=Debug ;;
        release)    CONFIG=Release ;;
        --run)      RUN=1 ;;
        --test)     TEST=1 ;;
        --clean)    CLEAN=1 ;;
        -h|--help)  usage ;;
        *)          echo "Unknown option: $arg"; usage 1 ;;
    esac
done

for tool in cmake ninja; do
    command -v "$tool" >/dev/null || { echo "Missing '$tool': brew install cmake ninja"; exit 1; }
done

# JUCE and Tracktion Engine are git submodules; fetch them on a fresh clone.
if [[ ! -f libs/JUCE/CMakeLists.txt || ! -d libs/tracktion_engine/modules ]]; then
    echo "== Fetching JUCE and Tracktion Engine (submodules)"
    git submodule update --init --depth 1 libs/JUCE libs/tracktion_engine
fi

DIR=build
[[ "$CONFIG" == Release ]] && DIR=build-release
[[ "$CLEAN" == 1 ]] && rm -rf "$DIR"

echo "== Building $CONFIG in $DIR/ (the first build takes a few minutes)"
cmake -S . -B "$DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" >/dev/null
cmake --build "$DIR"

APP="$DIR/UntitledStudio_artefacts/$CONFIG/Untitled Studio.app"
echo "== Built: $APP"

if [[ "$TEST" == 1 ]]; then
    echo "== Running self-test"
    "$APP/Contents/MacOS/Untitled Studio" --selftest
fi

if [[ "$RUN" == 1 ]]; then
    open "$APP"
fi
