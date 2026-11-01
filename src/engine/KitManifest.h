#pragma once

#include "Articulations.h"
#include "Kit.h"
#include "MidiMap.h"

#include <juce_core/juce_core.h>

#include <array>
#include <optional>
#include <vector>

// The parsed contents of a kit's kit.json: metadata, mics, pieces, velocity
// zones, round-robins and file paths, but no audio. The format is documented
// in docs/kit-format.md.
//
// A kit preset's kit.json only names a base kit and the changes it makes; it
// parses into the same structure, with the base kit's hits and the preset's
// choices applied, so nothing after parsing needs to know about presets.
//
// Parsing happens on the loader thread (or the message thread, for kit lists).
// The result is plain data: it is turned into a playable Kit (with decoded
// audio) by KitLoader.

namespace lithium
{

inline constexpr int kitFormatVersion = 2;
inline constexpr int maxRoundRobins = 1024;

/** Where one mic's audio for one hit comes from. */
struct MicSource
{
    int mic = 0;                           // index into KitManifest::mics
    juce::String file;                     // relative to the kit's audio folder
    std::array<int, 2> fileChannels {};    // 1-based channels in the file; [1] is 0 for mono mics
    std::int64_t start = 0;                // frames skipped at the start of the file
};

struct HitManifest
{
    std::vector<MicSource> sources;        // mics without a recording are simply absent
};

struct ZoneManifest
{
    int velocityLow = 1, velocityHigh = 127;
    float gainDb = 0.0f;
    float velocityRangeDb = 6.0f;
    std::vector<HitManifest> roundRobins;
};

struct ArticulationManifest
{
    juce::String id, name;
    juce::String chokeGroup;               // the group this articulation belongs to (can be choked)
    juce::StringArray chokes;              // groups this articulation chokes when it plays
    std::vector<ZoneManifest> zones;       // sorted by velocity

    // Steps for continuous control, where the kit sets them. Without them,
    // the standard articulation names decide (see Articulations.h).
    std::optional<float> openness;         // hi-hats: 0 = closed .. 1 = fully open
    std::optional<HatStroke> stroke;       // hi-hats: "tip" (bow) or "shank" (edge)
    std::optional<float> position;         // strike position: 0 = centre (or near the bell) .. 1 = edge
};

struct PieceManifest
{
    juce::String id, name;
    PieceRole role = PieceRole::other;
    int chokeNote = -1;                    // -1 = none given (cymbals then get one automatically)
    float tuneSemitones = 0.0f;            // the kit's own tuning, under the Tune parameter
    float volumeDb = 0.0f;                 // the kit's own level, under the Volume parameter
    float decay = 1.0f;                    // 0..1, the kit's own decay, scaling the Decay parameter
    std::vector<ArticulationManifest> articulations;
};

struct MicManifest
{
    juce::String id, name;
    int channels = 1;
    MicRole role = MicRole::close;
    float volumeDb = 0.0f, pan = 0.0f;
    bool invertPolarity = false;
};

struct KitManifest
{
    juce::File folder;                     // the kit (or preset) folder
    juce::File audioFolder;                // where the audio paths point: the kit folder, or a preset's base kit
    juce::String id;
    bool isPreset = false;
    juce::StringArray tags;
    KitCredits credits;
    double sampleRate = 0.0;
    std::vector<MicManifest> mics;
    std::vector<PieceManifest> pieces;
    std::vector<MidiMapEntry> midiMap;     // empty = use General MIDI
    juce::StringArray warnings;            // non-fatal problems found while parsing

    /** Reads and validates folder/kit.json. On failure, the Result says why.
        A preset's base kit is looked up by id next to the preset first, then
        anywhere in the kits folder (see findKitById). */
    static juce::Result parse (const juce::File& folder, KitManifest& result);

    /** Same, from an in-memory string (used by the tests). */
    static juce::Result parse (const juce::String& json, const juce::File& folder, KitManifest& result);

    int findMic (const juce::String& micId) const;
    int findPiece (const juce::String& pieceId) const;
};

/** What kit menus and the kit browser show, read without loading any audio. */
struct KitSummary
{
    KitCredits credits;
    juce::String id;
    bool isPreset = false;
    juce::StringArray tags;
    int numPieces = 0, numMics = 0;
    std::int64_t ramBytes = 0;             // from the importer's "stats"; 0 = not known
};

/** Reads a kit folder's summary. Returns false if the folder doesn't hold a
    readable kit (or a preset whose base kit is missing). */
bool readKitSummary (const juce::File& folder, KitSummary& result);

/** Reads just the credits from a kit folder's kit.json. */
bool readKitCredits (const juce::File& folder, KitCredits& result);

/** The id a kit folder's kit.json declares, or one made from its folder
    name if it declares none. Empty if the folder holds no kit. */
juce::String readKitId (const juce::File& folder);

/** Finds an installed kit by id, looking first in the folder that holds
    `near` (a kit's own content pack), then everywhere in the kits folder
    above it: kits directly inside it and kits inside its content packs. */
juce::File findKitById (const juce::String& id, const juce::File& near);

/** The id used for a kit whose kit.json doesn't declare one. */
juce::String defaultKitId (const juce::File& folder);

} // namespace lithium
