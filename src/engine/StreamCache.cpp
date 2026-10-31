#include "StreamCache.h"

#include "InMemorySampleSource.h"
#include "ParallelJobs.h"

#include <cstring>

namespace lithium::streamcache
{

namespace
{
    constexpr int indexVersion = 1;

    std::int64_t modificationTime (const juce::File& f)
    {
        return f.getLastModificationTime().toMilliseconds();
    }

    /** Decodes a whole file into the cache layout: channel after channel,
        each as packed little-endian integers. */
    bool decodeFile (juce::AudioFormatManager& formats, const juce::File& file, const juce::String& path,
                     double expectedRate, Entry& entry, std::vector<std::byte>& data, juce::String& error)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
        {
            error = "Can't read " + path + " (only WAV and FLAC are supported)";
            return false;
        }

        if (std::abs (reader->sampleRate - expectedRate) > 0.5)
        {
            error = path + " is " + juce::String (reader->sampleRate) + " Hz, but the kit is " + juce::String (expectedRate) + " Hz";
            return false;
        }

        const auto length = reader->lengthInSamples;
        const auto channels = (int) reader->numChannels;

        if (length <= 0 || length > std::numeric_limits<int>::max() || channels < 1)
        {
            error = path + " is empty or too long";
            return false;
        }

        entry.path = path;
        entry.sourceSize = file.getSize();
        entry.sourceTime = modificationTime (file);
        entry.numChannels = channels;
        entry.sourceBits = reader->usesFloatingPointData ? 32 : (int) reader->bitsPerSample;
        entry.bytesPerSample = entry.sourceBits <= 16 ? 2 : 3;
        entry.numFrames = length;

        const auto bytes = entry.bytesPerSample;
        data.resize ((size_t) channels * (size_t) length * (size_t) bytes);

        // The reader delivers fixed-point formats as full-scale 32-bit ints
        // (so a shift gives the stored depth) and floating-point formats as
        // float bit patterns in the same buffers.
        constexpr int chunk = 16384;
        std::vector<std::vector<int>> buffers ((size_t) channels, std::vector<int> (chunk));
        std::vector<int*> pointers;

        for (auto& b : buffers)
            pointers.push_back (b.data());

        const auto shift = 32 - bytes * 8;
        const auto isFloat = reader->usesFloatingPointData;
        const auto floatScale = bytes == 2 ? 32767.0f : 8388607.0f;

        for (std::int64_t pos = 0; pos < length; pos += chunk)
        {
            const auto n = (int) std::min<std::int64_t> (chunk, length - pos);

            if (! reader->read (pointers.data(), channels, pos, n, false))
            {
                error = "Error decoding " + path;
                return false;
            }

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* dest = data.data() + (size_t) entry.offsetOf (ch, pos);   // offset is still 0 here
                const auto* src = buffers[(size_t) ch].data();

                for (int i = 0; i < n; ++i, dest += bytes)
                {
                    int value;

                    if (isFloat)
                    {
                        float x;
                        std::memcpy (&x, src + i, sizeof (x));
                        value = (int) std::lrint (juce::jlimit (-1.0f, 1.0f, x) * floatScale);
                    }
                    else
                    {
                        value = src[i] >> shift;
                    }

                    SampleLayout::pack (dest, bytes, value);
                }
            }
        }

        return true;
    }
}

//==============================================================================
juce::File Pack::folderFor (const juce::File& cacheFolder, const juce::File& audioFolder)
{
    // The folder name is readable, and the hash of the full path keeps two
    // kits with the same folder name (in different packs) apart.
    const auto hash = juce::String::toHexString ((juce::int64) audioFolder.getFullPathName().hashCode64());
    const auto name = juce::File::createLegalFileName (audioFolder.getFileName()).substring (0, 60);
    return cacheFolder.getChildFile (name + "-" + hash);
}

Pack Pack::open (const juce::File& cacheFolder, const juce::File& audioFolder)
{
    Pack pack;
    pack.folder = folderFor (cacheFolder, audioFolder);
    pack.audioFolder = audioFolder;

    if (! pack.readIndex())
        pack.entries.clear();

    return pack;
}

const Entry* Pack::find (const juce::String& path) const
{
    const auto it = entries.find (path);

    if (it == entries.end())
        return nullptr;

    const auto source = audioFolder.getChildFile (path);

    if (source.getSize() != it->second.sourceSize || modificationTime (source) != it->second.sourceTime)
        return nullptr;     // changed since it was cached

    return &it->second;
}

bool Pack::readIndex()
{
    const auto json = juce::JSON::parse (folder.getChildFile ("index.json"));

    if ((int) json["version"] != indexVersion || json["audioFolder"].toString() != audioFolder.getFullPathName())
        return false;

    // Entries that point past the end of the data file (it was deleted or
    // truncated behind our back) are dropped, so they get cached again.
    const auto dataSize = getDataFile().getSize();

    if (const auto* list = json["files"].getArray())
    {
        for (const auto& v : *list)
        {
            Entry e;
            e.path = v["path"].toString();
            e.sourceSize = (juce::int64) v["size"];
            e.sourceTime = (juce::int64) v["time"];
            e.numChannels = (int) v["channels"];
            e.sourceBits = (int) v["bits"];
            e.bytesPerSample = (int) v["bytes"];
            e.numFrames = (juce::int64) v["frames"];
            e.offset = (juce::int64) v["offset"];

            const auto valid = e.path.isNotEmpty() && e.numChannels > 0 && e.numFrames > 0 && e.offset >= 0
                               && (e.bytesPerSample == 2 || e.bytesPerSample == 3)
                               && e.offsetOf (e.numChannels, 0) <= dataSize;

            if (valid)
                entries[e.path] = e;
        }
    }

    return true;
}

bool Pack::writeIndex() const
{
    juce::Array<juce::var> list;

    for (const auto& [path, e] : entries)
    {
        auto* o = new juce::DynamicObject();   // owned by the var it goes into (reference counted)
        o->setProperty ("path", e.path);
        o->setProperty ("size", (juce::int64) e.sourceSize);
        o->setProperty ("time", (juce::int64) e.sourceTime);
        o->setProperty ("channels", e.numChannels);
        o->setProperty ("bits", e.sourceBits);
        o->setProperty ("bytes", e.bytesPerSample);
        o->setProperty ("frames", (juce::int64) e.numFrames);
        o->setProperty ("offset", (juce::int64) e.offset);
        list.add (juce::var (o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", indexVersion);
    root->setProperty ("audioFolder", audioFolder.getFullPathName());
    root->setProperty ("files", list);

    // Written to a temporary file and moved into place in one rename, so a
    // reader (or a crash) never sees half an index.
    juce::TemporaryFile temp (folder.getChildFile ("index.json"));

    return temp.getFile().replaceWithText (juce::JSON::toString (juce::var (root), juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none)))
           && temp.overwriteTargetFileWithTemporary();
}

//==============================================================================
bool cacheFiles (const juce::File& cacheFolder, const juce::File& audioFolder, const juce::StringArray& paths,
                 double expectedSampleRate, Pack& pack, juce::String& error,
                 const std::function<bool()>& shouldAbort, const std::function<void (float)>& progress)
{
    const auto folder = Pack::folderFor (cacheFolder, audioFolder);

    if (! folder.createDirectory())
    {
        error = "Can't create the stream cache folder " + folder.getFullPathName();
        return false;
    }

    // Another instance, in this process or another host, may be filling the
    // same pack. Whoever gets the lock first caches the files; the other then
    // finds them in the index it re-reads below.
    // The mutex covers instances in this process: on POSIX the inter-process
    // lock is a per-process file lock, so it doesn't exclude our own threads.
    static std::mutex inProcess;
    const std::scoped_lock processLock (inProcess);
    juce::InterProcessLock lock ("LithiumDrumsCache-" + folder.getFileName());
    const juce::InterProcessLock::ScopedLockType scoped (lock);

    pack = Pack::open (cacheFolder, audioFolder);

    juce::StringArray missing;

    for (const auto& p : paths)
        if (pack.find (p) == nullptr)
            missing.addIfNotAlreadyThere (p);

    if (missing.isEmpty())
        return true;

    // FileOutputStream opens an existing file positioned at its end, so new
    // audio is appended after what's already cached.
    juce::FileOutputStream out (pack.getDataFile());

    if (out.failedToOpen())
    {
        error = "Can't write to the stream cache: " + out.getStatus().getErrorMessage();
        return false;
    }

    const auto startPosition = out.getPosition();
    std::mutex writeMutex;
    std::atomic<int> done { 0 };
    std::map<juce::String, Entry> added;

    const auto ok = runParallel ((size_t) missing.size(), shouldAbort, error,
                                 [&] (size_t i, juce::AudioFormatManager& formats, juce::String& jobError)
                                 {
                                     // Each worker decodes into its own buffer; only the
                                     // append itself is serialised.
                                     Entry entry;
                                     std::vector<std::byte> data;
                                     const auto& path = missing[(int) i];

                                     if (! decodeFile (formats, audioFolder.getChildFile (path), path, expectedSampleRate,
                                                       entry, data, jobError))
                                         return false;

                                     std::scoped_lock writeLock (writeMutex);
                                     entry.offset = out.getPosition();

                                     if (! out.write (data.data(), data.size()))
                                     {
                                         jobError = "Can't write to the stream cache (is the disk full?)";
                                         return false;
                                     }

                                     added[path] = entry;

                                     if (progress)
                                         progress ((float) ++done / (float) missing.size());

                                     return true;
                                 });

    out.flush();

    if (! ok || out.getStatus().failed())
    {
        if (error.isEmpty())
            error = "Can't write to the stream cache: " + out.getStatus().getErrorMessage();

        // Drop what this attempt appended; the index never listed it.
        out.setPosition (startPosition);
        out.truncate();
        return false;
    }

    for (auto& [path, entry] : added)
        pack.entries[path] = entry;     // replaces the entries of files that changed

    if (! pack.writeIndex())
    {
        error = "Can't write the stream cache index in " + folder.getFullPathName();
        return false;
    }

    return true;
}

std::int64_t getCacheSize (const juce::File& cacheFolder)
{
    std::int64_t total = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (cacheFolder, true, "*", juce::File::findFiles))
        total += entry.getFileSize();

    return total;
}

int clear (const juce::File& cacheFolder, const juce::Array<juce::File>& keep)
{
    int deleted = 0;

    for (const auto& pack : cacheFolder.findChildFiles (juce::File::findDirectories, false))
        if (! keep.contains (pack) && (pack.getChildFile ("index.json").existsAsFile() || pack.getChildFile ("audio.bin").existsAsFile())
            && pack.deleteRecursively())
            ++deleted;

    return deleted;
}

} // namespace lithium::streamcache
