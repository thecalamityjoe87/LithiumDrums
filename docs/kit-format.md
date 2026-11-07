# Lithium Drums kit format

A kit is a folder containing a `kit.json` manifest, the audio files it refers
to (WAV or FLAC), and its licence. A kit preset is a folder with only a
`kit.json`, which plays another kit's audio with its own choices (see "Kit
presets"). Kits live in the user's kits folder, grouped in content packs (see
"Content packs"), never in this repository:

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
  "id": "drskit-full",
  "name": "DRSKit Full",
  "author": "The DrumGizmo team ...",
  "sourceUrl": "https://drumgizmo.org/wiki/doku.php?id=kits:drskit",
  "license": { "name": "CC BY 4.0", "url": "https://creativecommons.org/licenses/by/4.0/", "file": "LICENSE.txt" },
  "credits": "DRSKit, a DrumGizmo kit ...",
  "description": "A handcrafted kit ...",
  "tags": ["acoustic", "jazz", "rock"],
  "sampleRate": 44100,
  "stats": { "pieces": 9, "mics": 11, "hits": 541, "ramBytes": 3226285056 },

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
| `type`        | no       | `"kit"` (the default) or `"preset"` (see "Kit presets"). |
| `id`          | no       | A stable id: presets name their base kit by it, and projects save it to find the kit again if its folder moves. Letters, digits, `_`, `-`, `.`. Defaults to the folder name, lower-cased. |
| `name`        | yes      | Shown in the kit menu, the kit browser and the About dialog. |
| `tags`        | no       | Words the kit browser shows and searches (`"jazz"`, `"brushes"`). |
| `stats`       | no       | Written by the importer for the kit browser: `pieces`, `mics`, `hits`, and `ramBytes` (sample data in RAM with every mic loaded). Never needed to play the kit. |
| `author`, `sourceUrl`, `credits`, `description` | no | Shown in the About dialog. |
| `license`     | no*      | `{ "name", "url", "file", "shareAlike" }`. `file` is the licence text inside the kit folder; `shareAlike` marks licences like CC BY-SA, whose adaptations must keep the licence (worked out from the name if left out). |
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
| `chokeNote` | A MIDI note that chokes this piece. Cymbals without one get a fixed note by role, the same on every kit: crashes 1-3 on 85-87, rides 1-2 on 88-89, chinas 1-2 on 90-91, the splash on 92 (see [midi.md](midi.md)). |
| `tune`     | The kit's own tuning of the piece, in semitones (-24 to 24). Default 0. |
| `volumeDb` | The kit's own level for the piece. Default 0. |
| `decay`    | The kit's own decay, 0-100 (100 = the samples' natural length). Default 100. |
| `articulations` | At least one. The first is what clicking the piece plays. |

The piece controls (Volume, Tune, Decay) work on top of `volumeDb`, `tune` and
`decay`, the way the mixer works on top of the mics' defaults: at their neutral
settings the piece plays as the kit sets it up. Presets use these to retune a
kit without touching its audio.

**Roles and the stage.** The rendered kit stage has eleven slots: kick, snare,
hi-hat, two rack toms, floor tom, two crashes, splash, ride and china. Pieces
are assigned to slots by role, in kit order: the first kick to the kick slot,
the first two `tom`s to the rack tom slots, and so on. A splash has the `crash`
role and is told apart by its id or name; the first goes to the splash slot and
never takes a crash's. A slot with no piece is drawn dimmed; a piece with no
slot (a second floor tom, a third crash, a second splash) still plays and still
gets a strip.

**Articulations** are the ways of hitting a piece (snare: center, rim, rest;
hi-hat: closed, open, pedal; ride: bow, bell). Each has an `id`, a `name`,
choke settings, an optional `channelMap`, and `zones`.

Standard ids (`center`, `off_center`, `rimshot`, `sidestick`, `closed`,
`half_open`, `open`, `pedal`, `bow`, `bell`, `edge`, ...) let kit-independent
MIDI maps find an articulation, and tell the e-kit controllers what it is (see
[midi.md](midi.md)). Kits that use other ids, or record more steps, can say so:

| Key        | Meaning |
|------------|---------|
| `openness` | Hi-hats: the articulation's step on the pedal's openness ladder, 0 (closed) to 1 (fully open). |
| `stroke`   | Hi-hats: `"tip"` (bow hits) or `"shank"` (edge hits). Default `tip`. |
| `position` | Drums and cymbals: the strike position, 0 (centre, or near the bell) to 1 (edge). A ride with several bow recordings uses this to follow positional sensing. |
| `positionRange` | Optional, `[low, high]` within 0..1: the positions this articulation plays alone. Between one articulation's range and the next, the two crossfade. Without it, each position plays alone to 35% of the way to its neighbours. Implies `position` (the middle of the range) if that isn't given. |

A snare recorded at the centre and off-centre, where the off-centre hit should
take over from just under halfway:

```json
{ "id": "center",     "positionRange": [0.0, 0.2], "zones": [ ... ] },
{ "id": "off_center", "positionRange": [0.4, 1.0], "zones": [ ... ] }
```

Hits at 0.2-0.4 blend the two. Kits without positional recordings still follow
positional sensing: the engine filters the centre hit towards the edge, and a
ride's bow blends towards its own `edge` hit (see [midi.md](midi.md)).

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

Any of the object forms can add `"start"`: frames to skip at the start of the
file, for recordings with pedal noise or silence before the hit (SFZ's
`offset`). An articulation's `"start"` applies to all its hits. Every mic of a
hit still plays from one shared position, counted from its own start.

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

A cymbal hit never chokes its own cymbal, whatever its `chokes` say: hitting
the bell over a ringing bow adds to the sound, as on a real cymbal. Only a grab
(an articulation whose id contains `choke`, `grab` or `stop`) silences its own
cymbal. A hi-hat hit only chokes its own hits that rang more open than it
leaves the hats, so closed and pedal hits stop open ones but open hits ring on
together. Chokes between different pieces apply as written. SFZ kits with
`off_by` on every cymbal and hi-hat region (the AVL Drumkits) rely on this;
see [midi.md](midi.md).

## MIDI map

`midiMap` is the kit's default note map: `{ "note", "piece", "articulation" }`
entries, where `articulation` is optional (the piece's first). It's what the
"Kit default" MIDI map plays, with General MIDI on the notes it leaves free. The
other maps (General MIDI, Roland V-Drums, Alesis and the user's own) are
written by role ("the second rack tom", "the first ride, bell"), so they fit
any kit; a request for a piece the kit doesn't have (a third rack tom) uses the
last piece of that role. See [midi.md](midi.md).

## Audio and memory

WAV and FLAC files are read with JUCE's readers. Audio in RAM is stored per
mic as 16-bit integers (if every file is 16-bit) or 24-bit integers (anything
deeper, including 32-bit float sources), and converted to float as it plays.
That's 2 or 3 bytes per sample instead of float's 4. When a mic is muted for a
couple of seconds its audio is unloaded, and reloaded when it's unmuted.

Small kits are held in RAM whole. Large ones keep only the first part of each
sample in RAM and stream the rest from a cache that's built from the kit's
audio the first time it streams. That needs nothing from the kit format; see
[streaming.md](streaming.md).

A file plays to its end, so every importer leaves out a recording more than
six times as long as the median of its zone's others (and over 5 s), with a
warning: that's a whole take left in a library by mistake, not one hit.
Salamander's `kick_OH_P_1.wav` is one, 44 seconds with sixteen soft kicks in
it; played as a soft kick, it went on playing kicks for most of a minute.

## Scaling to large libraries

- `kit.json` describes structure, not audio, so it stays small: imported
  DrumGizmo kits are 40-100 KB for 200-550 hits, because hits are plain file
  names and the channel map is written once per articulation.
- All sample access goes through one interface (`SampleSource`): a voice opens
  a cursor and reads forwards. The streaming source serves the start of each
  sample from RAM and the rest from disk, behind the same interface.
- Many short files or one long file per hit both work; the loader reads every
  file's header first, lays memory out once, then decodes in parallel.

## Kit presets

A kit preset (`"type": "preset"`) plays another kit's recordings with its own
choices, without a copy of any audio. One recording session can so become
several kits: DRSKit's sticks-only and brushes-only variants are presets of
DRSKit Full, and "MuldjordKit Low" is MuldjordKit tuned down.

```json
{
  "formatVersion": 2,
  "type": "preset",
  "id": "muldjordkit-dry",
  "name": "MuldjordKit Dry",
  "base": "muldjordkit",
  "description": "Close mics and overheads only, with the toms damped.",
  "tags": ["rock", "dry"],
  "pieces": [
    { "piece": "kick_left" },
    { "piece": "snare", "articulations": ["center"] },
    { "piece": "tom2", "id": "tom", "name": "Tom", "tune": -2, "decay": 60 },
    { "piece": "tom2", "id": "tom_high", "name": "High Tom", "tune": 3 }
  ],
  "mics": { "room": { "enabled": false }, "overheads": { "volumeDb": -6 } },
  "midiMap": [ { "note": 36, "piece": "kick_left" } ]
}
```

| Key           | Meaning |
|---------------|---------|
| `base`        | The id of the kit whose recordings the preset plays. It must be a kit, not another preset. It is looked for in the preset's own content pack first, then anywhere in the kits folder. |
| `pieces`      | Which of the base kit's pieces to use, in this order (the order decides stage slots and strips). Each names a base `piece` and may give `articulations` (which ones, in this order: the first is what a click plays), and a new `id`, `name`, `role`, `tune`, `volumeDb`, `decay` or `chokeNote`. A piece can be used twice under two ids, tuned differently. Leave `pieces` out to keep them all. |
| `mics`        | Changes to the base kit's mics, by id: `enabled: false` leaves a mic out (it is never loaded, so the preset needs less memory), and `volumeDb`, `pan` and `name` replace the kit's defaults. |
| `midiMap`     | The preset's own map. Without one, the base kit's map is used for the pieces the preset keeps (under their new ids). |
| `name`, `description`, `tags` | The preset's own. |

The credits and licence are always the base kit's: the About dialog and the
kit browser show them for the preset, with "Preset of DRSKit Full". A preset of
a share-alike kit is an adaptation of it too, so it belongs in the same pack.

## Piece swapping

Any piece of the loaded kit can play another installed kit's recordings of the
same kind of drum: MuldjordKit's snare in DRSKit, say. Pick it from the piece's
model button (on the Kit tab's selected-piece card and slots, and in the Edit
tab's piece bar). A swap is a choice the user makes, not part of a kit: it's saved with
the project and in presets, as the piece's id, the other kit's id and that
kit's piece id. Nothing about it is written into either kit.

The piece keeps its place in the kit: its id, name, role, choke note, stage
slot, parameters and MIDI notes. Only its articulations change, to the other
kit's, with the other kit's tuning, level and decay for the piece. A rack tom
can be swapped for a floor tom and the other way round; every other role only
for the same role. Swapping into a kit preset works too: the preset's base
kit is the kit.

Its recordings were made through the other kit's mics, so each of those mics
is mapped onto one of this kit's, in this order:

1. **The piece's own close mics** go to the swapped piece's own close mics: a
   snare's top and bottom mics, a kick's inside and outside mics, a tom's mic.
   A kit's own mics for a piece are worked out from names: a mic with the
   piece's id (the Basalt kits have a mic per piece), otherwise the mics of the
   same family (kick, snare, hi-hat, tom, ride, crash, china, from words in the
   mic's id and name). Kits number their tom mics across rack and floor toms
   (DRSKit's `tom2` is on its first floor tom), so tom mics are paired with the
   toms in kit order when there are as many of each. Mics with the same
   placement pair up (top with top, bottom with bottom, in with in); a second
   top mic is left out rather than put on the bottom mic.
2. **The other close mics**, which picked the piece up from across the kit,
   go to the mic with the same id, otherwise to one of the same family and
   placement, otherwise of the same family.
3. **Overheads and room mics**, when "Ambience follows piece" is on (it is
   unless switched off in the piece's model picker), go to this kit's overhead
   and room mics: the same id first, then the same number of channels, then in
   order. A stereo source on a mono mic plays its left channel; a mono source
   on a stereo mic plays in the middle. With it off, only the close mics are
   used, and the piece sits in this kit's room only through its own channel.
4. **If nothing would be left to hear** (a piece from a kit with a mic per
   piece going to an empty slot, or a kit recorded only as a stereo pair
   meeting one with only close mics), the piece's main mic goes to the swapped
   piece's own mic. Failing that, a close mic goes to one of this kit's close
   mics, which carries it to the piece's own channel: the same id, otherwise
   one with as many channels, then the most central. Anything else goes to the
   kit's overheads, otherwise to its first mic.

Only the mics that recorded the piece count: in a kit with a mic per piece, a
foot stomp is on its own mic and no other, so the others are never mapped or
listed. Each of this kit's mics takes at most one of the other kit's. Mics with
nowhere to go are left out, and the model picker and the model button's
tooltip list them ("Not used: Ride R"); apart from the last resort in step 4,
nothing is routed to a mic it doesn't belong on to make it fit. The levels stay as the other kit set them:
each mapped mic plays at its own kit's default level and polarity relative
to the mic it lands on, so a reversed snare bottom mic stays reversed.

The swapped piece's choke groups are its own (a hi-hat's closed hits still
choke its open ones); it can't choke, or be choked by, the rest of this kit
except through its choke note. Recordings at another sample rate (CrocellKit
and Virtuosity are 48 kHz) play at the right pitch. The other kit's audio is
read from its own folder, and when the kit streams, from that folder's own
stream cache pack (see [streaming.md](streaming.md)).

If the other kit isn't installed any more, the piece plays this kit's own
recordings and its model picker says why; the swap is kept, so it comes back
when the kit is installed again. `LithiumDrumsTests --swap-audit` tries every
swap between the installed kits and reports where every mic goes.

### Slots: filling and emptying

The Kit tab's Slots view has the same 24 slots for every kit
(`src/engine/KitSlots.cpp`), in a 6 × 4 grid: Kick, Snare, Snare 2, Hi-Hat,
Tom 1-3, Floor Tom 1-2, Crash 1-3, Splash, Ride, China, and Extra 1-9. When a
kit loads, each of its pieces goes, in kit order, to the first free slot of
its kind (a rack tom to the next Tom, a crash whose id or name says splash to
Splash), and anything with no named slot left (a second kick, a cowbell, a
gong) to the next Extra. Every installed library fits; Basalt Cobblestone's
nine percussion pieces fill all nine extras.

An empty slot can be filled from any installed kit, and a full one emptied.
Both are entries in the same swap list, saved the same way:

- **Filling** a slot is a swap whose piece id is `slot-` and the slot's id
  (`slot-tom3`). The new piece is added after the kit's own, named after the
  slot (or, in an Extra, after the piece), and mapped onto the kit's mics as
  above. In a named slot it plays as the slot's kind (a floor tom in Tom 3 is
  a rack tom, for its MIDI notes and its place on the stage); in an Extra, as
  what it is. A named slot takes the same kinds a swap does (rack and floor
  toms for each other, any crash or splash for Crash and Splash); an Extra
  takes anything.
- **Emptying** a slot is an entry with the piece's id marked `removed`
  instead of a source. The kit's own piece is remembered, so the model picker
  can put it back.

Slots are handed out from the kit as it is, before any of this, so filling or
emptying one never moves another piece to a different slot. Pieces after a
removed one move up in the kit's list; their mixer and shaping settings move
with them (matched by piece id), and an added piece starts at the defaults.

## Content packs

Every import writes a content pack: a folder in the kits folder with a
`pack.json`, the licence, and one subfolder per kit or preset.

```
Kits/
  Basalt Kits/
    pack.json
    LICENSE.txt
    Original AVL-Drumkits CC-BY-SA License.pdf
    Onyx/               kit.json, LICENSE.txt, audio/
    Garnet/
  DRSKit/
    pack.json
    DRSKit Full/        kit.json, LICENSE.txt, audio/
    DRSKit Brushes/     kit.json (a preset)
```

```json
{ "formatVersion": 1, "id": "basalt-kits", "name": "Basalt Kits", "author": "...", "sourceUrl": "...",
  "description": "...", "credits": "...",
  "license": { "name": "CC BY-SA 3.0", "url": "...", "file": "LICENSE.txt", "shareAlike": true } }
```

A mapping can give a pack and its kits names of their own. The AVL Drumkits
become the Basalt Kits pack (Onyx, Garnet, Amber, Amber Rods, Cobblestone),
because the AVL licence asks that modified versions be distributed under a
different name; the credits and licence still name Glen MacArthur's AVL
Drumkits.

The kit browser and the kit menu group kits by pack, and the About dialog
shows one credits card per pack. Kits can also sit directly in the kits folder
(without a pack); they're listed under "Other kits".

A pack is the unit to pass on. That matters most for share-alike licences
(CC BY-SA): a converted kit is an adaptation of the library it came from, and
must be shared under the same licence. Keeping each library in its own pack,
with its licence beside it, keeps share-alike content apart from everything
else (the plugin included) and easy to pass on correctly. The pack's
`LICENSE.txt` says so in plain words. Every kit folder also has its own
`LICENSE.txt` and the library's original notice, so a kit copied out of its
pack still carries its attribution.

---

# Importing

`LithiumImport` converts a downloaded library into a content pack. (The
plugin's Download buttons fetch the libraries it knows and run the same
importers; see [packs.md](packs.md).) It reads
[DrumGizmo](https://drumgizmo.org) kits and SFZ instruments, and tells them
apart by their files:

```
LithiumImport <folder, drumkit.xml or .sfz> [--kits-folder <dir>] [--mapping <file>]
              [--zones <n>] [--min-rr <n>] [--overwrite] [--copy]
```

Both importers fill in the same in-memory description of a pack (kits,
presets, licence), which one writer turns into files. The writer parses every
`kit.json` it writes back in, so a conversion that wouldn't load fails at
import time, and fills in the `stats` from the audio files' headers.

A mapping file says how a library's instruments become pieces, articulations
and mics. Built-in mappings for the supported libraries are in
`assets/import-maps/`, picked by name; `--mapping` uses another one. Without a
mapping, the importers work out what they can from names. All mappings share
these keys:

| Key             | Meaning |
|-----------------|---------|
| `matches`       | Names that pick this mapping: DrumGizmo kit names, or SFZ file names. |
| `pack`          | `{ "id", "name" }` of the content pack. |
| `author`, `sourceUrl`, `credits`, `description` | The library's credits. |
| `license`       | `{ "name", "url", "shareAlike", "files" }`; `files` are the library's own notices, copied as `Original <name>`. |
| `licenseNote`   | Terms the library adds to its licence, quoted in `LICENSE.txt`. |
| `levelDb`       | Lowers every mic's default fader by the same amount, for headroom. |
| `presets`       | Kit presets to write with the kits (in the format above; `base` is a kit id from this mapping). |

# Importing DrumGizmo kits

Given a folder, the importer reads every drumkit XML in it. DRSKit has five
(basic, full, minimal, no brushes, brushes only). Its mapping imports the full
kit as a kit and the other four as presets of it, since they're subsets of the
same recordings: same pieces, same samples.

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

## DrumGizmo mapping files

A DrumGizmo mapping says how a kit's instruments become pieces and
articulations, and how its channels become mics. Abridged:

```json
{
  "matches": ["DRSKit2", "DRSKit"],
  "pack": { "id": "drumgizmo-drskit", "name": "DRSKit" },
  "variants": {
    "DRSKit_full.xml":  { "name": "DRSKit Full", "id": "drskit-full", "midimap": "Midimap_full.xml" },
    "DRSKit_basic.xml": { "name": "DRSKit", "id": "drskit", "midimap": "Midimap_basic.xml", "presetOf": "DRSKit_full.xml" }
  },
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
- A variant with `presetOf` becomes a preset of that variant: its pieces,
  articulations and mics are chosen from the other's, and it keeps its own MIDI
  map. The importer checks that every instrument of the variant is the same
  instrument file in the other one; if not, it imports a kit of its own and
  says why. `preferArticulations` puts the listed articulations first in each
  piece (so a click on a brushes preset plays a brush).
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

# Importing SFZ instruments

An SFZ instrument is a list of regions: a sample, the keys and velocities it
plays on, and conditions such as round-robin position or controller ranges.
Headers group regions and set defaults for them (`<global>`, `<master>`,
`<group>`), and `#define` and `#include` build larger instruments out of
smaller files. The parser resolves all of that into one flat list, each region
with every opcode that applies to it.

A multi-mic SFZ kit (Virtuosity Drums) has a region per mic for every hit:
same key, same velocity range, same round-robin position, a different sample
per mic, each mic switched on and off by a controller. The importer turns
regions into hits like this:

1. **Fix the controllers.** The instrument is evaluated at one controller
   state: its own `set_ccN` (and high-resolution `set_hdccN`, 0-1) defaults,
   then the mapping's `cc`, then the kit's.
   A region whose `loccN`/`hiccN` range excludes the state is left out, so the
   mapping can switch every mic on, switch alternative layers off (Virtuosity's
   pitched-down "epic" layers), or pick snares on or off. Release-triggered
   regions are left out, and so are built-in samples such as `*silence`.
2. **Pick the regions of an articulation**: those on its `key`, optionally
   narrowed by `samples` and `exclude` wildcards (Salamander keeps its
   semi-open hi-hat on the closed hi-hat's key, told apart by CC64).
3. **Assign each region to a mic** by its sample path: the first mapping mic
   whose `samples` wildcard matches it. With `"mics": "perPiece"`, each piece
   gets a mic of its own instead: the AVL kits are one panned mono recording
   per piece, and a mic per piece keeps the pans and gives every piece a mixer
   channel.
4. **Velocity zones.** Every region's velocity range adds its edges, and each
   stretch between edges becomes a zone, so mics that split velocity
   differently still line up. Adjacent stretches that play exactly the same
   regions merge into one zone.
5. **Round-robins.** Within a zone, each mic's regions are put in round-robin
   order (`seq_position`, then `lorand` for random-robin kits), and hit n
   plays every mic's nth region together, sample-aligned.
6. **Levels.** A zone's `gainDb` is its regions' `volume`, `amplitude` and
   controller gains (`amplitude_onccN`/`amplitude_ccN` as ARIA plays them: a
   multiplier from 0 at CC 0 to N% at CC 127, which is how library mixers use
   them as faders), plus the velocity curve (`amp_veltrack`,
   `amp_velcurve_N`, or SFZ's default velocity-squared) at the zone's top.
   `velocityRangeDb` is the curve's drop to the bottom of the zone, so the level
   follows the SFZ's curve within the zone too.
7. **Chokes.** SFZ chokes by `group`/`off_by`: playing a region of group N
   stops regions with `off_by=N`. An articulation joins the group its regions
   are `off_by`, and chokes every group a region on its key belongs to, silence
   regions included (Virtuosity chokes through silent samples). A mapping can
   set `chokeGroup` and `chokes` itself instead.
8. **Start offsets and tuning.** `offset` becomes the hit's `start`. A piece
   whose regions all share one pitch shift (`transpose`, `tune`, key tracking)
   gets it as its `tune`.

Opcodes that change the sound in ways the format doesn't model yet (filters,
EQ, envelopes that don't sustain at 100%, `end`, loops) are ignored, and the
import lists them with how many regions use them.

An SFZ mapping adds these keys to the shared ones:

```json
{
  "format": "sfz",
  "matches": ["02-full-kit.sfz"],
  "cc": { "21": 0, "101": 127, "113": 0 },
  "exclude": ["*/Samples/perc/*"],
  "mics": [ { "id": "kick", "name": "Kick", "role": "close", "channels": 1, "samples": ["*Samples/kickmic/*"] },
            { "id": "overheads", "name": "Overheads", "role": "overheads", "channels": 2, "swapStereo": false,
              "samples": ["*Samples/oh/*"] } ],
  "audienceView": true,
  "pieces": [
    { "id": "hihat", "name": "Hi-Hat", "role": "hihat", "articulations": [
      { "id": "closed", "name": "Closed", "key": 42, "exclude": ["*SemiOpen*"], "chokes": ["hihat_open"] },
      { "id": "half_open", "name": "Half open", "key": 42, "note": 25, "cc": { "64": 64 }, "samples": ["*SemiOpen4*"],
        "chokeGroup": "hihat_open" } ] }
  ],
  "kits": [
    { "file": "Programs/02-full-kit.sfz", "id": "virtuosity", "name": "Virtuosity Drums" },
    { "file": "Programs/02-full-kit.sfz", "id": "virtuosity-snares-off", "name": "Virtuosity Snares Off", "cc": { "21": 127 } }
  ]
}
```

- `kits` lists the instruments to import. The same file can become several
  kits at different controller states. A kit can replace the shared `pieces`,
  add `extraPieces` or leave some out (`skipPieces`); `file` is matched against
  the end of each `.sfz` path, so it works whichever folder level the download
  was unpacked to.
- An articulation's MIDI note is its `key`, unless `note` gives another (two
  articulations read from one key need two notes); `notes` adds more.
- `swapStereo` swaps a stereo mic's sides. Kits recorded from the drummer's
  seat put the hi-hat on the left; Lithium Drums shows the kit from the
  audience's side, so the mappings swap them where needed. `audienceView`
  (the default) flips per-piece SFZ pans the same way.

Without a mapping, every `.sfz` in the folder becomes a kit, with one mic, one
piece per key (named after its samples, its role guessed from the name), and
the hi-hat keys gathered into one hi-hat piece.

# Importing folders of samples

Some libraries are just one-shot WAV files, one per hit, without an SFZ or
DrumGizmo description: The Open Source Drum Kit is a folder per piece, its
files numbered from the quietest hit to the loudest. A mapping with
`"format": "samples"` describes such a library, and the importer recognises it
by the mapping's `marker`, a file that's there in every copy of the library.

```json
{
  "format": "samples",
  "marker": "Plugin/Samples/kick/kick1.wav",
  "root": "Plugin/Samples",
  "mics": [ { "id": "kit", "name": "Kit (stereo)", "role": "overheads", "channels": 2, "volumeDb": -6 } ],
  "kits": [ { "id": "open-source-drumkit", "name": "Open Source Drum Kit" } ],
  "pieces": [
    { "id": "snare", "name": "Snare", "role": "snare", "articulations": [
      { "id": "center", "name": "Center", "files": ["snare/snare-top*.wav"], "exclude": ["snare/snare-top-*.wav"] }
    ] }
  ]
}
```

- `files` and `exclude` are wildcards on file names, in folders relative to
  `root`; `exclude` takes back what a broader pattern caught (`snare-top*`
  would otherwise take the `snare-top-buttend` hits too).
- Every file is one hit on the first mic: a stereo file on a stereo mic, a
  mono one on a mono mic.
- **Velocity zones.** Each file's peak level is measured, and the hits are
  sorted from quietest to loudest and grouped into velocity zones exactly as
  DrumGizmo power values are (see "From power values to velocity zones"),
  with the same `--zones` and `--min-rr` options. The file names' numbering
  isn't relied on.
- `chokeGroup` and `chokes` work as in the other mappings; `midiMap` gives
  the kit's own notes.

# What an import writes

- `pack.json`, `LICENSE.txt` (attribution, licence, what the conversion
  changed, and for share-alike licences what that means) and the library's own
  notices as `Original <name>`.
- A folder per kit with `kit.json`, its own `LICENSE.txt` and notice, and
  `audio/`: the kit's audio files at their original relative paths. They are
  hard-linked to the downloaded files where possible, so kits made from the
  same download share one copy on disk; `--copy` forces copies, and a kits
  folder on another drive gets copies anyway. The audio itself is unchanged.
  `kit.json` is written last, so an interrupted import never leaves a loadable
  kit behind.
- A folder per preset, with just its `kit.json`.

The imports were verified on DRSKit 2.1 (a kit and four presets), MuldjordKit
3.0 (a kit and three presets), CrocellKit 1.1 (a kit and three presets), the
five AVL Drumkits (as the Basalt Kits pack), the Salamander Drumkit (a kit and
two presets), Virtuosity Drums 0.925 (two kits and four presets), Big Rusty
Drums 1.100, SM Drums 1.2 (Sforzando edition) and Naked Drums (sfzinstruments
edition): every note in each MIDI map plays at three velocities, every mic
channel is audible, and closing the hi-hat chokes the open hi-hat.
`LithiumDrumsTests --midi-audit` repeats the check for every installed kit.

The SFZ reader also handles what ARIA-authored libraries do: several
`#define`s on one line (a value is one word), labels with spaces
(`region_label=10 one`), and sources three folders deep.
`LithiumDrumsTests --sfz-dump <file.sfz>` prints what it reads, for writing a
mapping.
