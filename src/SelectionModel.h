#pragma once

#include <juce_events/juce_events.h>

#include <atomic>

namespace lithium
{

/** Which piece of the loaded kit is selected (an index into the kit's
    pieces, or -1 for none). The stage, the piece strips and the overlay card
    all observe this one object, so they can never disagree.

    The processor owns it (rather than the editor) so the selection survives
    closing and reopening the plugin window. Changes happen on the message
    thread; the value itself is atomic only because some hosts save state
    from a background thread. */
class SelectionModel : private juce::AsyncUpdater
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void selectedPieceChanged (int piece) = 0;
    };

    int getSelected() const noexcept         { return selected.load(); }

    void select (int piece)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (piece == selected.exchange (piece))
            return;

        listeners.call ([piece] (Listener& l) { l.selectedPieceChanged (piece); });
    }

    /** For changes that may come from any thread (restoring state). Listeners
        hear about it asynchronously on the message thread. */
    void restore (int piece)
    {
        selected = piece;
        triggerAsyncUpdate();
    }

    void addListener (Listener* l)           { listeners.add (l); }
    void removeListener (Listener* l)        { listeners.remove (l); }

private:
    void handleAsyncUpdate() override
    {
        const auto p = getSelected();
        listeners.call ([p] (Listener& l) { l.selectedPieceChanged (p); });
    }

    std::atomic<int> selected { -1 };
    juce::ListenerList<Listener> listeners;
};

} // namespace lithium
