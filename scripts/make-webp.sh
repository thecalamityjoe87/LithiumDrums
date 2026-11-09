#!/usr/bin/env bash
# Rebuilds assets/renders/ (what the plugin embeds) from the PNG originals in
# assets/render-sources/, keeping the same folders: every .png becomes a
# lossy WebP at quality 90 (its transparency kept lossless), and the .json
# files beside them are copied. Files in renders/ with no original are
# deleted, so a removed render doesn't linger in the build.
#
# Run it after re-rendering anything (Blender, crop-layers.py, roomgen). Its
# output is committed, so builds don't need ImageMagick.
#
# Why WebP 90: the renders are 46 MB as PNG and 6 MB as WebP 90, and every
# plugin format carries its own copy; side by side the difference is only
# visible zoomed in, on the glitter finishes.
#
# Usage: scripts/make-webp.sh            (needs ImageMagick 7: magick)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/assets/render-sources"
dst="$root/assets/renders"

mkdir -p "$dst"

# Convert (in parallel) only what's new or changed since the last run.
export src dst
find "$src" -type f -name '*.png' -print0 | xargs -0 -P "$(nproc)" -I{} bash -c '
    in="$1"
    out="$dst/${in#"$src"/}"
    out="${out%.png}.webp"
    if [[ ! -f "$out" || "$in" -nt "$out" ]]; then
        mkdir -p "$(dirname "$out")"
        magick "$in" -quality 90 -define webp:alpha-quality=100 -define webp:method=6 "$out"
        echo "  ${out#"$dst"/}"
    fi' _ {}

find "$src" -type f -name '*.json' | while read -r in; do
    out="$dst/${in#"$src"/}"
    mkdir -p "$(dirname "$out")"
    cp -p "$in" "$out"
done

# Drop renders whose original is gone.
find "$dst" -type f | while read -r out; do
    rel="${out#"$dst"/}"
    case "$rel" in
        *.webp) [[ -f "$src/${rel%.webp}.png" ]] || { rm "$out"; echo "  removed $rel"; } ;;
        *.json) [[ -f "$src/$rel" ]] || { rm "$out"; echo "  removed $rel"; } ;;
    esac
done
find "$dst" -type d -empty -delete

echo "assets/renders: $(find "$dst" -type f | wc -l) files, $(du -sh "$dst" | cut -f1)"
