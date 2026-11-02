# MIDI maps and e-kits

A MIDI map says which note plays what. Lithium Drums ships five, picked in the
MIDI panel (the round MIDI button in the header, which also lights up when
notes arrive):

| Map | What it's for |
|---|---|
| Lithium Standard | The same on every kit: General MIDI, plus a fixed note for every other kind of hit. New instances start here. |
| Kit default | The kit's own map (from its library), with General MIDI on the notes it leaves free. Different for every kit. |
| General MIDI | MIDI files, keyboards and most drum MIDI packs. |
| Roland V-Drums | The factory note map of Roland modules (TD-17, TD-27, TD-50 and others), with continuous hi-hat and positional sensing. |
| Alesis | The factory note map of Alesis Nitro, Nitro Mesh, Turbo and Surge kits. |

The Roland notes come from Roland's published TD-27 default note map, and the
controller behaviour from the TD-50X data list: CC4 runs from open (0) to
closed (127), and positional sensing sends a CC before each hit, running from
the centre of the head (0) to its edge (127). The snare, ride and tom position
CC numbers are a setting on the module (SETUP > MIDI > CONTROL); the map
expects 16, 17 and 18, and the MIDI panel changes them. The Alesis notes are
from the appendix of the Nitro module's manual, plus the General MIDI ride bell
(53) and edge (59), which aren't on the Nitro chart but are what modules with
three-zone rides send. Alesis modules that send CC4
with their hi-hat notes, like the Strike, get continuous openness too.

Maps are kit-independent: they name a piece by role and which one of that role
("the second crash"), and an articulation by a standard name ("rimshot",
"bell"). Each name has a list of kit articulation names that can play it, best
first (`src/engine/Articulations.h`): a snare "rimshot" plays a kit's
`rimshot`, else its `rim`; a ride "edge" plays `edge`, `shank` or `crash`. A
name the piece has none of plays the piece's first articulation. E-kit maps
count toms with the role `toms`: tom 3 is the kit's third tom, rack toms first
and then floor toms, so it is a rack tom on MuldjordKit and the first floor tom
on DRSKit. A pad for a piece the kit lacks plays the last piece of that kind.

## Lithium Standard: one map for every kit

Switching kits shouldn't change what your notes play. Lithium Standard is
General MIDI (so GM files and drum MIDI play as written) plus a fixed note for
each kind of hit General MIDI has no note for:

| Notes | What |
|---|---|
| 21-25 | Rimshots: rack toms 1-3, floor toms 1-2 |
| 26-30 | Off-centre hits: rack toms 1-3, floor toms 1-2 |
| 32-34 | Brushes: rack tom 1, floor toms 1-2 |
| 96-107 | Snare: off-centre, muted, half muted, buzz, flam, rest, stick shot, stick shot 2, snares off, brush, brush sweep; snare 2 |
| 108-114 | Hi-hat: closed edge, half open, three-quarter open, open tip, foot splash, closed and open brush |
| 115 | The second splash (the first is General MIDI's 55) |
| 116-127 | Crash 1 bow and grab, crash 2 bow and grab, crash 3, ride edge (crash) and grab, ride 2 bell and edge, china 2, china grab, crash sizzle |
| 85-92 | Cymbal choke notes: crashes 1-3 on 85-87, rides 1-2 on 88-89, chinas 1-2 on 90-91, the splash on 92 |

These extra notes are `exact`: a kit without that hit leaves the note silent
(rather than playing something else), so a note always means the same kind of
hit. "Crash 2" is always the second crash, never a splash: splashes have their
own notes (55, and 92 to choke). A note that plays something is never also a
choke note, so a kit's own map that uses 85-95 (Virtuosity's) doesn't choke
cymbals.

MIDI learn saves notes the same way: by role and standard name, not by the
kit's own names. A note learned for Basalt's ride "tip" is saved as the ride
"bow", so it plays DRSKit's ride bow too; percussion is saved by name, so a
learned cowbell plays a cowbell on every kit that has one, and nothing on one
that doesn't.

## Nothing is left out

Every articulation of every kit can be played with every map. General MIDI
finds the percussion notes by name: 56 plays a piece called cowbell, 54 a
tambourine, 39 a clap (or snaps), 55 a splash (or else the second crash), and
so on; a kit without a cowbell plays nothing on 56 rather than some other
percussion. 35 plays a second kick where the kit has one, and 59, General
MIDI's second ride, plays the ride's crash (its edge or shank hit) on a kit
with only one ride, as most drum MIDI expects.

Kit default plays the kit's own map, and General MIDI on the notes the kit's
map leaves free: DRSKit's library puts its ride crash on 66, so under Kit
default it plays on 66 and on 59. The kit's own notes always win.

Whatever a map still doesn't reach (a kit's tenth snare articulation, a second
china, a bucket) gets a **spare note**: the first free note from 96 up, then
from 0 up, then from 60 up, in kit order. A spare never takes a note the map
has an entry for (even one this kit can't play), so the map's own notes keep
their meaning; under Lithium Standard spares land on 0-20 and 60-84. Notes
85-95 are kept for the cymbal choke notes. Spare notes aren't part of the map: they're worked out again for
each kit, they show dimmed in the MIDI panel, and learning a note for an
articulation takes its spare note away. They never follow the pedal or a
position.

`LithiumDrumsTests --midi-audit` checks every installed kit and preset against
every built-in map: how much each map plays, which pieces are only on spare
notes, that nothing is unreachable, and that every articulation makes a sound.

## Following the pedal and the strike position

A map entry can `follow` a controller. Following entries play their own
articulation until that controller has been received, so the same map still
plays a plain MIDI file correctly.

**`"follow": "pedal"`** (hi-hat bow and edge hits). The hi-hat pedal CC picks
how open the hit is. A kit's hi-hat articulations are steps on an openness
ladder: `closed` 0, `quarter_open` 0.25, `half_open` 0.5, `three_quarter` 0.75,
`open` 1 (and a few synonyms), or whatever the kit sets in its `kit.json`
(`"openness"`, see [kit-format.md](kit-format.md)). Bow hits use the tip
ladder, edge hits the shank ladder; a kit with no shank recordings at some
openness lends the tip recording. Pedal hits (`pedal`), splashes and brush
strokes are not on the ladder; they play as they are.

**`"follow": "position"`** (snare, tom and ride hits). The position CC for the
piece picks the strike position from a ladder: `center` 0, `off_center` 0.5,
`edge` 1 on drums, or `"position"` values set in `kit.json`. Rides only have
positions where the kit records the bow at several places and says so; none of
the installed libraries does, so on today's kits ride position has no effect.
Positions are recordings, never synthesized blends between bow and bell.

Between two steps, the nearer step plays alone. Only in the middle 30% of the
gap do both play, with an equal-power crossfade, so a slowly moving pedal never
jumps and most hits are still a single real recording. Openness of 0.1 plays
the closed recording; 0.25 (halfway between closed and half open) plays both at
-3 dB.

Closing the pedal on ringing hats damps them, as on real hats: a voice played
more than 0.4 more open than the pedal now is fades out over 80 ms, and once
the pedal is shut every open step fades out over 30 ms, like a closed hit.
Opening the pedal never damps anything.

## Cymbals and hats ring on

Hitting a ringing cymbal again, on the bow, the bell or the edge, adds to the
sound; it doesn't stop the cymbal. Some libraries choke each cymbal hit with
the next (SFZ `group=N off_by=N` on every region, as in the AVL Drumkits), so
playing the bell over the bow cut the bow off. Lithium Drums therefore never
lets a cymbal hit silence its own cymbal, except a grab: an articulation whose
id contains `choke`, `grab` or `stop`, the choke note, or polyphonic
aftertouch.

The same libraries put every hi-hat sound in one self-choking group, so each
half-open hit cut off the last. Real hats only stop ringing when they close,
so a hi-hat hit only chokes its own hits that rang more open than it leaves
the hats: a closed hit or the pedal chick stops open and half-open hits, while
open, half-open and swish hits ring on together, and a closed hit doesn't cut
the last closed hit's short tail. How open each hit leaves the hats is its
openness step, 0 for other closed and pedal sounds, and 1 for the rest
(splashes, swishes).

Chokes between different pieces apply as the kit says. A drum that reaches
its voice limit (12) fades its oldest hit over 100 ms, slowly enough that a
ride pattern doesn't lose wash audibly.

## The MIDI panel

The panel follows Addictive Drums 2's layout: the input monitor and the
controller settings on the left, the kit's articulations in the middle, and a
keyboard on the right.

The keyboard stands upright, high notes at the top, and shows every note with
its number and what it plays, tinted with the drum's colour (cymbal choke
notes show faded). Beside it, a mini keyboard of all 128 notes shows where the
mapped notes are; click or drag it, or use the mouse wheel, to move. Clicking
a key plays the note through the map, exactly as MIDI input would (pedal and
position following included), harder the further right you click, so the
velocity zones can be heard. Notes that come in light their keys and scroll
them into view. Clicking an articulation's name plays it and outlines its keys.

## MIDI learn and saved maps

In the MIDI panel, every articulation of the loaded kit is listed with its
notes. **Learn** takes the next note that arrives (from a pad, a MIDI keyboard, the
host, or a click on the on-screen keyboard) and makes it play that
articulation, so a map can be built before the e-kit is even connected; it takes the note away from
whatever it played before. A note chip removes that note. The **PEDAL** or
**POSITION** button turns following on or off for the articulation's notes. A
learned hi-hat note follows the pedal, and a learned head or bow hit follows
its position, wherever the kit has steps for it.

Learning on Kit default first spells the kit's own map out as role-based
entries, so the edited map also works on other kits.

Edits make the map "modified". **Save as** stores it as a JSON file in the
MIDI Maps folder (`~/.local/share/Lithium Drums/MIDI Maps` on Linux, next to
the default kits folder on macOS and Windows), where it appears in the map
menu from then on. **Revert** goes back to the saved map; **Set as default**
makes new instances start with this map. A project stores the whole map, not
just its name, so it plays the same on a machine without that saved map.

## File format

```json
{
  "format": "lithium-midi-map",
  "version": 1,
  "id": "roland-vdrums",
  "name": "Roland V-Drums",
  "controllers": { "hihatPedal": 4, "pedalInverted": false,
                   "snarePosition": 16, "ridePosition": 17, "tomPosition": 18 },
  "notes": [
    { "note": 38, "role": "snare", "articulation": "center", "follow": "position" },
    { "note": 43, "role": "toms", "nth": 2, "articulation": "center", "follow": "position" },
    { "note": 46, "role": "hihat", "articulation": "open", "follow": "pedal" }
  ]
}
```

| Field | Meaning |
|---|---|
| `controllers` | Controller numbers, 0-127, or -1 for off. `pedalInverted` is for pedals that send 127 when open. |
| `role` | A piece role (`kick`, `snare`, `hihat`, `tom`, `floor_tom`, `crash`, `ride`, `china`, `other`), or `toms` for any tom. |
| `nth` | Which piece of the role, from 0. Optional. |
| `articulation` | A standard articulation name (or any kit's own name). |
| `follow` | `pedal`, `position` or `none`. Optional. |
| `name` | Words to find the piece by, `|`-separated, best first (`"clap|snap"`). An `other` entry plays nothing if no piece matches; other roles fall back to `nth`. Optional. |
| `otherwise` | The articulation to play on the last piece of the role when the kit has fewer than `nth` asks for (General MIDI 59: the second ride, `otherwise` the ride's `edge`). Optional. |
| `exact` | Play only what the entry names: the nth piece itself (or the piece found by `name`) and the articulation or one of its aliases, else nothing. Lithium Standard's extra notes are exact. Optional. |

A saved map's id is `user:` plus its file name. The built-in maps are in
`assets/midi-maps/`; Kit default is General MIDI with `"useKitMap": true`
(the kit's own map first, General MIDI on the free notes).

## How it reaches the audio thread

A map is resolved against the loaded kit into a `NoteTable`: 128 entries of
articulation and follow mode, plus the controller numbers. It is plain data
(about 530 bytes), so the message thread copies it into a lock-free
single-producer queue, and the audio thread copies it out into the kit at the
start of a block: no allocation, no locks and no shared ownership. Each table
carries the serial of the kit it was made for. A table for a kit that is still
waiting to be swapped in is held until that kit plays; one for an older kit is
dropped. When a new kit arrives, the processor resolves the current map for it
and sends it again, so a map changed during a kit load is never lost.

## Tracking down unexpected hits

Start Lithium with `LITHIUM_HIT_LOG` set to a file name and it appends a line
for every hit and where it came from (a MIDI note and what it played, a click,
an on-screen key, a groove), every MIDI controller, any sound that starts
with no hit behind it, and any audio block that came late (the audio device
ran short):

```
LITHIUM_HIT_LOG=~/lithium-hits.log "Lithium Drums"
```

The audio thread only queues these; the processor's timer writes them, five
times a second.
