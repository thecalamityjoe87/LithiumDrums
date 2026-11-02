#include "KitSlots.h"

namespace lithium::kitslots
{

namespace
{
    const juce::String addedPrefix { "slot-" };

    /** Whether a kit's own piece goes in a slot of this kind when slots are
        handed out: stricter than accepts(), so a floor tom takes a Floor
        slot rather than a free Tom. */
    bool fits (Kind kind, PieceRole role, bool splash) noexcept
    {
        switch (kind)
        {
            case Kind::kick:     return role == PieceRole::kick;
            case Kind::snare:    return role == PieceRole::snare;
            case Kind::hihat:    return role == PieceRole::hihat;
            case Kind::tom:      return role == PieceRole::tom;
            case Kind::floorTom: return role == PieceRole::floorTom;
            case Kind::crash:    return role == PieceRole::crash && ! splash;
            case Kind::splash:   return role == PieceRole::crash && splash;
            case Kind::ride:     return role == PieceRole::ride;
            case Kind::china:    return role == PieceRole::china;
            case Kind::extra:    return true;
        }

        return false;
    }
}

const std::array<Slot, numSlots>& all()
{
    // A 6 x 4 grid on the page, in this order: the drums, the cymbals, then the extras.
    static const std::array<Slot, numSlots> slots {{
        { "kick", "Kick", Kind::kick },          { "snare", "Snare", Kind::snare },
        { "snare2", "Snare 2", Kind::snare },    { "hihat", "Hi-Hat", Kind::hihat },
        { "tom1", "Tom 1", Kind::tom },          { "tom2", "Tom 2", Kind::tom },
        { "tom3", "Tom 3", Kind::tom },          { "floor1", "Floor Tom 1", Kind::floorTom },
        { "floor2", "Floor Tom 2", Kind::floorTom }, { "crash1", "Crash 1", Kind::crash },
        { "crash2", "Crash 2", Kind::crash },    { "crash3", "Crash 3", Kind::crash },
        { "splash", "Splash", Kind::splash },    { "ride", "Ride", Kind::ride },
        { "china", "China", Kind::china },       { "extra1", "Extra 1", Kind::extra },
        { "extra2", "Extra 2", Kind::extra },    { "extra3", "Extra 3", Kind::extra },
        { "extra4", "Extra 4", Kind::extra },    { "extra5", "Extra 5", Kind::extra },
        { "extra6", "Extra 6", Kind::extra },    { "extra7", "Extra 7", Kind::extra },
        { "extra8", "Extra 8", Kind::extra },    { "extra9", "Extra 9", Kind::extra },
    }};

    return slots;
}

int find (const juce::String& slotId)
{
    for (size_t i = 0; i < all().size(); ++i)
        if (slotId == all()[i].id)
            return (int) i;

    return -1;
}

bool accepts (Kind kind, PieceRole role, bool splash) noexcept
{
    if (kind == Kind::tom || kind == Kind::floorTom)
        return role == PieceRole::tom || role == PieceRole::floorTom;

    if (kind == Kind::crash || kind == Kind::splash)
        return role == PieceRole::crash;

    return fits (kind, role, splash);
}

PieceRole roleFor (Kind kind, PieceRole own) noexcept
{
    if (kind == Kind::tom)
        return PieceRole::tom;

    if (kind == Kind::floorTom)
        return PieceRole::floorTom;

    return own;   // the other slots only take their own role, and Extras take anything as it is
}

std::vector<int> assign (const std::vector<PieceKind>& pieces)
{
    std::vector<int> result (pieces.size(), -1);
    std::array<bool, numSlots> taken {};

    auto firstFree = [&] (auto&& matches)
    {
        for (int s = 0; s < numSlots; ++s)
            if (! taken[(size_t) s] && matches (all()[(size_t) s].kind))
                return s;

        return -1;
    };

    for (size_t p = 0; p < pieces.size(); ++p)
    {
        const auto [role, splash] = pieces[p];
        auto s = firstFree ([&] (Kind k) { return k != Kind::extra && fits (k, role, splash); });

        if (s < 0)
            s = firstFree ([] (Kind k) { return k == Kind::extra; });

        if (s >= 0)
        {
            result[p] = s;
            taken[(size_t) s] = true;
        }
    }

    return result;
}

bool isSplash (PieceRole role, const juce::String& id, const juce::String& name)
{
    return role == PieceRole::crash && (id.containsIgnoreCase ("splash") || name.containsIgnoreCase ("splash"));
}

juce::String addedPieceId (int slot)
{
    return juce::isPositiveAndBelow (slot, numSlots) ? addedPrefix + all()[(size_t) slot].id : juce::String();
}

int slotOfAddedPiece (const juce::String& pieceId)
{
    return pieceId.startsWith (addedPrefix) ? find (pieceId.substring (addedPrefix.length())) : -1;
}

} // namespace lithium::kitslots
