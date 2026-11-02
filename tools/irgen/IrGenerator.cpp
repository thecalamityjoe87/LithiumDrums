// LithiumIrGen: makes the impulse responses the Room reverb ships with.
//
//   LithiumIrGen <output folder>                         synthesize the four built-in spaces
//   LithiumIrGen --convert <in.wav> <left> <right> <out.flac>
//                                                        turn two channels of a measured IR into a stereo IR
//   LithiumIrGen --info <ir> ...                         print each IR's length and decay time (T30), per band
//
// The synthesized spaces (studio, tiled room, hall, plate) are made here from
// nothing but numbers, so they're ours to release: they're CC0. Each is built
// the way a room actually sounds:
//
//  - Early reflections, from the image-source method: a shoebox room's walls
//    act as mirrors, so every reflection is a "mirror source" at a known
//    distance. Each one arrives after distance / speed of sound, at 1 /
//    distance, losing a little to every wall it bounced off.
//  - A late tail of noise that decays exponentially, split into four bands
//    that each decay at their own rate (highs die first in a real room), and
//    that fades in as the reflections pile up, so there's no hard seam between
//    the early part and the tail. Left and right get different noise, which
//    is what makes a reverb wide.
//
// The plate has no room: just a dense, bright tail that builds within a
// couple of milliseconds, like a steel sheet. The tiled room adds a flutter
// echo between its two hard parallel walls.
//
// --convert takes a measured IR (a multichannel WAV) and two of its channels
// as left and right. It trims the silence before the first arrival, matches
// the two sides' levels, takes 9 dB off the direct sound (so the return is
// mostly room, not a delayed copy of the dry drum) and fades out the end.
//
// The outputs are committed in assets/irs/, so the build only compiles this
// tool. It's deterministic: the same run makes the same files.

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{

constexpr double sampleRate = 48000.0;
constexpr double speedOfSound = 343.0;
constexpr double pi = juce::MathConstants<double>::pi;

// The four bands the tail decays in, split at these frequencies.
constexpr std::array<double, 3> crossovers { 250.0, 1000.0, 4000.0 };
constexpr int numBands = 4;

struct Space
{
    juce::String file;
    double lengthSeconds;
    std::array<double, numBands> rt60;       // seconds, per band: low, low-mid, high-mid, high
    std::array<double, 3> room;              // metres: width (x), depth (y), height (z); zero for the plate
    std::array<double, 3> source, centre;    // the drum, and the middle of the microphone pair
    double micSpacing = 0.6;                 // the pair's spacing, along x
    double wallReflection = 0.8;             // pressure kept per bounce (broadband)
    int reflectionOrder = 6;
    double tailLevelDb = 0.0;                // the tail against the first reflections
    double buildUpMs = 30.0;                 // how long the tail takes to fade in
    double flutterMs = 0.0;                  // the tiled room's flutter echo: its period
    double highPassHz = 30.0;
    juce::uint32 seed = 1;
};

//==============================================================================
/** A one-pole-pair (12 dB/octave) filter: Butterworth low- or high-pass,
    in the bilinear-transform biquad form. Only used offline here. */
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    static Biquad make (bool highPass, double hz)
    {
        const auto k = std::tan (pi * hz / sampleRate);
        const auto q = 1.0 / std::sqrt (2.0);
        const auto norm = 1.0 / (1.0 + k / q + k * k);
        Biquad f;
        f.b0 = (highPass ? 1.0 : k * k) * norm;
        f.b1 = (highPass ? -2.0 : 2.0) * f.b0;
        f.b2 = f.b0;
        f.a1 = 2.0 * (k * k - 1.0) * norm;
        f.a2 = (1.0 - k / q + k * k) * norm;
        return f;
    }

    double process (double x)
    {
        const auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

/** Splits a signal into the four bands with Linkwitz-Riley crossovers (two
    Butterworth filters in a row), which add back up to a flat response. */
std::array<std::vector<double>, numBands> splitBands (const std::vector<double>& x)
{
    std::array<std::vector<double>, numBands> bands;
    auto rest = x;

    for (size_t b = 0; b < crossovers.size(); ++b)
    {
        auto lp1 = Biquad::make (false, crossovers[b]), lp2 = lp1;
        auto hp1 = Biquad::make (true, crossovers[b]), hp2 = hp1;
        bands[b].resize (x.size());
        std::vector<double> high (x.size());

        for (size_t i = 0; i < x.size(); ++i)
        {
            bands[b][i] = lp2.process (lp1.process (rest[i]));
            high[i] = hp2.process (hp1.process (rest[i]));
        }

        rest = std::move (high);
    }

    bands[numBands - 1] = std::move (rest);
    return bands;
}

double gaussian (juce::Random& r)
{
    // Box-Muller: two uniform numbers make one normally distributed one.
    const auto u = std::max (1.0e-12, r.nextDouble());
    return std::sqrt (-2.0 * std::log (u)) * std::cos (2.0 * pi * r.nextDouble());
}

//==============================================================================
/** The early reflections for one microphone: an impulse train from the
    image sources of a shoebox room. */
std::vector<double> earlyReflections (const Space& s, std::array<double, 3> mic, int length)
{
    std::vector<double> out ((size_t) length, 0.0);
    const auto [w, d, h] = s.room;

    // Mirror images along one axis: n even keeps the source's side, odd mirrors it.
    auto image = [] (double size, double pos, int n)
    {
        return n % 2 == 0 ? n * size + pos : (n + 1) * size - pos;
    };

    const auto order = s.reflectionOrder;

    for (int nx = -order; nx <= order; ++nx)
    {
        for (int ny = -order; ny <= order; ++ny)
        {
            for (int nz = -order; nz <= order; ++nz)
            {
                const auto bounces = std::abs (nx) + std::abs (ny) + std::abs (nz);

                if (bounces > order)
                    continue;

                const auto dx = image (w, s.source[0], nx) - mic[0];
                const auto dy = image (d, s.source[1], ny) - mic[1];
                const auto dz = image (h, s.source[2], nz) - mic[2];
                const auto distance = std::sqrt (dx * dx + dy * dy + dz * dz);
                const auto position = distance / speedOfSound * sampleRate;
                const auto gain = std::pow (s.wallReflection, bounces) / std::max (0.5, distance);
                const auto i = (int) position;

                if (i + 1 >= length)
                    continue;

                // Between two samples, shared by both, so arrival times aren't rounded.
                const auto frac = position - i;
                out[(size_t) i] += gain * (1.0 - frac);
                out[(size_t) i + 1] += gain * frac;
            }
        }
    }

    return out;
}

/** The late tail for one side: decaying noise per band, fading in. */
std::vector<double> tail (const Space& s, int length, double startSeconds, juce::uint32 seed)
{
    juce::Random random ((juce::int64) seed);
    std::vector<double> noise ((size_t) length);

    for (auto& x : noise)
        x = gaussian (random);

    auto bands = splitBands (noise);
    std::vector<double> out ((size_t) length, 0.0);
    const auto buildUp = s.buildUpMs * 0.001;

    for (size_t b = 0; b < bands.size(); ++b)
    {
        // exp (-6.91 t / RT60) is a fall of 60 dB over RT60.
        const auto rate = 6.907755 / s.rt60[b];

        for (int i = 0; i < length; ++i)
        {
            const auto t = i / sampleRate - startSeconds;

            if (t < 0.0)
                continue;

            const auto fadeIn = 1.0 - std::exp (-t / std::max (0.0005, buildUp / 3.0));
            out[(size_t) i] += bands[b][(size_t) i] * std::exp (-rate * t) * fadeIn;
        }
    }

    return out;
}

double rms (const std::vector<double>& x, size_t from, size_t to)
{
    double sum = 0.0;

    for (auto i = from; i < std::min (to, x.size()); ++i)
        sum += x[i] * x[i];

    return std::sqrt (sum / (double) std::max<size_t> (1, std::min (to, x.size()) - from));
}

void fadeOut (std::vector<double>& x, int fadeLength)
{
    const auto n = (int) x.size();

    for (int i = std::max (0, n - fadeLength); i < n; ++i)
        x[(size_t) i] *= 0.5 * (1.0 + std::cos (pi * (i - (n - fadeLength)) / fadeLength));
}

void highPass (std::vector<double>& x, double hz)
{
    auto f1 = Biquad::make (true, hz), f2 = f1;

    for (auto& v : x)
        v = f2.process (f1.process (v));
}

bool writeFlac (const juce::File& file, const std::array<std::vector<double>, 2>& channels)
{
    // Peak at -1 dBFS. The plugin normalises every IR by its energy when it
    // loads it, so this only keeps the files from clipping.
    double peak = 1.0e-9;

    for (const auto& c : channels)
        for (const auto v : c)
            peak = std::max (peak, std::abs (v));

    const auto scale = juce::Decibels::decibelsToGain (-1.0) / peak;
    const auto length = (int) channels[0].size();
    juce::AudioBuffer<float> buffer (2, length);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < length; ++i)
            buffer.setSample (ch, i, (float) (channels[(size_t) ch][(size_t) i] * scale));

    file.deleteFile();
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

    if (stream == nullptr)
        return false;

    const auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24);
    auto writer = flac.createWriterFor (stream, options);

    if (writer == nullptr)
        return false;

    return writer->writeFromAudioSampleBuffer (buffer, 0, length);
}

//==============================================================================
std::array<std::vector<double>, 2> synthesize (const Space& s)
{
    const auto length = (int) (s.lengthSeconds * sampleRate);
    std::array<std::vector<double>, 2> out;

    for (int side = 0; side < 2; ++side)
    {
        const auto offset = (side == 0 ? -0.5 : 0.5) * s.micSpacing;
        const std::array<double, 3> mic { s.centre[0] + offset, s.centre[1], s.centre[2] };
        const auto isPlate = s.room[0] <= 0.0;

        std::vector<double> early ((size_t) length, 0.0);
        auto firstArrival = 0.0;

        if (! isPlate)
        {
            early = earlyReflections (s, mic, length);
            const auto dx = s.source[0] - mic[0], dy = s.source[1] - mic[1], dz = s.source[2] - mic[2];
            firstArrival = std::sqrt (dx * dx + dy * dy + dz * dz) / speedOfSound;

            // The walls soak up highs: each band of the early part loses
            // what the tail's decay says it loses at that point in time.
            auto bands = splitBands (early);
            std::fill (early.begin(), early.end(), 0.0);

            for (size_t b = 0; b < bands.size(); ++b)
                for (int i = 0; i < length; ++i)
                    early[(size_t) i] += bands[b][(size_t) i]
                                         * std::exp (-6.907755 / s.rt60[b] * std::max (0.0, i / sampleRate - firstArrival) * 0.5);
        }

        auto late = tail (s, length, firstArrival + (isPlate ? 0.0 : 0.004), s.seed * 2 + (juce::uint32) side);

        // The tail's level relative to the direct sound and first reflections.
        const auto earlyLevel = isPlate ? 1.0 : rms (early, 0, (size_t) ((firstArrival + 0.05) * sampleRate));
        const auto lateLevel = rms (late, (size_t) ((firstArrival + 0.02) * sampleRate), (size_t) ((firstArrival + 0.12) * sampleRate));
        const auto tailGain = earlyLevel / std::max (1.0e-12, lateLevel) * juce::Decibels::decibelsToGain (s.tailLevelDb);

        auto& x = out[(size_t) side];
        x.assign ((size_t) length, 0.0);

        for (int i = 0; i < length; ++i)
            x[(size_t) i] = early[(size_t) i] + late[(size_t) i] * tailGain;

        // Flutter echo between two hard parallel walls: a train of repeats of
        // the first reflections, decaying with the room, brightest in the highs.
        if (s.flutterMs > 0.0)
        {
            const auto period = (int) (s.flutterMs * 0.001 * sampleRate);
            const auto source = x;
            auto highs = splitBands (source)[3];
            const auto start = (int) (firstArrival * sampleRate);

            for (int k = 1; start + k * period < length; ++k)
            {
                const auto g = 0.32 * std::exp (-6.907755 / s.rt60[3] * k * period / sampleRate);

                for (int i = start; i < start + (int) (0.006 * sampleRate) && i + k * period < length; ++i)
                    x[(size_t) (i + k * period)] += g * highs[(size_t) i];
            }
        }

        // No silence before the first arrival: the reverb's pre-delay control places it.
        const auto lead = (int) (firstArrival * sampleRate);
        x.erase (x.begin(), x.begin() + std::max (0, lead - 24));
        x.resize ((size_t) length - (size_t) lead, 0.0);

        highPass (x, s.highPassHz);
        fadeOut (x, (int) (0.08 * sampleRate));
    }

    return out;
}

int synthesizeAll (const juce::File& folder)
{
    folder.createDirectory();

    std::vector<Space> spaces;

    // A small, treated tracking room: short, warm, not boomy.
    {
        Space s;
        s.file = "studio.flac";
        s.lengthSeconds = 1.2;
        s.rt60 = { 0.48, 0.40, 0.34, 0.24 };
        s.room = { 6.8, 5.4, 3.1 };
        s.source = { 3.0, 2.0, 0.8 };
        s.centre = { 3.4, 3.6, 1.7 };
        s.micSpacing = 0.5;
        s.wallReflection = 0.62;
        s.reflectionOrder = 5;
        s.tailLevelDb = -4.0;
        s.buildUpMs = 18.0;
        s.highPassHz = 40.0;
        s.seed = 11;
        spaces.push_back (s);
    }

    // A garage with painted block walls and a concrete floor: hard, bright,
    // with a flutter between the side walls.
    {
        Space s;
        s.file = "tiled-room.flac";
        s.lengthSeconds = 2.2;
        s.rt60 = { 1.25, 1.45, 1.35, 1.05 };
        s.room = { 3.4, 6.0, 2.6 };
        s.source = { 1.5, 2.0, 0.8 };
        s.centre = { 1.9, 4.2, 1.6 };
        s.micSpacing = 0.4;
        s.wallReflection = 0.93;
        s.reflectionOrder = 8;
        s.tailLevelDb = 0.0;
        s.buildUpMs = 12.0;
        s.flutterMs = 2.0 * 3.4 / speedOfSound * 1000.0;
        s.highPassHz = 50.0;
        s.seed = 23;
        spaces.push_back (s);
    }

    // A large concert hall: long, dark tail, the first reflections arriving late.
    {
        Space s;
        s.file = "hall.flac";
        s.lengthSeconds = 4.0;
        s.rt60 = { 2.9, 2.5, 1.9, 1.15 };
        s.room = { 26.0, 42.0, 16.0 };
        s.source = { 13.0, 8.0, 1.2 };
        s.centre = { 13.0, 22.0, 3.0 };
        s.micSpacing = 1.4;
        s.wallReflection = 0.86;
        s.reflectionOrder = 4;
        s.tailLevelDb = 2.0;
        s.buildUpMs = 70.0;
        s.highPassHz = 30.0;
        s.seed = 37;
        spaces.push_back (s);
    }

    // A plate: dense from the first millisecond, bright, with a lighter low end.
    {
        Space s;
        s.file = "plate.flac";
        s.lengthSeconds = 3.2;
        s.rt60 = { 1.5, 2.2, 2.4, 1.9 };
        s.room = { 0.0, 0.0, 0.0 };
        s.buildUpMs = 2.0;
        s.highPassHz = 110.0;
        s.seed = 53;
        spaces.push_back (s);
    }

    for (const auto& s : spaces)
    {
        const auto file = folder.getChildFile (s.file);

        if (! writeFlac (file, synthesize (s)))
        {
            std::cerr << "Couldn't write " << file.getFullPathName() << "\n";
            return 1;
        }

        std::cout << "Wrote " << file.getFullPathName() << "\n";
    }

    return 0;
}

//==============================================================================
int convert (const juce::File& in, int left, int right, const juce::File& outFile)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (in.createInputStream().release(), true));

    if (reader == nullptr || left >= (int) reader->numChannels || right >= (int) reader->numChannels)
    {
        std::cerr << "Can't read " << in.getFullPathName() << ", or it hasn't those channels\n";
        return 1;
    }

    if (! juce::exactlyEqual (reader->sampleRate, sampleRate))
    {
        std::cerr << "Expected a 48 kHz IR\n";
        return 1;
    }

    const auto length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> all ((int) reader->numChannels, length);
    reader->read (&all, 0, length, 0, true, true);

    std::array<std::vector<double>, 2> out;

    for (int side = 0; side < 2; ++side)
    {
        const auto* src = all.getReadPointer (side == 0 ? left : right);
        out[(size_t) side].assign (src, src + length);
    }

    // The first arrival on either side: the first sample within 20 dB of the peak.
    auto onset = length;

    for (const auto& x : out)
    {
        double peak = 0.0;

        for (const auto v : x)
            peak = std::max (peak, std::abs (v));

        for (int i = 0; i < length; ++i)
        {
            if (std::abs (x[(size_t) i]) > 0.1 * peak)
            {
                onset = std::min (onset, i);
                break;
            }
        }
    }

    const auto start = std::max (0, onset - 24);   // half a millisecond before it
    const auto directEnd = (size_t) (onset - start + (int) (0.0025 * sampleRate));

    for (auto& x : out)
    {
        x.erase (x.begin(), x.begin() + start);

        // 9 dB off the direct sound, with short ramps either side of it.
        for (size_t i = 0; i < x.size(); ++i)
        {
            const auto t = (double) i;
            const auto ramp = juce::jlimit (0.0, 1.0, (t - (double) directEnd) / 48.0);
            x[i] *= juce::jmap (ramp, juce::Decibels::decibelsToGain (-9.0), 1.0);
        }
    }

    // Match the two sides on their reverberation (from 20 ms on), not their
    // direct sound, which depends on each capsule's angle to the source.
    const auto tailFrom = (size_t) (0.02 * sampleRate);
    const auto l = rms (out[0], tailFrom, out[0].size()), r = rms (out[1], tailFrom, out[1].size());
    const auto mean = std::sqrt (l * r);

    for (auto& v : out[0]) v *= mean / l;
    for (auto& v : out[1]) v *= mean / r;

    for (auto& x : out)
    {
        highPass (x, 30.0);
        fadeOut (x, (int) (0.06 * sampleRate));
    }

    if (! writeFlac (outFile, out))
    {
        std::cerr << "Couldn't write " << outFile.getFullPathName() << "\n";
        return 1;
    }

    std::cout << "Wrote " << outFile.getFullPathName() << " (" << out[0].size() << " samples, sides balanced by "
              << juce::String (juce::Decibels::gainToDecibels (l / r), 1) << " dB)\n";
    return 0;
}

/** T30 of one signal: from the slope of its Schroeder energy decay curve
    between -5 and -35 dB. */
double t30 (const std::vector<double>& x)
{
    std::vector<double> edc (x.size());
    double sum = 0.0;

    for (size_t i = x.size(); i-- > 0;)
        edc[i] = sum += x[i] * x[i];

    auto timeAt = [&] (double db)
    {
        for (size_t i = 0; i < edc.size(); ++i)
            if (10.0 * std::log10 (edc[i] / edc[0] + 1.0e-30) <= db)
                return (double) i / sampleRate;

        return (double) edc.size() / sampleRate;
    };

    return 2.0 * (timeAt (-35.0) - timeAt (-5.0));
}

/** Prints an IR's length and its decay time, overall and per band. */
int info (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        std::cerr << "Can't read " << file.getFullPathName() << "\n";
        return 1;
    }

    const auto length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, length);
    reader->read (&buffer, 0, length, 0, true, true);
    std::cout << file.getFileName() << ": " << juce::String (length / reader->sampleRate, 2) << " s";

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const std::vector<double> x (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + length);
        std::cout << (ch == 0 ? "  L" : "  R") << " T30 " << juce::String (t30 (x), 2) << " s (bands";

        for (const auto& band : splitBands (x))
            std::cout << " " << juce::String (t30 (band), 2);

        std::cout << ")";
    }

    std::cout << "\n";
    return 0;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    if (args.size() == 5 && args[0] == "--convert")
        return convert (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]), args[2].getIntValue(), args[3].getIntValue(),
                        juce::File::getCurrentWorkingDirectory().getChildFile (args[4]));

    if (args.size() >= 2 && args[0] == "--info")
    {
        for (int i = 1; i < args.size(); ++i)
            if (info (juce::File::getCurrentWorkingDirectory().getChildFile (args[i])) != 0)
                return 1;

        return 0;
    }

    if (args.size() == 1 && ! args[0].startsWith ("-"))
        return synthesizeAll (juce::File::getCurrentWorkingDirectory().getChildFile (args[0]));

    std::cerr << "Usage: LithiumIrGen <output folder>\n"
                 "       LithiumIrGen --convert <in.wav> <left channel> <right channel> <out.flac>\n";
    return 1;
}
