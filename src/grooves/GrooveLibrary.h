#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

// Grooves: recorded drum performances as MIDI, to preview through the kit
// and drag into the host.
//
// Like kits, grooves live outside the source tree, in the user's grooves
// folder (Settings::getGroovesFolder()), as groove packs: a folder with a
// grooves.json manifest, the performances as standard MIDI files and the
// licence. The importer makes one from a downloaded library (the Groove
// MIDI Dataset; see GrooveImporter.h). The format is in docs/grooves.md.
//
// A groove is one take, often minutes long. The page shows it as phrases of
// a few bars each (the pack's phraseBars), plus the fills recorded in the
// same session. Nothing here depends on the plugin, so the importer can use
// it too.

namespace lithium::grooves
{

/** A short take (one or two bars) recorded with a groove, to play into a change. */
struct Fill
{
    juce::String id, name;
    juce::File file;
    int bars = 1;
};

/** One recorded take. */
struct Groove
{
    juce::String id, name;
    juce::String style, styleName;        // "jazz", "Jazz": the style key and how it's shown
    juce::String substyle;                // "Funk", or empty
    juce::String drummer;
    double bpm = 120.0;
    int numerator = 4, denominator = 4;
    int bars = 1;
    juce::File file;
    std::vector<Fill> fills;

    double beatsPerBar() const noexcept           { return numerator * 4.0 / denominator; }
    juce::String meter() const                    { return juce::String (numerator) + "/" + juce::String (denominator); }
};

struct Pack
{
    juce::String id, name, author, sourceUrl, description, credits, licenseName, licenseUrl;
    juce::File folder, licenseFile;

    // The MIDI map the files' notes are written in (a factory map id, such
    // as "roland-vdrums" for a library recorded on a Roland kit). Preview and
    // export read the notes through it.
    juce::String noteMap = "lithium-standard";

    // The hi-hat pedal controller in the files, and the value it reaches
    // when the hats are fully closed. Roland's TD-11 tops out around 90, not 127.
    int pedalController = 4;
    int pedalClosedAt = 127;

    int phraseBars = 4;
    std::vector<Groove> grooves;
};

struct Library
{
    std::vector<Pack> packs;                      // sorted by name

    int numGrooves() const noexcept;
};

/** Every groove pack in the grooves folder. Reads only the grooves.json
    files, not the MIDI. Not for the audio thread. */
Library scan (const juce::File& groovesFolder);

/** Reads one pack's grooves.json. */
juce::Result readPack (const juce::File& folder, Pack& result);

//==============================================================================
/** One hit of a recording. */
struct Hit
{
    double beat = 0.0;            // quarter notes from the start of the file
    int note = 0;
    int velocity = 0;

    // How open the hi-hat pedal was at the time, 0 = closed .. 1 = open,
    // or -1 if the file has no pedal data before this hit.
    float openness = -1.0f;
};

/** Reads a MIDI file's note-ons (every track and channel), with the pedal
    position at each. */
juce::Result readHits (const juce::File& midiFile, int pedalController, int pedalClosedAt, std::vector<Hit>& result);

/** The number of whole bars a take covers: up to the bar its last hit is
    in. A hit just before a bar line (played a little early) counts as that
    bar's. */
int countBars (const std::vector<Hit>&, double beatsPerBar);

/** What can be previewed and dragged out: some bars of a groove, a fill or a
    whole take. */
struct Clip
{
    juce::String name;            // "Funk 3, bars 5-8"
    double bpm = 120.0;
    int numerator = 4, denominator = 4;
    double lengthBeats = 0.0;

    // Beats from the start of the clip. A hit played slightly ahead of the
    // first bar line belongs to the clip and has a small negative beat: it
    // plays at the end of each loop, just before the next downbeat.
    std::vector<Hit> hits;
};

/** How far ahead of a bar line a hit can be and still count as that bar's,
    in beats (a 32nd note). Drummers often push a downbeat. */
inline constexpr double earlyHitBeats = 0.125;

/** Bars [firstBar, firstBar + numBars) of a take, as a clip. */
Clip makeClip (const Groove&, const std::vector<Hit>& takeHits, int firstBar, int numBars, const juce::String& name);

/** A groove's phrases: phraseBars each, the last one possibly shorter. */
int numPhrases (const Pack&, const Groove&) noexcept;
juce::Range<int> phraseBars (const Pack&, const Groove&, int phrase) noexcept;   // [first, end) bars

} // namespace lithium::grooves
