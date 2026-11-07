#pragma once

#include "Articulations.h"
#include "InMemorySampleSource.h"
#include "KitSchema.h"
#include "KitSlots.h"
#include "MidiMap.h"

#include <juce_core/juce_core.h>

#include <array>
#include <memory>
#include <vector>

// A loaded, playable kit.
//
// KitLoader builds a Kit on a background thread, then hands ownership to the
// audio thread (see KitHandoff). From then on only the audio thread touches it,
// until it is handed back for deletion. Everything is flattened into vectors
// indexed by int, so lookups on the audio thread are plain array accesses
// with no strings, maps or allocation.

namespace lithium
{

struct MicChannel
{
    int numChannels = 1;
    MicRole role = MicRole::close;
    int busChannel = 0;          // first channel of this mic in the engine's mic bus
    float defaultGain = 1.0f;    // the kit's default fader (linear)
    float defaultPan = 0.0f;     // the kit's default pan, -1..1

    // Close mics feed the channel of the drum being hit; overhead and room
    // mics are channels of their own. A kit with no close mics at all (one
    // stereo pair, say) sends every mic to the drum channels instead, so
    // each drum still has a channel to work on.
    bool direct = true;
};

struct Zone
{
    int velocityLow, velocityHigh;
    float gain;                  // linear, from the manifest's gainDb
    float velocityRangeDb;       // how much quieter the zone's lowest velocity plays than its highest
    int firstHit, numHits;       // the zone's round-robins, in Kit::hitSamples
};

/** What a hit's chokes do to the ringing hits of its own piece. */
enum class SelfChoke : std::uint8_t
{
    all,         // drums: as the kit says
    none,        // cymbals: the new hit adds to the ringing one (only grabs choke)
    moreOpen     // hi-hats: closing the hats stops the hits that rang more open
};

struct Articulation
{
    int piece;
    int firstZone, numZones;     // in Kit::zones, sorted by velocity
    std::uint32_t chokeGroupMask;  // groups this articulation belongs to
    std::uint32_t chokesMask;      // groups it chokes when played
    SelfChoke selfChoke;         // what its chokes do to its own piece's ringing hits (see KitLoader)
    float hatLevel;              // hi-hats: how open it leaves the hats (articulations::hatChokeLevel)
    float openness;              // hi-hats: its step on the openness ladder, -1 = not on it
    HatStroke stroke;            // hi-hats: which openness ladder it's on
};

/** The steps a continuous controller moves between: a hi-hat's openness
    levels, or a drum's strike positions. Sorted by value, at most one
    articulation per value.

    Each step plays alone from low to high; between one step's high and the
    next one's low, the two crossfade. */
struct Ladder
{
    static constexpr int maxSteps = 8;

    // Between two steps given as points, the middle of the gap (this
    // fraction of it) crossfades from one to the other.
    static constexpr float crossfadeWidth = 0.3f;

    int numSteps = 0;
    std::array<int, maxSteps> articulation {};   // index into Kit::articulations
    std::array<float, maxSteps> value {};        // 0..1
    std::array<float, maxSteps> low {}, high {};
};

struct PieceData
{
    static constexpr std::array<float, maxMics> unityGains() noexcept
    {
        std::array<float, maxMics> gains {};
        gains.fill (1.0f);
        return gains;
    }

    PieceRole role = PieceRole::other;
    int firstArticulation = 0, numArticulations = 0;
    int defaultChokeNote = -1;

    // The kit's (or kit preset's) own settings for the piece. The piece
    // parameters work on top of these, so their neutral values mean "as the
    // kit sets it up".
    float defaultGain = 1.0f;    // linear
    float defaultTune = 0.0f;    // semitones
    float defaultDecay = 1.0f;   // 0..1, multiplies the Decay parameter

    // A piece swapped in from another kit plays its own recordings, which
    // may be at another sample rate, through this kit's mics: micGain puts
    // each mic back at the level (and polarity) its own kit gave it.
    double sampleRate = 44100.0;
    std::array<float, maxMics> micGain = unityGains();

    // What the hi-hat pedal and the position controllers move between.
    std::array<Ladder, 2> hatLadders {};   // [0] tip (bow) hits, [1] shank (edge) hits
    Ladder positionLadder {};

    // Hits the pedal plays by itself (a chick, a foot splash), and the
    // ride's edge, which its bow blends towards on positional pads. Indices
    // into Kit::articulations, -1 if the piece has none.
    int pedalArticulation = -1, splashArticulation = -1, edgeArticulation = -1;

    const Ladder& hatLadder (HatStroke s) const noexcept   { return hatLadders[s == HatStroke::shank ? 1 : 0]; }
};

class Kit
{
public:
    juce::String name;
    juce::File folder;
    double sampleRate = 44100.0;
    std::uint64_t serial = 0;    // unique per loaded kit; identifies it in mic load messages

    int numMics = 0;
    std::array<MicChannel, maxMics> mics {};
    int numBusChannels = 0;

    std::vector<PieceData> pieces;               // at most maxPieces
    std::vector<Articulation> articulations;
    std::vector<Zone> zones;

    // Which articulation each MIDI note plays. Built by the loader from the
    // map chosen when the load was requested; when the map changes, the
    // processor sends a new table to the audio thread (see NoteTable).
    NoteTable notes;

    // One SampleId per hit per mic: hitSamples[hit * numMics + mic], with
    // invalidSampleId where that mic has no recording of the hit.
    std::vector<SampleId> hitSamples;
    // An InMemorySampleSource, or a StreamingSampleSource (which builds on it).
    // Voices use it only through the SampleSource interface.
    std::unique_ptr<InMemorySampleSource> samples;

    // When a kit is chosen from the kit menu, the mixer and piece parameters
    // are reset to neutral on the message thread after the kit arrives. Until
    // the processor confirms that has happened, the audio thread plays this
    // kit with neutral settings rather than whatever the previous kit had
    // (see LithiumDrumsProcessor). 0 = no reset pending.
    std::uint64_t resetToken = 0;

    int getNumHits() const noexcept                       { return numMics > 0 ? (int) hitSamples.size() / numMics : 0; }
    SampleId getSample (int hit, int mic) const noexcept  { return hitSamples[(size_t) (hit * numMics + mic)]; }

    // --- Round-robin state. Written by the audio thread only. ---------------
    //
    // Each zone plays its round-robins as a "shuffle bag": every round-robin
    // once, in random order, then a fresh shuffle. The first pick of a new bag
    // is never the last pick of the previous one, so a hit never repeats twice
    // in a row while still avoiding an audible fixed cycle.
    std::vector<std::uint16_t> rrOrder;      // per hit, a permutation within each zone
    std::vector<std::uint16_t> rrPosition;   // per zone, next index into its slice of rrOrder
    std::vector<std::uint16_t> rrLast;       // per zone, the round-robin played last

    static constexpr std::uint16_t noneYet = 0xffff;

    void initialiseRoundRobins()
    {
        rrOrder.assign ((size_t) getNumHits(), 0);
        rrPosition.assign (zones.size(), 0);
        rrLast.assign (zones.size(), noneYet);

        for (size_t z = 0; z < zones.size(); ++z)
        {
            const auto& zone = zones[z];

            for (int i = 0; i < zone.numHits; ++i)
                rrOrder[(size_t) (zone.firstHit + i)] = (std::uint16_t) i;

            rrPosition[z] = (std::uint16_t) zone.numHits;   // position == count means "bag empty"
        }
    }

    /** Returns the index of the next round-robin hit for a zone. */
    int nextRoundRobin (int zoneIndex, juce::Random& random) noexcept
    {
        const auto& zone = zones[(size_t) zoneIndex];
        const auto n = zone.numHits;
        auto* order = rrOrder.data() + zone.firstHit;
        auto& pos = rrPosition[(size_t) zoneIndex];
        auto& last = rrLast[(size_t) zoneIndex];

        if (n == 1)
            return zone.firstHit;

        if (pos >= n)
        {
            for (int i = n - 1; i > 0; --i)                       // Fisher-Yates shuffle
                std::swap (order[i], order[random.nextInt (i + 1)]);

            if (order[0] == last)                                  // no repeat across bags
                std::swap (order[0], order[1 + random.nextInt (n - 1)]);

            pos = 0;
        }

        last = order[pos++];
        return zone.firstHit + last;
    }
};

//==============================================================================
// What the UI needs to know about a kit. Lives on the message thread, and is
// kept separate from Kit so the UI never touches audio-thread data.

/** Attribution and licence, as CC BY requires them to be shown. For a kit
    preset these are the credits of the kit whose recordings it plays, with
    the preset's own name and description. */
struct KitCredits
{
    juce::String name, author, sourceUrl, licenseName, licenseUrl, credits, description;
    juce::File licenseFile;      // the licence text copied into the kit folder
    bool shareAlike = false;     // adaptations must carry the same licence (CC BY-SA)
    juce::String basedOn;        // presets: the name of the kit they're made from
};

struct KitZoneInfo
{
    int velocityLow = 1, velocityHigh = 127;
    int numRoundRobins = 1;
};

struct KitArticulationInfo
{
    juce::String id, name;
    std::vector<KitZoneInfo> zones;   // sorted by velocity
    ArticulationTraits traits;        // openness and strike position, if it's a step of either
};

struct KitPieceInfo
{
    juce::String id, name;
    PieceRole role = PieceRole::other;
    std::vector<KitArticulationInfo> articulations;
    int defaultChokeNote = -1;
    int firstArticulation = 0;        // index of the first one in the kit's (flattened) articulations
    bool hasDirectSignal = true;      // false if no mic that feeds the drum channels recorded it
    std::array<int, 2> hatSteps {};   // steps on the tip and shank openness ladders
    int positionSteps = 0;            // steps on the strike position ladder

    // Piece swapping (see PieceSwap.h). swappedFrom is the kit the piece's
    // recordings come from (empty for the kit's own); swapNotes says what
    // didn't come along (mics this kit has no match for), or why a swap
    // couldn't be made.
    juce::String swappedFrom, swappedPiece;   // the kit, and the piece's name there

    // The id of the kit whose recordings the piece plays: its own kit, a
    // preset's base kit, or the kit it was swapped in from. The UI shows that
    // kit's finish for it (see ui/KitArt.h).
    juce::String recordedIn;
    juce::StringArray swapNotes;
    int slot = -1;                    // its place on the Kit tab's slots (see KitSlots.h)

    /** Whether an articulation of this piece can follow the hi-hat pedal or a position controller. */
    bool canFollow (int articulation, Follow f) const noexcept
    {
        if (! juce::isPositiveAndBelow (articulation, (int) articulations.size()))
            return false;

        const auto& t = articulations[(size_t) articulation].traits;

        switch (f)
        {
            case Follow::pedal:    return t.openness >= 0.0f && hatSteps[t.stroke == HatStroke::shank ? 1 : 0] > 1;
            // Positional recordings to move between; or, without them, a hit
            // whose sound the engine changes with the position itself.
            case Follow::position: return positionSourceFor (role).has_value()
                                          && ((t.position >= 0.0f && positionSteps > 1) || t.emulatesPosition);
            case Follow::none:     break;
        }

        return false;
    }
};

struct KitMicInfo
{
    juce::String id, name;
    int numChannels = 1;
    MicRole role = MicRole::close;
    float defaultDb = 0.0f, defaultPan = 0.0f;
    bool direct = true;               // see MicChannel::direct
};

struct KitInfo
{
    KitCredits credits;
    juce::String id;             // the kit's id (see docs/kit-format.md), saved with projects
    juce::File folder;
    std::uint64_t serial = 0;    // the Kit's serial, for sending it a new NoteTable
    std::vector<MidiMapEntry> kitMidiMap;           // the kit's own default map (empty = it has none)
    bool isPreset = false;
    std::vector<KitPieceInfo> pieces;
    std::vector<KitMicInfo> mics;
    std::array<int, numStageSlots> slotPiece {};   // the piece shown in each stage slot, -1 for none
    kitslots::Owners slotOwners;                    // the kit's own piece in each Kit tab slot, before any swaps
    int bitsPerSample = 24;
    double sampleRate = 44100.0;
    std::size_t memoryBytes = 0;                    // with every mic loaded (for a streaming kit: preloads plus stream buffers)
    std::size_t fullMemoryBytes = 0;                // what the whole kit would take in RAM

    // Streaming: the first preloadFrames of each sample are in RAM, the rest
    // streams from the cache pack.
    bool streaming = false;
    int preloadFrames = 0;
    std::size_t streamBufferBytes = 0;
    juce::File streamCache;
    int numSampleFiles = 0;
    double loadSeconds = 0.0;
    juce::StringArray warnings;

    /** The stage slot a piece is drawn in, if it has one. */
    std::optional<StageSlot> slotOf (int piece) const noexcept
    {
        for (const auto& s : slotInfos)
            if (slotPiece[(size_t) index (s.slot)] == piece)
                return s.slot;

        return std::nullopt;
    }
};

/** Assigns pieces to stage slots by role: each slot takes the nth piece of
    its role, in kit order. Splashes (splash[p], if given) have the crash role
    but don't count as crashes, as in the MIDI maps: they go to the splash
    slot, and crashes never do. Pieces left over get no slot (they still play
    and still get a strip); slots left over are shown dimmed. */
inline std::array<int, numStageSlots> assignStageSlots (const std::vector<PieceRole>& roles, const std::vector<bool>& splash = {})
{
    std::array<int, numStageSlots> result;
    result.fill (-1);

    for (const auto& s : slotInfos)
    {
        int seen = 0;
        const auto isSplashSlot = s.slot == StageSlot::splash;

        for (size_t p = 0; p < roles.size(); ++p)
        {
            const auto isSplash = p < splash.size() && splash[p];

            if (s.role == PieceRole::crash && isSplash != isSplashSlot)
                continue;

            if (roles[p] == s.role && seen++ == s.nth)
            {
                result[(size_t) index (s.slot)] = (int) p;
                break;
            }
        }
    }

    return result;
}

} // namespace lithium
