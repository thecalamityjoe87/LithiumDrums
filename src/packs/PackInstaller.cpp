#include "PackInstaller.h"

#include "Archive.h"
#include "grooves/GrooveImporter.h"
#include "importer/Importer.h"

#include <LithiumAssets.h>

namespace lithium::packs
{

namespace
{
    std::vector<CatalogPack> parseCatalog()
    {
        const auto root = juce::JSON::parse (juce::String::fromUTF8 (LithiumAssets::catalog_json, LithiumAssets::catalog_jsonSize));
        std::vector<CatalogPack> packs;

        auto strings = [] (const juce::var& v)
        {
            juce::StringArray result;

            if (const auto* a = v.getArray())
                for (const auto& s : *a)
                    result.add (s.toString());

            return result;
        };

        if (const auto* list = root["packs"].getArray())
        {
            for (const auto& p : *list)
            {
                CatalogPack pack;
                pack.id = p["id"].toString();
                pack.name = p["name"].toString();
                pack.type = p["type"].toString() == "grooves" ? CatalogPack::Type::grooves : CatalogPack::Type::kits;
                pack.author = p["author"].toString();
                pack.licenseName = p["license"]["name"].toString();
                pack.licenseUrl = p["license"]["url"].toString();
                pack.shareAlike = (bool) p["license"]["shareAlike"];
                pack.homepage = p["homepage"].toString();
                pack.description = p["description"].toString();
                pack.kits = strings (p["kits"]);
                pack.presets = strings (p["presets"]);
                pack.unpackedSize = (std::int64_t) (juce::int64) p["unpackedSize"];
                pack.importFrom = p["importFrom"].toString();

                if (const auto* downloads = p["downloads"].getArray())
                {
                    for (const auto& d : *downloads)
                    {
                        CatalogPack::Download download;
                        download.url = d["url"].toString();
                        download.file = d["file"].toString();
                        download.size = (std::int64_t) (juce::int64) d["size"];
                        download.approximateSize = (std::int64_t) (juce::int64) d["approximateSize"];

                        // The name to save it as: the URL's last part, unescaped ("%20" back to a space).
                        if (download.file.isEmpty())
                            download.file = juce::URL::removeEscapeChars (download.url.fromLastOccurrenceOf ("/", false, false)
                                                                                     .upToFirstOccurrenceOf ("?", false, false));

                        download.file = juce::File::createLegalFileName (download.file);
                        pack.downloads.push_back (std::move (download));
                    }
                }

                packs.push_back (std::move (pack));
            }
        }

        return packs;
    }

    /** Where the files to import are: the folder the pack names, or else the
        unpacked folder, stepping into it while it holds nothing but one
        folder (most archives wrap everything in one). */
    juce::File importSource (const CatalogPack& pack, const juce::File& unpacked)
    {
        if (pack.importFrom.isNotEmpty())
            return unpacked.getChildFile (pack.importFrom);

        auto folder = unpacked;

        for (;;)
        {
            const auto children = folder.findChildFiles (juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false);

            if (children.size() != 1 || ! children[0].isDirectory())
                return folder;

            folder = children[0];
        }
    }
}

std::int64_t CatalogPack::downloadSize() const
{
    std::int64_t total = 0;

    for (const auto& d : downloads)
        total += d.size > 0 ? d.size : d.approximateSize;

    return total;
}

const std::vector<CatalogPack>& catalog()
{
    // Parsed once, the first time it's asked for. A function-local static is
    // initialised thread-safely, and lives until the plugin is unloaded.
    static const auto packs = parseCatalog();
    return packs;
}

const CatalogPack* findPack (const juce::String& id)
{
    for (const auto& p : catalog())
        if (p.id == id)
            return &p;

    return nullptr;
}

juce::String formatSize (std::int64_t bytes)
{
    const auto mb = (double) bytes / 1.0e6;   // decimal, as download sites quote sizes

    if (mb >= 1000.0)
        return juce::String (mb / 1000.0, 1) + " GB";

    return juce::String (std::max (1, juce::roundToInt (mb))) + " MB";
}

//==============================================================================
struct Installer::Context
{
    std::function<void (Phase, double progress, std::int64_t done, std::int64_t total, double speed, const juce::String&)> report;
    std::function<bool()> cancelled;
    std::function<void (const juce::String&)> log;
};

Installer::Installer() : juce::Thread ("Lithium pack installer")
{
    startThread (juce::Thread::Priority::background);
}

Installer::~Installer()
{
    // Stops a download at once (its partial file is kept, and carried on
    // from next time). An import in progress finishes first; it doesn't take long.
    {
        std::scoped_lock l (lock);
        queue.clear();

        for (auto& [id, status] : statuses)
            if (status.isActive())
                cancelledPack = id;
    }

    signalThreadShouldExit();
    wakeUp.signal();
    stopThread (120'000);
}

void Installer::install (const juce::String& packId, const juce::File& kitsFolder, const juce::File& groovesFolder)
{
    {
        std::scoped_lock l (lock);

        if (const auto it = statuses.find (packId); it != statuses.end() && it->second.isActive())
            return;

        queue.push_back ({ packId, kitsFolder, groovesFolder });
        statuses[packId] = Status {};
    }

    wakeUp.signal();
    sendChangeMessage();
}

void Installer::cancel (const juce::String& packId)
{
    {
        std::scoped_lock l (lock);

        // A queued pack just leaves the queue; the running one is told to stop.
        const auto queued = std::find_if (queue.begin(), queue.end(), [&] (const Job& j) { return j.packId == packId; });

        if (queued != queue.end())
        {
            queue.erase (queued);
            statuses[packId].phase = Phase::cancelled;
        }
        else
        {
            cancelledPack = packId;
        }
    }

    sendChangeMessage();
}

std::optional<Installer::Status> Installer::getStatus (const juce::String& packId) const
{
    std::scoped_lock l (lock);

    if (const auto it = statuses.find (packId); it != statuses.end())
        return it->second;

    return std::nullopt;
}

bool Installer::isBusy() const
{
    std::scoped_lock l (lock);
    return std::any_of (statuses.begin(), statuses.end(), [] (const auto& s) { return s.second.isActive(); });
}

void Installer::setStatus (const juce::String& packId, const Status& status)
{
    bool phaseChanged = false;

    {
        std::scoped_lock l (lock);
        auto& s = statuses[packId];
        phaseChanged = s.phase != status.phase;
        s = status;
    }

    // Progress is polled; listeners only hear about new phases.
    if (phaseChanged)
        sendChangeMessage();
}

juce::File Installer::workFolder (const juce::File& kitsFolder, const juce::String& packId)
{
    return kitsFolder.getChildFile (".downloads").getChildFile (packId);
}

void Installer::run()
{
    while (! threadShouldExit())
    {
        std::optional<Job> job;

        {
            std::scoped_lock l (lock);

            if (! queue.empty())
            {
                job = queue.front();
                queue.erase (queue.begin());
                cancelledPack = {};
            }
        }

        if (! job.has_value())
        {
            wakeUp.wait (-1);
            continue;
        }

        const auto* pack = findPack (job->packId);

        if (pack == nullptr)
        {
            setStatus (job->packId, { Phase::failed, 0.0, 0, 0, 0.0, "Unknown pack" });
            continue;
        }

        Context context;
        context.cancelled = [this, id = job->packId]
        {
            std::scoped_lock l (lock);
            return threadShouldExit() || cancelledPack == id;
        };
        context.report = [this, id = job->packId] (Phase phase, double progress, std::int64_t done, std::int64_t total, double speed,
                                                   const juce::String& message)
        {
            setStatus (id, { phase, progress, done, total, speed, message });
        };
        context.log = [] (const juce::String&) {};

        const auto result = run (*pack, job->kitsFolder, job->groovesFolder, context);

        if (result.wasOk())
            setStatus (job->packId, { Phase::done, 1.0, 0, 0, 0.0, "Installed" });
        else if (context.cancelled())
            setStatus (job->packId, { Phase::cancelled, 0.0, 0, 0, 0.0, "Cancelled" });
        else
            setStatus (job->packId, { Phase::failed, 0.0, 0, 0, 0.0, result.getErrorMessage() });
    }
}

juce::Result Installer::installNow (const CatalogPack& pack, const juce::File& kitsFolder, const juce::File& groovesFolder,
                                    const std::function<void (const juce::String&)>& log)
{
    Context context;
    context.cancelled = [] { return false; };
    context.log = log;

    // On a terminal, a line per phase and per 10%.
    context.report = [log, last = -1] (Phase phase, double progress, std::int64_t done, std::int64_t total, double speed,
                                       const juce::String& message) mutable
    {
        const auto step = (int) phase * 100 + (int) (progress * 10.0);

        if (step == last)
            return;

        last = step;
        juce::String line = message;

        if (phase == Phase::downloading && total > 0)
            line << "  " << formatSize (done) << " of " << formatSize (total) << ", " << formatSize ((std::int64_t) speed) << "/s";
        else if (phase != Phase::importing)
            line << "  " << juce::roundToInt (progress * 100.0) << "%";

        log (line);
    };

    return run (pack, kitsFolder, groovesFolder, context);
}

//==============================================================================
juce::Result Installer::run (const CatalogPack& pack, const juce::File& kitsFolder, const juce::File& groovesFolder, Context& context)
{
    // Two installs of the same pack (the standalone app and a plugin in a DAW, say) would trip over each other's files.
    juce::InterProcessLock processLock ("LithiumDrumsInstall_" + pack.id);

    if (! processLock.enter (0))
        return juce::Result::fail ("Another Lithium Drums is installing " + pack.name + " already");

    const auto work = workFolder (kitsFolder, pack.id);

    if (const auto r = work.createDirectory(); r.failed())
        return juce::Result::fail ("Can't create " + work.getFullPathName() + ": " + r.getErrorMessage());

    // Room for the downloads still to come, plus everything unpacked. The
    // import itself takes next to nothing: it hard-links the unpacked audio.
    std::int64_t toDownload = 0;

    for (const auto& d : pack.downloads)
        if (const auto f = work.getChildFile (d.file); d.size <= 0 || f.getSize() != d.size)
            toDownload += std::max<std::int64_t> (0, (d.size > 0 ? d.size : d.approximateSize) - work.getChildFile (d.file + ".part").getSize());

    const auto needed = toDownload + pack.unpackedSize + pack.unpackedSize / 20 + (64 << 20);

    if (const auto free = work.getBytesFreeOnVolume(); free > 0 && free < needed)
        return juce::Result::fail ("Not enough disk space: " + pack.name + " needs " + formatSize (needed) + " while it installs, and "
                                   + formatSize (free) + " is free");

    //==========================================================================
    // Download. Sizes for progress: what's known exactly, or roughly.
    const auto grandTotal = std::max<std::int64_t> (1, pack.downloadSize());
    std::int64_t before = 0;   // bytes of the downloads before this one

    for (const auto& d : pack.downloads)
    {
        const auto dest = work.getChildFile (d.file);
        const auto part = work.getChildFile (d.file + ".part");
        const auto expected = d.size > 0 ? d.size : d.approximateSize;

        if (d.size > 0 && dest.getSize() == d.size)
        {
            before += d.size;
            continue;   // downloaded already, by an earlier try
        }

        if (d.size <= 0 && dest.existsAsFile())
        {
            before += dest.getSize();
            continue;
        }

        const juce::URL url (d.url);
        auto offset = part.getSize();

        if (d.size > 0 && offset > d.size)
        {
            part.deleteFile();
            offset = 0;
        }

        int statusCode = 0;

        // An interrupted download carries on where it stopped, if the server can.
        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs (30'000)
                                 .withNumRedirectsToFollow (10)
                                 .withStatusCode (&statusCode)
                                 .withExtraHeaders (offset > 0 ? "Range: bytes=" + juce::String (offset) + "-" : juce::String());

        context.report (Phase::downloading, (double) (before + offset) / (double) grandTotal, before + offset, grandTotal, 0.0,
                        "Connecting to " + url.getDomain());
        auto in = url.createInputStream (options);

        if (in == nullptr)
            return juce::Result::fail ("Couldn't reach " + url.getDomain() + ". Check your internet connection and try again.");

        if (statusCode >= 400)
            return juce::Result::fail (url.getDomain() + " answered with error " + juce::String (statusCode) + " for " + d.file);

        // 206 is the rest of the file, as asked. Anything else (200, or 0 for
        // a local file) is all of it, so the partial file starts over.
        if (statusCode != 206)
        {
            part.deleteFile();
            offset = 0;
        }

        auto done = offset;

        {
            juce::FileOutputStream out (part, 1 << 20);   // opens at the end, so a resumed download appends

            if (out.failedToOpen())
                return juce::Result::fail ("Can't write " + part.getFullPathName());

            juce::HeapBlock<char> buffer (1 << 18);
            auto lastTime = juce::Time::getMillisecondCounterHiRes();
            auto lastDone = done;
            auto speed = 0.0;

            for (;;)
            {
                if (context.cancelled())
                    return juce::Result::fail ("Cancelled");

                const auto got = in->read (buffer, 1 << 18);

                if (got <= 0)
                    break;

                if (! out.write (buffer, (size_t) got))
                    return juce::Result::fail ("Can't write " + part.getFullPathName() + " (is the disk full?)");

                done += got;

                // The speed, smoothed over half-second steps.
                if (const auto now = juce::Time::getMillisecondCounterHiRes(); now - lastTime > 500.0)
                {
                    const auto instant = (double) (done - lastDone) * 1000.0 / (now - lastTime);
                    speed = speed <= 0.0 ? instant : speed * 0.7 + instant * 0.3;
                    lastTime = now;
                    lastDone = done;
                    context.report (Phase::downloading, (double) (before + done) / (double) grandTotal, before + done, grandTotal, speed,
                                    "Downloading " + pack.name);
                }
            }

            out.flush();

            if (out.getStatus().failed())
                return out.getStatus();
        }   // the file is closed here, before it's renamed

        if (d.size > 0 && done != d.size)
            return juce::Result::fail ("The download of " + d.file + " stopped early (" + formatSize (done) + " of "
                                       + formatSize (d.size) + "). Try again to carry on from there.");

        if (! part.moveFileTo (dest))
            return juce::Result::fail ("Can't rename " + part.getFullPathName());

        before += d.size > 0 ? d.size : std::max<std::int64_t> (done, expected);
    }

    //==========================================================================
    // Unpack, each archive weighted by its size.
    const auto unpacked = work.getChildFile ("unpacked");
    unpacked.deleteRecursively();
    unpacked.createDirectory();

    std::int64_t unpackTotal = 0, unpackDone = 0;

    for (const auto& d : pack.downloads)
        unpackTotal += work.getChildFile (d.file).getSize();

    for (const auto& d : pack.downloads)
    {
        const auto file = work.getChildFile (d.file);
        const auto size = file.getSize();

        if (! isArchive (file))
        {
            // A licence PDF and the like: kept beside what's unpacked.
            if (! file.copyFileTo (unpacked.getChildFile (d.file)))
                return juce::Result::fail ("Can't copy " + d.file);

            unpackDone += size;
            continue;
        }

        const auto r = extract (file, unpacked, [&] (double p)
        {
            context.report (Phase::unpacking, ((double) unpackDone + p * (double) size) / (double) std::max<std::int64_t> (1, unpackTotal),
                            0, 0, 0.0, "Unpacking " + pack.name);
            return ! context.cancelled();
        });

        if (r.failed())
        {
            unpacked.deleteRecursively();

            // A damaged download is deleted, so trying again fetches it afresh.
            if (r.getErrorMessage().contains ("damaged"))
                file.deleteFile();

            return r;
        }

        unpackDone += size;
    }

    //==========================================================================
    // Import, as LithiumImport would. A pack that's installed already is replaced.
    context.report (Phase::importing, 0.0, 0, 0, 0.0, "Importing " + pack.name);
    const auto source = importSource (pack, unpacked);
    juce::Result result = juce::Result::ok();

    if (pack.type == CatalogPack::Type::grooves)
    {
        grooves::ImportOptions options;
        options.source = source;
        options.groovesFolder = groovesFolder;
        options.overwrite = true;
        options.log = context.log;
        grooves::ImportedPack imported;
        result = grooves::importGrooveMidiDataset (options, imported);
    }
    else
    {
        importer::Options options;
        options.source = source;
        options.kitsFolder = kitsFolder;
        options.overwrite = true;
        options.log = [&] (const juce::String& line)
        {
            context.log (line);
            context.report (Phase::importing, 0.0, 0, 0, 0.0, line);
        };

        std::vector<importer::ImportedKit> kits;
        result = importer::importKits (options, kits);
    }

    // The downloads have done their job either way. The kits' audio is
    // hard-linked from the unpacked files, so it stays when they go; where
    // linking wasn't possible, the importer copied it instead.
    unpacked.deleteRecursively();

    if (result.wasOk())
    {
        work.deleteRecursively();

        if (const auto downloads = work.getParentDirectory(); downloads.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            downloads.deleteFile();
    }

    return result;
}

} // namespace lithium::packs
