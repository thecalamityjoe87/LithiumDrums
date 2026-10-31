# Lithium Drums

Lithium Drums is a sampled acoustic drum kit plugin. It plays real multi-mic drum recordings, with velocity zones, round-robins, and close, overhead and room mics, through a built-in mixer. It runs as a VST3, CLAP, AU (macOS) or standalone app.

It's early: this is Milestone 1. It plays the open-licensed DrumGizmo kits, converted with the importer described below. The kit format and the import are described in [docs/kit-format.md](docs/kit-format.md).

## Building

You need CMake 3.25+, Ninja and a C++20 compiler. JUCE and the CLAP extensions are downloaded by CMake on the first configure.

On Linux, install JUCE's dependencies first (Debian/Ubuntu package names):

```
sudo apt install build-essential cmake ninja-build libasound2-dev libjack-jackd2-dev \
    libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev \
    libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libxi-dev libgl-dev
```

Then:

```
scripts/build.sh release      # or: debug
```

The script just runs `cmake --preset <name>` and `cmake --build --preset <name>`. Output lands in `build/<preset>/`. On Windows, run the same presets from a Visual Studio developer prompt. On macOS, install Ninja with Homebrew.

## Getting the kits

The kits aren't in this repository; they're several gigabytes each. Download them from DrumGizmo, both under the Creative Commons Attribution 4.0 licence:

- DRSKit 2.1: https://drumgizmo.org/kits/DRSKit/DRSKit2_1.zip (2.8 GB, from https://drumgizmo.org/wiki/doku.php?id=kits:drskit)
- MuldjordKit 3: https://drumgizmo.org/kits/MuldjordKit/MuldjordKit3.zip (2.5 GB, from https://drumgizmo.org/wiki/doku.php?id=kits:muldjordkit)

Unzip them anywhere outside this repository and import them:

```
build/release/LithiumImport_artefacts/Release/LithiumImport ~/Downloads/DRSKit
build/release/LithiumImport_artefacts/Release/LithiumImport ~/Downloads/MuldjordKit3
```

DRSKit becomes five kits (its basic, full, minimal, brushes and no-brushes variants) and MuldjordKit one. They go into your kits folder: `~/.local/share/Lithium Drums/Kits` on Linux, `~/Library/Application Support/Lithium Drums/Kits` on macOS, `%LOCALAPPDATA%\Lithium Drums\Kits` on Windows. You can change it in the plugin's settings (the gear in the top right); the importer uses the same setting, or `--kits-folder`. On the same drive as the downloads, the audio is hard-linked rather than copied, so the imported kits take no extra space. Don't delete the downloads in that case: they hold the shared audio.

The kits' authors ask to be credited when you publish music made with them. The plugin's About dialog (the "i" button) shows the credits and licence of every installed kit.

## Running

Run the standalone app from `build/release/LithiumDrums_artefacts/Release/Standalone/`. It starts with DRSKit.

To use the plugin in a DAW, point the DAW at the build folder, or copy (or symlink) `VST3/Lithium Drums.vst3` into `~/.vst3` and `CLAP/Lithium Drums.clap` into `~/.clap`. Configuring with `-DLITHIUM_COPY_PLUGINS=ON` makes the build copy them for you.

Each kit plays from its own MIDI map (DrumGizmo's, for the imported kits), and cymbals are choked by notes from 85 up. Choose kits from the menu in the middle of the header. Muting a mic for a couple of seconds unloads it from memory.

## Tests

```
build/release/LithiumDrumsTests                    # engine, loader, importer, processor and UI hit-testing
build/release/LithiumHostSmokeTest_artefacts/Release/LithiumHostSmokeTest   # loads the VST3 like a host
build/release/LithiumDrumsTests --measure-kit "<kit folder>"   # plays a real kit and reports its memory use
```

The tests use a small synthesized kit that the build generates, so they don't need the real kits.

## Re-rendering the kit images

`assets/blender-kit.py` built the renders in `assets/kit-renders/`. To render the kit in another finish (the `finish` argument is the shell colour in linear RGB), run it with Blender, then crop the layers into place:

```
blender -b -P assets/blender-kit.py -- mode=layers scale=0.75 samples=128 finish=0.05,0.1,0.3
python3 assets/crop-layers.py
```

Scale 0.75 gives the 1.5× renders the UI expects. The build never runs Blender. Some distro Blender packages are built without OpenImageDenoise, so the renders come out grainy; use `samples=512` there instead.

The studio room behind the kit doesn't need Blender. `tools/roomgen/RoomGenerator.cpp` paints it through the same camera, so if you move the camera in the Blender script, move it there too. To repaint it (`--preview` also composites the kit over it, for checking):

```
build/release/LithiumRoomGen_artefacts/Release/LithiumRoomGen assets/kit-renders --preview room-preview.jpg
```

## Licences

The fonts (Barlow, Barlow Condensed, IBM Plex Mono) are under the SIL Open Font License; see `assets/fonts/`. JUCE is dual-licensed under the AGPLv3 and a commercial licence. The DrumGizmo kits are CC BY 4.0, by their authors.
