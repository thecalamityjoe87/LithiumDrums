#pragma once

#include "ChannelModules.h"
#include "engine/Kit.h"

namespace lithium
{

/** The Mixer inspector's Mics card: how loud drums are in the kit's close,
    overhead and room mics (each drum's send_close, send_overheads and
    send_room), drawn with the renders: the drum's (the Kit tab's slot
    picture, in the kit's finish) and the mics' (assets/renders/mics).

     - A drum's channel shows that drum with its close mic clipped to its
       rim (the kick's on a low stand), the overheads hanging above it and
       the room pair on stands off to the right, each with its level knob.
       A line from each mic to the drum fades with the level.
     - An overhead or room channel shows the whole kit (the Kit tab's
       stage, flattened) with the overheads hanging over it or the room pair
       beside it, and a knob per drum; the drum whose knob is under the
       mouse gets a line from the mics.

    Mics the kit doesn't have, or that didn't record the drum, are drawn
    dimmed with their knob disabled and a note. For overheads or room mics
    the kit doesn't have, the note comes with an Emulate button, which
    switches on the emulated pair (see engine/MicEmulation.h). An emulated
    pair is always marked as one: drawn faded inside a dashed outline, with
    an EMULATED tag, and its own channel's view says what it's made from,
    has a Turn off button and, for the room, the room it's in. */
class MicsModule final : public Module, private juce::Timer
{
public:
    explicit MicsModule (juce::AudioProcessorValueTreeState&);
    ~MicsModule() override;

    /** Asked to show a mic slot's own channel: the drum view's Edit
        overheads and Edit room buttons, which open the overheads' or room's view (where an emulated
        room's room is chosen, and every drum's level in them set). */
    std::function<void (int micSlot)> onOpenMic;

    /** A click on a drum in the picture (the drum view's drum, or one on the
        mic view's kit): the Edit page plays it, as Strike does. */
    std::function<void (int piece)> onStrike;

    /** Shows a drum's or an overhead/room mic's channel of this kit. The
        module keeps the KitInfo alive (it's shared, and the Mixer may swap
        in a new kit while we still point into the old one). */
    void show (std::shared_ptr<const KitInfo>, params::ChannelId);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    /** The drum drawn at a point (in this component), or -1 for none. */
    int drumAt (juce::Point<float>) const;

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
        bool emulated = false;                 // drum view: the role's mics are emulated ones, switched on
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

        /** A layer of the picture, for finding what a click lands on (by
            its pixels' alpha, front to back, as on the Kit tab's stage) and
            for flashing a drum that's hit. */
        struct HitLayer
        {
            bool slot = false;                 // a drum slot's layer; the others (stands, pedals) can't be clicked
            int piece = -1;                    // -1: no drum (a slot without one still hides what's behind it)
            float opacity = 1.0f;              // as drawn into the picture
            juce::Rectangle<int> rect;         // in `full`'s pixels
            juce::Image image;                 // shared with the image cache, not a copy
            juce::Image flash;                 // its hit flash, minus what's in front of it; made on its first hit
        };

        std::vector<HitLayer> hitLayers;       // back to front
    };

    /** Lights a drum up as it's clicked, as on the Kit tab's stage (see kitart::makeFlashImage). */
    void startFlash (int piece);
    void drawFlash (juce::Graphics&) const;

    /** A mic render (assets/renders/mics) placed on the card, and where its
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

    /** Emulation: the switches as they are, and the drawing that marks an emulated pair. */
    std::array<bool, numMicRoles> emulationState() const;
    static void drawEmulatedOutline (juce::Graphics&, juce::Rectangle<float> around);
    static void drawEmulatedTag (juce::Graphics&, juce::Point<float> centre, float width);

    juce::AudioProcessorValueTreeState& state;
    juce::TextButton reset { "Reset" };

    // Emulation (see the class comment). The drum view's Emulate / Turn off
    // button per role (close has none); the mic view's Turn off and room choice.
    std::array<juce::TextButton, numMicRoles> emulateButtons;
    juce::TextButton emulationOff { "Turn off" };
    // Drum view: under the overheads' and room's knobs, opening their own
    // channel (the first of that role, if the kit has several); and that slot, -1 for none.
    std::array<juce::TextButton, numMicRoles> configureButtons;
    std::array<int, numMicRoles> configureSlots { -1, -1, -1 };
    juce::ComboBox roomChoice;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> roomAttachment;
    bool micIsEmulated = false;                // the mic view's mic is an emulated one
    std::array<bool, numMicRoles> shownEmulation {};   // the switches when the view was built
    params::ChannelId shownChannel;

    std::shared_ptr<const KitInfo> kit;
    View view = View::none;
    MicRole micRole = MicRole::overheads;      // the mic view's mic
    juce::String overlay;                      // shown instead of the picture when nothing applies

    std::vector<Entry> entries;                // drum view: close, overheads, room; mic view: one per drum
    int hovered = -1;                          // the mic view's entry whose knob is under the mouse or being turned
    DrumPicture drumPicture;
    KitPicture kitPicture;

    // The flash under way: its picture, where it's drawn on the card, and how bright it is now (0: none).
    juce::Image flashImage;
    juce::Rectangle<float> flashArea;
    double flashStartMs = 0.0;
    float flashLevel = 0.0f;
    juce::Image cleanBase;                     // the base stage layer without its floor shadow (see buildKitPicture())

    // Geometry, worked out in resized() so the knobs and the picture agree.
    float floorY = 0.0f;
    std::array<PlacedMics, numMicRoles> drumMics;   // drum view: the close (or kick) mic, the overheads, the room pair
    PlacedMics kitMics;                             // mic view: the overheads over the kit, or the room pair beside it
};

} // namespace lithium
