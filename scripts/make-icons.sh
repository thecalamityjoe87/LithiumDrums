#!/usr/bin/env bash
# Regenerates assets/icon/ from the logo code (src/ui/Logo.cpp): the app icon
# at every size, the Windows .ico, and the installers' artwork. Run it after
# changing the logo; its output is committed, so builds don't need it.
#
# Usage: scripts/make-icons.sh [release|debug]
# Needs ImageMagick 7 (magick) for the .ico and .bmp files.
set -euo pipefail

preset="${1:-release}"
root="$(cd "$(dirname "$0")/.." && pwd)"
config="$(tr '[:lower:]' '[:upper:]' <<< "${preset:0:1}")${preset:1}"
out="$root/assets/icon"

cmake --build --preset "$preset" --target LithiumIconGen -j 4
"$root/build/$preset/LithiumIconGen_artefacts/$config/LithiumIconGen" "$out"

cd "$out"

# Windows: one .ico with the sizes Explorer and the taskbar use.
magick icon-16.png icon-24.png icon-32.png icon-48.png icon-64.png icon-256.png lithium-drums.ico

# Inno Setup wants 24-bit BMPs: flatten onto what's behind them (the side
# panel is opaque already; the small picture sits on the wizard's white top).
for f in installer-side installer-side-2x; do
    magick "$f.png" -background '#0e1318' -flatten -type TrueColor "BMP3:$f.bmp"
done
for f in installer-small installer-small-2x; do
    magick "$f.png" -background white -flatten -type TrueColor "BMP3:$f.bmp"
done

echo "Updated $out"
