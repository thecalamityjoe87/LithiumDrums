#pragma once

#include "Kit.h"
#include "SpscQueue.h"

#include <atomic>
#include <memory>

namespace lithium
{

/** Moves finished kits from the loader thread to the audio thread, and old
    kits back again for deletion, without locks. It does the same for mic
    stores when a muted mic is unloaded or reloaded.

    Kits:
      - The loader publishes a new kit by exchanging it into a single atomic
        pointer, the "mailbox". If the mailbox already held a kit that the
        audio thread never collected, the exchange hands that one back to the
        loader, which deletes it.
      - The audio thread empties the mailbox with an atomic exchange. Whoever
        receives a pointer from an exchange owns that kit, so exactly one
        thread owns each kit at any moment, and no thread can see a
        half-built one: the exchange publishes everything the loader wrote
        before it (release/acquire ordering).
      - The audio thread must never free memory, so a replaced kit goes into
        a lock-free "retired" queue. The loader thread deletes kits from that
        queue.

    Mic stores:
      - The loader sends { kit serial, mic, store } through a single-producer,
        single-consumer queue. A null store means "unload this mic".
      - The audio thread swaps the store into the matching kit and sends the
        store it replaced back through another queue for deletion. If the kit
        is gone by then, the new store goes straight back instead.
      - The loader only sends mic stores for the kit the audio thread is
        currently playing (getCurrentSerial()), so they can't miss their kit.

    std::atomic<std::shared_ptr> would read more simply, but on the major
    standard libraries it is implemented with an internal lock, and dropping
    the last reference on the audio thread would free a whole kit there. */
class KitHandoff
{
public:
    KitHandoff() = default;

    /** Deletes everything still held. Only call once no other thread uses this. */
    ~KitHandoff()
    {
        delete pending.exchange (nullptr);
        delete incoming;
        delete current;

        MicUpdate update;

        while (micUpdates.pop (update))
            delete update.store;

        collectGarbage();
    }

    //==============================================================================
    // Loader thread

    void publish (std::unique_ptr<Kit> kit)
    {
        delete pending.exchange (kit.release(), std::memory_order_acq_rel);
    }

    /** Sends a new store for one mic (null to unload it). Returns false, and
        gives the store back, if the queue is full; try again later. */
    bool sendMicStore (std::uint64_t kitSerial, int mic, std::unique_ptr<MicStore>& store)
    {
        if (! micUpdates.push ({ kitSerial, mic, store.get() }))
            return false;

        store.release();   // the queue owns it now
        return true;
    }

    void collectGarbage()
    {
        Kit* k = nullptr;

        while (retired.pop (k))
            delete k;

        MicStore* s = nullptr;

        while (retiredStores.pop (s))
            delete s;
    }

    /** The serial of the kit the audio thread is playing (0 = none yet). */
    std::uint64_t getCurrentSerial() const noexcept     { return currentSerial.load (std::memory_order_acquire); }

    //==============================================================================
    // Audio thread (or any thread while the audio thread is known to be stopped)

    /** Call at the start of each block. Returns true while a new kit is
        waiting to replace the current one. The caller fades out its voices
        and then calls commitIncoming() once nothing uses the current kit. */
    bool pollIncoming() noexcept
    {
        // Need room to retire both the kit being replaced and any older incoming one.
        if (retired.getFreeSpace() >= 2)
        {
            if (auto* k = pending.exchange (nullptr, std::memory_order_acq_rel))
            {
                if (incoming != nullptr)
                    retired.push (incoming);

                incoming = k;
            }
        }

        return incoming != nullptr;
    }

    void commitIncoming() noexcept
    {
        jassert (incoming != nullptr);

        if (current != nullptr)
            retired.push (current);

        current = incoming;
        incoming = nullptr;
        currentSerial.store (current->serial, std::memory_order_release);
    }

    /** Applies mic loads and unloads that have arrived. Call once per block. */
    void applyMicUpdates() noexcept
    {
        MicUpdate update;

        // Only take an update when its replaced store is sure to fit in the
        // retired queue, so nothing is ever dropped (and leaked).
        while (retiredStores.getFreeSpace() > 0 && micUpdates.pop (update))
        {
            std::unique_ptr<MicStore> store (update.store);

            if (current != nullptr && current->serial == update.kitSerial)
                store = current->samples->swapStore (update.mic, std::move (store));

            if (store != nullptr)
                retiredStores.push (store.release());
        }
    }

    Kit* getCurrent() const noexcept    { return current; }

private:
    struct MicUpdate
    {
        std::uint64_t kitSerial = 0;
        int mic = 0;
        MicStore* store = nullptr;   // owned by whichever queue holds the update
    };

    std::atomic<Kit*> pending { nullptr };
    Kit* incoming = nullptr;   // audio thread only
    Kit* current = nullptr;    // audio thread only
    std::atomic<std::uint64_t> currentSerial { 0 };
    SpscQueue<Kit*, 8> retired;
    SpscQueue<MicUpdate, 64> micUpdates;
    SpscQueue<MicStore*, 64> retiredStores;
};

} // namespace lithium
