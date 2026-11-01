#include "GrooveImporter.h"

#include "GrooveLibrary.h"

#include <map>

namespace lithium::grooves
{

namespace
{
    const juce::String packName { "Groove MIDI Dataset" };

    // The TD-11 sends the hi-hat pedal on CC4, 0 when open and 90 (not 127)
    // when fully closed: 62,000 of the dataset's 64,000 closed hi-hat hits
    // have it at exactly 90.
    constexpr int pedalController = 4, pedalClosedAt = 90;

    juce::File findRoot (const juce::File& folder)
    {
        for (const auto& f : { folder, folder.getChildFile ("groove") })
            if (f.getChildFile ("info.csv").existsAsFile() && f.getChildFile ("drummer1").isDirectory())
                return f;

        return {};
    }

    // How the dataset's style labels are shown. Anything not listed is
    // title-cased ("indie" -> "Indie").
    const std::map<juce::String, juce::String> styleNames {
        { "afrobeat", "Afrobeat" }, { "afrocuban", "Afro-Cuban" }, { "blues", "Blues" }, { "country", "Country" },
        { "dance", "Dance" }, { "funk", "Funk" }, { "gospel", "Gospel" }, { "highlife", "Highlife" },
        { "hiphop", "Hip-Hop" }, { "jazz", "Jazz" }, { "latin", "Latin" }, { "middleeastern", "Middle Eastern" },
        { "neworleans", "New Orleans" }, { "pop", "Pop" }, { "punk", "Punk" }, { "reggae", "Reggae" },
        { "rock", "Rock" }, { "soul", "Soul" },
    };

    const std::map<juce::String, juce::String> substyleNames {
        { "purdieshuffle", "Purdie Shuffle" }, { "secondline", "Second Line" }, { "chacha", "Cha-Cha" },
        { "mediumfast", "Medium Fast" }, { "bembe", juce::CharPointer_UTF8 ("Bemb\xc3\xa9") },
        { "brazilian-baiao", juce::CharPointer_UTF8 ("Bai\xc3\xa3o") }, { "brazilian-bossa", "Bossa Nova" },
        { "brazilian-frevo", "Frevo" }, { "brazilian-ijexa", juce::CharPointer_UTF8 ("Ijex\xc3\xa1") },
        { "brazilian-maracatu", "Maracatu" }, { "brazilian-samba", "Samba" },
        { "brazilian-sambareggae", "Samba-Reggae" }, { "brazilian-songo", "Songo" },
        { "dominican-merengue", "Merengue" }, { "venezuelan-joropo", "Joropo" },
        { "venezuelan-merengue", "Venezuelan Merengue" }, { "venezuelan-sangueo", "Sangueo" },
    };

    // Substyles that only mean something with their style.
    const std::map<juce::String, juce::String> compoundNames {
        { "funk/fast", "Fast Funk" }, { "jazz/fast", "Fast Jazz" }, { "jazz/mediumfast", "Medium-Fast Jazz" },
        { "pop/soft", "Soft Pop" }, { "reggae/slow", "Slow Reggae" }, { "jazz/linear", "Linear Jazz" },
        { "rock/halftime", "Halftime Rock" }, { "rock/indie", "Indie Rock" }, { "rock/prog", "Prog Rock" },
        { "rock/folk", "Folk Rock" }, { "rock/shuffle", "Rock Shuffle" }, { "blues/shuffle", "Blues Shuffle" },
        { "neworleans/shuffle", "New Orleans Shuffle" }, { "jazz/march", "Jazz March" }, { "jazz/funk", "Jazz Funk" },
        { "funk/rock", "Funk Rock" }, { "funk/latin", "Latin Funk" }, { "neworleans/funk", "New Orleans Funk" },
        { "jazz/fusion", "Fusion" }, { "pop/groove7", "Pop" },
    };

    juce::String titleCase (const juce::String& s)
    {
        juce::StringArray words;
        words.addTokens (s.replaceCharacter ('-', ' '), " ", {});

        for (auto& w : words)
            w = w.substring (0, 1).toUpperCase() + w.substring (1);

        return words.joinIntoString (" ");
    }

    juce::String nameOf (const std::map<juce::String, juce::String>& names, const juce::String& key)
    {
        const auto it = names.find (key);
        return it != names.end() ? it->second : titleCase (key);
    }

    struct Row
    {
        juce::String drummer, session, id, style, substyle, beatType, midiFile;
        double bpm = 120.0;
        int numerator = 4, denominator = 4;
        int drummerNumber = 0, take = 0;
    };

    juce::Result readInfo (const juce::File& csv, std::vector<Row>& rows)
    {
        juce::StringArray lines;
        lines.addLines (csv.loadFileAsString());
        lines.removeEmptyStrings();

        if (lines.isEmpty())
            return juce::Result::fail ("info.csv is empty");

        juce::StringArray header;
        header.addTokens (lines[0], ",", "\"");

        auto column = [&header] (const char* name) { return header.indexOf (name); };
        const int drummer = column ("drummer"), session = column ("session"), id = column ("id"), style = column ("style"),
                  bpm = column ("bpm"), beatType = column ("beat_type"), meter = column ("time_signature"),
                  midi = column ("midi_filename");

        if (std::min ({ drummer, session, id, style, bpm, beatType, meter, midi }) < 0)
            return juce::Result::fail ("info.csv doesn't have the Groove MIDI Dataset's columns");

        for (int i = 1; i < lines.size(); ++i)
        {
            juce::StringArray f;
            f.addTokens (lines[i], ",", "\"");

            if (f.size() < header.size())
                continue;

            Row r;
            r.drummer = f[drummer];
            r.session = f[session];
            r.id = f[id];
            r.style = f[style].upToFirstOccurrenceOf ("/", false, false);
            r.substyle = f[style].fromFirstOccurrenceOf ("/", false, false);
            r.beatType = f[beatType];
            r.midiFile = f[midi];
            r.bpm = f[bpm].getDoubleValue();
            r.numerator = f[meter].upToFirstOccurrenceOf ("-", false, false).getIntValue();
            r.denominator = f[meter].fromFirstOccurrenceOf ("-", false, false).getIntValue();
            r.drummerNumber = r.drummer.getTrailingIntValue();
            r.take = r.id.fromLastOccurrenceOf ("/", false, false).getIntValue();

            // The ten evaluation grooves are labelled "groove1" to "groove10";
            // those are numbers, not styles.
            if (r.substyle.startsWith ("groove"))
                r.substyle = {};

            if (r.midiFile.isEmpty() || r.midiFile.contains ("..") || r.bpm <= 0.0 || r.numerator <= 0 || r.denominator <= 0)
                continue;

            rows.push_back (std::move (r));
        }

        // In recording order: by drummer, session, then take.
        std::stable_sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b)
        {
            if (a.drummerNumber != b.drummerNumber)   return a.drummerNumber < b.drummerNumber;
            if (a.session != b.session)               return a.session.compareNatural (b.session) < 0;
            return a.take < b.take;
        });

        return juce::Result::ok();
    }

    juce::String meterOf (const Row& r)     { return juce::String (r.numerator) + "/" + juce::String (r.denominator); }
    juce::String idOf (const Row& r)        { return "gmd-" + r.id.replaceCharacter ('/', '-').replaceCharacter ('_', '-'); }
}

bool isGrooveMidiDataset (const juce::File& folder)
{
    return findRoot (folder) != juce::File();
}

juce::Result importGrooveMidiDataset (const ImportOptions& options, ImportedPack& result)
{
    auto log = [&options] (const juce::String& s) { if (options.log) options.log (s); };

    const auto root = findRoot (options.source);

    if (root == juce::File())
        return juce::Result::fail (options.source.getFullPathName() + " isn't the Groove MIDI Dataset (no info.csv and drummer folders)");

    std::vector<Row> rows;

    if (const auto r = readInfo (root.getChildFile ("info.csv"), rows); r.failed())
        return r;

    const auto folder = options.groovesFolder.getChildFile (packName);
    const auto manifest = folder.getChildFile ("grooves.json");

    if (manifest.existsAsFile() && ! options.overwrite)
        return juce::Result::fail (folder.getFullPathName() + " already exists (use --overwrite to replace it)");

    // The manifest goes first and comes back last, so a pack that's only half
    // written is never mistaken for a complete one.
    manifest.deleteFile();
    folder.getChildFile ("midi").deleteRecursively();

    if (const auto r = folder.createDirectory(); r.failed())
        return r;

    result = {};
    result.name = packName;
    result.folder = folder;

    // Each beat is a groove. A fill joins the groove it was recorded with:
    // the session's latest beat in the same style and tempo (the dataset
    // records a beat, then its fills), or failing that the next one.
    struct Take
    {
        const Row* row;
        int bars;
        std::vector<std::pair<const Row*, int>> fills;   // with their bars
    };

    std::vector<Take> takes;
    std::vector<std::pair<const Row*, int>> orphanFills;

    for (const auto& r : rows)
    {
        const auto source = root.getChildFile (r.midiFile);
        std::vector<Hit> hits;

        if (const auto read = readHits (source, pedalController, pedalClosedAt, hits); read.failed() || hits.empty())
        {
            result.warnings.add ("Skipped " + r.midiFile + (read.failed() ? ": " + read.getErrorMessage() : ": no notes"));
            continue;
        }

        const auto beatsPerBar = r.numerator * 4.0 / r.denominator;
        const auto bars = countBars (hits, beatsPerBar);

        const auto dest = folder.getChildFile ("midi").getChildFile (r.midiFile);

        if (! dest.getParentDirectory().createDirectory() || ! source.copyFileTo (dest))
            return juce::Result::fail ("Can't write " + dest.getFullPathName());

        if (r.beatType == "fill")
        {
            auto matches = [&r] (const Take& t) { return t.row->session == r.session && t.row->style == r.style
                                                         && t.row->substyle == r.substyle && juce::exactlyEqual (t.row->bpm, r.bpm); };

            if (auto it = std::find_if (takes.rbegin(), takes.rend(), matches); it != takes.rend())
                it->fills.push_back ({ &r, bars });
            else
                orphanFills.push_back ({ &r, bars });
        }
        else
        {
            takes.push_back ({ &r, bars, {} });
        }
    }

    // Fills recorded before their beat.
    for (const auto& [fill, bars] : orphanFills)
    {
        auto it = std::find_if (takes.begin(), takes.end(), [fill = fill] (const Take& t)
        {
            return t.row->session == fill->session && t.row->style == fill->style && juce::exactlyEqual (t.row->bpm, fill->bpm);
        });

        if (it != takes.end())
            it->fills.push_back ({ fill, bars });
        else
            result.warnings.add ("Left out " + fill->midiFile + ": no groove in its session to go with");
    }

    // Names: the style ("Funk 3"), or the substyle where there is one
    // ("Purdie Shuffle 2", "Halftime Rock 4"), numbered in recording order.
    std::map<juce::String, int> counts;
    juce::Array<juce::var> grooves;
    juce::DynamicObject::Ptr styles = new juce::DynamicObject();

    for (const auto& t : takes)
    {
        const auto& r = *t.row;
        const auto styleName = nameOf (styleNames, r.style);
        auto base = styleName;

        if (const auto compound = compoundNames.find (r.style + "/" + r.substyle); compound != compoundNames.end())
            base = compound->second;
        else if (r.substyle.isNotEmpty())
            base = nameOf (substyleNames, r.substyle);

        styles->setProperty (r.style, styleName);

        juce::DynamicObject::Ptr g = new juce::DynamicObject();
        g->setProperty ("id", idOf (r));
        g->setProperty ("name", base + " " + juce::String (++counts[base]));
        g->setProperty ("style", r.style);

        if (r.substyle.isNotEmpty())
            g->setProperty ("substyle", nameOf (substyleNames, r.substyle));

        g->setProperty ("drummer", "Drummer " + juce::String (r.drummerNumber));
        g->setProperty ("bpm", r.bpm);
        g->setProperty ("meter", meterOf (r));
        g->setProperty ("bars", t.bars);
        g->setProperty ("file", "midi/" + r.midiFile);

        juce::Array<juce::var> fills;

        for (const auto& [fill, bars] : t.fills)
        {
            juce::DynamicObject::Ptr f = new juce::DynamicObject();
            f->setProperty ("id", idOf (*fill));
            f->setProperty ("name", "Fill " + juce::String (fills.size() + 1));
            f->setProperty ("file", "midi/" + fill->midiFile);
            f->setProperty ("bars", bars);
            fills.add (f.get());
        }

        if (! fills.isEmpty())
            g->setProperty ("fills", fills);

        grooves.add (g.get());
        result.numGrooves++;
        result.numFills += (int) t.fills.size();
        result.numPhrases += (t.bars + options.phraseBars - 1) / options.phraseBars;
    }

    if (grooves.isEmpty())
        return juce::Result::fail ("No grooves found in " + root.getFullPathName());

    // The licence goes in as it came, as CC BY asks.
    const auto licence = root.getChildFile ("LICENSE");

    if (! licence.existsAsFile() || ! licence.copyFileTo (folder.getChildFile ("LICENSE.txt")))
        return juce::Result::fail ("Can't copy the dataset's LICENSE file");

    juce::DynamicObject::Ptr pack = new juce::DynamicObject();
    pack->setProperty ("format", "lithium-grooves");
    pack->setProperty ("version", 1);
    pack->setProperty ("id", "groove-midi-dataset");
    pack->setProperty ("name", packName);
    pack->setProperty ("author", "Jon Gillick, Adam Roberts, Jesse Engel, Douglas Eck and David Bamman (Google Magenta)");
    pack->setProperty ("sourceUrl", "https://magenta.tensorflow.org/datasets/groove");
    pack->setProperty ("description", juce::String (result.numGrooves) + " grooves and " + juce::String (result.numFills)
                                        + " fills played by ten drummers on a Roland TD-11 electronic kit, to a metronome.");
    pack->setProperty ("credits", "Groove MIDI Dataset by Jon Gillick, Adam Roberts, Jesse Engel, Douglas Eck and David Bamman, "
                                  "from \"Learning to Groove with Inverse Sequence Transformations\" (ICML 2019). "
                                  "Made available by Google LLC under the Creative Commons Attribution 4.0 International licence.");

    juce::DynamicObject::Ptr license = new juce::DynamicObject();
    license->setProperty ("name", "CC BY 4.0");
    license->setProperty ("url", "https://creativecommons.org/licenses/by/4.0/");
    license->setProperty ("file", "LICENSE.txt");
    pack->setProperty ("license", license.get());

    // The files are as the TD-11 recorded them, so they're read with the Roland map.
    pack->setProperty ("noteMap", "roland-vdrums");

    juce::DynamicObject::Ptr pedal = new juce::DynamicObject();
    pedal->setProperty ("controller", pedalController);
    pedal->setProperty ("closedAt", pedalClosedAt);
    pack->setProperty ("pedal", pedal.get());

    pack->setProperty ("phraseBars", options.phraseBars);
    pack->setProperty ("styles", styles.get());
    pack->setProperty ("grooves", grooves);

    if (! manifest.replaceWithText (juce::JSON::toString (juce::var (pack.get())) + "\n"))
        return juce::Result::fail ("Can't write " + manifest.getFullPathName());

    // Read it back, as the plugin will.
    if (Pack check; readPack (folder, check).failed() || (int) check.grooves.size() != result.numGrooves)
        return juce::Result::fail ("The written manifest doesn't read back: " + manifest.getFullPathName());

    log ("Wrote " + manifest.getFullPathName());
    return juce::Result::ok();
}

} // namespace lithium::grooves
