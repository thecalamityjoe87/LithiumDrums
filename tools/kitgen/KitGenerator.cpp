// LithiumKitGen: synthesizes a small drum kit in the real kit.json format,
// for the automated tests. (The plugin plays real recorded kits; this one
// only exists so the tests don't need multi-gigabyte downloads.)
//
//   LithiumKitGen <output-folder>
//
// Every hit is rendered as one dry "source" signal, then turned into mic
// signals the way a real kit would be heard: the close mic picks up its own
// drum directly, nearby close mics hear a quiet, dull, slightly late copy
// (bleed), the overheads hear everything from above with left/right delays
// that depend on where the piece sits, and the room mics hear it late,
// dark and reverberant. That makes the mic faders do audibly different things.
//
// It's deterministic: the same build always produces the same files.

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cmath>
#include <functional>
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>

namespace
{

constexpr double sampleRate = 44100.0;
constexpr int bitDepth = 24;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

struct LayerSpec
{
    int low, high;
    float amplitude;    // how hard the drum is hit
    float brightness;   // harder hits are brighter
};

constexpr std::array<LayerSpec, 3> layers {{
    { 1, 50, 0.3f, 0.45f },
    { 51, 95, 0.6f, 0.75f },
    { 96, 127, 1.0f, 1.0f },
}};

constexpr int roundRobins = 2;

using Signal = std::vector<float>;

struct HitParams
{
    float amplitude, brightness;
    float pitch;        // round-robin variation, around 1.0
    float decay;        // round-robin variation, around 1.0
    juce::Random& random;
};

using Synth = std::function<Signal (const HitParams&)>;

struct Bleed
{
    const char* mic;
    float gain;
    float delayMs;
};

struct ArticulationSpec
{
    const char* id;
    const char* name;
    double seconds;
    Synth synth;
    const char* chokeGroup = nullptr;
    const char* chokes = nullptr;
};

struct PieceSpec
{
    const char* key;
    const char* model;
    float position;          // -1 left .. +1 right, audience view
    float overheadGain;      // cymbals are loud in the overheads, the kick isn't
    float roomGain;
    std::vector<const char*> closeMics;
    std::vector<Bleed> bleed;
    std::vector<ArticulationSpec> articulations;
    bool flac;               // cymbals are written as FLAC, drums as WAV, to exercise both readers
};

//==============================================================================
// Signal helpers

int numSamples (double seconds)                 { return (int) std::lround (seconds * sampleRate); }
double time (int i)                             { return (double) i / sampleRate; }
float noise (juce::Random& r)                   { return r.nextFloat() * 2.0f - 1.0f; }
float envelope (double t, double tau)           { return (float) std::exp (-t / tau); }

void filter (Signal& s, const juce::IIRCoefficients& c)
{
    juce::IIRFilter f;
    f.setCoefficients (c);
    f.processSamples (s.data(), (int) s.size());
}

void lowPass (Signal& s, double hz)     { filter (s, juce::IIRCoefficients::makeLowPass (sampleRate, hz)); }
void highPass (Signal& s, double hz)    { filter (s, juce::IIRCoefficients::makeHighPass (sampleRate, hz)); }
void bandPass (Signal& s, double hz, double q)  { filter (s, juce::IIRCoefficients::makeBandPass (sampleRate, hz, q)); }

Signal filteredNoise (int n, juce::Random& r, double tau, std::function<void (Signal&)> shape)
{
    Signal s ((size_t) n);

    for (int i = 0; i < n; ++i)
        s[(size_t) i] = noise (r) * envelope (time (i), tau);

    shape (s);
    return s;
}

void mixInto (Signal& dest, const Signal& src, float gain)
{
    for (size_t i = 0; i < std::min (dest.size(), src.size()); ++i)
        dest[i] += src[i] * gain;
}

// A pitched drum body: a sine whose pitch falls from f0 to f1.
Signal sweep (int n, double f0, double f1, double sweepTau, double ampTau)
{
    Signal s ((size_t) n);
    double phase = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const auto t = time (i);
        phase += twoPi * (f1 + (f0 - f1) * std::exp (-t / sweepTau)) / sampleRate;
        s[(size_t) i] = (float) std::sin (phase) * envelope (t, ampTau);
    }

    return s;
}

// Inharmonic square-wave clusters: the classic recipe for metallic cymbal tones.
Signal metallic (int n, const std::vector<double>& freqs, double tau, double attackSeconds = 0.0)
{
    Signal s ((size_t) n);

    for (int i = 0; i < n; ++i)
    {
        const auto t = time (i);
        float v = 0.0f;

        for (const auto f : freqs)
            v += std::fmod (t * f, 1.0) < 0.5 ? 1.0f : -1.0f;

        const auto attack = attackSeconds > 0.0 ? (float) std::min (1.0, t / attackSeconds) : 1.0f;
        s[(size_t) i] = v / (float) freqs.size() * envelope (t, tau) * attack;
    }

    return s;
}

// Decaying sine partials (a struck bell or the ping of a ride).
Signal partials (int n, const std::vector<std::pair<double, double>>& freqsAndTaus, double pitch)
{
    Signal s ((size_t) n);

    for (int i = 0; i < n; ++i)
    {
        const auto t = time (i);
        double v = 0.0;

        for (const auto& [f, tau] : freqsAndTaus)
            v += std::sin (twoPi * f * pitch * t) * std::exp (-t / tau);

        s[(size_t) i] = (float) (v / (double) freqsAndTaus.size());
    }

    return s;
}

std::vector<double> scaled (std::vector<double> freqs, double k)
{
    for (auto& f : freqs)
        f *= k;

    return freqs;
}

const std::vector<double> hatFreqs { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };

//==============================================================================
// The instruments. Each returns a dry mono hit at full scale; level and
// brightness come from the velocity layer.

Signal kick (const HitParams& p, double seconds)
{
    const auto n = numSamples (seconds);
    auto s = sweep (n, (110.0 + 60.0 * p.brightness) * p.pitch, 47.0 * p.pitch, 0.035, 0.30 * p.decay);
    auto click = filteredNoise (n, p.random, 0.004, [&] (Signal& x) { highPass (x, 1800.0); lowPass (x, 3000.0 + 6000.0 * p.brightness); });
    mixInto (s, click, 0.5f * p.brightness);
    return s;
}

Signal snare (const HitParams& p, double seconds, float bodyGain, float wireGain, float ring)
{
    const auto n = numSamples (seconds);
    auto s = sweep (n, 200.0 * p.pitch, 182.0 * p.pitch, 0.02, 0.10 * p.decay);
    mixInto (s, sweep (n, 345.0 * p.pitch, 330.0 * p.pitch, 0.02, 0.07 * p.decay), 0.5f);

    for (auto& v : s)
        v *= bodyGain;

    auto wires = filteredNoise (n, p.random, 0.16 * p.decay, [&] (Signal& x)
    {
        highPass (x, 1500.0);
        lowPass (x, 3500.0 + 7000.0 * p.brightness);
    });
    mixInto (s, wires, wireGain);

    if (ring > 0.0f)
    {
        mixInto (s, sweep (n, 910.0 * p.pitch, 900.0 * p.pitch, 0.01, 0.09), ring);
        mixInto (s, filteredNoise (n, p.random, 0.002, [] (Signal& x) { highPass (x, 3000.0); }), ring * 1.5f);
    }

    return s;
}

Signal sidestick (const HitParams& p, double seconds)
{
    const auto n = numSamples (seconds);
    auto s = filteredNoise (n, p.random, 0.012, [&] (Signal& x) { bandPass (x, 1800.0 * p.pitch, 3.0); });

    for (auto& v : s)
        v *= 3.0f;

    mixInto (s, sweep (n, 560.0 * p.pitch, 520.0 * p.pitch, 0.01, 0.035), 0.6f);
    return s;
}

Signal hat (const HitParams& p, double seconds, double tau, float chick)
{
    const auto n = numSamples (seconds);
    auto s = metallic (n, scaled (hatFreqs, 1.7 * p.pitch), tau * p.decay);
    mixInto (s, filteredNoise (n, p.random, tau * p.decay, [] (Signal&) {}), 0.4f);
    highPass (s, 6000.0);
    highPass (s, 6000.0);
    lowPass (s, 9000.0 + 8000.0 * p.brightness);

    for (auto& v : s)
        v *= 3.5f;

    if (chick > 0.0f)
        mixInto (s, filteredNoise (n, p.random, 0.02, [] (Signal& x) { bandPass (x, 1100.0, 1.5); }), chick);

    return s;
}

Signal tom (const HitParams& p, double seconds, double f0, double f1, double tau)
{
    const auto n = numSamples (seconds);
    auto s = sweep (n, f0 * p.pitch, f1 * p.pitch, 0.06, tau * p.decay);
    mixInto (s, sweep (n, f0 * 1.52 * p.pitch, f1 * 1.5 * p.pitch, 0.05, tau * 0.4 * p.decay), 0.25f);
    mixInto (s, filteredNoise (n, p.random, 0.01, [&] (Signal& x) { bandPass (x, 2200.0, 0.8); }), 0.6f * p.brightness);
    return s;
}

Signal crash (const HitParams& p, double seconds)
{
    const auto n = numSamples (seconds);
    auto s = metallic (n, scaled (hatFreqs, 2.3 * p.pitch), 0.85 * p.decay, 0.002);
    mixInto (s, filteredNoise (n, p.random, 0.7 * p.decay, [&] (Signal& x) { highPass (x, 3000.0); }), 0.7f);
    highPass (s, 450.0);
    lowPass (s, 6000.0 + 10000.0 * p.brightness);

    for (auto& v : s)
        v *= 1.6f;

    return s;
}

Signal ride (const HitParams& p, double seconds, bool bell)
{
    const auto n = numSamples (seconds);
    Signal s;

    if (bell)
        s = partials (n, { { 690.0, 1.4 }, { 1404.0, 1.1 }, { 2050.0, 0.9 }, { 3215.0, 0.6 }, { 4410.0, 0.4 } }, p.pitch);
    else
        s = partials (n, { { 420.0, 0.9 }, { 1130.0, 1.2 }, { 2390.0, 0.8 }, { 3520.0, 0.5 }, { 5170.0, 0.3 } }, p.pitch);

    for (auto& v : s)
        v *= bell ? 2.2f : 1.0f;

    auto wash = filteredNoise (n, p.random, 1.1 * p.decay, [] (Signal& x) { highPass (x, 4000.0); });
    mixInto (s, wash, bell ? 0.1f : 0.35f);
    mixInto (s, filteredNoise (n, p.random, 0.003, [] (Signal& x) { highPass (x, 2500.0); }), 0.8f * p.brightness);
    return s;
}

Signal china (const HitParams& p, double seconds)
{
    const auto n = numSamples (seconds);
    auto s = filteredNoise (n, p.random, 0.55 * p.decay, [] (Signal&) {});
    const auto ring = metallic (n, scaled ({ 360.0, 517.0, 742.0 }, p.pitch), 10.0);

    for (size_t i = 0; i < s.size(); ++i)
        s[i] *= 0.5f + ring[i];

    bandPass (s, 2200.0, 0.6);
    lowPass (s, 4000.0 + 6000.0 * p.brightness);

    for (auto& v : s)
        v *= 2.4f;

    return s;
}

//==============================================================================
// Mics

constexpr float roomTailSeconds = 0.5f;
constexpr float bleedSeconds = 0.45f;

Signal delayed (const Signal& s, float ms, size_t length)
{
    Signal out (length, 0.0f);
    const auto offset = (size_t) std::lround (ms * 0.001 * sampleRate);

    for (size_t i = 0; i + offset < length && i < s.size(); ++i)
        out[i + offset] = s[i];

    return out;
}

void fadeOutTail (Signal& s, double seconds)
{
    const auto n = std::min (s.size(), (size_t) numSamples (seconds));

    for (size_t i = 0; i < n; ++i)
        s[s.size() - 1 - i] *= (float) i / (float) n;
}

using MicAudio = std::vector<Signal>;   // one Signal per channel

MicAudio closeMic (const PieceSpec& piece, const char* mic, const Signal& src)
{
    auto s = src;
    const juce::String m (mic);

    if (m == "kickIn")
    {
        // Inside the drum: more beater click, less boom.
        auto click = s;
        highPass (click, 1200.0);
        mixInto (s, click, 1.2f);
        highPass (s, 60.0);
    }
    else if (m == "kickOut")
    {
        // Outside the reso head: big low end, a little later.
        s = delayed (s, 0.6f, s.size());
        lowPass (s, 1800.0);
        for (auto& v : s) v *= 1.15f;
    }
    else if (m == "snareBottom")
    {
        // Under the snare: all wires, little body.
        highPass (s, 900.0);
        s = delayed (s, 0.3f, s.size());
        for (auto& v : s) v *= 1.3f;
    }
    else if (juce::String (piece.key) == "hihat")
    {
        highPass (s, 300.0);
    }

    for (auto& v : s)
        v *= 0.85f;

    return { s };
}

MicAudio bleedMic (const Bleed& b, const Signal& src)
{
    auto s = delayed (src, b.delayMs, std::min (src.size(), (size_t) numSamples (bleedSeconds)));
    lowPass (s, 3000.0);
    highPass (s, 120.0);

    for (auto& v : s)
        v *= b.gain;

    fadeOutTail (s, 0.08);
    return { s };
}

MicAudio overheads (const PieceSpec& piece, const Signal& src)
{
    // Spaced pair over the kit: the near mic is louder and earlier.
    const auto x = piece.position;
    auto left = delayed (src, 3.0f + 0.9f * std::max (0.0f, x), src.size());
    auto right = delayed (src, 3.0f + 0.9f * std::max (0.0f, -x), src.size());

    for (auto* ch : { &left, &right })
        highPass (*ch, 140.0);

    for (auto& v : left)  v *= piece.overheadGain * (0.55f - 0.3f * x);
    for (auto& v : right) v *= piece.overheadGain * (0.55f + 0.3f * x);

    return { left, right };
}

MicAudio roomMics (const PieceSpec& piece, const Signal& src)
{
    const auto length = src.size() + (size_t) numSamples (roomTailSeconds);
    auto left = delayed (src, 12.0f, length);
    auto right = delayed (src, 14.5f, length);

    juce::Reverb reverb;
    juce::Reverb::Parameters rp;
    rp.roomSize = 0.55f;
    rp.damping = 0.45f;
    rp.wetLevel = 0.9f;
    rp.dryLevel = 0.3f;
    rp.width = 1.0f;
    reverb.setParameters (rp);
    reverb.setSampleRate (sampleRate);
    reverb.processStereo (left.data(), right.data(), (int) length);

    for (auto* ch : { &left, &right })
    {
        lowPass (*ch, 6500.0);
        highPass (*ch, 50.0);

        for (auto& v : *ch)
            v *= piece.roomGain;

        fadeOutTail (*ch, 0.15);
    }

    return { left, right };
}

//==============================================================================
// Output

bool writeFile (const juce::File& file, const MicAudio& audio, bool flac)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
        return false;

    juce::WavAudioFormat wav;
    juce::FlacAudioFormat flacFormat;
    juce::AudioFormat& format = flac ? static_cast<juce::AudioFormat&> (flacFormat) : wav;

    const auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                            .withSampleRate (sampleRate)
                                                            .withNumChannels ((int) audio.size())
                                                            .withBitsPerSample (bitDepth));
    if (writer == nullptr)
        return false;

    std::vector<const float*> channels;

    for (const auto& ch : audio)
        channels.push_back (ch.data());

    return writer->writeFromFloatArrays (channels.data(), (int) channels.size(), (int) audio.front().size());
}

float peakOf (const MicAudio& audio)
{
    float peak = 0.0f;

    for (const auto& ch : audio)
        for (const auto v : ch)
            peak = std::max (peak, std::abs (v));

    return peak;
}

std::vector<PieceSpec> makeKit()
{
    using A = ArticulationSpec;
    using P = HitParams;

    return {
        { "kick", "Lithium Synth Kick 22x18", 0.05f, 0.3f, 0.45f, { "kickIn", "kickOut" },
          { { "snareTop", 0.04f, 2.0f }, { "floor", 0.06f, 1.6f } },
          { A { "hit", "Hit", 0.8, [] (const P& p) { return kick (p, 0.8); } } }, false },

        { "snare", "Lithium Brass Snare 14x6.5", 0.2f, 0.5f, 0.5f, { "snareTop", "snareBottom" },
          { { "hihat", 0.15f, 1.0f }, { "kickIn", 0.04f, 2.0f }, { "tom1", 0.08f, 1.2f } },
          { A { "center", "Center", 0.7, [] (const P& p) { return snare (p, 0.7, 1.0f, 0.55f, 0.0f); } },
            A { "rimshot", "Rimshot", 0.8, [] (const P& p) { return snare (p, 0.8, 1.2f, 0.6f, 0.45f); } },
            A { "sidestick", "Sidestick", 0.3, [] (const P& p) { return sidestick (p, 0.3); } } }, false },

        { "hihat", "Lithium Hats 14\"", 0.55f, 0.8f, 0.4f, { "hihat" },
          { { "snareTop", 0.12f, 1.0f } },
          { A { "closed", "Closed", 0.35, [] (const P& p) { return hat (p, 0.35, 0.045, 0.0f); }, nullptr, "hihat-open" },
            A { "open", "Open", 1.3, [] (const P& p) { return hat (p, 1.3, 0.55, 0.0f); }, "hihat-open", nullptr },
            A { "pedal", "Pedal", 0.35, [] (const P& p) { return hat (p, 0.35, 0.03, 0.5f); }, nullptr, "hihat-open" } }, true },

        { "tom1", "Lithium Rack Tom 10x8", 0.15f, 0.5f, 0.45f, { "tom1" },
          { { "tom2", 0.1f, 1.0f }, { "snareTop", 0.06f, 1.5f } },
          { A { "center", "Center", 0.9, [] (const P& p) { return tom (p, 0.9, 230.0, 185.0, 0.35); } } }, false },

        { "tom2", "Lithium Rack Tom 12x9", -0.05f, 0.5f, 0.45f, { "tom2" },
          { { "tom1", 0.1f, 1.0f }, { "kickIn", 0.03f, 2.0f } },
          { A { "center", "Center", 1.0, [] (const P& p) { return tom (p, 1.0, 165.0, 132.0, 0.45); } } }, false },

        { "floor", "Lithium Floor Tom 16x16", -0.35f, 0.5f, 0.5f, { "floor" },
          { { "kickIn", 0.04f, 2.0f }, { "tom2", 0.05f, 2.0f } },
          { A { "center", "Center", 1.2, [] (const P& p) { return tom (p, 1.2, 108.0, 86.0, 0.6); } } }, false },

        { "crash", "Lithium Crash 18\"", 0.4f, 1.0f, 0.5f, {},
          { { "hihat", 0.1f, 1.0f }, { "tom1", 0.06f, 1.5f } },
          { A { "hit", "Hit", 2.4, [] (const P& p) { return crash (p, 2.4); } } }, true },

        { "ride", "Lithium Ride 21\"", -0.6f, 1.0f, 0.45f, {},
          { { "floor", 0.08f, 1.2f } },
          { A { "bow", "Bow", 2.2, [] (const P& p) { return ride (p, 2.2, false); } },
            A { "bell", "Bell", 2.4, [] (const P& p) { return ride (p, 2.4, true); } } }, true },

        { "china", "Lithium China 18\"", -0.85f, 1.0f, 0.5f, {},
          { { "floor", 0.05f, 2.0f } },
          { A { "hit", "Hit", 1.8, [] (const P& p) { return china (p, 1.8); } } }, true },
    };
}


Signal renderSource (const PieceSpec& piece, const ArticulationSpec& art, size_t layerIndex, int rr)
{
    const auto& layer = layers[layerIndex];

    // Seeded per hit, so regenerating gives identical files.
    juce::Random random ((juce::int64) juce::String (piece.key).hashCode64()
                         + juce::String (art.id).hashCode() * 131 + (juce::int64) layerIndex * 17 + rr);
    const HitParams params { layer.amplitude, layer.brightness,
                             rr == 0 ? 1.0f : 0.985f, rr == 0 ? 1.0f : 0.94f, random };

    auto source = art.synth (params);

    for (auto& v : source)
        v *= layer.amplitude;

    fadeOutTail (source, 0.03);
    return source;
}

std::vector<std::pair<juce::String, MicAudio>> renderMics (const PieceSpec& piece, const Signal& source)
{
    std::vector<std::pair<juce::String, MicAudio>> result;

    for (auto* m : piece.closeMics)
        result.emplace_back (m, closeMic (piece, m, source));

    for (const auto& b : piece.bleed)
        result.emplace_back (b.mic, bleedMic (b, source));

    result.emplace_back ("overheads", overheads (piece, source));
    result.emplace_back ("room", roomMics (piece, source));
    return result;
}

} // namespace

//==============================================================================
struct MicDef
{
    const char* id;
    const char* name;
    int channels;
    const char* role;
    double pan;
};

// The kit's mics, with default pans from the audience's side like the stage.
constexpr MicDef micDefs[] {
    { "kickIn",      "Kick In",   1, "close",      0.0 },
    { "kickOut",     "Kick Out",  1, "close",      0.0 },
    { "snareTop",    "Snare Top", 1, "close",      0.15 },
    { "snareBottom", "Snare Btm", 1, "close",      0.15 },
    { "hihat",       "Hi-Hat",    1, "close",      0.45 },
    { "tom1",        "Tom 1",     1, "close",      0.2 },
    { "tom2",        "Tom 2",     1, "close",     -0.1 },
    { "floor",       "Floor Tom", 1, "close",     -0.4 },
    { "overheads",   "Overheads", 2, "overheads",  0.0 },
    { "room",        "Room",      2, "room",       0.0 },
};

int micChannels (const juce::String& id)
{
    for (const auto& m : micDefs)
        if (id == m.id)
            return m.channels;

    return 1;
}

juce::String roleFor (const juce::String& key)
{
    if (key == "tom1" || key == "tom2")  return "tom";
    if (key == "floor")                  return "floor_tom";
    return key;   // kick, snare, hihat, crash, ride, china are roles already
}

juce::var object()                                { return juce::var (new juce::DynamicObject()); }
void put (juce::var& o, const juce::Identifier& k, const juce::var& v)   { o.getDynamicObject()->setProperty (k, v); }

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: LithiumKitGen <output-folder>\n";
        return 1;
    }

    const juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    out.createDirectory();
    out.getChildFile ("kit.json").deleteFile();   // written last, so it only exists for a complete kit

    juce::Array<juce::var> pieces;
    float loudest = 0.0f;
    std::map<juce::String, float> peaks;
    int files = 0;

    for (const auto& piece : makeKit())
    {
        const auto ext = piece.flac ? juce::String (".flac") : juce::String (".wav");

        // Calibrate the whole piece from its loudest zone, so its hottest mic
        // peaks around -3 dBFS. One scale per piece keeps the level differences
        // between zones, articulations and mics intact.
        float piecePeak = 0.0f;

        for (const auto& art : piece.articulations)
            for (int rr = 0; rr < roundRobins; ++rr)
                for (const auto& [mic, audio] : renderMics (piece, renderSource (piece, art, layers.size() - 1, rr)))
                    piecePeak = std::max (piecePeak, peakOf (audio));

        const auto pieceScale = 0.7f / piecePeak;

        // The mics this piece has recordings of (own close mics, bleed,
        // overheads, room), and where each sits in the piece's multichannel files.
        juce::StringArray mics;

        for (auto* m : piece.closeMics)
            mics.add (m);

        for (const auto& b : piece.bleed)
            mics.add (b.mic);

        mics.add ("overheads");
        mics.add ("room");

        auto channelMap = object();
        int fileChannels = 0;

        for (const auto& m : mics)
        {
            if (micChannels (m) == 2)
                put (channelMap, juce::Identifier (m), juce::Array<juce::var> { fileChannels + 1, fileChannels + 2 });
            else
                put (channelMap, juce::Identifier (m), fileChannels + 1);

            fileChannels += micChannels (m);
        }

        // The kick is written as one file per mic, everything else as one
        // multichannel file per hit. Both forms are part of the format.
        const auto perMicFiles = juce::String (piece.key) == "kick";
        juce::Array<juce::var> articulations;

        for (const auto& art : piece.articulations)
        {
            auto artObject = object();
            put (artObject, "id", art.id);
            put (artObject, "name", art.name);

            if (art.chokeGroup != nullptr)
                put (artObject, "chokeGroup", art.chokeGroup);

            if (art.chokes != nullptr)
                put (artObject, "chokes", juce::Array<juce::var> { juce::var (art.chokes) });

            if (! perMicFiles)
                put (artObject, "channelMap", channelMap);

            juce::Array<juce::var> zones;

            for (size_t li = 0; li < layers.size(); ++li)
            {
                const auto& layer = layers[li];
                auto zone = object();
                put (zone, "velocity", juce::Array<juce::var> { layer.low, layer.high });
                juce::Array<juce::var> hits;

                for (int rr = 0; rr < roundRobins; ++rr)
                {
                    auto source = renderSource (piece, art, li, rr);

                    for (auto& v : source)
                        v *= pieceScale;

                    const auto stem = juce::String (piece.key) + "/" + art.id + "/v" + juce::String ((int) li + 1)
                                    + "-rr" + juce::String (rr + 1);
                    const auto rendered = renderMics (piece, source);

                    auto write = [&] (const juce::String& path, const MicAudio& audio)
                    {
                        if (! writeFile (out.getChildFile (path), audio, piece.flac))
                        {
                            std::cerr << "Failed to write " << path << "\n";
                            std::exit (1);
                        }

                        ++files;
                    };

                    for (const auto& [mic, audio] : rendered)
                    {
                        const auto p = peakOf (audio);
                        loudest = std::max (loudest, p);
                        auto& micPeak = peaks[juce::String (piece.key) + " " + mic];
                        micPeak = std::max (micPeak, p);
                    }

                    if (perMicFiles)
                    {
                        auto perMic = object();

                        for (const auto& [mic, audio] : rendered)
                        {
                            const auto path = stem + "-" + mic + ext;
                            write (path, audio);
                            put (perMic, juce::Identifier (mic), path);
                        }

                        auto hit = object();
                        put (hit, "files", perMic);
                        hits.add (hit);
                    }
                    else
                    {
                        // Every mic's channels in one file, padded to the longest.
                        size_t length = 0;

                        for (const auto& [mic, audio] : rendered)
                            length = std::max (length, audio.front().size());

                        MicAudio all;

                        for (const auto& [mic, audio] : rendered)
                        {
                            for (auto ch : audio)
                            {
                                ch.resize (length, 0.0f);
                                all.push_back (std::move (ch));
                            }
                        }

                        write (stem + ext, all);
                        hits.add (stem + ext);
                    }
                }

                put (zone, "roundRobins", hits);
                zones.add (zone);
            }

            put (artObject, "zones", zones);
            articulations.add (artObject);
        }

        auto pieceObject = object();
        put (pieceObject, "id", piece.key);
        put (pieceObject, "name", piece.model);
        put (pieceObject, "role", roleFor (piece.key));
        put (pieceObject, "articulations", articulations);
        pieces.add (pieceObject);
    }

    auto root = object();
    put (root, "formatVersion", 2);
    put (root, "type", "kit");
    put (root, "name", "Lithium Test Kit");
    put (root, "author", "LithiumKitGen");
    put (root, "description", "Synthesized for the automated tests. Not real drums.");

    auto license = object();
    put (license, "name", "CC0 1.0");
    put (license, "url", "https://creativecommons.org/publicdomain/zero/1.0/");
    put (root, "license", license);
    put (root, "sampleRate", sampleRate);

    juce::Array<juce::var> allMics;

    for (const auto& m : micDefs)
    {
        auto mic = object();
        put (mic, "id", m.id);
        put (mic, "name", m.name);
        put (mic, "channels", m.channels);
        put (mic, "role", m.role);

        if (std::abs (m.pan) > 1.0e-6)
            put (mic, "pan", m.pan);

        allMics.add (mic);
    }

    put (root, "mics", allMics);
    put (root, "pieces", pieces);

    const auto json = juce::JSON::toString (root, juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::multiLine));

    if (! out.getChildFile ("kit.json").replaceWithText (json + "\n"))
    {
        std::cerr << "Failed to write kit.json\n";
        return 1;
    }

    std::cout << "Wrote " << files << " sample files to " << out.getFullPathName()
              << " (peak " << juce::Decibels::gainToDecibels (loudest) << " dBFS)\n";

    for (const auto& [name, p] : peaks)
        if (p >= 0.9f)
            std::cerr << "Warning: " << name << " peaks at " << juce::Decibels::gainToDecibels (p) << " dBFS\n";

    return 0;
}
