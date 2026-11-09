# LithiumPacks: downloading kits and grooves from the app

Lithium Drums knows where each of its free libraries can be downloaded. A
library that isn't installed shows up greyed out: in the kit browser (its kits
as dashed cards, its pack in the list with a ↓), at the bottom of the kit menu,
and on the Grooves tab when no grooves are installed. Download fetches it from
its authors' site, unpacks it, converts it with the same importers
`LithiumImport` uses, and deletes the downloads again. Cancel stops it; trying
again carries on an interrupted download where it stopped.

The same thing works from a terminal:

```
LithiumImport --list-packs
LithiumImport --install drumgizmo-drskit      # or --install all
```

## The catalog

`assets/packs/catalog.json` is built into the plugin. Each pack has its id
(the id of the content pack the import writes, which is how the app knows it's
installed), its name, author, licence and description, the kits and kit presets it
becomes (for the greyed cards), its downloads, and how much it takes on disk
unpacked:

```json
{
  "id": "drumgizmo-crocellkit",
  "name": "CrocellKit",
  "type": "kits",
  "license": { "name": "CC BY 4.0", "url": "https://creativecommons.org/licenses/by/4.0/" },
  "kits": ["CrocellKit Full"],
  "presets": ["CrocellKit", "CrocellKit Small", "CrocellKit Tiny"],
  "downloads": [ { "url": "https://drumgizmo.org/kits/CrocellKit/CrocellKit1_1.zip", "size": 5646502341 } ],
  "unpackedSize": 9016575637,
  "importFrom": "CrocellKit"
}
```

A download's `size` is checked when it finishes; 0 means the server doesn't
say (GitHub's zips of a repository), in which case `approximateSize` is used
for the progress bar and the disk space check. `file` names the saved file
where the URL doesn't end in one. `importFrom` is the folder inside the
unpacked archives to import; without it, the importer starts at the top,
stepping into an archive's single top folder. A pack with several downloads
(the AVL Drumkits come as five zips and a licence PDF) has them all unpacked
into one folder.

## What happens on Download

The installer runs on a thread of its own, one per process, so every plugin
instance shows the same progress, and an install carries on when the editor
is closed.

1. **Space.** It checks there's room for the downloads still to come plus the
   unpacked files, and stops with a message if not.
2. **Download**, into `.downloads/<pack id>/` in the kits folder, as a `.part`
   file renamed when complete. An interrupted download is resumed with an HTTP
   range request where the server allows it.
3. **Unpack** zip (including Zip64, which the larger kits need), tar and 7z,
   with every zip entry's checksum verified. Entries that would land outside
   the folder (`..`, absolute paths) and links are skipped. A 7z holding one
   LZMA or LZMA2 stream, like the Salamander Drumkit's `.tar.7z`, is decoded as
   it's read, straight into the tar reader, so it never sits in memory whole.
4. **Import** with the DrumGizmo, SFZ or Groove MIDI Dataset importer. The
   pack replaces one already installed.
5. **Clean up.** The imported audio is hard-linked from the unpacked files
   (they're on the same drive, being inside the kits folder), so deleting them
   frees their space while the kits keep their audio.

A damaged download is deleted so the next try fetches it afresh. A failed or
cancelled install keeps what it downloaded, so trying again doesn't start
over. Two Lithium Drums processes never install the same pack at once.

## Dependencies

On Linux, JUCE downloads over HTTPS through libcurl
(`libcurl4-openssl-dev`); Windows and macOS use their own networking. 7z
archives are read with the LZMA SDK, the public-domain C part of 7-Zip,
fetched by CMake like JUCE.
