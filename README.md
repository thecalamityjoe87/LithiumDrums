# Lithium Drums

Lithium Drums is a sampled acoustic drum kit plugin. It plays real multi-mic drum recordings, with velocity zones, round-robins, and close, overhead and room mics, through a built-in mixer. It runs as a VST3, CLAP, AU (macOS) or standalone app.

It's early: this is Milestone 6 (Milestone 5, grooves, comes next). It plays real recorded kits from nine free libraries, converted with the importer described below: fourteen kits, plus sixteen kit presets that reuse their recordings with other pieces, tuning or mics. For rock, start with CrocellKit or Naked Drums. Large kits stream from disk, so DRSKit takes about 360 MB of RAM instead of 3 GB. The Edit tab shapes each drum (velocity response, tuning and pitch bend, tone, its volume envelope and how much each mic hears it) and runs its own channel of effects: EQ, transient shaper, compressor and saturation. The Mixer tab has a channel per drum, plus the overhead and room mics, a reverb and the master. The kit format, presets, content packs and both importers are described in [docs/kit-format.md](docs/kit-format.md), streaming in [docs/streaming.md](docs/streaming.md), shaping and mixing in [docs/mixer.md](docs/mixer.md), and MIDI maps and e-kits in [docs/midi.md](docs/midi.md).

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

The kits aren't in this repository. Download the libraries, unzip each into a folder of its own outside the repository, and import it. The DrumGizmo kits are big (2-5 GB each); so are SM Drums (2.4 GB) and Naked Drums (2.5 GB).

| Library | Licence | Download |
|---|---|---|
| DRSKit 2.1 (DrumGizmo) | CC BY 4.0 | https://drumgizmo.org/kits/DRSKit/DRSKit2_1.zip |
| MuldjordKit 3 (DrumGizmo) | CC BY 4.0 | https://drumgizmo.org/kits/MuldjordKit/MuldjordKit3.zip |
| AVL Drumkits (SFZ), imported as the Basalt Kits | CC BY-SA 3.0 | the five zips at https://www.bandshed.net/sounds/AVLDrumkits_SFZ/, plus the licence PDF from https://www.bandshed.net/avldrumkits/ |
| Salamander Drumkit | CC BY-SA 3.0 | http://download.linuxaudio.org/musical-instrument-libraries/sfz/salamander_drumkit_v1.tar.7z |
| Virtuosity Drums 0.925 | CC0 | https://versilian-studios.com/Distro/Virtuosity_Drums_v0.925.zip |
| CrocellKit 1.1 (DrumGizmo), a metal and rock kit | CC BY 4.0 | https://drumgizmo.org/kits/CrocellKit/CrocellKit1_1.zip |
| Naked Drums (Wilkinson Audio, SFZ edition), metal and hard rock | CC BY 4.0 | `git clone https://github.com/sfzinstruments/WilkinsonAudio.NakedDrums` |
| Big Rusty Drums 1.100 (Karoryfer) | CC0 | https://github.com/sfzinstruments/karoryfer.big-rusty-drums/releases/download/v1.100/Big_Rusty_Drums_1100.zip |
| SM Drums 1.2 (Sforzando edition) | free to use in your productions; no licence text | `SMDrums_Sforzando_1.2.zip` from https://smmdrums.wordpress.com/for-sfz-sforzando/ |

Unzip all five AVL zips (and the licence PDF) into one folder. Then point the importer at each library's folder:

```
build/release/LithiumImport_artefacts/Release/LithiumImport ~/Downloads/DRSKit
build/release/LithiumImport_artefacts/Release/LithiumImport ~/Downloads/AVLDrumkits
```

and so on for the others. Each library becomes a content pack in your kits folder: `~/.local/share/Lithium Drums/Kits` on Linux, `~/Library/Application Support/Lithium Drums/Kits` on macOS, `%LOCALAPPDATA%\Lithium Drums\Kits` on Windows. You can change it in the plugin's settings (the gear in the top right); the importer uses the same setting, or `--kits-folder`. On the same drive as the downloads, the audio is hard-linked rather than copied, so the kits take no extra space. Don't delete the downloads in that case: they hold the shared audio.

Import CrocellKit from its `CrocellKit` folder inside the zip; the others from the folder you unpacked or cloned. The kits' authors ask to be credited when you publish music made with them; the About dialog (the "i" button) shows the credits of every installed pack. SM Drums has no licence beyond its author's "use it in your productions", so don't pass that converted kit on. The Basalt Kits (from the AVL Drumkits) and the Salamander kits are share-alike: music you make with them is yours to license, but the converted kits themselves may only be passed on under the same licence, which is why each library is kept in a pack of its own.

## Running

Run the standalone app from `build/release/LithiumDrums_artefacts/Release/Standalone/`. It starts with DRSKit. Pick another kit from the menu in the middle of the header, or press Browse to see every kit and preset, search them and read their credits.

To use the plugin in a DAW, point the DAW at the build folder, or copy (or symlink) `VST3/Lithium Drums.vst3` into `~/.vst3` and `CLAP/Lithium Drums.clap` into `~/.clap`. Configuring with `-DLITHIUM_COPY_PLUGINS=ON` makes the build copy them for you.

Every kit plays from the same MIDI map, Lithium Standard: General MIDI plus a fixed note for every other kind of hit, so switching kits never changes what your notes play. Cymbals are choked by notes 85-92, also the same on every kit. The MIDI panel also offers each library's own map (Kit default), General MIDI and the e-kit maps. Muting a mic for a couple of seconds unloads it from memory.

To play from an electronic kit, open the MIDI panel (the round MIDI button in the header, which flashes as notes arrive) and pick Roland V-Drums or Alesis. With those maps the hi-hat follows the pedal continuously (CC4), and snare and tom hits follow positional sensing where the kit has off-centre recordings. The panel's keyboard shows what every note plays and plays it when clicked. Click Learn next to any articulation and hit a pad (or click a key) to remap it, then Save as to keep the map for other projects, or Set as default to start every new instance with it.

For mixing in the DAW, the plugin has sixteen stereo aux outs besides the main out. Enable them in the host, then pick an output for each channel (or the reverb) at the bottom of its strip in the Mixer tab; several can share one, such as all the toms on Aux 4.

Kits over 512 MB stream from disk. The first time one streams, its audio is decoded into a stream cache (`~/.cache/Lithium Drums/Stream Cache` on Linux, `~/Library/Caches/...` on macOS, `%LOCALAPPDATA%\Lithium Drums\Stream Cache` on Windows), which takes a few seconds and about as much disk space as the kit takes in RAM. The header says "Caching" while that happens. After that, the kit loads in a fraction of a second. Settings has the streaming mode, the preload size and a button to clear the cache, and the DISK readout in the header shows how fast the kit is streaming.

## Tests

```
build/release/LithiumDrumsTests                    # engine, loader, streaming, presets, both importers, processor and UI hit-testing
build/release/LithiumHostSmokeTest_artefacts/Release/LithiumHostSmokeTest   # loads the VST3 like a host
build/release/LithiumDrumsTests --measure-kit "<kit folder>"   # plays a real kit and reports its memory use
build/release/LithiumDrumsTests --measure-kit "<kit folder>" --stream --stress 30 --cold   # streams it, under load, from a cold cache
build/release/LithiumDrumsTests --measure-kit "<kit folder>" --stress 30 --full-mix       # with every effect and all the shaping switched on
build/release/LithiumDrumsTests --measure-kit "<kit folder>" --map roland-vdrums           # through the Roland map, sweeping the hi-hat pedal and snare position
build/release/LithiumDrumsTests --midi-audit                                               # every installed kit against every MIDI map, and every articulation played once
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

The fonts (Barlow, Barlow Condensed, IBM Plex Mono) are under the SIL Open Font License; see `assets/fonts/`. JUCE is dual-licensed under the AGPLv3 and a commercial licence. The kits are their authors', under the licences in the table above.
