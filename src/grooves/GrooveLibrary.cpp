#include "GrooveLibrary.h"

#include <cmath>

namespace lithium::grooves
{

namespace
{
    // Paths in a manifest are relative to the pack, and may not leave it.
    juce::File childFile (const juce::File& folder, const juce::String& path)
    {
        return path.isEmpty() || path.contains ("..") ? juce::File() : folder.getChildFile (path);
    }

    void parseMeter (const juce::String& text, int& numerator, int& denominator)
    {
        const auto n = text.upToFirstOccurrenceOf ("/", false, false).getIntValue();
        const auto d = text.fromFirstOccurrenceOf ("/", false, false).getIntValue();

        // Only real meters: 1-32 beats of halves to 32nds.
        if (n >= 1 && n <= 32 && (d == 2 || d == 4 || d == 8 || d == 16 || d == 32))
        {
            numerator = n;
            denominator = d;
        }
    }
}

int Library::numGrooves() const noexcept
{
    int n = 0;

    for (const auto& p : packs)
        n += (int) p.grooves.size();

    return n;
}

juce::Result readPack (const juce::File& folder, Pack& result)
{
    const auto file = folder.getChildFile ("grooves.json");

    if (! file.existsAsFile())
        return juce::Result::fail ("No grooves.json in " + folder.getFullPathName());

    juce::var root;

    if (const auto r = juce::JSON::parse (file.loadFileAsString(), root); r.failed())
        return juce::Result::fail (file.getFullPathName() + ": " + r.getErrorMessage());

    if (root["format"].toString() != "lithium-grooves")
        return juce::Result::fail (file.getFullPathName() + " is not a groove pack manifest");

    result = {};
    result.folder = folder;
    result.id = root["id"].toString();
    result.name = root["name"].toString();
    result.author = root["author"].toString();
    result.sourceUrl = root["sourceUrl"].toString();
    result.description = root["description"].toString();
    result.credits = root["credits"].toString();

    const auto& license = root["license"];
    result.licenseName = license["name"].toString();
    result.licenseUrl = license["url"].toString();
    result.licenseFile = childFile (folder, license["file"].toString());

    if (const auto map = root["noteMap"].toString(); map.isNotEmpty())
        result.noteMap = map;

    if (const auto& pedal = root["pedal"]; pedal.isObject())
    {
        result.pedalController = juce::jlimit (-1, 127, (int) pedal.getProperty ("controller", 4));
        result.pedalClosedAt = juce::jlimit (1, 127, (int) pedal.getProperty ("closedAt", 127));
    }

    result.phraseBars = juce::jlimit (1, 64, (int) root.getProperty ("phraseBars", 4));

    if (result.name.isEmpty())
        result.name = folder.getFileName();

    if (result.id.isEmpty())
        result.id = folder.getFileName().toLowerCase().replaceCharacter (' ', '-');

    const auto& styles = root["styles"];

    if (const auto* grooves = root["grooves"].getArray())
    {
        for (const auto& g : *grooves)
        {
            Groove groove;
            groove.id = g["id"].toString();
            groove.name = g["name"].toString();
            groove.style = g["style"].toString();
            groove.styleName = styles[juce::Identifier (groove.style.isNotEmpty() ? groove.style : "-")].toString();
            groove.substyle = g["substyle"].toString();
            groove.drummer = g["drummer"].toString();
            groove.bpm = juce::jlimit (20.0, 400.0, (double) g.getProperty ("bpm", 120.0));
            parseMeter (g["meter"].toString(), groove.numerator, groove.denominator);
            groove.bars = std::max (1, (int) g.getProperty ("bars", 1));
            groove.file = childFile (folder, g["file"].toString());

            if (groove.styleName.isEmpty())
                groove.styleName = groove.style.substring (0, 1).toUpperCase() + groove.style.substring (1);

            if (const auto* fills = g["fills"].getArray())
            {
                for (const auto& f : *fills)
                {
                    Fill fill;
                    fill.id = f["id"].toString();
                    fill.name = f["name"].toString();
                    fill.file = childFile (folder, f["file"].toString());
                    fill.bars = std::max (1, (int) f.getProperty ("bars", 1));

                    if (fill.file != juce::File())
                        groove.fills.push_back (std::move (fill));
                }
            }

            if (groove.id.isNotEmpty() && groove.file != juce::File())
                result.grooves.push_back (std::move (groove));
        }
    }

    return juce::Result::ok();
}

Library scan (const juce::File& groovesFolder)
{
    Library library;

    for (const auto& entry : juce::RangedDirectoryIterator (groovesFolder, false, "*", juce::File::findDirectories))
    {
        // A folder without a manifest (or a half-finished import) is skipped.
        if (Pack pack; ! entry.getFile().getFileName().startsWithChar ('.') && readPack (entry.getFile(), pack).wasOk())
            library.packs.push_back (std::move (pack));
    }

    std::sort (library.packs.begin(), library.packs.end(),
               [] (const auto& a, const auto& b) { return a.name.compareNatural (b.name) < 0; });

    return library;
}

//==============================================================================
juce::Result readHits (const juce::File& midiFile, int pedalController, int pedalClosedAt, std::vector<Hit>& result)
{
    result.clear();

    juce::FileInputStream in (midiFile);
    juce::MidiFile file;

    if (in.failedToOpen() || ! file.readFrom (in))
        return juce::Result::fail ("Can't read " + midiFile.getFullPathName());

    // SMPTE-timed files (a negative time format) have no beats to speak of.
    const auto ppq = (int) file.getTimeFormat();

    if (ppq <= 0)
        return juce::Result::fail (midiFile.getFileName() + " is timed in SMPTE frames, not beats");

    // All tracks merged into one time-ordered sequence. addSequence() keeps
    // each track's order for events at the same time, so a pedal change
    // written just before a hit at the same tick still applies to it.
    juce::MidiMessageSequence events;

    for (int t = 0; t < file.getNumTracks(); ++t)
        events.addSequence (*file.getTrack (t), 0.0);

    events.sort();

    auto openness = -1.0f;
    const auto closedAt = (float) juce::jlimit (1, 127, pedalClosedAt);

    for (const auto* e : events)
    {
        const auto& m = e->message;

        if (m.isController() && m.getControllerNumber() == pedalController)
            openness = 1.0f - juce::jmin (1.0f, (float) m.getControllerValue() / closedAt);
        else if (m.isNoteOn())
            result.push_back ({ m.getTimeStamp() / ppq, m.getNoteNumber(), m.getVelocity(), openness });
    }

    return juce::Result::ok();
}

int countBars (const std::vector<Hit>& hits, double beatsPerBar)
{
    auto last = 0.0;

    for (const auto& h : hits)
        last = std::max (last, h.beat);

    return std::max (1, (int) std::floor ((last + earlyHitBeats) / beatsPerBar) + 1);
}

Clip makeClip (const Groove& groove, const std::vector<Hit>& takeHits, int firstBar, int numBars, const juce::String& name)
{
    Clip clip;
    clip.name = name;
    clip.bpm = groove.bpm;
    clip.numerator = groove.numerator;
    clip.denominator = groove.denominator;
    clip.lengthBeats = numBars * groove.beatsPerBar();

    const auto start = firstBar * groove.beatsPerBar();
    const auto end = start + clip.lengthBeats;

    // Each hit belongs to the bar it was aimed at: a pushed downbeat just
    // before this clip's first bar line is this clip's, and one just before
    // its end is the next clip's. The very first bar of a take has nothing
    // before it, so there's no window to shift.
    const auto from = firstBar == 0 ? -1.0 : start - earlyHitBeats;
    const auto to = end - earlyHitBeats;

    for (const auto& h : takeHits)
    {
        if (h.beat >= from && h.beat < to)
        {
            auto hit = h;
            hit.beat = std::max (-earlyHitBeats, h.beat - start);
            clip.hits.push_back (hit);
        }
    }

    return clip;
}

int numPhrases (const Pack& pack, const Groove& groove) noexcept
{
    return (groove.bars + pack.phraseBars - 1) / pack.phraseBars;
}

juce::Range<int> phraseBars (const Pack& pack, const Groove& groove, int phrase) noexcept
{
    const auto first = phrase * pack.phraseBars;
    return { first, std::min (groove.bars, first + pack.phraseBars) };
}

} // namespace lithium::grooves
