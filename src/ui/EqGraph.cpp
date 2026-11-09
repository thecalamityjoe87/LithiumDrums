#include "EqGraph.h"

#include "Parameters.h"
#include "Theme.h"

#include <numeric>

namespace lithium
{

namespace
{
    constexpr float nodeRadius = 7.0f;
    constexpr std::array<const char*, 5> nodeLabels { "HP", "L", "1", "2", "H" };
    constexpr std::array<const char*, 5> bandNames { "High-pass", "Low shelf", "Mid 1", "Mid 2", "High shelf" };

    std::array<float, 13> asArray (const EqSettings& s)
    {
        return { s.enabled ? 1.0f : 0.0f, s.hpfHz, s.lowHz, s.lowDb, s.mid1Hz, s.mid1Db, s.mid1Q,
                 s.mid2Hz, s.mid2Db, s.mid2Q, s.highHz, s.highDb, 0.0f };
    }

    float valueOf (const juce::RangedAudioParameter* p, float fallback)
    {
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : fallback;
    }

    juce::String formatHz (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
    }
}

EqGraph::EqGraph (juce::AudioProcessorValueTreeState& s) : state (s)
{
    startTimerHz (30);
}

EqGraph::~EqGraph()
{
    if (spectrum != nullptr)
        spectrum->setWanted (false);
}

void EqGraph::setSpectrum (SpectrumTap* tap)
{
    spectrum = tap;
    window.resize ((size_t) fftSize);
    fftBuffer.assign ((size_t) fftSize * 2, 0.0f);   // the FFT works in place and needs twice the room
    juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) fftSize,
                                                              juce::dsp::WindowingFunction<float>::hann, false);
}

bool EqGraph::updateSpectrum()
{
    // Only while it's on screen, for a drum's or mic's channel: the audio
    // thread copies nothing otherwise.
    const auto wanted = isShowing() && channel.isValid() && ! channel.isMaster;
    spectrum->setTarget (SpectrumTap::keyFor (channel.isPiece, channel.slot));
    spectrum->setWanted (wanted);

    if (! wanted)
    {
        // One last repaint to take the old spectrum off (if there was one).
        const auto hadSpectrum = ! levelsDb[0].empty();

        for (auto& l : levelsDb)
            l.clear();

        return hadSpectrum;
    }

    // A Hann window's sum turns an FFT magnitude back into the sine's amplitude.
    const auto windowSum = std::accumulate (window.begin(), window.end(), 0.0f);
    constexpr float floorDb = -100.0f, fallDbPerFrame = 1.5f;   // peaks jump up, then sink back slowly
    auto changed = false;

    for (size_t p = 0; p < (size_t) SpectrumTap::numPoints; ++p)
    {
        const auto point = (SpectrumTap::Point) p;
        auto& levels = levelsDb[p];

        if (levels.empty())
        {
            levels.assign ((size_t) fftSize / 2, floorDb);
            changed = true;   // the spectrum's first frame
        }

        const auto written = spectrum->getWrittenCount (point);
        auto fresh = written != lastWritten[p] && spectrum->read (point, fftBuffer.data(), fftSize);
        lastWritten[p] = written;

        // An idle drum's channel is still processed, so the tap gets
        // silence: no need for an FFT to know that's all floor.
        if (fresh)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (fftBuffer.data(), fftSize);
            fresh = ! juce::exactlyEqual (range.getStart(), 0.0f) || ! juce::exactlyEqual (range.getEnd(), 0.0f);
        }

        // Nothing new: just let it sink. Once everything is on the floor
        // nothing changes, and the graph stops repainting.
        if (! fresh)
        {
            for (auto& l : levels)
            {
                if (l > floorDb)
                {
                    l = std::max (floorDb, l - fallDbPerFrame);
                    changed = true;
                }
            }

            continue;
        }

        for (size_t i = 0; i < (size_t) fftSize; ++i)
            fftBuffer[i] *= window[i];

        std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (fftBuffer.data());

        for (size_t bin = 0; bin < levels.size(); ++bin)
        {
            const auto db = juce::Decibels::gainToDecibels (fftBuffer[bin] * 2.0f / windowSum, floorDb);
            const auto level = std::max (db, levels[bin] - fallDbPerFrame);
            changed = changed || ! juce::exactlyEqual (level, levels[bin]);
            levels[bin] = level;
        }
    }

    return changed;
}

void EqGraph::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    using namespace theme;

    if (spectrum == nullptr || levelsDb[0].empty())
        return;

    // Its own scale (0 dB at the top, -78 dB at the bottom), tilted up 3 dB
    // an octave around 1 kHz so that a mix reads about level across the
    // range, as analysers usually show it.
    constexpr float topDb = 0.0f, bottomDb = -78.0f, tiltDbPerOctave = 3.0f;
    const auto binHz = (float) sampleRate / (float) fftSize;

    auto pathOf = [&] (const std::vector<float>& levels)
    {
        juce::Path p;
        p.startNewSubPath (plot.getX(), plot.getBottom());

        for (auto x = plot.getX(); x <= plot.getRight(); x += 2.0f)
        {
            const auto hz = hzForX (x);
            const auto at = juce::jlimit (1.0f, (float) levels.size() - 2.0f, hz / binHz);
            const auto i = (size_t) at;
            const auto db = juce::jmap (at - (float) i, levels[i], levels[i + 1]) + tiltDbPerOctave * std::log2 (hz / 1000.0f);
            const auto y = juce::jmap (juce::jlimit (bottomDb, topDb, db), bottomDb, topDb, plot.getBottom(), plot.getY());
            p.lineTo (x, y);
        }

        p.lineTo (plot.getRight(), plot.getBottom());
        p.closeSubPath();
        return p;
    };

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (plot.toNearestInt());
    g.setColour (colours::textMuted.withAlpha (0.16f));
    g.fillPath (pathOf (levelsDb[SpectrumTap::beforeEq]));

    const auto after = pathOf (levelsDb[SpectrumTap::afterEq]);
    g.setColour (colours::accent.withAlpha (0.10f));
    g.fillPath (after);
    g.setColour (colours::accent.withAlpha (0.35f));
    g.strokePath (after, juce::PathStrokeType (1.0f));
}

void EqGraph::setChannel (params::ChannelId newChannel)
{
    channel = newChannel;
    enabled = nullptr;
    bands = {};

    if (channel.isValid())
    {
        auto get = [this] (std::string_view name) { return state.getParameter (channel.param (name)); };
        enabled = get (params::eqOn);
        bands[highPass] = { get (params::eqHighPass), nullptr, nullptr };
        bands[low] = { get (params::eqLowFreq), get (params::eqLowGain), nullptr };
        bands[mid1] = { get (params::eqMid1Freq), get (params::eqMid1Gain), get (params::eqMid1Q) };
        bands[mid2] = { get (params::eqMid2Freq), get (params::eqMid2Gain), get (params::eqMid2Q) };
        bands[high] = { get (params::eqHighFreq), get (params::eqHighGain), nullptr };
    }

    shown = readSettings();
    repaint();
}

EqSettings EqGraph::readSettings() const
{
    EqSettings s;
    s.enabled = valueOf (enabled, 1.0f) >= 0.5f;
    s.hpfHz = valueOf (bands[highPass].freq, s.hpfHz);
    s.lowHz = valueOf (bands[low].freq, s.lowHz);
    s.lowDb = valueOf (bands[low].gain, s.lowDb);
    s.mid1Hz = valueOf (bands[mid1].freq, s.mid1Hz);
    s.mid1Db = valueOf (bands[mid1].gain, s.mid1Db);
    s.mid1Q = valueOf (bands[mid1].q, s.mid1Q);
    s.mid2Hz = valueOf (bands[mid2].freq, s.mid2Hz);
    s.mid2Db = valueOf (bands[mid2].gain, s.mid2Db);
    s.mid2Q = valueOf (bands[mid2].q, s.mid2Q);
    s.highHz = valueOf (bands[high].freq, s.highHz);
    s.highDb = valueOf (bands[high].gain, s.highDb);
    return s;
}

void EqGraph::timerCallback()
{
    // The spectrum repaints only while it moves: a silent channel's has
    // sunk to the floor and stays there.
    if (spectrum != nullptr && updateSpectrum())
        repaint();

    const auto now = readSettings();
    const auto a = asArray (now), b = asArray (shown);

    if (! std::equal (a.begin(), a.end(), b.begin(), [] (float x, float y) { return juce::exactlyEqual (x, y); }))
    {
        shown = now;
        repaint();
    }
}

//==============================================================================
juce::Rectangle<float> EqGraph::getPlotArea() const
{
    // Room on the left for the dB scale, and for the high-pass node when it sits at 20 Hz.
    return getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom (14.0f).withTrimmedLeft (36.0f).withTrimmedRight (8.0f);
}

float EqGraph::xForHz (float hz) const
{
    const auto area = getPlotArea();
    const auto t = std::log (juce::jlimit (minHz, maxHz, hz) / minHz) / std::log (maxHz / minHz);
    return area.getX() + t * area.getWidth();
}

float EqGraph::hzForX (float x) const
{
    const auto area = getPlotArea();
    const auto t = juce::jlimit (0.0f, 1.0f, (x - area.getX()) / area.getWidth());
    return minHz * std::pow (maxHz / minHz, t);
}

float EqGraph::yForDb (float db) const
{
    const auto area = getPlotArea().reduced (0.0f, nodeRadius);
    return juce::jmap (juce::jlimit (-rangeDb, rangeDb, db), rangeDb, -rangeDb, area.getY(), area.getBottom());
}

float EqGraph::dbForY (float y) const
{
    const auto area = getPlotArea().reduced (0.0f, nodeRadius);
    return juce::jlimit (-rangeDb, rangeDb, juce::jmap (y, area.getY(), area.getBottom(), rangeDb, -rangeDb));
}

juce::Point<float> EqGraph::nodePosition (Band b) const
{
    switch (b)
    {
        case highPass: return { xForHz (shown.hpfHz), yForDb (0.0f) };
        case low:      return { xForHz (shown.lowHz), yForDb (shown.lowDb) };
        case mid1:     return { xForHz (shown.mid1Hz), yForDb (shown.mid1Db) };
        case mid2:     return { xForHz (shown.mid2Hz), yForDb (shown.mid2Db) };
        case high:     return { xForHz (shown.highHz), yForDb (shown.highDb) };
        case numBands: break;
    }

    return {};
}

int EqGraph::bandAt (juce::Point<float> p) const
{
    if (! channel.isValid())
        return -1;

    // The nearest node within reach, so overlapping nodes stay usable.
    int best = -1;
    float bestDistance = nodeRadius + 5.0f;

    for (int b = 0; b < numBands; ++b)
    {
        const auto d = nodePosition ((Band) b).getDistanceFrom (p);

        if (d < bestDistance)
        {
            best = b;
            bestDistance = d;
        }
    }

    return best;
}

void EqGraph::setParam (juce::RangedAudioParameter* p, float value)
{
    if (p != nullptr)
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

//==============================================================================
void EqGraph::paint (juce::Graphics& g)
{
    using namespace theme;

    const auto plot = getPlotArea();
    g.setColour (colours::background);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);

    // Grid: octave-ish frequency lines, and every 6 dB.
    g.setFont (mono (10.0f));

    for (const auto hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const auto x = xForHz (hz);
        const auto major = juce::exactlyEqual (hz, 100.0f) || juce::exactlyEqual (hz, 1000.0f) || juce::exactlyEqual (hz, 10000.0f);
        g.setColour (major ? colours::borderStrong : colours::border);
        g.fillRect (juce::Rectangle<float> (x, plot.getY(), 1.0f, plot.getHeight()));
        g.setColour (colours::textMuted);
        g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz),
                    juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 12.0f), juce::Justification::centred);
    }

    for (const auto db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
    {
        const auto y = yForDb (db);
        g.setColour (juce::exactlyEqual (db, 0.0f) ? colours::borderStrong : colours::border);
        g.fillRect (juce::Rectangle<float> (plot.getX(), y, plot.getWidth(), 1.0f));
        g.setColour (colours::textMuted);
        g.drawText ((db > 0.0f ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (0.0f, y - 6.0f, 22.0f, 12.0f),
                    juce::Justification::centredRight);
    }

    if (! channel.isValid())
        return;

    paintSpectrum (g, plot);

    // The curve. Switched off, it shows the settings faintly, over a flat line.
    auto curveOf = [&] (const EqSettings& s)
    {
        const auto coefficients = dsp::designEq (s, sampleRate);
        juce::Path p;

        for (auto x = plot.getX(); x <= plot.getRight(); x += 2.0f)
        {
            const auto y = yForDb (dsp::magnitudeDb (coefficients, hzForX (x), sampleRate));
            p.isEmpty() ? p.startNewSubPath (x, y) : p.lineTo (x, y);
        }

        return p;
    };

    auto settingsOn = shown;
    settingsOn.enabled = true;
    const auto curve = curveOf (settingsOn);
    const auto zeroY = yForDb (0.0f);

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (plot.toNearestInt());

        if (shown.enabled)
        {
            auto fill = curve;
            fill.lineTo (plot.getRight(), zeroY);
            fill.lineTo (plot.getX(), zeroY);
            fill.closeSubPath();
            g.setColour (colours::accent.withAlpha (0.12f));
            g.fillPath (fill);
            g.setColour (colours::accent);
            g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));
        }
        else
        {
            g.setColour (colours::textMuted.withAlpha (0.45f));
            g.strokePath (curve, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));
        }
    }

    // Nodes.
    for (int b = 0; b < numBands; ++b)
    {
        const auto pos = nodePosition ((Band) b);
        const auto active = b == hoverBand || b == dragBand;
        const auto off = ! shown.enabled || (b == highPass && shown.hpfHz <= EqSettings::hpfOffHz + 0.5f);
        const auto circle = juce::Rectangle<float> (nodeRadius * 2.0f, nodeRadius * 2.0f).withCentre (pos);

        g.setColour (off ? colours::raisedHigh : (active ? colours::accent.brighter (0.2f) : colours::accent));
        g.fillEllipse (circle);
        g.setColour (active ? colours::text : (off ? colours::borderStrong : colours::background));
        g.drawEllipse (circle, active ? 1.5f : 1.0f);

        g.setColour (off ? colours::textMuted : colours::background);
        g.setFont (label (b == highPass ? 9.0f : 11.0f, Weight::bold));
        g.drawText (nodeLabels[(size_t) b], circle, juce::Justification::centred, false);
    }

    // A readout for the node under the mouse.
    if (const auto b = dragBand >= 0 ? dragBand : hoverBand; b >= 0)
    {
        const auto& band = bands[(size_t) b];
        auto text = juce::String (bandNames[(size_t) b]) + "  ";

        if (b == highPass)
            text << (shown.hpfHz <= EqSettings::hpfOffHz + 0.5f ? juce::String ("Off") : formatHz (shown.hpfHz));
        else
            text << formatHz (valueOf (band.freq, 0.0f)) << "  " << (valueOf (band.gain, 0.0f) > 0.05f ? "+" : "")
                 << juce::String (valueOf (band.gain, 0.0f), 1) << " dB";

        if (band.q != nullptr)
            text << "  Q " << juce::String (valueOf (band.q, 1.0f), 2);

        const auto font = mono (11.0f);
        const auto w = juce::GlyphArrangement::getStringWidth (font, text) + 16.0f;
        const auto pos = nodePosition ((Band) b);
        auto box = juce::Rectangle<float> (w, 20.0f).withCentre ({ pos.x, pos.y - 22.0f });

        if (box.getY() < plot.getY())
            box.setY (pos.y + 14.0f);

        box = box.constrainedWithin (plot);
        g.setColour (colours::raisedHigh.withAlpha (0.95f));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (colours::borderStrong);
        g.drawRoundedRectangle (box.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (colours::text);
        g.setFont (font);
        g.drawText (text, box, juce::Justification::centred, false);
    }
}

//==============================================================================
void EqGraph::mouseMove (const juce::MouseEvent& e)
{
    const auto b = bandAt (e.position);
    setMouseCursor (b >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);

    if (b != hoverBand)
    {
        hoverBand = b;
        repaint();
    }
}

void EqGraph::mouseExit (const juce::MouseEvent&)
{
    hoverBand = -1;
    repaint();
}

void EqGraph::mouseDown (const juce::MouseEvent& e)
{
    dragBand = bandAt (e.position);

    if (dragBand < 0)
        return;

    // One gesture per drag, so hosts record (and undo) it as one change.
    const auto& band = bands[(size_t) dragBand];

    for (auto* p : { band.freq, band.gain })
        if (p != nullptr)
            p->beginChangeGesture();

    // Grabbing a node of a switched-off EQ switches it on.
    if (enabled != nullptr && enabled->getValue() < 0.5f)
    {
        enabled->beginChangeGesture();
        enabled->setValueNotifyingHost (1.0f);
        enabled->endChangeGesture();
    }
}

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0)
        return;

    const auto& band = bands[(size_t) dragBand];
    setParam (band.freq, hzForX (e.position.x));
    setParam (band.gain, dbForY (e.position.y));
    shown = readSettings();
    repaint();
}

void EqGraph::mouseUp (const juce::MouseEvent&)
{
    if (dragBand >= 0)
    {
        const auto& band = bands[(size_t) dragBand];

        for (auto* p : { band.freq, band.gain })
            if (p != nullptr)
                p->endChangeGesture();
    }

    dragBand = -1;
    repaint();
}

void EqGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto b = bandAt (e.position);

    if (b < 0)
        return;

    // Flat again: the gain to 0 dB, or the high-pass off.
    auto* p = b == highPass ? bands[highPass].freq : bands[(size_t) b].gain;

    if (p != nullptr)
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getDefaultValue());
        p->endChangeGesture();
    }
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto b = bandAt (e.position);

    if (b != mid1 && b != mid2)
        return;

    auto* q = bands[(size_t) b].q;
    const auto delta = juce::exactlyEqual (wheel.deltaY, 0.0f) ? wheel.deltaX : wheel.deltaY;

    if (q == nullptr || juce::exactlyEqual (delta, 0.0f))
        return;

    q->beginChangeGesture();
    setParam (q, valueOf (q, 1.0f) * std::pow (1.25f, (wheel.isReversed ? -delta : delta) * 4.0f));
    q->endChangeGesture();
}

} // namespace lithium
