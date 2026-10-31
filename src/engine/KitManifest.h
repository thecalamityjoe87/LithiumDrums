#pragma once

#include "Kit.h"
#include "MidiMap.h"

#include <juce_core/juce_core.h>

#include <array>
#include <vector>

// The parsed contents of a kit's kit.json: metadata, mics, pieces, velocity
// zones, round-robins and file paths, but no audio. The format is documented
// in docs/kit-format.md.
//
// Parsing happens on the loader thread. The result is plain data: it is turned
// into a playable Kit (with decoded audio) by KitLoader.

namespace lithium
{

inline constexpr int kitFormatVersion = 2;
inline constexpr int maxRoundRobins = 1024;

/** Where one mic's audio for one hit comes from. */
struct MicSource
{
    int mic = 0;                           // index into KitManifest::mics
    juce::String file;                     // relative to the kit folder
    std::array<int, 2> fileChannels {};    // 1-based channels in the file; [1] is 0 for mono mics
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
};

struct PieceManifest
{
    juce::String id, name;
    PieceRole role = PieceRole::other;
    int chokeNote = -1;                    // -1 = none given (cymbals then get one automatically)
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
    juce::File folder;
    KitCredits credits;
    double sampleRate = 0.0;
    std::vector<MicManifest> mics;
    std::vector<PieceManifest> pieces;
    std::vector<MidiMapEntry> midiMap;     // empty = use General MIDI
    juce::StringArray warnings;            // non-fatal problems found while parsing

    /** Reads and validates folder/kit.json. On failure, the Result says why. */
    static juce::Result parse (const juce::File& folder, KitManifest& result);

    /** Same, from an in-memory string (used by the tests). */
    static juce::Result parse (const juce::String& json, const juce::File& folder, KitManifest& result);

    int findMic (const juce::String& id) const;
    int findPiece (const juce::String& id) const;
};

/** Reads just the credits from a kit folder's kit.json, for kit lists and the
    About dialog. Returns false if the folder doesn't hold a readable kit. */
bool readKitCredits (const juce::File& folder, KitCredits& result);

} // namespace lithium
