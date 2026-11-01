#pragma once

#include "KitSchema.h"

#include <array>
#include <optional>
#include <string_view>

// The standard articulation vocabulary.
//
// Kits name their articulations freely ("rim" in one kit is "rimshot" in
// another, a ride's bow is "tip" in a third), but kit-independent MIDI maps
// (General MIDI, Roland, Alesis) have to ask for them by some standard name.
// This file says, per role, which of a kit's articulations stand in for each
// standard name, and which hi-hat articulations are steps of openness and
// which drum articulations are strike positions, for continuous pedal (CC4)
// and positional control. Everything is constexpr, so any thread can use it.

namespace lithium
{

/** Which part of the stick hits the hi-hat. Bow hits (Roland's "bow") use
    the tip, edge hits the shank. Both have their own ladder of openness. */
enum class HatStroke : std::uint8_t { none, tip, shank };

/** What an articulation is, beyond its name: a step on the hi-hat's
    openness ladder (0 = closed, 1 = fully open) and/or a strike position on
    a drum head or cymbal bow (0 = centre, 1 = edge). -1 = not a step. */
struct ArticulationTraits
{
    float openness = -1.0f;
    HatStroke stroke = HatStroke::none;
    float position = -1.0f;
};

namespace articulations
{
    inline constexpr int maxFallbacks = 5;

    struct Alias
    {
        PieceRole role;
        std::string_view id;                                  // the standard name a map asks for
        std::array<std::string_view, maxFallbacks> matches;   // the kit articulation ids that can play it, best first
    };

    // A map that asks for an id not listed here gets that exact id, or the
    // piece's first articulation.
    inline constexpr Alias aliases[] {
        { PieceRole::kick,     "hit",          { "hit", "center" } },

        { PieceRole::snare,    "center",       { "center", "hit", "head" } },
        { PieceRole::snare,    "off_center",   { "off_center", "edge", "center" } },
        { PieceRole::snare,    "edge",         { "edge", "off_center", "center" } },
        { PieceRole::snare,    "rimshot",      { "rimshot", "rim", "stick_shot" } },
        { PieceRole::snare,    "rim",          { "rim", "rimshot", "stick_shot" } },
        { PieceRole::snare,    "sidestick",    { "sidestick", "cross_stick", "stick", "rim_click" } },
        { PieceRole::snare,    "cross_stick",  { "cross_stick", "sidestick", "stick", "rim_click" } },
        { PieceRole::snare,    "brush",        { "brush", "center" } },

        { PieceRole::tom,      "center",       { "center", "hit" } },
        { PieceRole::tom,      "off_center",   { "off_center", "edge", "center" } },
        { PieceRole::tom,      "edge",         { "edge", "off_center", "center" } },
        { PieceRole::tom,      "rimshot",      { "rimshot", "rim", "center" } },
        { PieceRole::tom,      "rim",          { "rim", "rimshot", "center" } },
        { PieceRole::floorTom, "center",       { "center", "hit" } },
        { PieceRole::floorTom, "off_center",   { "off_center", "edge", "center" } },
        { PieceRole::floorTom, "edge",         { "edge", "off_center", "center" } },
        { PieceRole::floorTom, "rimshot",      { "rimshot", "rim", "center" } },
        { PieceRole::floorTom, "rim",          { "rim", "rimshot", "center" } },

        { PieceRole::hihat,    "closed",       { "closed", "tight", "closed_tip" } },
        { PieceRole::hihat,    "closed_edge",  { "closed_edge", "closed_shank", "closed" } },
        { PieceRole::hihat,    "half_open",    { "half_open", "three_quarter", "quarter_open", "open" } },
        { PieceRole::hihat,    "open",         { "open", "open_tip", "three_quarter", "half_open" } },
        { PieceRole::hihat,    "open_edge",    { "open_edge", "open_shank", "open", "open_tip", "half_open" } },
        { PieceRole::hihat,    "pedal",        { "pedal", "foot", "closed" } },
        { PieceRole::hihat,    "splash",       { "splash", "swish", "foot_splash", "pedal" } },

        // A crash is usually struck on its edge with the shank ("hit"); a bow hit is the tip.
        { PieceRole::crash,    "hit",          { "hit", "edge" } },
        { PieceRole::crash,    "bow",          { "bow", "tip", "hit" } },
        { PieceRole::crash,    "edge",         { "edge", "hit" } },
        { PieceRole::china,    "hit",          { "hit", "edge" } },
        { PieceRole::china,    "bow",          { "bow", "tip", "hit" } },
        { PieceRole::china,    "edge",         { "edge", "hit" } },

        { PieceRole::ride,     "bow",          { "bow", "tip" } },
        { PieceRole::ride,     "edge",         { "edge", "shank", "crash", "bow" } },
        { PieceRole::ride,     "bell",         { "bell", "bell_tip", "bell_shank" } },
    };

    /** The kit articulation ids that can play a standard one, best first. A
        name with no alias only matches itself. */
    constexpr std::array<std::string_view, maxFallbacks> matchesFor (PieceRole role, std::string_view id) noexcept
    {
        for (const auto& a : aliases)
            if (a.role == role && a.id == id)
                return a.matches;

        return { id };
    }

    /** The standard name a kit's own articulation id is known by: the
        alias it's the best match of ("tip" on a ride is a "bow", "shank" its
        "edge"), or the id itself if it's no alias's match. Saving learned
        notes by these names keeps a map meaning the same on every kit. */
    constexpr std::string_view standardName (PieceRole role, std::string_view id) noexcept
    {
        std::string_view best = id;
        auto bestRank = maxFallbacks;

        for (const auto& a : aliases)
        {
            if (a.role != role)
                continue;

            for (int i = 0; i < maxFallbacks && i < bestRank; ++i)
            {
                if (a.matches[(size_t) i] == id)
                {
                    best = a.id;
                    bestRank = i;
                }
            }
        }

        return best;
    }

    struct Step
    {
        std::string_view id;
        float value;
        HatStroke stroke = HatStroke::none;
    };

    // Hi-hat openness. Brush articulations are left out: a stick hit should
    // never turn into a brush hit because the pedal moved.
    inline constexpr Step hatSteps[] {
        { "closed",        0.0f,  HatStroke::tip },
        { "tight",         0.0f,  HatStroke::tip },
        { "closed_tip",    0.0f,  HatStroke::tip },
        { "closed_shank",  0.0f,  HatStroke::shank },
        { "closed_edge",   0.0f,  HatStroke::shank },
        { "quarter_open",  0.25f, HatStroke::tip },
        { "half_open",     0.5f,  HatStroke::tip },
        { "three_quarter", 0.75f, HatStroke::tip },
        { "open",          1.0f,  HatStroke::tip },
        { "open_tip",      1.0f,  HatStroke::tip },
        { "open_shank",    1.0f,  HatStroke::shank },
        { "open_edge",     1.0f,  HatStroke::shank },
    };

    // Strike positions on a drum head. Ride (and other cymbal) positions are
    // only known when the kit says so in kit.json: no standard name means
    // "bow, near the bell" or "bow, near the edge".
    inline constexpr Step headSteps[] {
        { "center",     0.0f },
        { "off_center", 0.5f },
        { "edge",       1.0f },
    };

    /** What an articulation id means for a piece of the given role, when the
        kit doesn't say. */
    constexpr ArticulationTraits defaultTraits (PieceRole role, std::string_view id) noexcept
    {
        ArticulationTraits t;

        if (role == PieceRole::hihat)
        {
            for (const auto& s : hatSteps)
            {
                if (s.id == id)
                {
                    t.openness = s.value;
                    t.stroke = s.stroke;
                }
            }
        }

        if (role == PieceRole::snare || role == PieceRole::tom || role == PieceRole::floorTom)
            for (const auto& s : headSteps)
                if (s.id == id)
                    t.position = s.value;

        return t;
    }

    /** Whether an articulation is a cymbal grab (a "choke" hit), the only
        kind of hit that may silence its own cymbal. */
    constexpr bool isGrab (std::string_view id) noexcept
    {
        for (const auto word : { "choke", "grab", "stop" })
            if (id.find (word) != std::string_view::npos)
                return true;

        return false;
    }

    /** How open a hi-hat articulation leaves the hats, for chokes: its
        openness step if it has one; 0 for closed and pedal sounds; 1 for
        the rest (splashes, swishes, open brush strokes). */
    constexpr float hatChokeLevel (std::string_view id, float openness) noexcept
    {
        if (openness >= 0.0f)
            return openness;

        for (const auto word : { "closed", "tight", "pedal", "foot", "chick" })
            if (id.find (word) != std::string_view::npos && id.find ("splash") == std::string_view::npos)
                return 0.0f;

        return 1.0f;
    }

    constexpr std::optional<HatStroke> strokeFromKey (std::string_view key) noexcept
    {
        if (key == "tip")   return HatStroke::tip;
        if (key == "shank") return HatStroke::shank;
        return std::nullopt;
    }
}

} // namespace lithium
