#pragma once

#include "Kit.h"
#include "MidiMap.h"

// Resolving MIDI maps against a kit, and editing them in terms of a kit's
// pieces (MIDI learn). These are plain functions of their arguments, with
// no state, so they can run on whichever thread has the map and the
// KitInfo: the loader thread while building a kit, the message thread when
// the user changes the map.

namespace lithium
{

/** Resolves a map against a kit: which articulation each note plays, and
    which notes follow the pedal or a position controller. Standard
    articulation names are matched through their aliases (Articulations.h);
    a name with no match plays the piece's first articulation.

    Every articulation the map doesn't reach then gets a spare note: the
    first free note from 96 up, then from 0 up, then from 60 up. A spare
    never takes a note the map has an entry for (even one this kit can't
    play) or 85-95, kept for cymbal choke notes. So everything in a kit can
    be played with any map, a map's own notes mean the same on every kit,
    and MIDI learn replaces a spare note with a chosen one. */
NoteTable resolveMidiMap (const MidiMap&, const KitInfo&);

/** The entry that plays one of the kit's articulations (index into the
    kit's flattened articulations) from a note, in kit-independent terms:
    the piece's role and its place among the kit's pieces of that role (or
    its name, for percussion and splashes), and the articulation's standard
    name. So a learned note plays the same kind of hit on every kit. It
    follows the pedal or a position controller where the articulation can. */
RoleMapEntry entryForArticulation (const KitInfo&, int articulation, int note);

/** A copy of the map that can be edited note by note. A "Kit default" map
    plays the kit's own map, which names this kit's pieces, with General MIDI
    on the free notes; the copy spells out what it plays for this kit as
    role-based entries instead. Other maps are copied as they are. */
MidiMap editableCopy (const MidiMap&, const KitInfo&);

/** Which piece and articulation (within the piece) a flattened articulation index is. */
std::pair<int, int> pieceAndArticulation (const KitInfo&, int articulation);

/** Whether a crash-role piece is a splash. Splashes have their own notes
    (and choke note), so they don't count as crash 1, 2 or 3. */
bool isSplash (const KitPieceInfo&);

/** "C1", "F#2": note names with middle C (60) as C3, as most DAWs and
    e-kit modules show them. */
juce::String noteName (int note);

} // namespace lithium
