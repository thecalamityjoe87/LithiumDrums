#!/usr/bin/env bash
# Usage: scripts/package.sh [debug|release] [output-directory]
set -euo pipefail

preset="${1:-release}"
output_dir="${2:-dist}"
root_dir="$(cd "$(dirname "$0")/.." && pwd)"
version="$(sed -n 's/^project(LithiumDrums VERSION \([^ )]*\).*/\1/p' "$root_dir/CMakeLists.txt")"

if [[ -z "$version" ]]; then
    echo "Could not determine the project version from CMakeLists.txt" >&2
    exit 1
fi

case "$(uname -s)" in
    Linux)
        platform="linux-$(uname -m)"
        archive_extension="tar.gz"
        ;;
    Darwin)
        platform="macos-$(uname -m)"
        archive_extension="zip"
        ;;
    *)
        echo "Unsupported host. Use scripts/package.ps1 on Windows." >&2
        exit 1
        ;;
esac

"$root_dir/scripts/build.sh" "$preset"

artefacts="$root_dir/build/$preset/LithiumDrums_artefacts/$(tr '[:lower:]' '[:upper:]' <<< "${preset:0:1}")${preset:1}"
if [[ ! -d "$artefacts" ]]; then
    echo "Build artifacts not found: $artefacts" >&2
    exit 1
fi

package_name="LithiumDrums-$version-$platform"
staging="$root_dir/$output_dir/$package_name"
archive="$root_dir/$output_dir/$package_name.$archive_extension"
rm -rf "$staging" "$archive"
mkdir -p "$staging/plugins" "$staging/standalone" "$staging/tools" \
    "$staging/share/lithium-drums/presets"

copy_required() {
    local source="$1"
    local destination="$2"
    if [[ ! -e "$source" ]]; then
        echo "Expected build output not found: $source" >&2
        exit 1
    fi
    cp -a "$source" "$destination"
}

copy_required "$artefacts/VST3/Lithium Drums.vst3" "$staging/plugins/"
copy_required "$artefacts/CLAP/Lithium Drums.clap" "$staging/plugins/"
copy_required "$artefacts/Standalone/Lithium Drums" "$staging/standalone/"
copy_required "$root_dir/assets/presets/." "$staging/share/lithium-drums/presets/"
copy_required "$root_dir/README.md" "$staging/"

if [[ "$platform" == macos-* && -e "$artefacts/AU/Lithium Drums.component" ]]; then
    copy_required "$artefacts/AU/Lithium Drums.component" "$staging/plugins/"
fi

if [[ -e "$root_dir/build/$preset/LithiumImport_artefacts/$([[ "$preset" == release ]] && echo Release || echo Debug)/LithiumImport" ]]; then
    copy_required "$root_dir/build/$preset/LithiumImport_artefacts/$([[ "$preset" == release ]] && echo Release || echo Debug)/LithiumImport" "$staging/tools/"
fi

mkdir -p "$root_dir/$output_dir"
if [[ "$archive_extension" == tar.gz ]]; then
    tar -C "$root_dir/$output_dir" -czf "$archive" "$package_name"
else
    ditto -c -k --sequesterRsrc --keepParent "$staging" "$archive"
fi
rm -rf "$staging"

echo "Created $archive"
