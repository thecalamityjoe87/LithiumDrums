# Disk streaming

Big multi-mic kits don't fit comfortably in RAM. DRSKit takes 3.1 GB and
MuldjordKit 2.6 GB, even held as 24-bit integers. So Lithium Drums keeps only
the start of every sample in RAM (the *preload*) and streams the rest from
disk while it plays. A voice never waits for the disk: it starts from the
preload, and by the time it gets to the end of the preload, the rest has been
read.

## When a kit streams

The Settings panel (the gear in the header) has three modes:

- **Large kits** (the default) streams kits that would take more than 512 MB
  in RAM. DRSKit, MuldjordKit and Virtuosity stream. Salamander and the Basalt
  kits don't: they're small enough that streaming them would save little.
- **Every kit** streams everything.
- **Never** loads every kit fully into RAM, as Milestones 1 and 2 did.

The preload size is set per sample, per mic: 8192, 16384 (the default),
32768 or 65536 frames, which is 186 ms to 1.5 s at 44.1 kHz. A larger preload
uses more RAM and survives a slower disk. Changing either setting reloads the
kit, keeping the mixer as it is.

The environment variables `LITHIUM_STREAMING` (`auto`, `always` or `never`)
and `LITHIUM_CACHE_FOLDER` override the saved mode and cache folder.

## The stream cache

Kits keep their audio as the libraries ship it. That's a poor format to
stream from. A DrumGizmo hit is one WAV file with all 13 to 16 mics
interleaved as 32-bit floats, so reading one mic means reading all of them.
A FLAC file can't be read from an arbitrary frame without decoding from the
nearest seek point.

So the first time a kit streams, its audio is decoded once into a cache
*pack*:

```
<cache folder>/<audio folder name>-<hash>/
    index.json   which files are cached, and where
    audio.bin    every file's channels, one after another
```

In `audio.bin`, each channel of each file is one contiguous run of 16- or
24-bit little-endian integers, so reading frames *a* to *b* of one mic is a
single read with no decoding. Files of 16 bits or fewer stay 16-bit;
everything else, including 32-bit float, becomes 24-bit, which is what the
RAM copy holds as well. Streamed audio is therefore bit-identical to the same
kit loaded into RAM, and a test checks that.

A pack belongs to an audio folder rather than to a kit, so a kit and the
presets made from it share one. Loading DRSKit Brushes after DRSKit Full
caches nothing new. Each entry remembers its source file's size and
modification time. A file that changes is decoded again and appended. Nothing
is rewritten in place, and `index.json` is replaced in one atomic rename
after the new audio is on disk, so a crash or a cancelled load never leaves
an index pointing at half-written audio. Instances in other processes are
kept from appending at the same time by an inter-process lock.

The cache folder is a per-user cache location:

| OS      | Stream cache                                          |
|---------|-------------------------------------------------------|
| Linux   | `$XDG_CACHE_HOME/Lithium Drums/Stream Cache` (usually `~/.cache/...`) |
| macOS   | `~/Library/Caches/Lithium Drums/Stream Cache`         |
| Windows | `%LOCALAPPDATA%\Lithium Drums\Stream Cache`           |

The cache can be deleted at any time; kits rebuild it the next time they
stream. **Clear cache** in Settings deletes every pack except the ones being
streamed right now. If there isn't room on the disk for a kit's pack, the kit
loads into RAM instead and says why in the kit menu's tooltip.

Building a pack also speeds up loading. A cached kit doesn't open its source
files at all, not even to read their headers: DRSKit Full loads in 0.2 s
from the cache, against 2.7 s decoding it into RAM.

## How streaming works

`StreamingSampleSource` implements the same `SampleSource` interface as the
in-memory source, so the voices didn't change. It builds on the in-memory
source: the preloads live in the same per-mic stores, so unloading a muted
mic works as before. A muted mic's preload is unloaded after a couple of
seconds and read back from the cache (no decoding) when the mic is unmuted.

Each mic has one *stream slot* per voice, each with a ring buffer of 12288
frames (three 4096-frame chunks) per channel. Everything is allocated when the
kit loads: DRSKit's slots take 39 MB and MuldjordKit's 45 MB.

One disk thread per process (`DiskStreamer`) serves every streaming kit of
every plugin instance. It loops over the slots, reads the next chunk for every
slot whose ring has room (the voices with the least audio buffered go first),
and sleeps for 2 ms when there is nothing to do. The audio thread never wakes
it, because signalling another thread isn't real-time safe on every platform.
Polling every 2 ms is far quicker than any preload runs out.

The audio thread and the disk thread share each slot without locks:

- **Opening and closing.** When a voice starts a hit, the audio thread claims
  a free slot (from a per-mic free list that only it touches) and writes the
  sample and start position under a *sequence lock*. The slot's generation
  number is odd while it's being changed and even once it's done. The disk
  thread reads the fields, then checks the generation again. If it changed,
  the disk thread skips the slot until its next pass. The audio thread never
  waits.
- **Filling.** The disk thread writes audio only into the part of the ring
  the voice has finished with (behind `readFrame`, which the audio thread
  publishes after each read). Then it publishes `filled`: the generation it
  read for, packed with the frame up to which the ring is valid. The
  release/acquire ordering makes sure the audio thread never sees `filled`
  before the audio it describes.
- **Reuse.** A slot is free again as soon as the voice closes it, even if the
  disk thread is halfway through a read for it. The audio thread only trusts
  `filled` when its generation matches the slot's current one, so audio left
  over from a slot's previous hit never plays.

If the disk does fall behind, the voice plays silence for the missing frames
(a *dropout*) and carries on in step with its other mics. The disk thread
then skips ahead to where the voice is. The header's DISK readout turns red
for a few seconds after a dropout, and its tooltip counts them.

## Measurements

These were measured on Linux with an NVMe SSD and 14 cores, using the default
16384-frame preload unless noted. "Stress" plays a dense groove in real time
for 30 s: 16ths at 160 BPM, every hit at velocity 90 or more, a crash or the
china on every beat, and a tom fill every other bar. That keeps 57 or 58
voices and 640 to 740 streams going. "Cold" means the cache file was evicted
from the OS page cache first, so every read went to the disk.

| Kit | Fully in RAM | Streaming (preloads + buffers) | Cache on disk | Stress, cold cache |
|---|---|---|---|---|
| DRSKit Full | 3077 MB | 357 MB (318 + 39) | 3.0 GB | 73 MB/s, 0 dropouts |
| DRSKit Full, 8192 preload | | 204 MB | | 81 MB/s, 0 dropouts |
| MuldjordKit | 2599 MB | 257 MB (212 + 45) | 2.7 GB | 91 MB/s, 0 dropouts |
| MuldjordKit, 8192 preload | | 151 MB | | 100 MB/s, 0 dropouts |
| Virtuosity Drums | 1999 MB | 221 MB (194 + 27) | 2.0 GB | 52 MB/s, 0 dropouts (warm) |

MuldjordKit's pack is a little bigger than its RAM footprint because the
cache keeps every channel of the source files, including one the kit doesn't
use. Building a pack the first time took 2.5 to 3 s on this machine (the
DrumGizmo WAVs decode quickly). Streaming added no measurable engine load:
the stress run cost 14.5% of one core streaming, against 14.1% from RAM.

To measure a kit yourself:

```
build/release/LithiumDrumsTests --measure-kit "<kit folder>" --stream [--preload 8192] --stress 30 [--cold]
```
