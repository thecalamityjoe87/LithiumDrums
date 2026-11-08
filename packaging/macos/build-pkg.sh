#!/usr/bin/env bash
# Builds the macOS installer, LithiumDrums-<version>-macos.pkg, from a build's
# artefacts. Run by scripts/package.sh on macOS; needs Xcode's command-line
# tools (pkgbuild, productbuild).
#
# Usage: packaging/macos/build-pkg.sh <artefacts dir> <version> <output dir> <notices file>
#
# Unsigned, the package works, but macOS warns that it's from an unidentified
# developer, and plugins inside it may be blocked from loading. To sign and
# notarize (needs an Apple Developer account), set before running:
#   LITHIUM_SIGN_APP="Developer ID Application: Isaac Joseph (TEAMID)"
#   LITHIUM_SIGN_INSTALLER="Developer ID Installer: Isaac Joseph (TEAMID)"
#   LITHIUM_NOTARY_PROFILE=<profile saved with: xcrun notarytool store-credentials>
set -euo pipefail

artefacts="$1"
version="$2"
output_dir="$3"
notices="$4"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
id="io.github.thecalamityjoe87.lithiumdrums"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Sign the bundles first: the signature must be inside the package.
sign_bundle() {
    if [[ -n "${LITHIUM_SIGN_APP:-}" ]]; then
        codesign --force --deep --timestamp --options runtime --sign "$LITHIUM_SIGN_APP" "$1"
    fi
}

# One component package per choice. Each gets its own root folder holding
# just its bundle, installed into its standard location.
component() {
    local name="$1" source="$2" location="$3"
    mkdir -p "$work/roots/$name"
    cp -R "$source" "$work/roots/$name/"
    sign_bundle "$work/roots/$name/$(basename "$source")"

    # Turn off bundle relocation: otherwise Installer "upgrades" a copy it
    # finds anywhere on the disk (an old build, say) instead of installing
    # to the plug-in folder.
    pkgbuild --analyze --root "$work/roots/$name" "$work/$name.plist" > /dev/null
    plutil -replace BundleIsRelocatable -bool NO "$work/$name.plist" 2> /dev/null || true

    pkgbuild --root "$work/roots/$name" --component-plist "$work/$name.plist" \
        --identifier "$id.$name" --version "$version" --install-location "$location" \
        "$work/pkgs/$name.pkg" > /dev/null
}

mkdir -p "$work/pkgs" "$work/resources"
component app  "$artefacts/Standalone/Lithium Drums.app" /Applications
component vst3 "$artefacts/VST3/Lithium Drums.vst3"      /Library/Audio/Plug-Ins/VST3
component clap "$artefacts/CLAP/Lithium Drums.clap"      /Library/Audio/Plug-Ins/CLAP
component au   "$artefacts/AU/Lithium Drums.component"   /Library/Audio/Plug-Ins/Components

# Factory presets: where Settings::getFactoryPresetsFolder looks on macOS.
mkdir -p "$work/roots/presets/Presets"
cp -R "$root/assets/presets/." "$work/roots/presets/Presets/"
cp "$notices" "$work/roots/presets/THIRD-PARTY-NOTICES.txt"
cp "$root/LICENSE" "$work/roots/presets/LICENSE.txt"
pkgbuild --root "$work/roots/presets" --identifier "$id.presets" --version "$version" \
    --install-location "/Library/Application Support/Lithium Drums" "$work/pkgs/presets.pkg" > /dev/null

cp "$here/resources/"*.html "$work/resources/"
cp "$root/LICENSE" "$work/resources/LICENSE.txt"
sed "s/@VERSION@/$version/g" "$here/distribution.xml" > "$work/distribution.xml"

if [[ -f "$root/assets/icon/installer-mac-background.png" ]]; then
    cp "$root/assets/icon/installer-mac-background.png" "$work/resources/background.png"
else
    sed -i '' '/<background/d' "$work/distribution.xml"
fi

mkdir -p "$output_dir"
pkg="$output_dir/LithiumDrums-$version-macos.pkg"
sign_args=()
[[ -n "${LITHIUM_SIGN_INSTALLER:-}" ]] && sign_args=(--sign "$LITHIUM_SIGN_INSTALLER" --timestamp)

productbuild --distribution "$work/distribution.xml" --resources "$work/resources" \
    --package-path "$work/pkgs" ${sign_args[@]+"${sign_args[@]}"} "$pkg"

if [[ -n "${LITHIUM_NOTARY_PROFILE:-}" ]]; then
    xcrun notarytool submit "$pkg" --keychain-profile "$LITHIUM_NOTARY_PROFILE" --wait
    xcrun stapler staple "$pkg"
fi

echo "Created $pkg"
