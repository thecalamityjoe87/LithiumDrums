#include "IrLoader.h"

#include "LithiumAssets.h"
#include "dsp/ConvolutionReverb.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace lithium
{

namespace irs
{
    const std::vector<IrInfo>& builtIn()
    {
        // A function-local static is built once, on first use, and C++
        // guarantees that's thread-safe.
        static const std::vector<IrInfo> list = []
        {
            std::vector<IrInfo> result;
            const auto json = juce::JSON::parse (juce::String::fromUTF8 (LithiumAssets::irs_json, LithiumAssets::irs_jsonSize));

            if (const auto* items = json["irs"].getArray())
            {
                for (const auto& v : *items)
                {
                    IrInfo i;
                    i.id = v["id"].toString();
                    i.name = v["name"].toString();
                    i.file = v["file"].toString();
                    i.description = v["description"].toString();
                    i.author = v["author"].toString();
                    i.source = v["source"].toString();
                    i.sourceUrl = v["sourceUrl"].toString();
                    i.licenseName = v["licenseName"].toString();
                    i.licenseUrl = v["licenseUrl"].toString();
                    i.changes = v["changes"].toString();
                    result.push_back (i);
                }
            }

            jassert (! result.empty());   // irs.json is compiled in
            return result;
        }();

        return list;
    }
}

namespace
{
    constexpr int thumbnailPoints = 512;
    constexpr float cutAtDb = -80.0f;

    /** T30, from the Schroeder energy decay curve of the IR's channels summed. */
    float measureRt60 (const juce::AudioBuffer<float>& ir, double rate)
    {
        std::vector<double> edc ((size_t) ir.getNumSamples());
        double sum = 0.0;

        for (int i = ir.getNumSamples(); --i >= 0;)
        {
            for (int ch = 0; ch < ir.getNumChannels(); ++ch)
                sum += (double) ir.getSample (ch, i) * ir.getSample (ch, i);

            edc[(size_t) i] = sum;
        }

        auto timeAt = [&] (double db)
        {
            for (size_t i = 0; i < edc.size(); ++i)
                if (10.0 * std::log10 (edc[i] / std::max (1.0e-30, edc[0]) + 1.0e-30) <= db)
                    return (double) i / rate;

            return (double) edc.size() / rate;
        };

        return (float) std::max (0.05, 2.0 * (timeAt (-35.0) - timeAt (-5.0)));
    }
}

IrLoader::IrLoader (ConvolutionReverb& r) : juce::Thread ("Lithium IR loader"), reverb (r)
{
    startThread (juce::Thread::Priority::low);
}

IrLoader::~IrLoader()
{
    stopThread (4000);
}

void IrLoader::request (const Request& r)
{
    {
        std::scoped_lock lock (mutex);

        if ((pending && r == wanted) || (! pending && anyLoaded && r == loaded))
            return;

        wanted = r;
        pending = true;
        status.loading = true;
    }

    notify();
}

IrLoader::Status IrLoader::getStatus() const
{
    std::scoped_lock lock (mutex);
    return status;
}

bool IrLoader::decode (const Request& r, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::String name;

    if (r.builtIn >= 0)
    {
        const auto& info = irs::builtIn()[(size_t) juce::jmin (r.builtIn, (int) irs::builtIn().size() - 1)];
        int size = 0;

        // The resource name is the file name without dashes, its dot made an
        // underscore ("tiled-room.flac" is "tiledroom_flac").
        if (const auto* data = LithiumAssets::getNamedResource (info.file.removeCharacters ("-").replaceCharacter ('.', '_').toRawUTF8(), size))
            reader.reset (formats.createReaderFor (std::make_unique<juce::MemoryInputStream> (data, (size_t) size, false)));

        name = info.name;
    }
    else
    {
        if (! r.file.existsAsFile())
        {
            error = r.file.getFileName() + " isn't there any more.";
            return false;
        }

        reader.reset (formats.createReaderFor (r.file));
        name = r.file.getFileNameWithoutExtension();
    }

    if (reader == nullptr)
    {
        error = "Couldn't read " + name + ": it isn't a WAV, FLAC or AIFF file.";
        return false;
    }

    // Long files are cut at 10 s: longer isn't a room, and would cost a lot to convolve.
    const auto length = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (10.0 * reader->sampleRate));

    if (length < 16)
    {
        error = name + " is too short to be an impulse response.";
        return false;
    }

    // Stereo IRs as they are, mono ones on both sides, more channels: the first two.
    const auto channels = (int) std::min (2u, reader->numChannels);
    juce::AudioBuffer<float> buffer (channels, length);
    reader->read (&buffer, 0, length, 0, true, channels > 1);

    // Normalise by energy, the way juce::dsp::Convolution would, but once,
    // before the Decay trim (otherwise shortening an IR would make it louder).
    double energy = 0.0;

    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < length; ++i)
            energy += (double) buffer.getSample (ch, i) * buffer.getSample (ch, i);

    if (energy <= 1.0e-20)
    {
        error = name + " is silent.";
        return false;
    }

    buffer.applyGain ((float) (0.125 / std::sqrt (energy / channels)));

    source = std::move (buffer);
    sourceRate = reader->sampleRate;
    sourceRt60 = measureRt60 (source, sourceRate);
    sourceName = name;
    return true;
}

juce::AudioBuffer<float> IrLoader::trimmed (float decay) const
{
    decay = juce::jlimit (0.1f, 1.0f, decay);
    auto ir = source;

    if (decay < 0.999f)
    {
        // An extra decay rate so the IR falls 60 dB in decay * RT60 rather than RT60.
        const auto extraRate = 6.9077553 / (decay * sourceRt60) - 6.9077553 / sourceRt60;
        const auto cutAfter = (int) ((-cutAtDb / 20.0f * 2.3025851f) / (6.9077553 / (decay * sourceRt60)) * sourceRate);
        const auto length = juce::jlimit (16, ir.getNumSamples(), cutAfter);

        for (int ch = 0; ch < ir.getNumChannels(); ++ch)
        {
            auto* x = ir.getWritePointer (ch);

            for (int i = 0; i < length; ++i)
                x[i] *= (float) std::exp (-extraRate * i / sourceRate);
        }

        ir.setSize (ir.getNumChannels(), length, true);
    }

    // A short fade at the end, so a cut never clicks.
    const auto fade = std::min (ir.getNumSamples() / 4, (int) (0.02 * sourceRate));

    for (int ch = 0; ch < ir.getNumChannels(); ++ch)
        ir.applyGainRamp (ch, ir.getNumSamples() - fade, fade, 1.0f, 0.0f);

    return ir;
}

void IrLoader::run()
{
    while (! threadShouldExit())
    {
        Request r;

        {
            std::scoped_lock lock (mutex);

            if (! pending)
                r.builtIn = -2;
            else
                r = wanted;
        }

        if (r.builtIn == -2)
        {
            wait (-1);   // until request() calls notify()
            continue;
        }

        juce::String error;
        const auto sameSource = r.builtIn == decodedFrom.builtIn && r.file == decodedFrom.file;
        auto ok = sameSource || decode (r, error);

        // A custom file that can't be read keeps the IR that was playing; if
        // none was (a project restored without its file), the first built-in
        // one plays, so the return is never silent.
        if (! ok && ! anyLoaded && r.builtIn < 0)
        {
            juce::String ignored;
            auto fallback = r;
            fallback.builtIn = 0;
            ok = decode (fallback, ignored);
            r.builtIn = ok ? 0 : r.builtIn;
        }

        if (ok)
        {
            decodedFrom = r;
            auto ir = trimmed (r.decay);
            const auto seconds = (float) (ir.getNumSamples() / sourceRate);

            std::vector<float> thumbnail;
            const auto step = std::max (1, ir.getNumSamples() / thumbnailPoints);
            float peak = 1.0e-9f;

            for (int start = 0; start < ir.getNumSamples(); start += step)
            {
                const auto n = std::min (step, ir.getNumSamples() - start);
                auto m = 0.0f;

                for (int ch = 0; ch < ir.getNumChannels(); ++ch)
                    m = std::max (m, ir.getMagnitude (ch, start, n));

                thumbnail.push_back (m);
                peak = std::max (peak, m);
            }

            for (auto& t : thumbnail)
                t /= peak;

            {
                std::scoped_lock lock (engineLock);
                reverb.getEngine().loadImpulseResponse (std::move (ir), sourceRate, juce::dsp::Convolution::Stereo::yes,
                                                        juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
                reverb.setTailSeconds (seconds);
            }

            std::scoped_lock lock (mutex);
            status.name = sourceName;
            status.error = {};
            status.seconds = seconds;
            status.rt60 = sourceRt60;
            status.thumbnail = std::move (thumbnail);
        }

        std::scoped_lock lock (mutex);

        if (! ok)
            status.error = error;

        ++status.serial;
        loaded = r;
        anyLoaded = anyLoaded || ok;

        // A newer request may have come in meanwhile; if not, this one's done.
        if (wanted == r)
        {
            pending = false;
            status.loading = false;
        }
    }
}

} // namespace lithium
