#include "MidiMapping.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace lithium
{

namespace
{
    /** The nth piece of the role whose id or name contains one of the
        words, trying the words in order ("clap|snap": a clap if there is
        one, else snaps). -1 if there aren't that many. */
    int findByName (const KitInfo& info, PieceRole role, const juce::String& words, int nth = 0)
    {
        for (const auto& w : juce::StringArray::fromTokens (words, "|", {}))
        {
            int seen = 0;

            for (size_t p = 0; p < info.pieces.size(); ++p)
                if (const auto& piece = info.pieces[p];
                    w.isNotEmpty() && piece.role == role && (piece.id.containsIgnoreCase (w) || piece.name.containsIgnoreCase (w)))
                    if (seen++ == nth)
                        return (int) p;
        }

        return -1;
    }

    struct PieceChoice
    {
        int piece = -1;
        bool substitute = false;     // the kit has no nth piece; this is the last one instead
    };

    /** The kit piece a role-based entry plays (-1 for none). */
    PieceChoice pieceFor (const RoleMapEntry& e, const KitInfo& info)
    {
        std::vector<int> candidates;

        if (e.name.isNotEmpty())
        {
            // An exact entry takes the nth match (Lithium Standard's second
            // splash); others the first, and nth is for their fallback.
            if (const auto p = findByName (info, e.role, e.name, e.exact ? e.nth : 0); p >= 0)
                return { p, false };

            // A cowbell note never plays the tambourine: percussion is only
            // ever played by name. A splash note on a kit without a splash
            // falls back to nth, and still plays a crash.
            if (e.role == PieceRole::other || e.exact)
                return {};
        }

        // "toms" counts every tom, rack toms first: an e-kit's "Tom 3" is
        // the third tom whether the kit has three rack toms or two floor toms.
        const std::array roles { e.anyTom ? PieceRole::tom : e.role, PieceRole::floorTom };

        for (size_t r = 0; r < (e.anyTom ? 2u : 1u); ++r)
            for (size_t p = 0; p < info.pieces.size(); ++p)
                if (info.pieces[p].role == roles[r] && ! isSplash (info.pieces[p]))
                    candidates.push_back ((int) p);

        // A kit whose only crashes are splashes still has crashes.
        if (candidates.empty() && e.role == PieceRole::crash)
            for (size_t p = 0; p < info.pieces.size(); ++p)
                if (info.pieces[p].role == PieceRole::crash)
                    candidates.push_back ((int) p);

        if (candidates.empty())
            return {};

        const auto substitute = e.nth >= (int) candidates.size();
        return { candidates[(size_t) (substitute ? (int) candidates.size() - 1 : e.nth)], substitute };
    }

    /** The articulation (flattened index) that plays a standard articulation
        name on a piece: the best of its aliases the piece has, or its first
        (-1 instead, if exact). */
    int articulationFor (const KitInfo& info, int piece, const juce::String& id, bool exact = false)
    {
        const auto& p = info.pieces[(size_t) piece];

        // The string must outlive the string_views that matchesFor() returns.
        const auto wanted = id.toStdString();

        for (const auto match : articulations::matchesFor (p.role, wanted))
        {
            if (match.empty())
                break;

            for (size_t a = 0; a < p.articulations.size(); ++a)
                if (p.articulations[a].id == juce::String (match.data(), match.size()))
                    return p.firstArticulation + (int) a;
        }

        if (exact)
            return -1;

        return p.firstArticulation;
    }

    /** The map's own entries, before spare notes. */
    void resolveEntries (NoteTable& table, const MidiMap& map, const KitInfo& info)
    {
        // Kit default: the kit's own map first. Its entries (General MIDI)
        // then fill the notes the kit's map leaves free, so a kit whose
        // library puts something on an unusual note still plays it where
        // drum MIDI expects it too (DRSKit's ride crash is on 66 and 59).
        std::array<bool, 128> kitNotes {};

        if (map.useKitMap)
        {
            for (const auto& e : info.kitMidiMap)
            {
                for (size_t p = 0; p < info.pieces.size(); ++p)
                {
                    if (info.pieces[p].id == e.piece)
                    {
                        table.targets[(size_t) e.note].articulation = (std::int16_t) articulationFor (info, (int) p, e.articulation);
                        kitNotes[(size_t) e.note] = true;
                        break;
                    }
                }
            }
        }

        for (const auto& e : map.entries)
        {
            if (kitNotes[(size_t) e.note])
                continue;

            const auto [piece, substitute] = pieceFor (e, info);

            if (piece < 0 || (e.exact && substitute))
                continue;

            const auto& pieceInfo = info.pieces[(size_t) piece];
            const auto articulation = articulationFor (info, piece, substitute && e.otherwise.isNotEmpty() ? e.otherwise : e.articulation, e.exact);

            if (articulation < 0)
                continue;
            auto& target = table.targets[(size_t) e.note];

            target.articulation = (std::int16_t) articulation;

            // Only where the kit has steps to move between; elsewhere the note
            // just plays its articulation.
            if (pieceInfo.canFollow (articulation - pieceInfo.firstArticulation, e.follow))
                target.follow = e.follow;

            if (const auto source = positionSourceFor (pieceInfo.role))
                target.source = *source;
        }
    }

    /** Gives every articulation the table doesn't reach a free note of its
        own, so nothing in a kit is ever unplayable, whatever the map. */
    void addSpareNotes (NoteTable& table, const MidiMap& map, const KitInfo& info)
    {
        std::array<bool, 128> taken {};

        // The map's own notes keep their meaning even where this kit can't
        // play them, so a spare never lands on one.
        for (const auto& e : map.entries)
            taken[(size_t) e.note] = true;
        std::vector<bool> reached (info.pieces.empty() ? 0 : (size_t) (info.pieces.back().firstArticulation
                                                                        + (int) info.pieces.back().articulations.size()));

        for (int n = 0; n < 128; ++n)
        {
            if (const auto a = table.targets[(size_t) n].articulation; a >= 0)
            {
                taken[(size_t) n] = true;
                reached[(size_t) a] = true;
            }
        }

        for (const auto& p : info.pieces)
            if (juce::isPositiveAndBelow (p.defaultChokeNote, 128))
                taken[(size_t) p.defaultChokeNote] = true;

        // Cymbals without a choke note of their own get one from 85 up (see
        // KitLoader), so spares keep clear of 85-95.
        for (int n = 85; n <= 95; ++n)
            taken[(size_t) n] = true;

        std::vector<int> spares;

        for (const auto [from, to] : { std::pair { 96, 127 }, std::pair { 0, 20 }, std::pair { 60, 84 } })
            for (int n = from; n <= to; ++n)
                if (! taken[(size_t) n])
                    spares.push_back (n);

        auto next = spares.begin();

        for (const auto& p : info.pieces)
        {
            for (int a = p.firstArticulation; a < p.firstArticulation + (int) p.articulations.size(); ++a)
            {
                if (reached[(size_t) a] || next == spares.end())
                    continue;

                auto& target = table.targets[(size_t) *next++];
                target.articulation = (std::int16_t) a;
                target.spare = true;
            }
        }
    }
}

namespace
{
    /** The kit piece a piece's velocity settings are for (-1 for none). Only
        the piece they name: a curve for the third crash never lands on the
        second. */
    int pieceForVelocity (const PieceVelocity& pv, const KitInfo& info)
    {
        // On the kit they were made on, a piece named by its id is that very piece.
        if (pv.name.isNotEmpty())
            for (size_t p = 0; p < info.pieces.size(); ++p)
                if (info.pieces[p].role == pv.role && info.pieces[p].id == pv.name)
                    return (int) p;

        RoleMapEntry e;
        e.role = pv.role;
        e.nth = pv.nth;
        e.name = pv.name;
        e.exact = true;

        const auto [piece, substitute] = pieceFor (e, info);
        return substitute ? -1 : piece;
    }

    /** Each note's adjust and each piece's velocity table. */
    void resolveVelocity (NoteTable& table, const VelocitySettings& velocity, const KitInfo& info)
    {
        std::array<const PieceVelocity*, maxPieces> settings {};

        for (size_t p = 0; p < info.pieces.size() && p < settings.size(); ++p)
        {
            settings[p] = findPieceVelocity (velocity, info, (int) p);
            table.velocityCurves[p] = makeVelocityTable (settings[p] != nullptr ? settings[p]->curve : VelocityCurve {}, velocity.global);
        }

        for (size_t n = 0; n < table.targets.size(); ++n)
        {
            const auto a = table.targets[n].articulation;
            const auto [p, local] = pieceAndArticulation (info, a);

            if (p >= 0 && p < maxPieces && settings[(size_t) p] != nullptr)
                table.velocityAdjust[n] = (std::int8_t) velocityAdjustFor (*settings[(size_t) p], info, a);
        }
    }
}

NoteTable resolveMidiMap (const MidiMap& map, const KitInfo& info)
{
    NoteTable table;
    table.controllers = map.controllers;

    if (info.pieces.empty())
        return table;

    resolveEntries (table, map, info);
    addSpareNotes (table, map, info);
    resolveVelocity (table, map.velocity, info);
    return table;
}

const PieceVelocity* findPieceVelocity (const VelocitySettings& velocity, const KitInfo& info, int piece)
{
    for (const auto& pv : velocity.pieces)
        if (pieceForVelocity (pv, info) == piece)
            return &pv;

    return nullptr;
}

void setPieceVelocity (VelocitySettings& velocity, const KitInfo& info, int piece, PieceVelocity pv)
{
    if (! juce::isPositiveAndBelow (piece, (int) info.pieces.size()))
        return;

    std::erase_if (velocity.pieces, [&] (const auto& o) { return pieceForVelocity (o, info) == piece; });

    // Named the way a learned note names its piece.
    const auto key = entryForArticulation (info, info.pieces[(size_t) piece].firstArticulation, 0);
    pv.role = key.role;
    pv.nth = key.nth;
    pv.name = key.name;
    pv.curve = pv.curve.sanitised();

    if (! pv.isNeutral())
        velocity.pieces.push_back (std::move (pv));
}

juce::String velocityAdjustKey (const KitInfo& info, int articulation)
{
    const auto [p, a] = pieceAndArticulation (info, articulation);

    if (p < 0)
        return {};

    const auto& piece = info.pieces[(size_t) p];
    const auto id = piece.articulations[(size_t) a].id.toStdString();   // outlives the string_view
    const auto standard = articulations::standardName (piece.role, id);
    return { standard.data(), standard.size() };
}

int velocityAdjustFor (const PieceVelocity& pv, const KitInfo& info, int articulation)
{
    const auto piece = pieceAndArticulation (info, articulation).first;

    if (piece < 0)
        return 0;

    // The articulation's own standard name wins over an alias that also reaches it.
    const auto key = velocityAdjustKey (info, articulation);
    auto amount = 0;

    for (const auto& [name, adjust] : pv.adjust)
    {
        if (name == key)
            return adjust;

        if (articulationFor (info, piece, name, true) == articulation)
            amount = adjust;
    }

    return amount;
}

void setVelocityAdjust (PieceVelocity& pv, const KitInfo& info, int articulation, int amount)
{
    const auto piece = pieceAndArticulation (info, articulation).first;

    if (piece < 0)
        return;

    std::erase_if (pv.adjust, [&] (const auto& a) { return articulationFor (info, piece, a.first, true) == articulation; });
    pv.setAdjust (velocityAdjustKey (info, articulation), amount);
}

VelocityTable makeVelocityTable (const VelocityCurve& curve, float global)
{
    VelocityTable table {};

    for (size_t v = 0; v < table.size(); ++v)
        table[v] = (std::uint8_t) juce::jlimit (1, 127, juce::roundToInt (applyGlobalVelocity (curve ((float) v), global)));

    return table;
}

std::pair<int, int> pieceAndArticulation (const KitInfo& info, int articulation)
{
    for (size_t p = 0; p < info.pieces.size(); ++p)
    {
        const auto& piece = info.pieces[p];
        const auto local = articulation - piece.firstArticulation;

        if (juce::isPositiveAndBelow (local, (int) piece.articulations.size()))
            return { (int) p, local };
    }

    return { -1, -1 };
}

RoleMapEntry entryForArticulation (const KitInfo& info, int articulation, int note)
{
    const auto [p, a] = pieceAndArticulation (info, articulation);
    RoleMapEntry e;
    e.note = note;

    if (p < 0)
        return e;

    const auto& piece = info.pieces[(size_t) p];
    const auto kitId = piece.articulations[(size_t) a].id.toStdString();
    const auto standard = articulations::standardName (piece.role, kitId);
    e.role = piece.role;
    e.articulation = juce::String (standard.data(), standard.size());

    // Percussion and splashes are found by name on other kits; everything
    // else by its place among the kit's pieces of its role, counted the way
    // the resolver counts (crashes without the splashes).
    if (piece.role == PieceRole::other || isSplash (piece))
    {
        e.name = piece.id;
    }
    else
    {
        for (int i = 0; i < p; ++i)
            if (info.pieces[(size_t) i].role == piece.role && ! isSplash (info.pieces[(size_t) i]))
                ++e.nth;
    }

    // Following costs nothing until the controller is received, so a
    // learned hi-hat note follows the pedal, and a learned head or bow hit
    // its position, wherever the kit can.
    if (piece.canFollow (a, Follow::pedal))
        e.follow = Follow::pedal;
    else if (piece.canFollow (a, Follow::position))
        e.follow = Follow::position;

    return e;
}

MidiMap editableCopy (const MidiMap& map, const KitInfo& info)
{
    auto copy = map;
    copy.useKitMap = false;

    if (map.useKitMap && ! info.kitMidiMap.empty())
    {
        const auto table = resolveMidiMap (map, info);
        copy.entries.clear();

        for (int note = 0; note < 128; ++note)
        {
            // Spare notes aren't part of the map; they're worked out again
            // from whatever it leaves out.
            if (const auto a = table.targets[(size_t) note].articulation; a >= 0 && ! table.targets[(size_t) note].spare)
            {
                auto e = entryForArticulation (info, a, note);
                e.follow = Follow::none;   // the kit's map didn't follow anything either
                copy.assign (e);
            }
        }
    }

    return copy;
}

bool isSplash (const KitPieceInfo& piece)
{
    return piece.role == PieceRole::crash && (piece.id.containsIgnoreCase ("splash") || piece.name.containsIgnoreCase ("splash"));
}

juce::String noteName (int note)
{
    return juce::MidiMessage::getMidiNoteName (note, true, true, 3);
}

} // namespace lithium
