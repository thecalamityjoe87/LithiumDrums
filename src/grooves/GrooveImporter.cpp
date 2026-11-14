#include "GrooveImporter.h"

#include "GrooveLibrary.h"

#include <cmath>
#include <map>
#include <set>
#include <utility>

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

//==============================================================================
namespace
{
    // At most this many files are imported, in case a very large folder (a
    // whole home folder, say) is picked by mistake.
    constexpr int maxMidiFiles = 50000;

    // Folder names that say what's in a folder rather than its style.
    bool isGenericFolder (const juce::String& name)
    {
        static const juce::StringArray generic { "midi", "midi files", "grooves", "groove", "beats", "beat", "fills", "fill",
                                                 "loops", "patterns", "drums", "drum", "files" };
        if (generic.contains (name.trim().toLowerCase()))
            return true;

        // A version number ("e-gmd-v1.0.0", "Pack 2.1"): the folder a
        // download unpacked into, not a style.
        for (int i = 1; i + 1 < name.length(); ++i)
            if (name[i] == '.' && juce::CharacterFunctions::isDigit (name[i - 1]) && juce::CharacterFunctions::isDigit (name[i + 1]))
                return true;

        return false;
    }

    // "Fill 03", "fills", "big_fill", "Fill-in": any word starting with "fill".
    bool hasFillWord (const juce::String& s)
    {
        juce::String word;

        for (auto c : s.toLowerCase() + " ")
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c))
            {
                word += c;
                continue;
            }

            if (word.startsWith ("fill"))
                return true;

            word.clear();
        }

        return false;
    }

    // Genre words, as keys (see keyOf) with how they're shown. Two-word
    // genres are matched first, so "death_metal" is Death Metal, not Metal.
    // The keys match the Groove MIDI Dataset's, so its styles merge.
    const std::vector<std::pair<juce::String, juce::String>> genrePairs {
        { "deathmetal", "Death Metal" }, { "blackmetal", "Black Metal" }, { "numetal", "Nu Metal" },
        { "altrock", "Alt Rock" }, { "alternativerock", "Alt Rock" }, { "poppunk", "Pop Punk" },
        { "hiphop", "Hip-Hop" }, { "neworleans", "New Orleans" }, { "afrocuban", "Afro-Cuban" },
        { "middleeastern", "Middle Eastern" }, { "drumbass", "Drum & Bass" },
    };

    const std::map<juce::String, juce::String> genreWords {
        { "rock", "Rock" }, { "metal", "Metal" }, { "punk", "Punk" }, { "djent", "Djent" }, { "thrash", "Thrash" },
        { "doom", "Doom" }, { "metalcore", "Metalcore" }, { "deathcore", "Deathcore" }, { "hardcore", "Hardcore" },
        { "grunge", "Grunge" }, { "emo", "Emo" }, { "ska", "Ska" }, { "indie", "Indie" }, { "alternative", "Alt Rock" },
        { "prog", "Prog" }, { "fusion", "Fusion" }, { "funk", "Funk" }, { "soul", "Soul" }, { "jazz", "Jazz" },
        { "swing", "Swing" }, { "blues", "Blues" }, { "country", "Country" }, { "gospel", "Gospel" }, { "pop", "Pop" },
        { "disco", "Disco" }, { "dance", "Dance" }, { "house", "House" }, { "techno", "Techno" }, { "trap", "Trap" },
        { "rnb", "R&B" }, { "motown", "Motown" }, { "reggae", "Reggae" }, { "latin", "Latin" }, { "samba", "Samba" },
        { "bossa", "Bossa Nova" }, { "afrobeat", "Afrobeat" }, { "highlife", "Highlife" }, { "hiphop", "Hip-Hop" },
        { "neworleans", "New Orleans" }, { "afrocuban", "Afro-Cuban" }, { "middleeastern", "Middle Eastern" },
        { "breakbeat", "Breakbeat" }, { "dnb", "Drum & Bass" },
    };

    // The first genre named in some text ("1_funk-groove1_138" -> Funk), or empty.
    juce::String genreIn (const juce::String& text)
    {
        juce::StringArray words;
        juce::String word;

        for (auto c : text.toLowerCase() + " ")
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c))
                word += c;
            else if (word.isNotEmpty())
                words.add (std::exchange (word, {}));
        }

        words.removeString ("and");   // "drum and bass"

        for (int i = 0; i < words.size(); ++i)
        {
            if (i + 1 < words.size())
                for (const auto& [key, shown] : genrePairs)
                    if (words[i] + words[i + 1] == key)
                        return shown;

            if (const auto it = genreWords.find (words[i]); it != genreWords.end())
                return it->second;
        }

        return {};
    }

    // "Alt Rock" -> "altrock": a style key, matching the Groove MIDI
    // Dataset's ("punk"), so the same style from two packs is one chip.
    juce::String keyOf (const juce::String& name)
    {
        juce::String key;

        for (auto c : name.toLowerCase())
            if (juce::CharacterFunctions::isLetterOrDigit (c))
                key += c;

        return key;
    }

    // "Metal/Fast/blast_01.mid" -> "metal-fast-blast-01": an id that stays
    // the same when the folder is imported again, so projects find their groove.
    juce::String slugOf (const juce::String& path)
    {
        juce::String slug;

        for (auto c : path.toLowerCase())
            slug += juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c) : juce::String ("-");

        while (slug.contains ("--"))
            slug = slug.replace ("--", "-");

        return slug.trimCharactersAtStart ("-").trimCharactersAtEnd ("-");
    }

    // The first tempo and time signature in the file.
    void readTempoAndMeter (const juce::File& f, double& bpm, int& numerator, int& denominator)
    {
        bpm = 120.0;
        numerator = denominator = 4;

        juce::FileInputStream in (f);
        juce::MidiFile file;

        if (in.failedToOpen() || ! file.readFrom (in, false))
            return;

        juce::MidiMessageSequence tempos, meters;
        file.findAllTempoEvents (tempos);
        file.findAllTimeSigEvents (meters);
        tempos.sort();
        meters.sort();

        if (tempos.getNumEvents() > 0)
            if (const auto secondsPerBeat = tempos.getEventPointer (0)->message.getTempoSecondsPerQuarterNote(); secondsPerBeat > 0.0)
                bpm = juce::jlimit (20.0, 400.0, std::round (6000.0 / secondsPerBeat) / 100.0);   // to 0.01 BPM

        if (meters.getNumEvents() > 0)
        {
            int n = 4, d = 4;
            meters.getEventPointer (0)->message.getTimeSignatureInfo (n, d);

            // Only meters a pack can hold (see GrooveLibrary.cpp's parseMeter).
            if (n >= 1 && n <= 32 && (d == 2 || d == 4 || d == 8 || d == 16 || d == 32))
            {
                numerator = n;
                denominator = d;
            }
        }
    }

    struct MidiItem
    {
        juce::String path;              // relative to the source, with '/'
        juce::String folder;            // path's folder, "" at the top
        juce::String group;             // folder without the generic names: "Metal/Fast"
        juce::String name, style, substyle;
        bool fill = false;
        double bpm = 120.0;
        int numerator = 4, denominator = 4, bars = 1;
        std::vector<size_t> fills;      // grooves only: the fills that go with it
    };
}

juce::File packFolderFor (const juce::File& groovesFolder, const juce::String& name)
{
    return groovesFolder.getChildFile (juce::File::createLegalFileName (name.trim()));
}

juce::Result importMidiFolder (const MidiFolderOptions& options, ImportedPack& result)
{
    auto log = [&options] (const juce::String& s) { if (options.log) options.log (s); };

    const auto& source = options.source;

    if (! source.isDirectory())
        return juce::Result::fail (source.getFullPathName() + " isn't a folder");

    const auto name = options.name.trim().isNotEmpty() ? options.name.trim() : source.getFileName();
    const auto folder = packFolderFor (options.groovesFolder, name);
    const auto manifest = folder.getChildFile ("grooves.json");

    // Reading a folder into itself (or a folder holding the grooves folder)
    // would copy the pack into the pack, and replacing it would delete the files being read.
    if (folder == source || folder.isAChildOf (source) || source.isAChildOf (folder) || source.isAChildOf (options.groovesFolder))
        return juce::Result::fail ("The MIDI folder and the grooves folder (" + options.groovesFolder.getFullPathName()
                                   + ") can't be inside one another");

    if (manifest.existsAsFile() && ! options.overwrite)
        return juce::Result::fail (folder.getFullPathName() + " already exists (use --overwrite to replace it)");

    // Every MIDI file, in folder order. Hidden files and folders are skipped.
    std::vector<juce::File> files;

    for (const auto& entry : juce::RangedDirectoryIterator (source, true, "*", juce::File::findFiles))
    {
        const auto& f = entry.getFile();
        const auto path = f.getRelativePathFrom (source).replaceCharacter ('\\', '/');

        if (! (f.hasFileExtension ("mid") || f.hasFileExtension ("midi")) || path.startsWithChar ('.') || path.contains ("/."))
            continue;

        if ((int) files.size() == maxMidiFiles)
        {
            result.warnings.add ("Stopped at " + juce::String (maxMidiFiles) + " files");
            break;
        }

        files.push_back (f);
    }

    std::sort (files.begin(), files.end(), [&source] (const juce::File& a, const juce::File& b)
    {
        return a.getRelativePathFrom (source).compareNatural (b.getRelativePathFrom (source)) < 0;
    });

    if (files.empty())
        return juce::Result::fail ("No MIDI files (.mid or .midi) in " + source.getFullPathName());

    manifest.deleteFile();
    folder.getChildFile ("midi").deleteRecursively();

    if (const auto r = folder.createDirectory(); r.failed())
        return r;

    const auto warnings = result.warnings;
    result = {};
    result.name = name;
    result.folder = folder;
    result.warnings = warnings;

    std::vector<MidiItem> items;

    for (const auto& f : files)
    {
        std::vector<Hit> hits;

        // CC4 from 0 (open) to 127 (closed), as most e-kits and drum programs send it.
        if (const auto read = readHits (f, 4, 127, hits); read.failed() || hits.empty())
        {
            // Short reasons: the file's own path is already in the message.
            const auto why = ! read.failed() ? juce::String ("no notes")
                            : read.getErrorMessage().contains ("SMPTE") ? juce::String ("timed in SMPTE frames, not beats")
                                                                         : juce::String ("not a MIDI file it can read");
            result.warnings.add ("Skipped " + f.getRelativePathFrom (source) + ": " + why);
            continue;
        }

        MidiItem item;
        item.path = f.getRelativePathFrom (source).replaceCharacter ('\\', '/');
        item.folder = item.path.contains ("/") ? item.path.upToLastOccurrenceOf ("/", false, false) : juce::String();
        item.name = f.getFileNameWithoutExtension().replaceCharacter ('_', ' ').trim();
        item.fill = hasFillWord (item.path);
        readTempoAndMeter (f, item.bpm, item.numerator, item.denominator);
        item.bars = countBars (hits, item.numerator * 4.0 / item.denominator);

        juce::StringArray folders;
        folders.addTokens (item.folder, "/", {});
        folders.removeEmptyStrings();

        for (int i = folders.size(); --i >= 0;)
            if (isGenericFolder (folders[i]))
                folders.remove (i);

        item.group = folders.joinIntoString ("/");

        // The style: the first folder that names a genre ("Death Metal",
        // "Punk Grooves"); else a genre in the file's name, for packs that
        // keep it there ("drummer1/session1/1_funk_80_beat.mid"); else the
        // first folder; else the pack's name.
        const auto genreFolder = std::find_if (folders.begin(), folders.end(), [] (const juce::String& dir) { return genreIn (dir).isNotEmpty(); });
        const auto genreInName = genreIn (f.getFileNameWithoutExtension());

        if (genreFolder != folders.end())
            item.style = *genreFolder;
        else if (genreInName.isNotEmpty())
            item.style = genreInName;
        else
            item.style = folders.isEmpty() ? name : folders[0];

        // The substyle: the last folder, where it isn't the style ("Metal/Fast" is Metal, Fast).
        if (genreFolder != folders.end() || genreInName.isEmpty())
            if (folders.size() > 1 && folders[folders.size() - 1] != item.style)
                item.substyle = folders[folders.size() - 1];

        const auto dest = folder.getChildFile ("midi").getChildFile (item.path);

        if (! dest.getParentDirectory().createDirectory() || ! f.copyFileTo (dest))
            return juce::Result::fail ("Can't write " + dest.getFullPathName());

        items.push_back (std::move (item));
    }

    // Each fill goes with the grooves in its folder, or the nearest folder
    // above with some. Folders are compared without the generic ones, so
    // "Metal/Fills/x.mid" goes with "Metal/Grooves/y.mid". Where some of
    // those grooves share the fill's style and tempo, only those count.
    //
    // A song-sized folder (a few grooves: verse, chorus, ...) gives the fill
    // to each of them. A bigger one gives it to the nearest groove before it
    // in file order (or after it, if none is before): a dataset recorded as
    // beat, fill, beat, fill would otherwise hand every fill to every groove.
    // A fill with no groove anywhere above is a groove itself.
    constexpr size_t maxShared = 8;
    std::vector<size_t> orphans;

    // The grooves in each folder, in file order, so a big import stays quick.
    std::map<juce::String, std::vector<size_t>> groovesIn;

    for (size_t g = 0; g < items.size(); ++g)
        if (! items[g].fill)
            groovesIn[items[g].group].push_back (g);

    for (size_t i = 0; i < items.size(); ++i)
    {
        if (! items[i].fill)
            continue;

        for (auto group = items[i].group;;)
        {
            auto matches = [&] (const MidiItem& g)
            {
                return g.style == items[i].style && juce::roundToInt (g.bpm) == juce::roundToInt (items[i].bpm);
            };

            std::vector<size_t> candidates, matching;

            if (const auto it = groovesIn.find (group); it != groovesIn.end())
                candidates = it->second;

            for (const auto g : candidates)
                if (matches (items[g]))
                    matching.push_back (g);

            if (! matching.empty())
                candidates = std::move (matching);

            if (candidates.size() > maxShared)
            {
                // The last candidate before the fill, else the first after it (candidates are in file order).
                const auto after = std::upper_bound (candidates.begin(), candidates.end(), i);
                candidates = { after != candidates.begin() ? *std::prev (after) : *after };
            }

            for (const auto g : candidates)
                items[g].fills.push_back (i);

            if (! candidates.empty())
                break;

            if (group.isEmpty())
            {
                orphans.push_back (i);
                break;
            }

            // One folder up ("Metal/Fast" -> "Metal" -> "").
            group = group.contains ("/") ? group.upToLastOccurrenceOf ("/", false, false) : juce::String();
        }
    }

    for (const auto i : orphans)
        items[i].fill = false;

    juce::Array<juce::var> grooves;
    juce::DynamicObject::Ptr styles = new juce::DynamicObject();
    std::set<size_t> fillsUsed;

    for (const auto& item : items)
    {
        if (item.fill)
            continue;

        const auto styleKey = keyOf (item.style).isNotEmpty() ? keyOf (item.style) : juce::String ("imported");
        styles->setProperty (styleKey, item.style);

        juce::DynamicObject::Ptr g = new juce::DynamicObject();
        g->setProperty ("id", slugOf (item.path));
        g->setProperty ("name", item.name);
        g->setProperty ("style", styleKey);

        if (item.substyle.isNotEmpty())
            g->setProperty ("substyle", item.substyle);

        g->setProperty ("bpm", item.bpm);
        g->setProperty ("meter", juce::String (item.numerator) + "/" + juce::String (item.denominator));
        g->setProperty ("bars", item.bars);
        g->setProperty ("file", "midi/" + item.path);

        juce::Array<juce::var> fills;

        for (const auto f : item.fills)
        {
            juce::DynamicObject::Ptr fill = new juce::DynamicObject();
            fill->setProperty ("id", slugOf (items[f].path));
            fill->setProperty ("name", items[f].name);
            fill->setProperty ("file", "midi/" + items[f].path);
            fill->setProperty ("bars", items[f].bars);
            fills.add (fill.get());
            fillsUsed.insert (f);
        }

        if (! fills.isEmpty())
            g->setProperty ("fills", fills);

        grooves.add (g.get());
        result.numGrooves++;
        result.numPhrases += (item.bars + options.phraseBars - 1) / options.phraseBars;
    }

    result.numFills = (int) fillsUsed.size();

    if (grooves.isEmpty())
        return juce::Result::fail ("None of the MIDI files in " + source.getFullPathName() + " has any notes");

    juce::DynamicObject::Ptr pack = new juce::DynamicObject();
    pack->setProperty ("format", "lithium-grooves");
    pack->setProperty ("version", 1);
    pack->setProperty ("id", "midi-" + slugOf (name));
    pack->setProperty ("name", name);
    pack->setProperty ("description", juce::String (result.numGrooves) + " grooves and " + juce::String (result.numFills)
                                        + " fills imported from your own MIDI files.");
    pack->setProperty ("noteMap", options.noteMap);

    juce::DynamicObject::Ptr pedal = new juce::DynamicObject();
    pedal->setProperty ("controller", 4);
    pedal->setProperty ("closedAt", 127);
    pack->setProperty ("pedal", pedal.get());

    pack->setProperty ("phraseBars", options.phraseBars);
    pack->setProperty ("styles", styles.get());
    pack->setProperty ("grooves", grooves);

    if (! manifest.replaceWithText (juce::JSON::toString (juce::var (pack.get())) + "\n"))
        return juce::Result::fail ("Can't write " + manifest.getFullPathName());

    if (Pack check; readPack (folder, check).failed() || (int) check.grooves.size() != result.numGrooves)
        return juce::Result::fail ("The written manifest doesn't read back: " + manifest.getFullPathName());

    log ("Wrote " + manifest.getFullPathName());
    return juce::Result::ok();
}

} // namespace lithium::grooves
