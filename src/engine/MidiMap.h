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
    one CC for the snare, one for the ride's bow and one shared by the toms. */
enum class PositionSource : std::uint8_t { snare, ride, toms };
inline constexpr int numPositionSources = 3;

constexpr std::optional<PositionSource> positionSourceFor (PieceRole role) noexcept
{
    switch (role)
    {
        case PieceRole::snare:    return PositionSource::snare;
        case PieceRole::ride:     return PositionSource::ride;
        case PieceRole::tom:
        case PieceRole::floorTom: return PositionSource::toms;
        case PieceRole::kick:
        case PieceRole::hihat:
        case PieceRole::crash:
        case PieceRole::china:
        case PieceRole::other:    break;
    }

    return std::nullopt;
}

/** The controller numbers a map listens to. -1 = off. */
struct ControllerSettings
{
    int hihatPedal = 4;
    bool pedalInverted = false;    // Roland and most others send 0 = open, 127 = closed
    std::array<int, numPositionSources> position { 16, 17, 18 };

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

    /** Lithium Standard, Kit default, General MIDI, Roland, Alesis: the maps
        that ship with the plugin, in menu order. */
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
