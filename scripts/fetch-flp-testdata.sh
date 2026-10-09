#!/bin/sh
# Fetches the FL Studio test files used by --selftest (testdata/flp, gitignored).
# They come from the PyFLP project (GPLv3): used as test input only, never committed.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git clone --depth 1 --quiet https://github.com/demberto/PyFLP.git "$tmp/pyflp"
rm -rf "$root/testdata/flp"
mkdir -p "$root/testdata"
cp -R "$tmp/pyflp/tests/assets" "$root/testdata/flp"
echo "FL test files copied to $root/testdata/flp"
