#include "PieceStrip.h"

#include "Theme.h"

namespace lithium
{

class PieceStrip::AuditionButton final : public juce::Button
{
public:
    AuditionButton() : juce::Button ("Audition") {}

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isDown) override
    {
        getLookAndFeel().drawButtonBackground (g, *this, {}, isMouseOver, isDown);

        const auto c = getLocalBounds().toFloat().getCentre();
        juce::Path triangle;
        triangle.addTriangle (c.x - 4.0f, c.y - 5.5f, c.x - 4.0f, c.y + 5.5f, c.x + 6.0f, c.y);

        g.setColour (isMouseOver ? theme::colours::accent : theme::colours::textMuted);
        g.fillPath (triangle);
    }
};

//==============================================================================
PieceStrip::PieceStrip (LithiumDrumsProcessor& p, int index, const KitPieceInfo& pieceInfo)
    : processor (p),
      piece (index),
      name (pieceInfo.name),
      group (info (pieceInfo.role).group),
      audition (std::make_unique<AuditionButton>()),
      volumeAttachment (p.getParameters(), params::pieceParamId (index, "volume"), volume),
      muteAttachment (p.getParameters(), params::pieceParamId (index, "mute"), mute),
      soloAttachment (p.getParameters(), params::pieceParamId (index, "solo"), solo)
{
    using namespace theme::colours;

    // Swapping drums between kits comes in a later milestone; for now the
    // list holds just the kit's own drum.
    model.addItem ("Kit default", 1);
    model.setSelectedId (1, juce::dontSendNotification);
    model.setTooltip ("Drum model");
    addAndMakeVisible (model);

    volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setDoubleClickReturnValue (true, 0.0);
    volume.getProperties().set ("unityValue", 0.0);
    volume.setColour (juce::Slider::trackColourId, theme::groupColour (group));
    volume.setTooltip ("Volume (double-click for 0 dB)");
    volume.onValueChange = [this] { repaint (getReadoutArea()); };
    addAndMakeVisible (volume);

    mute.setClickingTogglesState (true);
    mute.setTooltip ("Mute");
    mute.setColour (juce::TextButton::buttonOnColourId, textMuted);
    mute.setColour (juce::TextButton::textColourOnId, background);
    addAndMakeVisible (mute);

    solo.setClickingTogglesState (true);
    solo.setTooltip ("Solo");
    addAndMakeVisible (solo);

    audition->setTooltip ("Audition");
    audition->onClick = [this] { processor.audition (piece, LithiumDrumsProcessor::auditionVelocity); };
    addAndMakeVisible (*audition);

    processor.getSelection().addListener (this);
    selected = processor.getSelection().getSelected() == piece;
}

PieceStrip::~PieceStrip()
{
    processor.getSelection().removeListener (this);
}

void PieceStrip::selectedPieceChanged (int p)
{
    selected = p == piece;
    repaint();
}

void PieceStrip::mouseDown (const juce::MouseEvent&)
{
    processor.getSelection().select (piece);
}

juce::Rectangle<int> PieceStrip::getReadoutArea() const
{
    return { 12, 86, getWidth() - 24, 16 };
}

void PieceStrip::paint (juce::Graphics& g)
{
    using namespace theme;

    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (selected ? colours::raisedHigh : colours::raised);
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (selected ? colours::accent : colours::border);
    g.drawRoundedRectangle (bounds, 8.0f, selected ? 1.5f : 1.0f);

    // Group dot and name
    auto header = juce::Rectangle<float> (12.0f, 12.0f, (float) getWidth() - 24.0f, 20.0f);
    g.setColour (groupColour (group));
    g.fillEllipse (header.removeFromLeft (8.0f).withSizeKeepingCentre (8.0f, 8.0f));
    header.removeFromLeft (8.0f);
    g.setColour (colours::text);
    g.setFont (body (15.0f, Weight::semibold));
    g.drawText (name, header, juce::Justification::centredLeft, true);

    // Volume label and readout
    const auto readout = getReadoutArea().toFloat();
    g.setColour (colours::textMuted);
    drawTrackedText (g, "VOLUME", label (11.0f), 0.1f, readout, juce::Justification::centredLeft);

    const auto db = volume.getValue();
    const auto text = db <= params::minVolumeDb + 0.05 ? juce::String ("-inf")
                                                       : (db > 0.05 ? "+" : "") + juce::String (db, 1);
    g.setColour (colours::text);
    g.setFont (mono (12.0f));
    g.drawText (text + " dB", readout, juce::Justification::centredRight, false);
}

void PieceStrip::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (28);
    model.setBounds (area.removeFromTop (26));

    area.removeFromTop (48);    // volume label and readout row (see getReadoutArea)
    volume.setBounds (area.removeFromTop (20).expanded (2, 0));

    auto buttons = area.removeFromBottom (28);
    const auto w = (buttons.getWidth() - 12) / 3;
    mute.setBounds (buttons.removeFromLeft (w));
    buttons.removeFromLeft (6);
    solo.setBounds (buttons.removeFromLeft (w));
    buttons.removeFromLeft (6);
    audition->setBounds (buttons);
}

} // namespace lithium
