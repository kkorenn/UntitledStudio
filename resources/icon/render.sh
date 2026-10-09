#!/bin/sh
# Renders icon.svg to icon_1024.png (needs librsvg: brew install librsvg).
cd "$(dirname "$0")" && rsvg-convert -w 1024 -h 1024 icon.svg -o icon_1024.png
