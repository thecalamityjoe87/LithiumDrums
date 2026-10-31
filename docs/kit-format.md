# Lithium Drums kit format

A kit is a folder containing a `kit.json` manifest, the audio files it refers
to (WAV or FLAC), and its licence. Kits live in the user's kits folder, one
folder per kit, never in this repository:

| OS      | Default kits folder                                      |
|---------|----------------------------------------------------------|
| Linux   | `$XDG_DATA_HOME/Lithium Drums/Kits` (usually `~/.local/share/...`) |
| macOS   | `~/Library/Application Support/Lithium Drums/Kits`       |
| Windows | `%LOCALAPPDATA%\Lithium Drums\Kits`                      |

The folder can be changed in the plugin's settings. The `LITHIUM_KITS_FOLDER`
environment variable overrides it (the tests use that).

This document describes format version 2. Version 1 (the fixed nine-piece
format of the first prototype) is no longer read.

## Overview

```json
{
  "formatVersion": 2,
  "type": "kit",
  "name": "DRSKit",
  "author": "The DrumGizmo team ...",
  "sourceUrl": "https://drumgizmo.org/wiki/doku.php?id=kits:drskit",
  "license": { "name": "CC BY 4.0", "url": "https://creativecommons.org/licenses/by/4.0/", "file": "LICENSE.txt" },
  "credits": "DRSKit, a DrumGizmo kit ...",
  "description": "A handcrafted kit ...",
  "sampleRate": 44100,

  "mics": [
    { "id": "snare_top", "name": "Snare Top", "channels": 1, "role": "close", "volumeDb": -6, "pan": 0.15 },
    { "id": "overheads", "name": "Overheads", "channels": 2, "role": "overheads" }
  ],

  "pieces": [
    { "id": "snare", "name": "Snare", "role": "snare", "articulations": [
      { "id": "center", "name": "Center",
        "channelMap": { "snare_top": 10, "overheads": [7, 6] },
        "zones": [
          { "velocity": [1, 40],   "velocityRangeDb": 6, "roundRobins": [ "audio/Snare/samples/1-Snare.wav", "..." ] },
          { "velocity": [41, 127], "velocityRangeDb": 4, "roundRobins": [ "..." ] }
        ] }
    ] }
  ],

  "midiMap": [ { "note": 38, "piece": "snare", "articulation": "center" } ]
}
```

Unknown keys are ignored, so tools can add their own (the importer records an
`importedFrom` block, for example).

## Metadata and licence

| Key           | Required | Meaning |
|---------------|----------|---------|
| `formatVersion` | yes    | `2` |
| `type`        | no       | `"kit"` (the default). See "Kit presets" below. |
| `name`        | yes      | Shown in the kit menu and the About dialog. |
| `author`, `sourceUrl`, `credits`, `description` | no | Shown in the About dialog. |
| `license`     | no*      | `{ "name", "url", "file" }`. `file` is the licence text inside the kit folder. |
| `sampleRate`  | yes      | Every audio file must be at this rate. The engine resamples to the host's rate. |

\* Optional for the format, but required in practice for any kit made from
someone else's recordings. Creative Commons Attribution licences require the
author, source and licence to be credited; the About dialog shows them for
every installed kit, and the importer always fills them in.

## Mics

`mics` lists the kit's mic channels in mixer order. Each becomes one strip in
the mixer.

| Key        | Meaning |
|------------|---------|
| `id`       | Used by channel maps. Letters, digits, `_`, `-`, `.` |
| `name`     | Shown in the UI and as the host's parameter names. |
| `channels` | `1` (mono) or `2` (stereo). |
| `role`     | `close`, `overheads` or `room`: which per-piece send scales this mic. Default `close`. |
| `volumeDb` | The kit's default fader for this mic. Default 0. |
| `pan`      | The kit's default pan, -1 (left) to 1 (right). For stereo mics it's a balance. Default 0. |
| `invertPolarity` | Flip the mic's polarity (some kits have out-of-phase snare mics). |

The mixer's own volume and pan controls are trims on top of the kit's
defaults, so 0 dB and centre mean "as the kit was mixed". A kit has at most
24 mics.

## Pieces, articulations and zones

`pieces` lists the kit's drums and cymbals (at most 24). Each has:

| Key        | Meaning |
|------------|---------|
| `id`       | Used by the MIDI map. |
| `name`     | Shown on its strip and the stage. |
| `role`     | `kick`, `snare`, `hihat`, `tom` (rack tom), `floor_tom`, `crash`, `ride`, `china` or `other`. |
| `chokeNote` | A MIDI note that chokes this piece. Cymbals without one get the next free note from 85 up. |
| `articulations` | At least one. The first is what clicking the piece plays. |

**Roles and the stage.** The rendered kit stage has nine slots: kick, snare,
hi-hat, two rack toms, floor tom, crash, ride and china. Pieces are assigned to
slots by role, in kit order: the first kick to the kick slot, the first two
`tom`s to the rack tom slots, and so on. A slot with no piece is drawn dimmed;
a piece with no slot (a second floor tom, a second crash) still plays and still
gets a strip.

**Articulations** are the ways of hitting a piece (snare: center, rim, rest;
hi-hat: closed, open, pedal; ride: bow, bell). Each has an `id`, a `name`,
choke settings, an optional `channelMap`, and `zones`.

**Zones** split the velocity range 1-127. They must not overlap; a velocity in
a gap uses the nearest zone. Each zone has:

| Key               | Meaning |
|-------------------|---------|
| `velocity`        | `[low, high]` |
| `roundRobins`     | The zone's hits (1 to 1024), played in shuffled order without immediate repeats. |
| `gainDb`          | Extra gain for the zone. Default 0. |
| `velocityRangeDb` | How much quieter the zone's lowest velocity plays than its highest. Default 6. This keeps the level continuous inside a zone; set it to 0 for no scaling. |

Near the boundary between two zones, both play with an equal-power crossfade
(this can be turned off with the Velocity Crossfade parameter).

## Hits and channel maps

A hit is one recording of one stroke, through every mic at once. All of a
hit's mics play from one shared position, so they stay sample-aligned and
phase-coherent. A hit can be written in four ways:

1. **A file name**: one multichannel file, read through the articulation's
   `channelMap`. This is the compact form, and what imported DrumGizmo kits use.
   ```json
   "channelMap": { "kick_back": 4, "snare_top": 10, "overheads": [7, 6] },
   "roundRobins": [ "audio/Snare/samples/1-Snare.wav", "audio/Snare/samples/2-Snare.wav" ]
   ```
2. **`{ "file", "channelMap" }`**: a multichannel file with its own map.
3. **A list of those**: when a hit's mics are spread over several multichannel files.
4. **`{ "files": { "<mic>": "file.wav" } }`**: one file per mic. A stereo mic's
   file has two channels; a mono file on a stereo mic plays centred.

A channel map gives, for each mic, the file channel (1-based) for a mono mic,
or `[left, right]` for a stereo one. A single number on a stereo mic plays that
channel centred. Mics missing from a hit's map stay silent for that hit (the
kick drum might not reach the floor tom mic, for instance).

Paths are relative to the kit folder and can't leave it.

## Choke groups

An articulation can belong to a choke group (`chokeGroup`) and can choke
groups (`chokes`). When it plays, every sounding voice in a group it chokes
fades out over a few milliseconds (never a hard cut):

```json
{ "id": "closed", "chokes": ["hihat_open"], "...": "..." },
{ "id": "pedal",  "chokes": ["hihat_open"], "...": "..." },
{ "id": "open",   "chokeGroup": "hihat_open", "...": "..." }
```

Cymbals are also choked by their `chokeNote` and by polyphonic aftertouch,
which e-kits send when a cymbal edge is grabbed.

## MIDI map

`midiMap` is the kit's default note map: `{ "note", "piece", "articulation" }`
entries, where `articulation` is optional (the piece's first). A kit without a
map uses Lithium Drums' General MIDI map instead. That map is written by role
("the second rack tom", "the first ride, bell"), so it fits any kit; a request
for a piece the kit doesn't have (a third rack tom) uses the last piece of that
role. E-kit maps (Roland, Alesis) will use the same role-based form.

## Audio and memory

WAV and FLAC files are read with JUCE's readers. The whole kit is held in RAM
for now, stored per mic as 16-bit integers (if every file is 16-bit) or 24-bit
integers (anything deeper, including 32-bit float sources), and converted to
float as it plays. That's 2 or 3 bytes per sample instead of float's 4. When a
mic is muted for a couple of seconds its audio is unloaded, and reloaded when
it's unmuted.

## Scaling to large libraries

- `kit.json` describes structure, not audio, so it stays small: imported
  DrumGizmo kits are 40-100 KB for 200-550 hits, because hits are plain file
  names and the channel map is written once per articulation.
- All sample access goes through one interface (`SampleSource`), shaped for
  streaming: a voice opens a cursor and reads forwards, and a streaming source
  may serve the start of each sample from RAM and the rest from disk.
- Many short files or one long file per hit both work; the loader reads every
  file's header first, lays memory out once, then decodes in parallel.

## Kit presets (reserved)

A later version will add kits of `"type": "preset"`: a kit that refers to
another kit's samples (by kit id) and adds its own choice of pieces, tuning,
mixer settings and processing, without copying any audio. One recorded kit
can then provide several distinct kits. This version rejects other types with
a clear message, so presets made for a newer version fail cleanly rather than
loading wrongly.

---

# Importing DrumGizmo kits

`LithiumImport` converts a downloaded [DrumGizmo](https://drumgizmo.org) kit
into this format:

```
LithiumImport <kit folder or drumkit.xml> [--kits-folder <dir>] [--mapping <file>]
              [--zones <n>] [--min-rr <n>] [--overwrite] [--copy]
```

Given a folder, it imports every drumkit XML in it. DRSKit has five (basic,
full, minimal, no brushes, brushes only), which become five kits.

## What DrumGizmo kits contain

- **A drumkit XML** per variant. It lists the kit's channels (mic names) and
  its instruments. Each instrument points to an instrument XML and has a
  `channelmap` that renames the instrument's channels to kit channels.
  Instruments in the same `group` choke each other (normally the hi-hat). The
  newer format (DrumGizmo 0.9.17 and later) adds a `<metadata>` block (title,
  author, licence, website) and explicit `<chokes>`; older kits, including
  DRSKit and MuldjordKit, only have `name` and `description` attributes.
- **Instrument XMLs**, each listing samples. A sample has a `power` value and
  an `audiofile` per channel: a file and a 1-based `filechannel`. In practice
  each sample is one multichannel WAV with one channel per mic (DRSKit: 13
  channels, MuldjordKit: 16, 32-bit float at 44.1 kHz). The older instrument
  format 1.0 has no power values; it assigns samples to velocity ranges
  instead, and the importer uses the middle of each sample's range.
- **MIDI maps**: `<map note="38" instr="Snare"/>`. Kits with variants have one
  map per variant (`Midimap_basic.xml` for `DRSKit_basic.xml`).

## From power values to velocity zones

DrumGizmo has no velocity layers. Each sample has a `power` value (its relative
strength), and DrumGizmo maps MIDI velocity linearly onto the instrument's
power range, the quietest sample at velocity 1 and the loudest at 127, then
picks a sample near that power. The importer turns this into zones:

1. Each sample's power is mapped onto velocity 1-127 the same way, linearly
   between the instrument's quietest and loudest sample, so imported kits
   respond to velocity as they do in DrumGizmo.
2. The samples are sorted by power and split into at most `--zones` (default 8)
   contiguous groups of at least `--min-rr` (default 3) samples each. The split
   is optimal 1-D clustering (Jenks natural breaks, by dynamic programming):
   it minimises the spread of velocities within each group. Zone sizes
   therefore come from the data: where the recorded hits bunch together they
   share a zone, and where there's a gap in the dynamics a boundary falls
   there. An instrument with fewer samples than the minimum gets one zone.
3. Each zone's velocity range reaches halfway to its neighbours, so the zones
   cover 1-127 with no gaps.
4. The samples in a zone become its round-robins.
5. `velocityRangeDb` is set from the power at the zone's two velocity edges
   (at most 12 dB), so the level rises smoothly across the zone the way the
   samples' powers do.

More zones follow DrumGizmo's continuous selection more closely; fewer zones
give more round-robins per zone (less "machine gun" on repeated hits). The
defaults balance the two for kits with 15-60 samples per instrument.

## Mapping files

A mapping file says how a kit's instruments become pieces and articulations,
and how its channels become mics. Built-in mappings for DRSKit and MuldjordKit
are in `assets/import-maps/`; `--mapping` uses another. Abridged:

```json
{
  "matches": ["DRSKit2", "DRSKit"],
  "variants": { "DRSKit_basic.xml": { "name": "DRSKit", "midimap": "Midimap_basic.xml" } },
  "author": "...", "sourceUrl": "...", "credits": "...",
  "license": { "name": "CC BY 4.0", "url": "...", "files": ["README.md"] },
  "levelDb": -6,
  "ignoreChannels": ["Trigger"],
  "mics": [
    { "id": "overheads", "name": "Overheads", "role": "overheads", "channels": ["OHR", "OHL"] },
    { "id": "snare_bottom", "name": "Snare Btm", "channels": ["Snare_bottom"], "invertPolarity": true }
  ],
  "pieces": [
    { "id": "hihat", "name": "Hi-Hat", "role": "hihat", "articulations": [
      { "instrument": "Hihat_closed", "id": "closed", "name": "Closed", "chokes": ["hihat_open"] },
      { "instrument": "Hihat_open",   "id": "open",   "name": "Open",   "chokeGroup": "hihat_open" }
    ] }
  ]
}
```

- `matches` is compared with the drumkit's name to pick a built-in mapping.
- A mic with two `channels` is a stereo pair, left first. The built-in mappings
  lay stereo pairs and pans out from the audience's side, to match the kit
  stage, so the drummer's left overhead plays on the right.
- `levelDb` lowers every mic's default fader by the same amount. Summed at
  unity, a dozen close and room mics of a hard hit clip; this keeps their
  balance and leaves headroom.
- Instruments missing from a variant are skipped, and pieces left with no
  articulations are dropped. Instruments or channels the mapping doesn't
  mention are still imported (with a warning), so nothing is lost.

Without a mapping, the importer works it out from names: room and overhead
channel pairs ending in L/R become stereo mics, every other channel a mono
close mic; each instrument becomes a piece with a role guessed from its name;
instruments sharing a choke group become articulations of one piece, where
those named "open" are choked by the others; and the licence is recognised
from the kit's metadata or its README.

## What the import writes

- `kit.json`, written last, so an interrupted import never leaves a loadable
  kit behind.
- `audio/`, the kit's WAV files at their original relative paths. They are
  hard-linked to the downloaded files where possible, so DRSKit's five variants
  share one copy of the audio; `--copy` forces copies, and a kits folder on
  another drive gets copies anyway. The audio itself is unchanged.
- `LICENSE.txt`, naming the kit, author, source and licence, and saying what
  the conversion changed.
- The kit's own notice (its README or licence file) as `Original <name>`.

The import was verified on DRSKit 2.1 (all five variants) and MuldjordKit 3.0:
every note in each kit's MIDI map plays at three velocities, every mic channel
is audible, and closing the hi-hat chokes the open hi-hat.
