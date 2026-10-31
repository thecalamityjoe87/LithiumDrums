#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

// Reads SFZ instrument files into a flat list of regions.
//
// SFZ is a text format: headers (<control>, <global>, <master>, <group>,
// <region>) followed by opcode=value pairs. Opcodes set on a header apply to
// every region under it, so <global> sets defaults for its masters, <master>
// for its groups, and <group> for its regions; a region's own opcodes win.
// The parser resolves all of that, along with #define variables and
// #include files, and hands back each region with every opcode that applies
// to it. It doesn't interpret the opcodes; SfzImporter does that.
//
// Paths in #include and sample= are relative to the folder of the top-level
// .sfz file (not the included file), as SFZ players treat them.

namespace lithium::sfz
{

struct Region
{
    std::map<juce::String, juce::String> opcodes;   // every opcode that applies, after inheritance
    juce::String sample;                             // as written, with default_path applied and '/' separators
    juce::File sampleFile;                           // resolved; empty for built-in samples such as *silence
    juce::String where;                              // file and line, for messages

    bool isBuiltInSample() const noexcept            { return sample.startsWithChar ('*'); }
    bool has (const juce::String& opcode) const      { return opcodes.count (opcode) > 0; }
    juce::String get (const juce::String& opcode, const juce::String& fallback = {}) const;
    double getNumber (const juce::String& opcode, double fallback) const;

    /** A key opcode (lokey, hikey, pitch_keycenter): a MIDI note number or a
        note name such as c4 (60) or f#2. */
    int getNote (const juce::String& opcode, int fallback) const;
};

struct Instrument
{
    juce::File file;
    std::vector<Region> regions;
    std::map<int, double> controllerDefaults;        // <control> set_ccN values
    juce::StringArray warnings;
};

/** Parses an .sfz file and everything it includes. */
juce::Result parse (const juce::File& sfzFile, Instrument& result);

/** Same, from text standing in for the contents of sfzFile (for the tests). */
juce::Result parse (const juce::String& text, const juce::File& sfzFile, Instrument& result);

/** A MIDI note number from a number or an SFZ note name (c4 = 60), or -1. */
int noteNumber (const juce::String&);

} // namespace lithium::sfz
