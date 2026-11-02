#pragma once

#include "Importer.h"

#include <array>

// The importers' shared output side. An importer describes what it found as
// a PackModel (kits, presets, licence), and writePack() turns that into a
// content pack in the kits folder:
//
//   <kits folder>/<pack name>/pack.json        the pack's name and licence
//   <kits folder>/<pack name>/LICENSE.txt      attribution, licence, what was changed
//   <kits folder>/<pack name>/Original ...     the library's own licence notice
//   <kits folder>/<pack name>/<kit>/kit.json   each kit, with its audio in audio/
//   <kits folder>/<pack name>/<preset>/kit.json
//
// Each kit folder also gets its own LICENSE.txt and original notice, so a kit
// copied out of its pack still carries its attribution.

namespace lithium::importer
{

/** One kit, as an importer found it. File paths are relative to the
    pack's source root; mics are referred to by index. */
struct KitModel
{
    struct Mic
    {
        juce::String id, name, role { "close" };
        int channels = 1;
        double volumeDb = 0.0, pan = 0.0;
        bool invertPolarity = false;
    };

    /** One audio file of a hit, and which of its channels feed which mics. */
    struct Part
    {
        juce::String file;
        std::vector<std::pair<int, std::array<int, 2>>> channels;   // mic -> 1-based file channels ([1] = 0: one channel)
        std::int64_t start = 0;                                     // frames to skip at the start of the file
    };

    struct Hit           { std::vector<Part> parts; };

    struct Zone
    {
        int velocityLow = 1, velocityHigh = 127;
        double gainDb = 0.0, velocityRangeDb = 0.0;
        std::vector<Hit> hits;
    };

    struct Articulation
    {
        juce::String id, name, chokeGroup;
        juce::StringArray chokes;
        std::vector<Zone> zones;
    };

    struct Piece
    {
        juce::String id, name, role;
        double tune = 0.0, volumeDb = 0.0;
        std::vector<Articulation> articulations;
    };

    struct MidiEntry
    {
        int note = 0;
        juce::String piece, articulation;
    };

    juce::String id, name, description;
    juce::StringArray tags;
    std::vector<Mic> mics;
    std::vector<Piece> pieces;
    std::vector<MidiEntry> midiMap;
    juce::var importedFrom;        // recorded as is in kit.json
    juce::StringArray warnings;

    int findMic (const juce::String& micId) const;
    const Piece* findPiece (const juce::String& pieceId) const;
};

/** A kit preset: a kit from the same pack, plus changes. The JSON parts are
    written as they are; their format is documented in docs/kit-format.md. */
struct PresetModel
{
    juce::String id, name, description, baseId;
    juce::StringArray tags;
    juce::var pieces;              // array of piece choices, or void for all of the base kit's pieces
    juce::var mics;                // object of mic changes, or void
    juce::var midiMap;             // array, or void to follow the base kit's map
};

struct PackModel
{
    juce::File sourceRoot;         // audio paths in the kits are relative to this
    juce::String id, name, author, sourceUrl, description, credits;
    juce::String licenseName, licenseUrl;
    bool shareAlike = false;
    juce::StringArray licenseFiles;   // the library's own notices, relative to sourceRoot
    juce::String conversionNote;      // what the conversion changed, for LICENSE.txt
    juce::String licenseNote;         // terms the library adds to its licence, quoted in LICENSE.txt
    juce::String sourceFormat;        // "DrumGizmo", "SFZ"

    std::vector<KitModel> kits;
    std::vector<PresetModel> presets;
    juce::StringArray warnings;
};

/** Writes a pack into options.kitsFolder. Kits are written before presets,
    and each kit.json last of all in its folder, so an interrupted import
    never leaves a kit that looks complete. */
juce::Result writePack (const PackModel&, const Options&, std::vector<ImportedKit>& results);

//==============================================================================
// Helpers shared by the importers.

juce::String toId (const juce::String& name);

/** A piece role guessed from an instrument or sample name. */
juce::String roleForName (const juce::String& name);

/** Recognises the usual Creative Commons licences in a licence notice:
    { name, url }, or empty strings. */
std::pair<juce::String, juce::String> detectLicense (const juce::String& text);

/** The first built-in mapping (assets/import-maps) whose "matches" list
    contains one of the candidates (case-insensitive), with its "format"
    equal to the given one. Void if none. */
juce::var findBuiltInMapping (const juce::StringArray& candidates, const juce::String& format);

/** Every built-in mapping of a format. */
std::vector<juce::var> builtInMappings (const juce::String& format);

/** The presets a mapping file lists ("presets"), for those whose base kit
    is among the kits being imported. */
std::vector<PresetModel> presetsFromMapping (const juce::var& mapping, const std::vector<KitModel>& kits);

juce::StringArray toStringArray (const juce::var&);
juce::var object();
void put (juce::var& obj, const juce::Identifier& key, const juce::var& value);

} // namespace lithium::importer
