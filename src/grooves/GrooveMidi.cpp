#include "GrooveMidi.h"

#include "engine/MidiMapping.h"

#include <cmath>

namespace lithium::grooves
{

namespace
{
    constexpr int ppq = 480;
    constexpr int noteLengthTicks = ppq / 8;   // drums are one-shots; a 32nd note keeps piano rolls readable

    /** A hi-hat hit with a pedal position plays the openness step nearest
        it, on the same stroke's ladder (tip or shank) where that has steps.
        That's what an e-kit module does with the pedal, minus the blend
        between neighbouring steps, which a plain MIDI note can't carry. */
    int nearestOpennessStep (const KitInfo& kit, int articulation, float openness)
    {
        const auto [p, local] = pieceAndArticulation (kit, articulation);

        if (p < 0)
            return articulation;

        const auto& piece = kit.pieces[(size_t) p];
        const auto stroke = piece.articulations[(size_t) local].traits.stroke;

        for (const auto sameStrokeOnly : { true, false })
        {
            auto best = -1;
            auto bestDistance = 2.0f;

            for (size_t a = 0; a < piece.articulations.size(); ++a)
            {
                const auto& t = piece.articulations[a].traits;

                if (t.openness < 0.0f || (sameStrokeOnly && t.stroke != stroke))
                    continue;

                if (const auto d = std::abs (t.openness - openness); d < bestDistance)
                {
                    best = (int) a;
                    bestDistance = d;
                }
            }

            if (best >= 0)
                return piece.firstArticulation + best;
        }

        return articulation;
    }

    int numArticulations (const KitInfo& kit)
    {
        return kit.pieces.empty() ? 0 : kit.pieces.back().firstArticulation + (int) kit.pieces.back().articulations.size();
    }
}

std::vector<ResolvedHit> resolve (const Clip& clip, const MidiMap& packMap, const KitInfo& kit)
{
    const auto table = resolveMidiMap (packMap, kit);
    std::vector<ResolvedHit> result;
    result.reserve (clip.hits.size());

    for (const auto& h : clip.hits)
    {
        if (! juce::isPositiveAndBelow (h.note, 128))
            continue;

        const auto& target = table.targets[(size_t) h.note];

        if (target.articulation < 0)
            continue;

        // Strike position isn't recorded with the hits, so position
        // following plays the note's own articulation.
        const auto articulation = target.follow == Follow::pedal && h.openness >= 0.0f
                                    ? nearestOpennessStep (kit, target.articulation, h.openness)
                                    : (int) target.articulation;

        result.push_back ({ h.beat, articulation, juce::jlimit (1, 127, h.velocity), h.note });
    }

    return result;
}

PieceGroup groupOf (const MidiMap& packMap, int note)
{
    if (const auto* e = packMap.find (note))
        return e->anyTom ? PieceGroup::toms : info (e->role).group;

    return PieceGroup::other;
}

std::optional<PieceRole> roleOf (const MidiMap& packMap, int note)
{
    if (const auto* e = packMap.find (note))
        return e->anyTom ? PieceRole::tom : e->role;

    return std::nullopt;
}

std::vector<int> notesForArticulations (const NoteTable& table, int numArticulations)
{
    std::vector<int> notes ((size_t) std::max (0, numArticulations), -1);
    std::vector<bool> isSpare (notes.size(), false);

    for (int n = 0; n < 128; ++n)
    {
        const auto& t = table.targets[(size_t) n];

        if (! juce::isPositiveAndBelow ((int) t.articulation, numArticulations))
            continue;

        auto& note = notes[(size_t) t.articulation];
        const auto spare = (bool) isSpare[(size_t) t.articulation];

        // Notes go up, so the first one found is the lowest; a real (non-spare) note replaces a spare.
        if (note < 0 || (spare && ! t.spare))
        {
            note = n;
            isSpare[(size_t) t.articulation] = t.spare;
        }
    }

    return notes;
}

juce::MidiFile toMidiFile (const Clip& clip, const MidiMap& packMap, const KitInfo* kit, const NoteTable& current)
{
    juce::MidiMessageSequence track;

    track.addEvent (juce::MidiMessage::textMetaEvent (3, clip.name), 0.0);   // the track name
    track.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60'000'000.0 / clip.bpm)), 0.0);
    track.addEvent (juce::MidiMessage::timeSignatureMetaEvent (clip.numerator, clip.denominator), 0.0);

    auto addNote = [&] (double beat, int note, int velocity)
    {
        const auto tick = std::round (std::max (0.0, beat) * ppq);

        // Channel 10, the drum channel, which every host and module expects drums on.
        track.addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) velocity), tick);
        track.addEvent (juce::MidiMessage::noteOff (10, note), tick + noteLengthTicks);
    };

    if (kit != nullptr)
    {
        const auto notes = notesForArticulations (current, numArticulations (*kit));

        for (const auto& h : resolve (clip, packMap, *kit))
        {
            // The recorded note, where it means the same in the user's map
            // (a kick on 36 stays on 36, though General MIDI's 35 plays it too).
            const auto same = current.targets[(size_t) h.note].articulation == h.articulation;

            if (const auto note = same ? h.note : notes[(size_t) h.articulation]; note >= 0)
                addNote (h.beat, note, h.velocity);
        }
    }
    else
    {
        for (const auto& h : clip.hits)
            addNote (h.beat, h.note, juce::jlimit (1, 127, h.velocity));
    }

    // The end of the track marks the clip's length, so a host that takes the
    // length from the file loops it on the bar line, not on the last hit.
    track.sort();
    track.addEvent (juce::MidiMessage::endOfTrack(), std::max (std::round (clip.lengthBeats * ppq), track.getEndTime()));
    track.updateMatchedPairs();

    juce::MidiFile file;
    file.setTicksPerQuarterNote (ppq);
    file.addTrack (track);
    return file;
}

juce::String fileNameFor (const Clip& clip)
{
    auto name = clip.name.replace (",", "") + " " + juce::String (juce::roundToInt (clip.bpm)) + "bpm";
    return juce::File::createLegalFileName (name) + ".mid";
}

} // namespace lithium::grooves
