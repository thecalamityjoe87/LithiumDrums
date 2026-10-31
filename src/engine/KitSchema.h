#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The fixed vocabulary shared by the kit format, the engine and the UI.
//
// A kit defines its own pieces and mic channels (DRSKit has 9 pieces and 13
// mics, MuldjordKit 12 pieces and 15 mics), so those are runtime data. What
// stays fixed is the vocabulary they're described in: piece roles, mic roles,
// the nine places on the rendered kit stage, and upper limits that let the
// audio thread and the host-facing parameters use fixed-size arrays.
// Everything here is constexpr, so it can be used freely from any thread.

namespace lithium
{

// Upper limits. Parameters are created once per slot up to these limits
// (hosts need a fixed parameter list), and a kit's pieces and mics fill the
// slots in order.
inline constexpr int maxPieces = 24;
inline constexpr int maxMics = 24;
inline constexpr int maxChannelsPerMic = 2;
inline constexpr int maxMicBusChannels = maxMics * maxChannelsPerMic;

//==============================================================================
enum class PieceRole : std::uint8_t { kick, snare, hihat, tom, floorTom, crash, ride, china, other };
inline constexpr int numPieceRoles = 9;

// Colour groups for the UI.
enum class PieceGroup : std::uint8_t { kick, snare, hats, toms, cymbals, other };

struct RoleInfo
{
    PieceRole role;
    std::string_view key;        // as written in kit.json
    std::string_view displayName;
    PieceGroup group;
    bool isCymbal;               // gets a default choke note
};

inline constexpr std::array<RoleInfo, numPieceRoles> roleInfos {{
    { PieceRole::kick,     "kick",      "Kick",      PieceGroup::kick,    false },
    { PieceRole::snare,    "snare",     "Snare",     PieceGroup::snare,   false },
    { PieceRole::hihat,    "hihat",     "Hi-Hat",    PieceGroup::hats,    false },
    { PieceRole::tom,      "tom",       "Tom",       PieceGroup::toms,    false },
    { PieceRole::floorTom, "floor_tom", "Floor Tom", PieceGroup::toms,    false },
    { PieceRole::crash,    "crash",     "Crash",     PieceGroup::cymbals, true },
    { PieceRole::ride,     "ride",      "Ride",      PieceGroup::cymbals, true },
    { PieceRole::china,    "china",     "China",     PieceGroup::cymbals, true },
    { PieceRole::other,    "other",     "Other",     PieceGroup::other,   false },
}};

constexpr const RoleInfo& info (PieceRole r) noexcept      { return roleInfos[static_cast<size_t> (r)]; }

constexpr std::optional<PieceRole> roleFromKey (std::string_view key) noexcept
{
    for (const auto& r : roleInfos)
        if (r.key == key)
            return r.role;

    return std::nullopt;
}

//==============================================================================
// The nine pieces in the pre-rendered kit (assets/kit-renders). A loaded kit's
// pieces are assigned to these slots by role: the first kick to the kick
// slot, the first two rack toms to tom1 and tom2, and so on.
enum class StageSlot : std::uint8_t { kick, snare, hihat, tom1, tom2, floorTom, crash, ride, china };
inline constexpr int numStageSlots = 9;

struct SlotInfo
{
    StageSlot slot;
    std::string_view renderKey;  // file name in assets/kit-renders, and the key in layout.json
    PieceRole role;
    int nth;                     // which piece of that role (0 = the first)
};

inline constexpr std::array<SlotInfo, numStageSlots> slotInfos {{
    { StageSlot::kick,     "kick",  PieceRole::kick,     0 },
    { StageSlot::snare,    "snare", PieceRole::snare,    0 },
    { StageSlot::hihat,    "hihat", PieceRole::hihat,    0 },
    { StageSlot::tom1,     "tom1",  PieceRole::tom,      0 },
    { StageSlot::tom2,     "tom2",  PieceRole::tom,      1 },
    { StageSlot::floorTom, "floor", PieceRole::floorTom, 0 },
    { StageSlot::crash,    "crash", PieceRole::crash,    0 },
    { StageSlot::ride,     "ride",  PieceRole::ride,     0 },
    { StageSlot::china,    "china", PieceRole::china,    0 },
}};

constexpr int index (StageSlot s) noexcept                 { return static_cast<int> (s); }
constexpr const SlotInfo& info (StageSlot s) noexcept      { return slotInfos[static_cast<size_t> (s)]; }

constexpr std::optional<StageSlot> slotFromRenderKey (std::string_view key) noexcept
{
    for (const auto& s : slotInfos)
        if (s.renderKey == key)
            return s.slot;

    return std::nullopt;
}

//==============================================================================
// Which per-piece send scales a mic: close mics, overheads or room.
enum class MicRole : std::uint8_t { close, overheads, room };
inline constexpr int numMicRoles = 3;

inline constexpr std::array<std::string_view, numMicRoles> micRoleKeys { "close", "overheads", "room" };

constexpr std::optional<MicRole> micRoleFromKey (std::string_view key) noexcept
{
    for (size_t i = 0; i < micRoleKeys.size(); ++i)
        if (micRoleKeys[i] == key)
            return static_cast<MicRole> (i);

    return std::nullopt;
}

} // namespace lithium
