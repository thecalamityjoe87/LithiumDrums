# Grooves

The Grooves tab plays recorded drum performances through the loaded kit, in
time with the host, and lets you drag them into a track as MIDI.

The grooves come from the [Groove MIDI Dataset](https://magenta.tensorflow.org/datasets/groove)
(Jon Gillick, Adam Roberts, Jesse Engel, Douglas Eck and David Bamman, Google
Magenta, CC BY 4.0): 1,150 performances by ten drummers, most of them hired
professionals, played on a Roland TD-11 electronic kit to a metronome. They're
real playing, not quantized patterns, so they keep the drummers' timing and
dynamics, ghost notes and hi-hat pedal work included.

## Getting them

With no grooves installed, the Grooves tab shows the dataset greyed out with
a Download button (3 MB), which downloads, converts and installs it (see
[packs.md](packs.md)); `LithiumImport --install groove-midi-dataset` does the
same from a terminal. To convert a copy you downloaded yourself, unzip
`groove-v1.0.0-midionly.zip` and run `LithiumImport <folder>` on it.

It becomes a groove pack in the grooves folder: `~/.local/share/Lithium
Drums/Grooves` on Linux, `~/Library/Application Support/Lithium Drums/Grooves`
on macOS, `%LOCALAPPDATA%\Lithium Drums\Grooves` on Windows
(`--grooves-folder` puts it elsewhere, and the `LITHIUM_GROOVES_FOLDER`
environment variable points the plugin at another one). Rescan on the Grooves
tab reads the folder again. `--phrase-bars <n>` changes how many bars each
phrase card holds (4 by default).

## How the dataset becomes grooves

The dataset is a list of takes in `info.csv`, each a "beat" (a groove, often a
minute or more long) or a "fill" (one or two bars). The importer copies the
MIDI files as they were recorded and adds only what the files don't say:

- **Grooves.** Each beat is a groove, named after its style and numbered in
  recording order: "Rock 12", "Purdie Shuffle 2", "Halftime Rock 4".
- **Fills.** Every fill was recorded in a session right after the beat it goes
  with, at the same tempo and style, so each fill joins the latest such beat in
  its session (or the next one, if it came first).
- **Bars.** A take covers bars up to the one its last hit is in. Tick 0 is a
  bar line: in 109 of 115 rock takes checked, the backbeat falls on beats 2 and
  4 counted from there.
- **The hi-hat pedal.** The TD-11 sends the pedal on CC4, from 0 (open) to 90
  (closed), not 127: 62,000 of the dataset's 64,000 closed hi-hat hits have it
  at exactly 90. The pack records that, so 90 reads as fully closed.

On the page each groove is split into phrases of four bars, plus a "whole take"
card and the fills. A hit played up to a 32nd note ahead of a phrase's first
bar line counts as that phrase's (drummers push downbeats), and in a loop it
plays just before the downbeat comes round again.

Each card shows what's worth knowing while browsing: its bars, the tempo and
meter, the style, a feel and how busy it is (one to three dots), with a
preview of its first bar on three rows (kick, snare, hi-hat or ride, on a
16-step grid; toms, crashes and ghost notes are left out, and a louder hit is a
bigger dot). The feel and the dots are worked out from the hits: "Shuffle" when
the off-beat eighths fall late (two thirds of the way through the beat),
"Half-time" when the snare's backbeat is on beat 3 rather than 2 and 4,
otherwise "Straight"; the dots count the hits per bar (up to 14, up to 24,
more). The selected card is drawn in full in the pane below the cards: a lane
per kind of piece across all its bars, with the playhead while it plays.

## Preview and export

The files keep the TD-11's notes, so they're read through the Roland V-Drums
map (see [midi.md](midi.md)). Each hit is resolved to the articulation it
plays on the loaded kit. A hi-hat hit plays the openness step nearest to where
the pedal was at the time: a kit with closed, half-open and open hats plays
all three, where the dataset's open-hat notes alone would only give two.

The preview plays those articulations. Dragging a card (or the full view) into the host (or Save
MIDI) writes, for each hit, the note that plays the same articulation in the
**current** MIDI map: the recorded note itself where it plays the same hit
there (a kick stays on 36, though General MIDI's 35 plays it too), otherwise
the lowest note that plays it and isn't a spare. So the clip
sounds the same in the host as in the preview, with Lithium Standard, General
MIDI or an e-kit map. Without a kit loaded, the recorded notes are written as
they are. The file has the clip's tempo and meter, 480 ticks per beat, one
32nd-note-long note per hit on channel 10, and ends on the clip's last bar
line. Dragged files are written to a "Dragged Grooves" folder next to the
stream cache, since some hosts link to a dropped file instead of copying it.

Tempo:

- **Host** plays at the host's tempo. While the host plays, the preview locks
  to its beat position, so a 4-bar phrase lines up with the host's 4-bar grid,
  and follows its loops and jumps. In the standalone app, which has no host,
  it plays at the groove's own tempo.
- **Groove** plays at the tempo it was recorded at.
- **Custom** plays at a tempo you drag (shift for fine steps).

½× and 2× play it at half or double time. Loop off plays the clip once.

## Threads

The page builds a clip and the processor resolves it against the kit on the
message thread, into a `GroovePattern`: a sorted list of (beat, articulation,
velocity). The pattern goes to the audio thread through a lock-free queue and
comes back through another for deletion, like a kit does, so the audio thread
never allocates or frees. When a new kit arrives, the processor resolves the
clip again and sends the new pattern, which carries on from the same position;
until it arrives, a pattern made for the old kit stays silent. Each block, the
`GroovePlayer` works out which hits fall in it, and the processor plays them
on their exact samples, interleaved with the host's MIDI.

## The groove pack format

A groove pack is a folder in the grooves folder holding `grooves.json`, the
MIDI files and the licence:

```json
{
  "format": "lithium-grooves",
  "version": 1,
  "id": "groove-midi-dataset",
  "name": "Groove MIDI Dataset",
  "author": "...",
  "sourceUrl": "https://magenta.tensorflow.org/datasets/groove",
  "credits": "...",
  "license": { "name": "CC BY 4.0", "url": "https://creativecommons.org/licenses/by/4.0/", "file": "LICENSE.txt" },
  "noteMap": "roland-vdrums",
  "pedal": { "controller": 4, "closedAt": 90 },
  "phraseBars": 4,
  "styles": { "rock": "Rock", "hiphop": "Hip-Hop" },
  "grooves": [
    {
      "id": "gmd-drummer1-session1-4",
      "name": "Jazz Funk 1",
      "style": "jazz",
      "substyle": "Funk",
      "drummer": "Drummer 1",
      "bpm": 116,
      "meter": "4/4",
      "bars": 37,
      "file": "midi/drummer1/session1/4_jazz-funk_116_beat_4-4.mid",
      "fills": [ { "id": "gmd-drummer1-session1-6", "name": "Fill 1", "file": "midi/...", "bars": 1 } ]
    }
  ]
}
```

`noteMap` is the id of a built-in MIDI map the files are written in, and
`pedal` the hi-hat pedal controller and the value it reaches when closed. The
manifest is written last, so a pack whose import didn't finish has none and is
skipped. Paths are relative to the pack and can't leave it. Other libraries
can become packs with an importer of their own, or a hand-written manifest.
