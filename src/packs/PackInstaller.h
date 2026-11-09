#pragma once

#include <juce_events/juce_events.h>

#include <map>
#include <mutex>
#include <optional>
#include <vector>

// LithiumPacks: the kit and groove libraries Lithium Drums knows how to get.
//
// The catalog (assets/packs/catalog.json, built into the plugin) lists each
// library: where to download it, its licence and credits, and the kits or
// grooves it becomes. The kit browser and the Grooves page show packs that
// aren't installed yet greyed out, with a Download button. The Installer
// does the rest on a background thread: it downloads the archives (resuming
// an interrupted download), unpacks them, runs the same importers as
// LithiumImport, and deletes the downloads again. The imported audio is
// hard-linked from the unpacked files, which share the kits folder's drive,
// so deleting them frees the space without touching the kits.

namespace lithium::packs
{

struct CatalogPack
{
    enum class Type { kits, grooves };

    struct Download
    {
        juce::String url, file;          // file: the name to save it as (default: the URL's last part)
        std::int64_t size = 0;           // exact size in bytes, 0 = unknown
        std::int64_t approximateSize = 0;
    };

    juce::String id, name, author, licenseName, licenseUrl, homepage, description;
    Type type = Type::kits;
    bool shareAlike = false;
    juce::StringArray kits, presets;     // what it becomes, for the greyed-out cards
    std::vector<Download> downloads;
    std::int64_t unpackedSize = 0;
    juce::String importFrom;             // the folder inside the unpacked archives to import

    std::int64_t downloadSize() const;   // exact or approximate
};

/** The built-in catalog. */
const std::vector<CatalogPack>& catalog();
const CatalogPack* findPack (const juce::String& id);

/** The kit pack that installs a kit or kit preset of this name, if any. */
const CatalogPack* findPackWithKit (const juce::String& kitName);

/** "2.8 GB", "3 MB". */
juce::String formatSize (std::int64_t bytes);

//==============================================================================
/** Downloads and installs packs, one at a time, on a thread of its own.
    One per process: get it through juce::SharedResourcePointer<Installer>,
    so every plugin instance shares it and sees the same progress.

    The message thread calls install() and cancel() and reads getStatus();
    listeners hear (asynchronously) when a pack changes phase. While
    something is installing, the UI polls getStatus() for the progress. */
class Installer final : public juce::ChangeBroadcaster, private juce::Thread
{
public:
    Installer();
    ~Installer() override;

    enum class Phase { queued, downloading, unpacking, importing, done, failed, cancelled };

    struct Status
    {
        Phase phase = Phase::queued;
        double progress = 0.0;                 // 0..1 within the phase
        std::int64_t bytesDone = 0, bytesTotal = 0;
        double bytesPerSecond = 0.0;
        juce::String message;                  // an error, or what's happening

        bool isActive() const noexcept         { return phase == Phase::queued || phase == Phase::downloading
                                                         || phase == Phase::unpacking || phase == Phase::importing; }
    };

    /** Queues a pack. Kit packs go into kitsFolder, groove packs into
        groovesFolder. Does nothing if it's already queued or installing. */
    void install (const juce::String& packId, const juce::File& kitsFolder, const juce::File& groovesFolder);

    void cancel (const juce::String& packId);

    /** Nothing if the pack hasn't been asked for since the plugin started. */
    std::optional<Status> getStatus (const juce::String& packId) const;

    bool isBusy() const;

    /** Installs a pack on the calling thread, for LithiumImport and the
        tests. log gets each step. */
    static juce::Result installNow (const CatalogPack&, const juce::File& kitsFolder, const juce::File& groovesFolder,
                                    const std::function<void (const juce::String&)>& log);

    /** The folder downloads and unpacked files go to while a pack installs:
        a hidden folder in the kits folder, so they're on the same drive and
        the imported audio can be hard-linked rather than copied. */
    static juce::File workFolder (const juce::File& kitsFolder, const juce::String& packId);

private:
    struct Job
    {
        juce::String packId;
        juce::File kitsFolder, groovesFolder;
    };

    struct Context;
    static juce::Result run (const CatalogPack&, const juce::File& kitsFolder, const juce::File& groovesFolder, Context&);

    void run() override;
    void setStatus (const juce::String& packId, const Status&);

    mutable std::mutex lock;                     // guards everything below
    std::vector<Job> queue;
    std::map<juce::String, Status> statuses;
    juce::String cancelledPack;
    juce::WaitableEvent wakeUp;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Installer)
};

} // namespace lithium::packs
