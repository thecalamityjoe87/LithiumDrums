#pragma once

#include "Parameters.h"
#include "engine/SpectrumTap.h"
#include "engine/dsp/ChannelEq.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace lithium
{

/** The EQ curve of one mixer channel (a drum's or a mic's), with a node per band to drag.

    Drag a node sideways for frequency and up or down for gain; the mouse
    wheel over a mid band changes its width (Q); double-click puts a band's
    gain back to 0 dB (or switches the high-pass off). The curve is computed
    with the same designEq() the audio uses, so what's drawn is what's heard.

    It reads the parameters on a timer rather than listening to them:
    parameter listeners can be called on the audio thread (automation),
    where a component must never be touched. */
class EqGraph final : public juce::Component, private juce::Timer
{
public:
    explicit EqGraph (juce::AudioProcessorValueTreeState&);
    ~EqGraph() override;

    /** Draws a live spectrum of the shown channel behind the curve: its sound
        before the EQ (a faint fill) and after it (in the accent colour). The
        tap is the engine's (MixerFeedback); it must outlive this graph. */
    void setSpectrum (SpectrumTap*);

    /** Shows (and edits) one channel's EQ. An invalid id shows nothing. */
    void setChannel (params::ChannelId);

    void setSampleRate (double rate)          { sampleRate = rate > 0.0 ? rate : 48000.0; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr float minHz = 20.0f, maxHz = 20000.0f, rangeDb = 18.0f;

private:
    enum Band { highPass, low, mid1, mid2, high, numBands };

    struct BandParams
    {
        juce::RangedAudioParameter* freq = nullptr;
        juce::RangedAudioParameter* gain = nullptr;   // null for the high-pass
        juce::RangedAudioParameter* q = nullptr;      // mids only
    };

    void timerCallback() override;
    void updateSpectrum();
    void paintSpectrum (juce::Graphics&, juce::Rectangle<float> plot) const;
    EqSettings readSettings() const;
    juce::Rectangle<float> getPlotArea() const;
    float xForHz (float hz) const;
    float hzForX (float x) const;
    float yForDb (float db) const;
    float dbForY (float y) const;
    juce::Point<float> nodePosition (Band) const;
    int bandAt (juce::Point<float>) const;
    static void setParam (juce::RangedAudioParameter*, float value);

    juce::AudioProcessorValueTreeState& state;
    double sampleRate = 48000.0;
    params::ChannelId channel;
    juce::RangedAudioParameter* enabled = nullptr;
    std::array<BandParams, numBands> bands;
    EqSettings shown;
    int hoverBand = -1, dragBand = -1;

    // The spectrum: a 2048-point FFT of the latest samples, 30 times a second.
    static constexpr int fftOrder = 11, fftSize = 1 << fftOrder;
    SpectrumTap* spectrum = nullptr;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window, fftBuffer;
    std::array<std::vector<float>, SpectrumTap::numPoints> levelsDb;   // per bin, smoothed; empty until there's data
    std::array<std::uint64_t, SpectrumTap::numPoints> lastWritten {};
};

} // namespace lithium
