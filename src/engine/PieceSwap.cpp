#include "PieceSwap.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <map>
#include <set>

namespace lithium::swaps
{

namespace
{
    // Kinds of drum, for telling which close mic records what. Rack and
    // floor toms are one family: kits number their tom mics across both
    // (DRSKit's "tom2" mic is on its first floor tom).
    enum class Family { kick, snare, hihat, tom, ride, crash, china, other };

    Family familyOf (PieceRole role) noexcept
    {
        switch (role)
        {
            case PieceRole::kick:     return Family::kick;
            case PieceRole::snare:    return Family::snare;
            case PieceRole::hihat:    return Family::hihat;
            case PieceRole::tom:
            case PieceRole::floorTom: return Family::tom;
            case PieceRole::ride:     return Family::ride;
            case PieceRole::crash:    return Family::crash;
            case PieceRole::china:    return Family::china;
            case PieceRole::other:    break;
        }

        return Family::other;
    }

    /** The words in a mic's id and name, in lower case: "snare_top1" and
        "snareTop" both give "snare" and "top". */
    juce::StringArray wordsOf (const MicManifest& mic)
    {
        juce::StringArray words;
        juce::String word;
        juce::juce_wchar previous = 0;

        for (const auto c : mic.id + " " + mic.name)
        {
            const auto startsWord = juce::CharacterFunctions::isUpperCase (c) && juce::CharacterFunctions::isLowerCase (previous);

            if (! juce::CharacterFunctions::isLetter (c) || startsWord)
            {
                if (word.isNotEmpty())
                    words.add (word.toLowerCase());

                word.clear();
            }

            if (juce::CharacterFunctions::isLetter (c))
                word += juce::String::charToString (c);

            previous = c;
        }

        if (word.isNotEmpty())
            words.add (word.toLowerCase());

        return words;
    }

    Family familyOf (const MicManifest& mic)
    {
        const auto words = wordsOf (mic);
        auto any = [&] (std::initializer_list<const char*> keys)
        {
            return std::any_of (keys.begin(), keys.end(), [&] (const char* k) { return words.contains (k); });
        };

        if (any ({ "kick", "kik", "bd", "bass" }))               return Family::kick;
        if (any ({ "snare", "sn", "sd" }))                       return Family::snare;
        if (any ({ "hihat", "hh", "hat", "hats" }))              return Family::hihat;
        if (any ({ "tom", "toms", "floor", "ft", "rack" }))      return Family::tom;
        if (any ({ "ride" }))                                    return Family::ride;
        if (any ({ "crash", "splash" }))                         return Family::crash;
        if (any ({ "china" }))                                   return Family::china;
        return Family::other;
    }

    /** Which side of the drum a mic is on: "top"/"bottom" on a snare, "in"/"out" on a kick. Empty if it doesn't say. */
    juce::String placementOf (const MicManifest& mic)
    {
        static const std::pair<const char*, const char*> synonyms[] {
            { "top", "top" }, { "bottom", "bottom" }, { "btm", "bottom" }, { "bot", "bottom" }, { "under", "bottom" },
            { "in", "in" }, { "inside", "in" }, { "back", "in" }, { "out", "out" }, { "outside", "out" }, { "front", "out" },
            { "sub", "sub" }, { "left", "left" }, { "right", "right" },
        };

        for (const auto& word : wordsOf (mic))
            for (const auto& [key, placement] : synonyms)
                if (word == key)
                    return placement;

        return {};
    }

    /** The order own mics are paired in: the main mic of a drum first (no
        placement, a snare's top, a kick's inside mic), its others after. */
    int placementRank (const juce::String& placement)
    {
        static const juce::StringArray order { "", "top", "in", "left", "out", "right", "bottom", "sub" };
        const auto i = order.indexOf (placement);
        return i < 0 ? order.size() : i;
    }

    float gainOf (const MicManifest& mic)
    {
        return juce::Decibels::decibelsToGain (mic.volumeDb, -200.0f) * (mic.invertPolarity ? -1.0f : 1.0f);
    }

    std::vector<int> closeMics (const KitManifest& kit)
    {
        std::vector<int> result;

        for (size_t m = 0; m < kit.mics.size(); ++m)
            if (kit.mics[m].role == MicRole::close)
                result.push_back ((int) m);

        return result;
    }

    bool isNamedAfterPiece (const KitManifest& kit, const MicManifest& mic)
    {
        return kit.findPiece (mic.id) >= 0;
    }

    /** Which of a kit's mics have a recording of this piece. In a kit with a
        mic per piece, that's often just the piece's own. */
    std::vector<bool> micsRecording (const KitManifest& kit, int piece)
    {
        std::vector<bool> used (kit.mics.size(), false);

        for (const auto& a : kit.pieces[(size_t) piece].articulations)
            for (const auto& zone : a.zones)
                for (const auto& hit : zone.roundRobins)
                    for (const auto& source : hit.sources)
                        used[(size_t) source.mic] = true;

        return used;
    }

    /** Where a close mic goes when the target has no mic of its own for it,
        as with an empty slot: any close mic carries it to the piece's own
        channel. The mic of the same name, else one with as many channels,
        then the most central, so the piece isn't panned off to one side.
        -1 if the kit has no close mics. */
    int closeMicFor (const MicManifest& mic, const KitManifest& target)
    {
        int best = -1;
        auto bestScore = 0.0f;

        for (const auto t : closeMics (target))
        {
            const auto& candidate = target.mics[(size_t) t];

            if (candidate.id == mic.id)
                return t;

            const auto score = (candidate.channels == mic.channels ? 0.0f : 10.0f) + std::abs (candidate.pan);

            if (best < 0 || score < bestScore)
            {
                best = t;
                bestScore = score;
            }
        }

        return best;
    }
}

//==============================================================================
bool canReplace (PieceRole target, PieceRole source) noexcept
{
    return target == source || (familyOf (target) == Family::tom && familyOf (source) == Family::tom);
}

std::vector<int> ownMics (const KitManifest& kit, int piece)
{
    const auto& p = kit.pieces[(size_t) piece];
    const auto close = closeMics (kit);

    // Kits with a mic per piece (the Basalt kits, CrocellKit's toms) name it after the piece.
    for (const auto m : close)
        if (kit.mics[(size_t) m].id == p.id)
            return { m };

    const auto family = familyOf (p.role);

    if (family == Family::other)
        return {};

    // The family's mics that aren't some other piece's own named mic.
    std::vector<int> mics;

    for (const auto m : close)
        if (familyOf (kit.mics[(size_t) m]) == family && ! isNamedAfterPiece (kit, kit.mics[(size_t) m]))
            mics.push_back (m);

    if (family != Family::tom)
        return mics;   // a snare's top and bottom mics, a kick's in and out mics

    // Toms: one mic each, numbered across the rack and floor toms in kit
    // order. Pair them up if the counts agree; otherwise every tom mic counts.
    std::vector<int> toms;

    for (size_t i = 0; i < kit.pieces.size(); ++i)
    {
        const auto& t = kit.pieces[i];

        if (familyOf (t.role) == Family::tom
            && std::none_of (close.begin(), close.end(), [&] (int m) { return kit.mics[(size_t) m].id == t.id; }))
            toms.push_back ((int) i);
    }

    if (toms.size() == mics.size())
        for (size_t i = 0; i < toms.size(); ++i)
            if (toms[i] == piece)
                return { mics[i] };

    return mics;
}

std::vector<int> mapMics (const KitManifest& source, int sourcePiece, const KitManifest& target, int targetPiece, bool ambience)
{
    std::vector<int> result (source.mics.size(), -1);
    std::vector<bool> taken (target.mics.size(), false);

    // Only the mics that recorded this piece go anywhere: the others have
    // nothing of it to carry.
    const auto recorded = micsRecording (source, sourcePiece);

    auto assign = [&] (int s, int t)
    {
        result[(size_t) s] = t;
        taken[(size_t) t] = true;
    };

    // 1. The piece's own mics onto the target piece's own mics, the same
    //    placement where both say (top to top, bottom to bottom), else in order.
    auto sourceOwn = ownMics (source, sourcePiece);
    const auto targetOwn = ownMics (target, targetPiece);

    sourceOwn.erase (std::remove_if (sourceOwn.begin(), sourceOwn.end(), [&] (int m) { return ! recorded[(size_t) m]; }), sourceOwn.end());

    std::stable_sort (sourceOwn.begin(), sourceOwn.end(), [&] (int a, int b)
    {
        return placementRank (placementOf (source.mics[(size_t) a])) < placementRank (placementOf (source.mics[(size_t) b]));
    });

    for (const auto s : sourceOwn)
    {
        const auto placement = placementOf (source.mics[(size_t) s]);

        for (const auto t : targetOwn)
        {
            if (! taken[(size_t) t] && placement.isNotEmpty() && placementOf (target.mics[(size_t) t]) == placement)
            {
                assign (s, t);
                break;
            }
        }
    }

    for (const auto s : sourceOwn)
    {
        if (result[(size_t) s] >= 0)
            continue;

        // A second top mic doesn't go onto the bottom mic just because it's free.
        const auto placement = placementOf (source.mics[(size_t) s]);
        const auto targetHasPlacement = std::any_of (targetOwn.begin(), targetOwn.end(), [&] (int t)
        {
            return placement.isNotEmpty() && placementOf (target.mics[(size_t) t]) == placement;
        });

        if (targetHasPlacement)
            continue;

        for (const auto t : targetOwn)
        {
            if (! taken[(size_t) t])
            {
                assign (s, t);
                break;
            }
        }
    }

    // 2. The other close mics (what the rest of the kit's mics picked up of
    //    the piece): to the mic with the same id, else one of the same
    //    family and placement, else of the same family.
    const auto sourceClose = closeMics (source);
    const auto targetClose = closeMics (target);

    auto mapLeftovers = [&] (auto&& matches)
    {
        for (const auto s : sourceClose)
        {
            if (! recorded[(size_t) s] || result[(size_t) s] >= 0 || std::find (sourceOwn.begin(), sourceOwn.end(), s) != sourceOwn.end())
                continue;

            for (const auto t : targetClose)
            {
                if (! taken[(size_t) t] && std::find (targetOwn.begin(), targetOwn.end(), t) == targetOwn.end()
                    && matches (source.mics[(size_t) s], target.mics[(size_t) t]))
                {
                    assign (s, t);
                    break;
                }
            }
        }
    };

    mapLeftovers ([] (const MicManifest& s, const MicManifest& t) { return s.id == t.id; });
    mapLeftovers ([] (const MicManifest& s, const MicManifest& t)
    {
        return familyOf (s) != Family::other && familyOf (s) == familyOf (t) && placementOf (s) == placementOf (t);
    });
    mapLeftovers ([] (const MicManifest& s, const MicManifest& t) { return familyOf (s) != Family::other && familyOf (s) == familyOf (t); });

    // 3. Overheads onto overheads and room mics onto room mics: the same id
    //    first, then the same number of channels, then in order.
    if (ambience)
    {
        for (const auto role : { MicRole::overheads, MicRole::room })
        {
            for (int pass = 0; pass < 3; ++pass)
            {
                for (size_t s = 0; s < source.mics.size(); ++s)
                {
                    if (source.mics[s].role != role || ! recorded[s] || result[s] >= 0)
                        continue;

                    for (size_t t = 0; t < target.mics.size(); ++t)
                    {
                        const auto& sm = source.mics[s];
                        const auto& tm = target.mics[t];
                        const auto fits = pass == 0 ? sm.id == tm.id : pass == 1 ? sm.channels == tm.channels : true;

                        if (tm.role == role && ! taken[t] && fits)
                        {
                            assign ((int) s, (int) t);
                            break;
                        }
                    }
                }
            }
        }
    }

    // 4. If nothing at all would be heard (a piece from a kit with a mic per
    //    piece going to an empty slot, a kit recorded only as a stereo pair
    //    meeting one with only close mics, or ambience off on a kit with no
    //    close mics), the piece's main mic goes onto the target piece's own
    //    mic. Failing that a close mic goes onto a close mic, so the piece
    //    has its own channel; anything else onto the kit's overheads, else
    //    its first mic.
    if (std::none_of (result.begin(), result.end(), [] (int t) { return t >= 0; }) && ! target.mics.empty())
    {
        const auto first = std::find (recorded.begin(), recorded.end(), true);

        if (first != recorded.end())
        {
            const auto s = ! sourceOwn.empty() ? sourceOwn.front() : (int) std::distance (recorded.begin(), first);
            auto t = ! targetOwn.empty() ? targetOwn.front() : -1;

            if (t < 0 && source.mics[(size_t) s].role == MicRole::close)
                t = closeMicFor (source.mics[(size_t) s], target);

            for (size_t m = 0; m < target.mics.size() && t < 0; ++m)
                if (target.mics[m].role == MicRole::overheads)
                    t = (int) m;

            result[(size_t) s] = std::max (0, t);
        }
    }

    return result;
}

//==============================================================================
void apply (KitManifest& kit, const PieceSwaps& swaps)
{
    // Every piece's slot, from the kit as it comes: emptying or filling one
    // slot never moves another piece to a different one.
    {
        std::vector<kitslots::PieceKind> kinds;

        for (const auto& p : kit.pieces)
            kinds.push_back ({ p.role, kitslots::isSplash (p.role, p.id, p.name) });

        const auto slots = kitslots::assign (kinds);
        kit.slotOwners = {};

        for (size_t i = 0; i < kit.pieces.size(); ++i)
        {
            kit.pieces[i].slot = slots[i];

            if (slots[i] >= 0)
                kit.slotOwners[(size_t) slots[i]] = { kit.pieces[i].id, kit.pieces[i].name };
        }
    }

    // Each source kit is read once, however many of its pieces are used.
    std::map<juce::String, std::unique_ptr<KitManifest>> sources;
    juce::StringArray removed;

    for (const auto& swap : swaps)
    {
        if (swap.removed)
        {
            removed.add (swap.piece);
            continue;
        }

        if (swap.sourceKit.isEmpty())
            continue;

        // A piece to replace, or an empty slot to fill.
        auto targetIndex = kit.findPiece (swap.piece);
        const auto addTo = targetIndex < 0 ? kitslots::slotOfAddedPiece (swap.piece) : -1;

        if (targetIndex < 0 && (addTo < 0 || kit.slotOwners[(size_t) addTo].id.isNotEmpty() || (int) kit.pieces.size() >= maxPieces))
            continue;

        // Why the swap couldn't be made, on the piece it was for (a slot it
        // would have filled just stays empty).
        auto problem = [&] (const juce::String& text)
        {
            if (targetIndex >= 0)
                kit.pieces[(size_t) targetIndex].swapProblem = text;
        };

        const auto targetName = targetIndex >= 0 ? kit.pieces[(size_t) targetIndex].name.toLowerCase() : juce::String();
        auto& source = sources[swap.sourceKit];

        if (source == nullptr)
        {
            source = std::make_unique<KitManifest>();
            const auto folder = findKitById (swap.sourceKit, kit.folder);

            if (folder == juce::File())
            {
                problem ("The kit \"" + swap.sourceKit + "\" isn't installed, so this is the kit's own "
                         + targetName + ". Install it again to get the swap back.");
                source.reset();
                continue;
            }

            if (const auto r = KitManifest::parse (folder, *source); r.failed())
            {
                problem ("Couldn't read the kit it comes from: " + r.getErrorMessage());
                source.reset();
                continue;
            }
        }

        const auto sourceIndex = source->findPiece (swap.sourcePiece);

        if (addTo >= 0)
        {
            // An empty slot: a new piece of the slot's kind for the source
            // piece to fill, named after the slot (or for an Extra, the piece).
            const auto& slot = kitslots::all()[(size_t) addTo];

            if (sourceIndex < 0)
                continue;

            const auto& sp = source->pieces[(size_t) sourceIndex];

            if (! kitslots::accepts (slot.kind, sp.role, kitslots::isSplash (sp.role, sp.id, sp.name)))
                continue;

            PieceManifest added;
            added.id = swap.piece;
            added.name = slot.kind == kitslots::Kind::extra ? sp.name : juce::String (slot.name);
            added.role = kitslots::roleFor (slot.kind, sp.role);
            added.chokeNote = sp.chokeNote;
            added.slot = addTo;
            kit.pieces.push_back (std::move (added));
            targetIndex = (int) kit.pieces.size() - 1;
        }

        auto& target = kit.pieces[(size_t) targetIndex];

        if (sourceIndex < 0 || ! canReplace (target.role, source->pieces[(size_t) sourceIndex].role))
        {
            target.swapProblem = source->credits.name + " has no \"" + swap.sourcePiece + "\" that can stand in for the "
                                 + target.name.toLowerCase() + ", so this is the kit's own.";
            continue;
        }

        // The audio: this kit's own folder (a preset of the same recordings), or another source.
        int audio = 0;

        if (source->audioFolder != kit.audioFolder)
        {
            for (size_t i = 0; i < kit.swapSources.size() && audio == 0; ++i)
                if (kit.swapSources[i].folder == source->audioFolder)
                    audio = (int) i + 1;

            if (audio == 0)
            {
                kit.swapSources.push_back ({ source->audioFolder, source->sampleRate });
                audio = (int) kit.swapSources.size();
            }
        }

        const auto micMap = mapMics (*source, sourceIndex, kit, targetIndex, swap.ambience);
        const auto recorded = micsRecording (*source, sourceIndex);

        SwapManifest info;
        info.kitId = source->isPreset ? source->baseId : source->id;
        info.kitName = source->credits.name;
        info.pieceName = source->pieces[(size_t) sourceIndex].name;
        info.sampleRate = source->sampleRate;
        info.micGain.fill (1.0f);

        for (size_t s = 0; s < micMap.size(); ++s)
        {
            if (micMap[s] >= 0)
                info.micGain[(size_t) micMap[s]] = gainOf (source->mics[s]) / gainOf (kit.mics[(size_t) micMap[s]]);
            else if (recorded[s] && (swap.ambience || source->mics[s].role == MicRole::close))
                info.dropped.add (source->mics[s].name);
        }

        // The replacement keeps the piece's place in this kit: its id (for
        // parameters and MIDI maps), name, role, choke note and slot.
        auto replacement = source->pieces[(size_t) sourceIndex];
        replacement.id = target.id;
        replacement.name = target.name;
        replacement.role = target.role;
        replacement.chokeNote = target.chokeNote;
        replacement.slot = target.slot;

        // Its choke groups are its own: renamed so they can't meet this
        // kit's, and only those within the piece (a hi-hat's closed hits
        // choking its open ones) are kept.
        const auto prefix = "swap:" + target.id + ":";
        std::set<juce::String> groups;

        for (const auto& a : replacement.articulations)
            if (a.chokeGroup.isNotEmpty())
                groups.insert (a.chokeGroup);

        for (auto& a : replacement.articulations)
        {
            if (a.chokeGroup.isNotEmpty())
                a.chokeGroup = prefix + a.chokeGroup;

            juce::StringArray chokes;

            for (const auto& g : a.chokes)
                if (groups.count (g) > 0)
                    chokes.add (prefix + g);

            a.chokes = chokes;

            for (auto& zone : a.zones)
            {
                for (auto& hit : zone.roundRobins)
                {
                    std::vector<MicSource> mapped;

                    for (auto s : hit.sources)
                    {
                        if (const auto t = micMap[(size_t) s.mic]; t >= 0)
                        {
                            // A mono mic has no second channel (0). On a stereo
                            // mic it plays in the middle: the same channel twice.
                            // (A stereo mic on a mono one plays its left: [0].)
                            if (kit.mics[(size_t) t].channels == 2 && s.fileChannels[1] < 1)
                                s.fileChannels[1] = s.fileChannels[0];

                            s.mic = t;
                            s.source = audio;
                            mapped.push_back (s);
                        }
                    }

                    hit.sources = std::move (mapped);
                }
            }
        }

        replacement.swap = std::move (info);
        target = std::move (replacement);
    }

    // Emptied slots last, so the pieces they leave out can't be found by a
    // swap above (and the kit's other pieces keep their ids and slots).
    kit.pieces.erase (std::remove_if (kit.pieces.begin(), kit.pieces.end(),
                                      [&] (const PieceManifest& p) { return removed.contains (p.id); }),
                      kit.pieces.end());
}

std::optional<PieceSwap> find (const PieceSwaps& swaps, const juce::String& pieceId)
{
    for (const auto& s : swaps)
        if (s.piece == pieceId)
            return s;

    return std::nullopt;
}

void set (PieceSwaps& swaps, const PieceSwap& swap)
{
    swaps.erase (std::remove_if (swaps.begin(), swaps.end(), [&] (const PieceSwap& s) { return s.piece == swap.piece; }),
                 swaps.end());

    if (swap.sourceKit.isNotEmpty() || swap.removed)
        swaps.push_back (swap);
}

//==============================================================================
namespace
{
    const juce::Identifier swapType { "swap" }, pieceId { "piece" }, kitId { "kit" }, sourcePieceId { "sourcePiece" },
        ambienceId { "ambience" }, removedId { "removed" };
}

juce::ValueTree toValueTree (const PieceSwaps& swaps)
{
    juce::ValueTree tree (treeType);

    for (const auto& s : swaps)
    {
        juce::ValueTree child (swapType, { { pieceId, s.piece }, { kitId, s.sourceKit },
                                           { sourcePieceId, s.sourcePiece }, { ambienceId, s.ambience } });

        if (s.removed)
            child.setProperty (removedId, true, nullptr);

        tree.appendChild (child, nullptr);
    }

    return tree;
}

PieceSwaps fromValueTree (const juce::ValueTree& tree)
{
    PieceSwaps swaps;

    for (const auto& child : tree)
    {
        if (! child.hasType (swapType))
            continue;

        PieceSwap s { child[pieceId].toString(), child[kitId].toString(), child[sourcePieceId].toString(),
                      (bool) child.getProperty (ambienceId, true), (bool) child.getProperty (removedId, false) };

        if (s.piece.isNotEmpty() && (s.removed || (s.sourceKit.isNotEmpty() && s.sourcePiece.isNotEmpty())))
            set (swaps, s);
    }

    return swaps;
}

} // namespace lithium::swaps
