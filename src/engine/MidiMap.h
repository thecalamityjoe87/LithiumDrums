#pragma once

#include "KitSchema.h"

#include <juce_core/juce_core.h>

#include <array>
#include <optional>
#include <vector>

// Note-to-articulation maps, loaded from JSON so new maps are data rather
// than code (the format is in docs/midi.md). There are two kinds:
//
//  - A kit's own default map (in its kit.json) names the kit's piece IDs
//    directly: MidiMapEntry.
//  - Kit-independent maps (General MIDI, Roland V-Drums, Alesis, and maps
//    the user makes with MIDI learn) can't know a kit's piece IDs, so they
//    name a role and which piece of that role: "the second rack tom", "the
//    first ride, bell". MidiMap holds those.
//
// Either way the map is resolved against a kit (see MidiMapping.h) into a
// NoteTable: a flat 128-entry lookup table the audio thread can use directly.

namespace lithium
{

/** How a note's articulation can follow a controller, for e-kits. A note
    only follows once that controller has actually been received, so a map
    with follow set still plays its fixed articulations from a plain MIDI
    file. */
enum class Follow : std::uint8_t
{
    none,
    pedal,       // a hi-hat hit picks its openness from the hi-hat pedal (CC4)
    position     // a drum or ride hit picks its strike position from a position CC
};

/** Which strike position controller a piece listens to. Roland modules send
    one CC for the snare, one for the ride's bow, one shared by the toms, and
    one for the hi-hat's bow (with a digital hi-hat, the VH-14D). The order
    is the saved order; the UI shows them snare, hi-hat, ride, toms. */
enum class PositionSource : std::uint8_t { snare, ride, toms, hihat };
inline constexpr int numPositionSources = 4;

/** The order the UI shows the positional pads in. */
inline constexpr std::array<PositionSource, numPositionSources> positionDisplayOrder {
    PositionSource::snare, PositionSource::hihat, PositionSource::ride, PositionSource::toms };

constexpr std::optional<PositionSource> positionSourceFor (PieceRole role) noexcept
{
    switch (role)
    {
        case PieceRole::snare:    return PositionSource::snare;
        case PieceRole::ride:     return PositionSource::ride;
        case PieceRole::tom:
        case PieceRole::floorTom: return PositionSource::toms;
        case PieceRole::hihat:    return PositionSource::hihat;
        case PieceRole::kick:
        case PieceRole::crash:
        case PieceRole::china:
        case PieceRole::other:    break;
    }

    return std::nullopt;
}

/** When to play a pedal chick from the hi-hat pedal's controller alone. */
enum class ChickMode : std::uint8_t
{
    off,
    automatic,   // until the module sends a pedal note of its own (most do: note 44)
    always
};

/** How the e-kit's controllers are read: the hi-hat pedal (openness), and
    the positional pads (strike position). These describe the player's
    hardware, not the sound, so they're kept in the MIDI map (saved with
    each map, and with the project), never in a preset. Plain data, copied
    to the audio thread inside a NoteTable. Controller numbers: -1 = off;
    MIDI channels 1-16, 0 = any. */
struct ControllerSettings
{
    static constexpr int numThresholds = 4;

    // The hi-hat pedal.
    int hihatPedal = 4;
    int hihatChannel = 0;
    bool pedalInverted = false;    // Roland and most others send 0 = open, 127 = closed
    int pedalMin = 0, pedalMax = 127;   // calibration: the lowest and highest values the pedal really sends
    float pedalCurve = 0.0f;       // -1..1: below 0 the hats open later in the pedal's travel, above 0 sooner

    // Where (in pedal openness, 0..1) the sound moves up a step: from closed
    // to a quarter open, a quarter to half, half to three quarters, and three
    // quarters to fully open. A kit with fewer steps is spread over the same
    // curve (see DrumEngine::hatLadderValue).
    std::array<float, numThresholds> hatThresholds { 0.125f, 0.375f, 0.625f, 0.875f };

    ChickMode chickFromPedal = ChickMode::automatic;
    float chickSensitivity = 0.5f; // 0..1: how slow a close still plays a chick
    bool footSplash = true;        // closing and quickly reopening plays the kit's foot splash

    // The positional pads, by PositionSource (snare, ride, toms).
    std::array<int, numPositionSources> position { 16, 17, 18, 19 };
    std::array<int, numPositionSources> positionChannel {};
    std::array<bool, numPositionSources> positionEnabled { true, true, true, true };
    std::array<int, numPositionSources> positionMin {}, positionMax { 127, 127, 127, 127 };
    std::array<float, numPositionSources> positionCurve {};

    // For kits without positional recordings: how much the tone changes at
    // the edge (drums), or how much of the edge hit is blended in (ride).
    std::array<float, numPositionSources> positionEmulation { 0.5f, 0.5f, 0.5f, 0.5f };

    bool operator== (const ControllerSettings&) const = default;
};

/** One entry of a kit's own map (kit.json "midiMap"). */
struct MidiMapEntry
{
    int note = 0;
    juce::String piece;
    juce::String articulation;    // empty = the piece's first articulation
};

/** One entry of a kit-independent map. */
struct RoleMapEntry
{
    int note = 0;
    PieceRole role = PieceRole::other;
    bool anyTom = false;          // role "toms": the nth of all toms, rack toms first, then floor toms
    int nth = 0;                  // which piece of the role; past the last one means the last one
    juce::String articulation;    // a standard articulation id (see Articulations.h)
    Follow follow = Follow::none;

    // Words to look for in the piece's id or name, "|"-separated, best
    // first: General MIDI's cowbell is the "other" piece called cowbell. The
    // first piece that matches plays; with no match, a role of "other" plays
    // nothing and other roles fall back to nth.
    juce::String name;

    // The articulation to play when the kit has fewer pieces of the role
    // than nth asks for, on the last of them. General MIDI's 59 is the
    // second ride, or on a one-ride kit, that ride's crash. Empty = the
    // entry's own articulation.
    juce::String otherwise;

    // Plays only what it names: the nth piece itself (not the last one when
    // the kit has fewer), found by name if it has one, and the articulation
    // or one of its aliases (not the piece's first articulation). A kit
    // without that hit leaves the note silent, so in a map like Lithium
    // Standard each note always means the same kind of hit.
    bool exact = false;

    bool operator== (const RoleMapEntry&) const = default;
};

struct MidiMap
{
    juce::String id, name, description;

    /** "Kit default": play the kit's own map, and these entries (General
        MIDI) on the notes it leaves free. */
    bool useKitMap = false;

    ControllerSettings controllers;
    std::vector<RoleMapEntry> entries;

    static juce::Result parse (const juce::String& json, MidiMap& result);
    juce::String toJson() const;

    const RoleMapEntry* find (int note) const;

    /** Adds an entry, replacing whatever the note played before. */
    void assign (const RoleMapEntry&);
    void remove (int note);

    bool sameMapping (const MidiMap& other) const
    {
        return useKitMap == other.useKitMap && controllers == other.controllers && entries == other.entries;
    }

    // --- The built-in maps (assets/midi-maps). ------------------------------
    static MidiMap lithiumStandard();
    static MidiMap generalMidi();
    static MidiMap kitDefault();
    static MidiMap rolandVDrums();
    static MidiMap alesis();

    /** Lithium Standard, Kit default, General MIDI, Roland, Alesis, then 2BOX,
        EFNOTE, Lemon, Millenium and Yamaha: the maps that ship with the plugin, in
        menu order. */
    static std::vector<MidiMap> factoryMaps();
    static MidiMap factoryMap (const juce::String& id);   // lithiumStandard() if the id is unknown

    static constexpr const char* kitDefaultId = "kit-default";
};

//==============================================================================
/** Where one MIDI note goes, resolved against one kit. */
struct NoteTarget
{
    std::int16_t articulation = -1;          // index into the kit's articulations, -1 = unmapped
    Follow follow = Follow::none;            // only set where the kit has steps to follow
    PositionSource source = PositionSource::snare;   // for Follow::position
    bool spare = false;                      // given automatically, to something the map doesn't reach
};

/** A MIDI map resolved against one kit, for the audio thread: plain data,
    trivially copyable, so it can be passed through a lock-free queue by
    value and copied in the audio callback without allocating. */
struct NoteTable
{
    std::array<NoteTarget, 128> targets {};
    ControllerSettings controllers;
};

static_assert (std::is_trivially_copyable_v<NoteTable>, "NoteTable is copied on the audio thread");

} // namespace lithium
