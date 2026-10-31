#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <map>

// The stream cache: a kit's audio, decoded once into a form that can be read
// straight off the disk while it plays.
//
// Kits keep their audio as the libraries ship it: multichannel 32-bit float
// WAVs for the DrumGizmo kits (13-16 mics interleaved in one file), FLAC for
// Virtuosity, and so on. Streaming one mic of a DrumGizmo hit from the
// original file would mean reading every mic's interleaved data, and FLAC
// can't be read from an arbitrary frame without decoding from the nearest
// seek point. So the first time a kit streams, its audio is decoded into a
// cache "pack": every file's channels, one after the other, each as one
// contiguous run of 16- or 24-bit little-endian integers. Reading frames
// [a, b) of one channel is then a single contiguous read with no decoding.
//
// A pack belongs to an audio folder, so a kit and the presets made from it
// share one. Each file's entry remembers the source file's size and
// modification time; a changed file is simply decoded again (appended). The
// cache can be deleted at any time and is rebuilt as kits are loaded.
//
//   <cache folder>/<audio folder name>-<hash>/
//       index.json   the entries, rewritten atomically after each append
//       audio.bin    the packed audio, only ever appended to

namespace lithium::streamcache
{

struct Entry
{
    juce::String path;                 // relative to the audio folder
    std::int64_t sourceSize = 0;       // of the source file when it was cached
    std::int64_t sourceTime = 0;       // its modification time, ms since 1970
    int numChannels = 0;
    int sourceBits = 0;                // 32 for floating-point sources
    int bytesPerSample = 3;            // 2 for sources of 16 bits or fewer, 3 otherwise
    std::int64_t numFrames = 0;
    std::int64_t offset = 0;           // where channel 0 starts in audio.bin; the other channels follow

    /** The byte offset of a frame of one channel in audio.bin. */
    std::int64_t offsetOf (int channel, std::int64_t frame) const noexcept
    {
        return offset + ((std::int64_t) channel * numFrames + frame) * bytesPerSample;
    }
};

/** One audio folder's pack: its index, read into memory. */
class Pack
{
public:
    /** The pack folder for an audio folder (whether or not it exists yet). */
    static juce::File folderFor (const juce::File& cacheFolder, const juce::File& audioFolder);

    /** Reads the pack's index. A missing or unreadable index gives an empty pack. */
    static Pack open (const juce::File& cacheFolder, const juce::File& audioFolder);

    /** The entry for a file, if it is cached and the source hasn't changed since. */
    const Entry* find (const juce::String& path) const;

    juce::File getDataFile() const      { return folder.getChildFile ("audio.bin"); }
    juce::File getFolder() const        { return folder; }

private:
    bool readIndex();
    bool writeIndex() const;

    friend bool cacheFiles (const juce::File&, const juce::File&, const juce::StringArray&, double, Pack&,
                            juce::String&, const std::function<bool()>&, const std::function<void (float)>&);

    juce::File folder, audioFolder;
    std::map<juce::String, Entry> entries;
};

/** Makes sure every file in paths is in the audio folder's pack, decoding
    and appending the ones that aren't (in parallel), and returns the updated
    pack. Files must be WAV or FLAC at expectedSampleRate.

    Safe against other processes doing the same: an inter-process lock is
    held while appending, and the index only ever lists audio that has been
    completely written. On failure or abort, nothing half-written is listed. */
bool cacheFiles (const juce::File& cacheFolder, const juce::File& audioFolder, const juce::StringArray& paths,
                 double expectedSampleRate, Pack& pack, juce::String& error,
                 const std::function<bool()>& shouldAbort = {}, const std::function<void (float)>& progress = {});

/** Bytes used by every pack in the cache folder. */
std::int64_t getCacheSize (const juce::File& cacheFolder);

/** Deletes every pack except the ones whose folders are in keep (packs that
    are streaming right now). Returns the number of packs deleted. */
int clear (const juce::File& cacheFolder, const juce::Array<juce::File>& keep);

} // namespace lithium::streamcache
