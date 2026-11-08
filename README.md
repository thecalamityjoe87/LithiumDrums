# Lithium Drums

Lithium Drums is a sampled acoustic drum kit plugin. It plays real multi-mic drum recordings, with velocity zones, round-robins, and close, overhead and room mics, through a built-in mixer. It runs as a VST3, CLAP, AU (macOS) or standalone app.

It's early: this is Milestone 8 (version 0.9.1). It plays real recorded kits from eleven free libraries, which it downloads and converts for you: sixteen kits, plus eighteen kit presets that reuse their recordings with other pieces, tuning or mics. For rock, start with CrocellKit or Naked Drums. Large kits stream from disk, so DRSKit takes about 360 MB of RAM instead of 3 GB. The Edit tab shapes each drum (velocity response, tuning and pitch bend, tone and its volume envelope), sets how much each mic hears it on a picture of the drum and its mics, and runs its transient shaper, saturation and compressor. The Mixer tab has a channel per drum, plus the overhead and room mics, three effect returns and the master; selecting a channel shows its EQ, pan, stereo width and sends, and each channel sends to the returns (Room, a convolution reverb; Plate, an algorithmic one; and a tempo-synced Delay), whose effects are on the FX tab along with the master bus chain (EQ, glue and crush compressors, tape, transient shaper, limiter). Any drum can be swapped for the same kind of drum from another installed kit, and the whole sound saved as a preset. The Grooves tab plays 1,150 recorded drum performances through the kit, in time with your host, and drags them into a track as MIDI. The kit format, kit presets, piece swapping, content packs and the importers are described in [docs/kit-format.md](docs/kit-format.md), streaming in [docs/streaming.md](docs/streaming.md), shaping and mixing in [docs/mixer.md](docs/mixer.md), the effects in [docs/fx.md](docs/fx.md), MIDI maps and e-kits in [docs/midi.md](docs/midi.md), and grooves in [docs/grooves.md](docs/grooves.md).

## Building

You need CMake 3.25+, Ninja and a C++20 compiler. JUCE, the CLAP extensions and the LZMA SDK (for unpacking 7z downloads) are downloaded by CMake on the first configure.

On Linux, install JUCE's dependencies first (Debian/Ubuntu package names):

```
sudo apt install build-essential cmake ninja-build libasound2-dev libjack-jackd2-dev \
    libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev \
    libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libxi-dev libgl-dev libcurl4-openssl-dev
```

Then:

```
scripts/build.sh release      # or: debug
```

The script just runs `cmake --preset <name>` and `cmake --build --preset <name>`. Output lands in `build/<preset>/`. The build runs one compile per 2.5 GB of RAM, since JUCE's sources take a lot of memory to compile and a compile per core can run a machine out of it. On Windows, run the same presets from a Visual Studio developer prompt. On macOS, install Ninja with Homebrew.

To create a distributable archive, run `scripts/package.sh release` on Linux or
macOS. It creates a platform-specific archive in `dist/` containing the VST3
and CLAP plugins, the standalone app, the importer utility, factory presets,
and this README. macOS packages also contain the AU component. On Windows,
run `powershell -ExecutionPolicy Bypass -File scripts/package.ps1`; use
`scripts/build.ps1` for a direct Windows build. The scripts accept `debug` or
`release` as the first argument (or `-Preset` in PowerShell) and an optional
output directory.

## Getting the kits

The kits and grooves aren't in this repository; they're free libraries by other people, and Lithium Drums downloads them for you. Open the kit browser (Browse, in the header): every library that isn't installed yet is there, greyed out, with a Download button. It fetches the library from its authors' site, converts it and deletes the download again, so there's nothing to unzip or keep around. The Grooves tab does the same for the grooves. From a terminal, `LithiumImport --list-packs` lists them and `LithiumImport --install <id>` (or `all`) installs one. How it works is in [docs/packs.md](docs/packs.md).

| Library | Download | Licence |
|---|---|---|
| DRSKit 2.1 (DrumGizmo) | 2.8 GB | CC BY 4.0 |
| MuldjordKit 3 (DrumGizmo) | 2.5 GB | CC BY 4.0 |
| CrocellKit 1.1 (DrumGizmo), metal and rock | 5.6 GB | CC BY 4.0 |
| Naked Drums (Wilkinson Audio), metal and hard rock | 2.5 GB | CC BY 4.0 |
| Virtuosity Drums 0.925 | 1.2 GB | CC0 |
| Big Rusty Drums 1.100 (Karoryfer) | 620 MB | CC0 |
| SM Drums 1.2 | 2.4 GB | free to use in your productions; no licence text |
| The Open Source Drum Kit (Real Music Media), with a gong | 530 MB | public domain |
| AVL Drumkits, as the Basalt Kits | 90 MB | CC BY-SA 3.0 |
| Salamander Drumkit | 360 MB | CC BY-SA 3.0 |
| Tchimera Drum Kit (Tchackpoum's Ludwig Black Cortex recordings), made for e-kits | 800 MB | CC BY-SA 4.0 (music made with it: CC0) |
| Groove MIDI Dataset (Google Magenta) | 3 MB | CC BY 4.0 |

Kits go into your kits folder: `~/.local/share/Lithium Drums/Kits` on Linux, `~/Library/Application Support/Lithium Drums/Kits` on macOS, `%LOCALAPPDATA%\Lithium Drums\Kits` on Windows. You can change it in the plugin's settings (the gear in the top right). Grooves go into a Grooves folder next to it.

If you've downloaded a library yourself, `LithiumImport <folder>` converts it from the unpacked folder (for CrocellKit, the `CrocellKit` folder inside the zip; for The Open Source Drum Kit, the `the-open-source-drumkit-master` folder). On the same drive, the audio is hard-linked rather than copied, so the kits take no extra space, and deleting the download afterwards is fine.

The kits' authors ask to be credited when you publish music made with them; the About dialog (the "i" button) shows the credits of every installed pack. SM Drums has no licence beyond its author's "use it in your productions", so don't pass that converted kit on. The Basalt Kits (from the AVL Drumkits), the Salamander kits and the Tchimera Drum Kit are share-alike: music you make with them is yours to license, but the converted kits themselves may only be passed on under the same licence, which is why each library is kept in a pack of its own.

## Running

Run the standalone app from `build/release/LithiumDrums_artefacts/Release/Standalone/`. It starts with DRSKit (with nothing installed yet, press Browse and download a kit; it loads when it's in). The box in the middle of the header is the preset menu: your presets, and under "Start from a kit" every installed kit. Browse shows every kit and kit preset, to search them and read their credits.

To build a kit of your own, switch the Kit tab to Slots (top right). Every kit has the same 24 slots; click a piece's model to swap in that kind of piece from any kit you have, or click an empty slot's Add to fill it (the kit reloads in the background, then plays the new piece once). Choose Empty to take a piece out. "Ambience follows piece" brings the swapped piece's own overhead and room sound along. Save the result with Save as... in the preset menu; the arrows either side step through your presets, and an asterisk shows when the sound has changed since it was saved. Presets go in a `Presets` folder next to the kits folder, one `.lithiumpreset` file each.

To use the plugin in a DAW, point the DAW at the build folder, or copy (or symlink) `VST3/Lithium Drums.vst3` into `~/.vst3` and `CLAP/Lithium Drums.clap` into `~/.clap`. Configuring with `-DLITHIUM_COPY_PLUGINS=ON` makes the build copy them for you.

Every kit plays from the same MIDI map, Lithium Standard: General MIDI plus a fixed note for every other kind of hit, so switching kits never changes what your notes play. Cymbals are choked by notes 85-92, also the same on every kit. The MIDI panel also offers each library's own map (Kit default), General MIDI and the e-kit maps. Muting a mic for a couple of seconds unloads it from memory.

To play from an electronic kit, open the MIDI panel (the round MIDI button in the header, which flashes as notes arrive) and pick Roland V-Drums or Alesis. With those maps the hi-hat follows the pedal continuously (CC4), with chicks and foot splashes from the pedal, and snare, tom and ride hits follow positional sensing. The panel's E-Drums view sets up the pedal (calibration, curve, where the openness steps change) and the pads, and has a monitor of the MIDI coming in. Those settings belong to the MIDI map, not to presets, so loading a preset never changes your hardware setup. The panel's keyboard shows what every note plays and plays it when clicked. Click Learn next to any articulation and hit a pad (or click a key) to remap it, then Save as to keep the map for other projects, or Set as default to start every new instance with it.

On the Grooves tab, pick a style and a groove, then click any of its phrase or fill tiles to hear it on the loaded kit. With the tempo on Host it follows your project's tempo, and while the project plays it stays in step with it. Drag a tile (or the Drag MIDI handle) onto a track to get its MIDI, written in the notes of whichever MIDI map you use.

For mixing in the DAW, the plugin has sixteen stereo aux outs besides the main out. Enable them in the host, then pick an output for each channel (or return) at the bottom of its strip in the Mixer tab; several can share one, such as all the toms on Aux 4. The plugin has no latency unless you switch on the limiter's lookahead on the FX tab, which adds 2 ms and tells the host.

Kits over 512 MB stream from disk. The first time one streams, its audio is decoded into a stream cache (`~/.cache/Lithium Drums/Stream Cache` on Linux, `~/Library/Caches/...` on macOS, `%LOCALAPPDATA%\Lithium Drums\Stream Cache` on Windows), which takes a few seconds and about as much disk space as the kit takes in RAM. The header says "Caching" while that happens. After that, the kit loads in a fraction of a second. Settings has the streaming mode, the preload size and a button to clear the cache, and the DISK readout in the header shows how fast the kit is streaming.

## Tests

```
build/release/LithiumDrumsTests                    # engine, loader, streaming, presets, the importers, processor and UI hit-testing
build/release/LithiumHostSmokeTest_artefacts/Release/LithiumHostSmokeTest   # loads the VST3 like a host
build/release/LithiumDrumsTests --measure-kit "<kit folder>"   # plays a real kit and reports its memory use
build/release/LithiumDrumsTests --measure-kit "<kit folder>" --stream --stress 30 --cold   # streams it, under load, from a cold cache
build/release/LithiumDrumsTests --measure-kit "<kit folder>" --stress 30 --full-mix       # with every effect and all the shaping switched on
build/release/LithiumDrumsTests --measure-kit "<kit folder>" --map roland-vdrums           # through the Roland map, sweeping the hi-hat pedal and snare position
build/release/LithiumDrumsTests --midi-audit                                               # every installed kit against every MIDI map, and every articulation played once
build/release/LithiumDrumsTests --swap-audit                                               # every installed kit's pieces swapped into every other kit, then a few played
build/release/LithiumDrumsTests --fx-cost                                                  # what each effect costs, as a share of a core
```

The tests use a small synthesized kit that the build generates, so they don't need the real kits.

## Re-rendering the kit images

`assets/blender-kit.py` built the renders in `assets/kit-renders/`: the stage's layers, and each piece on its own for the Kit tab's slot cards. To render the kit in another finish (the `finish` argument is the shell colour in linear RGB), run it with Blender in both modes, then crop the results into place:

```
blender -b -P assets/blender-kit.py -- mode=layers scale=0.75 samples=128 finish=0.05,0.1,0.3
blender -b -P assets/blender-kit.py -- mode=pieces scale=2.25 samples=128 finish=0.05,0.1,0.3
python3 assets/crop-layers.py
```

Each kit's drums also come in its own finish (`assets/kit-finishes.json`), so a kit with swapped-in pieces shows each piece in the finish of the kit it came from. To render one, add `kit=<kit id> only=kick,tom1,tom2,floor,snare out=<folder>` to both Blender commands, then run `python3 assets/crop-layers.py --finish <kit id> <folder>`. Kits not listed keep the default red. To redo just one drum, add `border=auto`: Blender renders only the part of the frame that drum covers, which is much faster, and the crop is the same.

Scale 0.75 gives the 1.5× renders the UI expects. The build never runs Blender. Some distro Blender packages are built without OpenImageDenoise, so the renders come out grainy; use `samples=512` there instead.

The studio room behind the kit doesn't need Blender. `tools/roomgen/RoomGenerator.cpp` paints it through the same camera, so if you move the camera in the Blender script, move it there too. To repaint it (`--preview` also composites the kit over it, for checking):

```
build/release/LithiumRoomGen_artefacts/Release/LithiumRoomGen assets/kit-renders --preview room-preview.jpg
```

## Licences

The grooves are the Groove MIDI Dataset's drummers', under CC BY 4.0; the About dialog credits them. The Room reverb's Seminar Room IR is from the HOMULA-RIR dataset (Politecnico di Milano, CC BY 4.0); its other IRs are synthesized by `tools/irgen` and are CC0. The fonts (Barlow, Barlow Condensed, IBM Plex Mono) are under the SIL Open Font License; see `assets/fonts/`. JUCE is dual-licensed under the AGPLv3 and a commercial licence. The kits are their authors', under the licences in the table above.
