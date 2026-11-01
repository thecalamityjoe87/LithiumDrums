#pragma once

#include "GrooveLibrary.h"
#include "engine/Kit.h"

// Turning a clip into what the kit plays, and into MIDI for the host.
//
// A groove pack's files are written in some MIDI map's notes (the Groove MIDI
// Dataset in Roland V-Drums notes, with the hi-hat pedal on CC4). Preview and
// export both start the same way: each hit is resolved, through that map,
// to the articulation it plays on the loaded kit, picking the hi-hat
// openness step nearest the recorded pedal position. Preview plays those
// articulations; export writes, for each, the note that plays it in the
// user's current MIDI map. So a dragged groove plays in the host exactly as
// it did in the preview, whichever map is in use.

namespace lithium::grooves
{

struct ResolvedHit
{
    double beat;
    int articulation;      // index into the kit's articulations
    int velocity;
    int note;              // as recorded
};

/** Which articulation each of the clip's hits plays on the kit, through the
    pack's note map. Hits the kit can't play are left out. Message thread
    (or any thread but the audio thread: it allocates). */
std::vector<ResolvedHit> resolve (const Clip&, const MidiMap& packMap, const KitInfo&);

/** The colour group a recorded note belongs to, through the pack's note map. */
PieceGroup groupOf (const MidiMap& packMap, int note);

/** The note that plays each of the kit's articulations in a resolved map:
    the lowest note that isn't a spare, or else the spare. -1 = none. */
std::vector<int> notesForArticulations (const NoteTable&, int numArticulations);

/** The clip as a one-track MIDI file (480 PPQ, with its tempo and meter).

    With a kit, the notes are the ones `current` (the user's map, resolved
    against that kit) plays each hit's articulation with: the recorded note
    if it plays the same there, otherwise notesForArticulations(). Without one, the
    recorded notes are written as they are. Hits pushed ahead of the first
    bar line start at its downbeat. */
juce::MidiFile toMidiFile (const Clip&, const MidiMap& packMap, const KitInfo*, const NoteTable& current);

/** A file name for a clip: "Funk 3 bars 5-8 95bpm.mid". */
juce::String fileNameFor (const Clip&);

} // namespace lithium::grooves
