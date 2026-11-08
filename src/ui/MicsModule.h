#pragma once

#include "ChannelModules.h"
#include "engine/Kit.h"

namespace lithium
{

/** The Mixer inspector's Mics card: how loud drums are in the kit's close,
    overhead and room mics (each drum's send_close, send_overheads and
    send_room), drawn with the renders: the drum's (the Kit tab's slot
    picture, in the kit's finish) and the mics' (assets/mic-renders).

     - A drum's channel shows that drum with its close mic clipped to its
       rim (the kick's on a low stand), the overheads hanging above it and
       the room pair on stands off to the right, each with its level knob.
       A line from each mic to the drum fades with the level.
     - An overhead or room channel shows the whole kit (the Kit tab's
       stage, flattened) with the overheads hanging over it or the room pair
       beside it, and a knob per drum; the drum whose knob is under the
       mouse gets a line from the mics.

    Mics the kit doesn't have, or that didn't record the drum, are drawn
    dimmed with their knob disabled and a note. */
class MicsModule final : public Module, private juce::Timer
{
public:
    explicit MicsModule (juce::AudioProcessorValueTreeState&);
    ~MicsModule() override;

    /** Shows a drum's or an overhead/room mic's channel of this kit. The
        module keeps the KitInfo alive (it's shared, and the Mixer may swap
        in a new kit while we still point into the old one). */
    void show (std::shared_ptr<const KitInfo>, params::ChannelId);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    /** One knob and what it draws: a mic role in the drum view, or a drum in
        the mic view. */
    struct Entry
    {
        int piece = -1;                        // the drum whose level it is
        std::unique_ptr<Knob> knob;
        std::atomic<float>* level = nullptr;   // the parameter's value (0..100), read on the message thread
        juce::String parameterId;
        bool available = true;
        juce::String note;                     // why it isn't available
        float shownLevel = -1.0f;              // what the picture was last drawn with
    };

    /** The drum view's drum: its render, and where it's drawn (worked out in resized()). */
    struct DrumPicture
    {
        int piece = -1;
        juce::Image full;                      // the render as loaded
        juce::Image scaled;                    // shrunk to `area`, so painting doesn't resample the full render
        juce::Rectangle<float> area;
        juce::Point<float> aim;                // where the mics point: the head, or the cymbal
    };

    /** The mic view's whole kit: the Kit tab's stage flattened into one
        picture (see buildKitPicture()), and where each drum is on it. */
    struct KitPicture
    {
        std::shared_ptr<const KitInfo> builtFor;           // rebuilt when the kit changes
        juce::Image full, scaled;
        juce::Rectangle<float> area;
        std::vector<juce::Rectangle<float>> pieceRects;    // per drum, in `full`'s pixels; empty if it has no place on the stage
    };

    /** A mic render (assets/mic-renders) placed on the card, and where its
        mics' fronts land, which is where their pickup lines start. */
    struct PlacedMics
    {
        juce::Image image, scaled;             // as loaded, and shrunk to `area`
        juce::Rectangle<float> area;
        std::vector<juce::Point<float>> tips;
        float splitOffset = 0.0f;              // a pair drawn with each half moved out this far, or in if negative (see drawMics())
    };

    /** Which of a render's points placeMics() puts where it's asked: the
        close mic's clip, a stand's foot, or the middle of the mics' fronts. */
    enum class Anchor { clip, foot, tips };

    /** A render ("mics-close", ...) at `scale` card pixels per render pixel,
        with its anchor point at `at`. */
    static PlacedMics placeMics (const juce::String& name, float scale, Anchor, juce::Point<float> at);

    /** Draws placed mics, a split pair as its two halves moved apart. */
    static void drawMics (juce::Graphics&, const PlacedMics&);

    enum class View { none, drum, mic };

    void timerCallback() override;
    void resetLevels();
    float levelOf (const Entry&) const;
    void buildKitPicture();
    juce::Point<float> aimOnKit (int piece) const;
    int entryFor (const juce::Component*) const;
    void setHovered (int entry);

    void paintDrumView (juce::Graphics&);
    void paintMicView (juce::Graphics&);

    juce::AudioProcessorValueTreeState& state;
    juce::TextButton reset { "Reset" };

    std::shared_ptr<const KitInfo> kit;
    View view = View::none;
    MicRole micRole = MicRole::overheads;      // the mic view's mic
    juce::String overlay;                      // shown instead of the picture when nothing applies

    std::vector<Entry> entries;                // drum view: close, overheads, room; mic view: one per drum
    int hovered = -1;                          // the mic view's entry whose knob is under the mouse or being turned
    DrumPicture drumPicture;
    KitPicture kitPicture;
    juce::Image cleanBase;                     // the base stage layer without its floor shadow (see buildKitPicture())

    // Geometry, worked out in resized() so the knobs and the picture agree.
    float floorY = 0.0f;
    std::array<PlacedMics, numMicRoles> drumMics;   // drum view: the close (or kick) mic, the overheads, the room pair
    PlacedMics kitMics;                             // mic view: the overheads over the kit, or the room pair beside it
};

} // namespace lithium
