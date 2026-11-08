#!/usr/bin/env bash
# Usage: scripts/package.sh [debug|release] [output-directory]
#
# Builds, then creates in the output folder:
#   Linux:  LithiumDrums-<version>-linux-<arch>.tar.gz, holding the graphical
#           installer ("Install Lithium Drums") with the files it installs
#           in payload/ beside it.
#   macOS:  LithiumDrums-<version>-macos.pkg (the Installer app; see
#           packaging/macos/build-pkg.sh) and a .zip of the same files.
# On Windows, use scripts/package.ps1.
set -euo pipefail

preset="${1:-release}"
output_dir="${2:-dist}"
root_dir="$(cd "$(dirname "$0")/.." && pwd)"
version="$(sed -n 's/^project(LithiumDrums VERSION \([^ )]*\).*/\1/p' "$root_dir/CMakeLists.txt")"
config="$(tr '[:lower:]' '[:upper:]' <<< "${preset:0:1}")${preset:1}"

if [[ -z "$version" ]]; then
    echo "Could not determine the project version from CMakeLists.txt" >&2
    exit 1
fi

case "$(uname -s)" in
    Linux)  platform="linux-$(uname -m)" ;;
    Darwin) platform="macos-$(uname -m)" ;;
    *)
        echo "Unsupported host. Use scripts/package.ps1 on Windows." >&2
        exit 1
        ;;
esac

"$root_dir/scripts/build.sh" "$preset"

artefacts="$root_dir/build/$preset/LithiumDrums_artefacts/$config"
if [[ ! -d "$artefacts" ]]; then
    echo "Build artifacts not found: $artefacts" >&2
    exit 1
fi

copy_required() {
    if [[ ! -e "$1" ]]; then
        echo "Expected build output not found: $1" >&2
        exit 1
    fi
    cp -a "$1" "$2"
}

package_name="LithiumDrums-$version-$platform"
mkdir -p "$root_dir/$output_dir"
staging="$root_dir/$output_dir/$package_name"
rm -rf "$staging"

# The files to install. On Linux they go in payload/, beside the installer.
files="$staging"
[[ "$platform" == linux-* ]] && files="$staging/payload"
mkdir -p "$files/plugins" "$files/standalone" "$files/tools" "$files/presets"

copy_required "$artefacts/VST3/Lithium Drums.vst3" "$files/plugins/"
copy_required "$artefacts/CLAP/Lithium Drums.clap" "$files/plugins/"
copy_required "$root_dir/assets/presets/." "$files/presets/"
copy_required "$root_dir/README.md" "$files/"
copy_required "$root_dir/LICENSE" "$files/"
python3 -I "$root_dir/scripts/third-party-notices.py" "$root_dir/build/_deps" "$files/THIRD-PARTY-NOTICES.txt"

importer="$root_dir/build/$preset/LithiumImport_artefacts/$config/LithiumImport"
[[ -e "$importer" ]] && copy_required "$importer" "$files/tools/"

if [[ "$platform" == linux-* ]]; then
    copy_required "$artefacts/Standalone/Lithium Drums" "$files/standalone/"
    mkdir -p "$files/icons"
    for size in 16 24 32 48 64 128 256 512; do
        copy_required "$root_dir/assets/icon/icon-$size.png" "$files/icons/"
    done

    copy_required "$root_dir/build/$preset/LithiumDrumsInstaller_artefacts/$config/Install Lithium Drums" "$staging/"
    cp "$root_dir/LICENSE" "$staging/"
    cat > "$staging/README.txt" <<EOF
Lithium Drums $version, by Isaac Joseph (Calamity Audio)

To install, double-click "Install Lithium Drums".

It installs the standalone app, the VST3 and CLAP plugins into your home
folder (no password needed), and adds Lithium Drums to your app menu. Run it
again to update or remove Lithium Drums.

From a terminal:  ./"Install Lithium Drums" --install   (or --remove, --help)

Lithium Drums is free software under the GNU AGPLv3; see LICENSE.
EOF

    tar -C "$root_dir/$output_dir" -czf "$root_dir/$output_dir/$package_name.tar.gz" "$package_name"
    echo "Created $root_dir/$output_dir/$package_name.tar.gz"
else
    copy_required "$artefacts/Standalone/Lithium Drums.app" "$files/standalone/"
    [[ -e "$artefacts/AU/Lithium Drums.component" ]] && copy_required "$artefacts/AU/Lithium Drums.component" "$files/plugins/"

    "$root_dir/packaging/macos/build-pkg.sh" "$artefacts" "$version" "$root_dir/$output_dir" "$files/THIRD-PARTY-NOTICES.txt"

    ditto -c -k --sequesterRsrc --keepParent "$staging" "$root_dir/$output_dir/$package_name.zip"
    echo "Created $root_dir/$output_dir/$package_name.zip"
fi

rm -rf "$staging"
