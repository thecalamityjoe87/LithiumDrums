#include "engine/Mixer.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace lithium
{

namespace
{
    constexpr double rate = 48000.0;

    float rmsDb (const float* x, int n)
    {
        double sum = 0.0;

        for (int i = 0; i < n; ++i)
            sum += (double) x[i] * x[i];

        return (float) (10.0 * std::log10 (std::max (1.0e-20, sum / n)));
    }

    /** Runs a sine through an EQ and returns its gain in dB, once settled. */
    float measureEq (const EqSettings& settings, double hz)
    {
        ChannelEq eq;
        eq.prepare (rate);
        constexpr int length = 24000;
        std::vector<float> in ((size_t) length), out ((size_t) length);

        for (int i = 0; i < length; ++i)
            in[(size_t) i] = out[(size_t) i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / rate);

        // In awkward block sizes, as the mixer may see between MIDI events.
        for (int start = 0; start < length;)
        {
            const auto n = std::min (length - start, 37 + (start / 37) % 200);
            float* channel = out.data() + start;
            eq.process (&channel, 1, n, settings);
            start += n;
        }

        constexpr int tail = 12000;
        return rmsDb (out.data() + length - tail, tail) - rmsDb (in.data() + length - tail, tail);
    }

    /** The decay time of an impulse response, from the slope of its
        Schroeder energy decay curve between -5 and -35 dB (T30). */
    double measureRt60 (const std::vector<float>& ir)
    {
        std::vector<double> edc (ir.size());
        double sum = 0.0;

        for (size_t i = ir.size(); i-- > 0;)
            edc[i] = sum += (double) ir[i] * ir[i];

        auto timeAt = [&] (double db)
        {
            for (size_t i = 0; i < edc.size(); ++i)
                if (10.0 * std::log10 (edc[i] / edc[0]) <= db)
                    return (double) i / rate;

            return (double) edc.size() / rate;
        };

        return 2.0 * (timeAt (-35.0) - timeAt (-5.0));
    }
}

class MixerTests final : public juce::UnitTest
{
public:
    MixerTests() : juce::UnitTest ("Mixer", "Lithium") {}

    void runTest() override
    {
        beginTest ("The EQ curve on screen is what the EQ does");
        {
            EqSettings s;
            s.hpfHz = 60.0f;
            s.lowHz = 120.0f;   s.lowDb = 4.0f;
            s.mid1Hz = 450.0f;  s.mid1Db = -6.0f; s.mid1Q = 2.0f;
            s.mid2Hz = 3200.0f; s.mid2Db = 5.0f;  s.mid2Q = 0.8f;
            s.highHz = 9000.0f; s.highDb = -3.0f;
            const auto curve = dsp::designEq (s, rate);

            for (const auto hz : { 40.0, 80.0, 120.0, 300.0, 450.0, 1000.0, 3200.0, 7000.0, 12000.0 })
            {
                const auto drawn = dsp::magnitudeDb (curve, hz, rate);
                const auto heard = measureEq (s, hz);
                expectWithinAbsoluteError (heard, drawn, 0.15f, "at " + juce::String (hz) + " Hz");
            }

            // Spot checks against the settings themselves.
            expectWithinAbsoluteError (dsp::magnitudeDb (curve, 3200.0, rate), 5.0f, 0.6f);
            expect (dsp::magnitudeDb (curve, 20.0, rate) < -20.0f, "the 24 dB/octave high-pass should cut 20 Hz hard");
        }

        beginTest ("A flat EQ, or a switched-off one, leaves the audio untouched");
        {
            for (const auto enabled : { true, false })
            {
                EqSettings s;
                s.enabled = enabled;

                if (! enabled)
                {
                    s.hpfHz = 200.0f;
                    s.mid1Db = 9.0f;
                }

                ChannelEq eq;
                eq.prepare (rate);
                juce::Random random (7);
                std::vector<float> audio (4096), original;

                for (auto& x : audio)
                    x = random.nextFloat() * 2.0f - 1.0f;

                original = audio;
                float* channel = audio.data();
                eq.process (&channel, 1, (int) audio.size(), s);
                expect (audio == original, enabled ? "flat EQ changed the audio" : "switched-off EQ changed the audio");
            }
        }

        beginTest ("Switching the EQ and compressor on and off doesn't click");
        {
            ChannelEq eq;
            Compressor comp;
            eq.prepare (rate);
            comp.prepare (rate);
            EqSettings eqOn;
            eqOn.hpfHz = 300.0f;
            eqOn.mid1Hz = 1000.0f;
            eqOn.mid1Db = 12.0f;
            EqSettings eqOff = eqOn;
            eqOff.enabled = false;
            CompSettings compOn;
            compOn.enabled = true;
            compOn.thresholdDb = -30.0f;
            compOn.ratio = 10.0f;
            compOn.attackMs = 0.1f;
            CompSettings compOff = compOn;
            compOff.enabled = false;

            constexpr double hz = 220.0;
            std::vector<float> block (256);
            float previous = 0.0f, largestStep = 0.0f;
            int sample = 0;

            for (int b = 0; b < 200; ++b)
            {
                for (auto& x : block)
                    x = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * sample++ / rate);

                float* channel = block.data();
                const auto phase = (b / 25) % 4;   // toggles every 25 blocks
                eq.process (&channel, 1, (int) block.size(), phase == 1 || phase == 2 ? eqOn : eqOff);
                comp.process (&channel, 1, (int) block.size(), phase >= 2 ? compOn : compOff);

                for (const auto x : block)
                {
                    largestStep = std::max (largestStep, std::abs (x - previous));
                    previous = x;
                }
            }

            // The sine's own largest step, boosted by up to 12 dB by the EQ.
            const auto sineStep = 0.5f * (float) (2.0 * juce::MathConstants<double>::pi * hz / rate);
            expect (largestStep < sineStep * 4.5f, "a jump of " + juce::String (largestStep) + " against " + juce::String (sineStep));
        }

        beginTest ("The compressor follows its threshold, ratio and mix");
        {
            for (const auto mix : { 1.0f, 0.0f })
            {
                Compressor comp;
                comp.prepare (rate);
                CompSettings s;
                s.enabled = true;
                s.thresholdDb = -18.0f;
                s.ratio = 4.0f;
                s.attackMs = 1.0f;
                s.releaseMs = 50.0f;
                s.makeupDb = 3.0f;
                s.mix = mix;

                std::vector<float> audio (24000, 0.5f);   // -6 dB, steady
                float* channel = audio.data();
                comp.process (&channel, 1, (int) audio.size(), s);

                // 12 dB over a 4:1 threshold comes out 3 dB over: 9 dB of reduction, then 3 dB of makeup.
                const auto outDb = juce::Decibels::gainToDecibels (audio.back());
                const auto expected = mix > 0.5f ? -18.0f + 3.0f + 3.0f : -6.0f;
                expectWithinAbsoluteError (outDb, expected, 0.1f, "mix " + juce::String (mix));

                if (mix > 0.5f)
                    expectWithinAbsoluteError (comp.takeMaxGainReductionDb(), 9.0f, 0.1f);
            }

            // The soft knee is continuous at both of its ends.
            const auto t = -20.0f, r = 3.0f, k = Compressor::kneeDb;
            expectWithinAbsoluteError (Compressor::transfer (t - k / 2, t, r), t - k / 2, 1.0e-4f);
            expectWithinAbsoluteError (Compressor::transfer (t + k / 2, t, r), t + k / 2 / r, 1.0e-4f);
        }

        beginTest ("Saturation leaves quiet signals alone and rounds off loud ones");
        {
            Saturator sat;
            sat.prepare (rate);
            SaturationSettings s;
            s.enabled = true;
            s.driveDb = 12.0f;

            auto run = [&] (float amplitude)
            {
                std::vector<float> x (24000);

                for (size_t i = 0; i < x.size(); ++i)
                    x[i] = amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 200.0 * (double) i / rate);

                float* ch = x.data();
                sat.process (&ch, 1, (int) x.size(), s);
                return juce::FloatVectorOperations::findMaximum (x.data() + 12000, 12000);
            };

            expectWithinAbsoluteError (run (0.01f), 0.01f, 0.0005f);
            expectLessThan (run (0.9f), 0.35f);   // 12 dB of drive puts a 0.9 peak far into the curve
        }

        beginTest ("The transient shaper sharpens attacks and trims tails");
        {
            // A drum-like burst: an instant attack and an exponential tail.
            auto burst = []
            {
                std::vector<float> x (24000);

                for (size_t i = 0; i < x.size(); ++i)
                    x[i] = 0.5f * (float) std::exp (-(double) i / (0.08 * rate))
                         * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 180.0 * (double) i / rate);

                return x;
            };

            auto shape = [&] (float attack, float sustain)
            {
                TransientShaper t;
                t.prepare (rate);
                TransientSettings s { true, attack, sustain };
                auto x = burst();
                float* ch = x.data();

                // Switching on fades in over 20 ms; start it before the burst.
                std::vector<float> silence (2048, 0.0f);
                float* sp = silence.data();
                t.process (&sp, 1, (int) silence.size(), s);
                t.process (&ch, 1, (int) x.size(), s);
                return std::pair { juce::FloatVectorOperations::findMaximum (x.data(), 480),          // the first 10 ms
                                   rmsDb (x.data() + 9600, 4800) };                                   // 200 to 300 ms
            };

            const auto [plainHead, plainTail] = shape (0.0f, 0.0f);
            const auto [sharpHead, sharpTail] = shape (1.0f, 0.0f);
            const auto [_, shortTail] = shape (0.0f, -1.0f);
            expect (sharpHead > plainHead * 1.3f, juce::String (sharpHead) + " vs " + juce::String (plainHead));
            expectLessThan (shortTail, plainTail - 3.0f);
            juce::ignoreUnused (sharpTail, _);
        }

        beginTest ("Stereo width narrows to mono, widens, and leaves a mono or unit-width channel alone");
        {
            constexpr int length = 4800, tail = 2000;
            std::vector<float> left ((size_t) length), right ((size_t) length);

            for (int i = 0; i < length; ++i)
            {
                left[(size_t) i] = 0.8f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / rate);
                right[(size_t) i] = 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * i / rate);
            }

            // The width applied to a copy of the signal, then the tail's largest differences from what went in.
            auto run = [&] (float width, int channels, float& sideError, float& middleError, float& sideGain)
            {
                StereoWidth stereoWidth;
                stereoWidth.prepare (rate);
                auto l = left, r = right;
                float* both[2] = { l.data(), r.data() };
                stereoWidth.process (both, channels, length, width);
                sideError = middleError = 0.0f;
                float inSide = 0.0f, outSide = 0.0f;

                for (int i = length - tail; i < length; ++i)
                {
                    const auto in = (size_t) i;
                    sideError = std::max (sideError, std::abs ((l[in] - r[in]) - (left[in] - right[in])));
                    middleError = std::max (middleError, std::abs ((l[in] + r[in]) - (left[in] + right[in])));
                    inSide = std::max (inSide, std::abs (left[in] - right[in]));
                    outSide = std::max (outSide, std::abs (l[in] - r[in]));
                }

                sideGain = outSide / inSide;
            };

            float sideError = 0.0f, middleError = 0.0f, gain = 0.0f;
            run (1.0f, 2, sideError, middleError, gain);
            expect (sideError < 1.0e-7f && middleError < 1.0e-7f, "unit width changes the signal");

            run (0.0f, 2, sideError, middleError, gain);
            expect (gain < 1.0e-4f, "mono still has a side of " + juce::String (gain));
            expect (middleError < 1.0e-4f, "mono changed the middle");

            run (2.0f, 2, sideError, middleError, gain);
            expect (std::abs (gain - 2.0f) < 1.0e-3f, "double width has a side of " + juce::String (gain));
            expect (middleError < 1.0e-4f, "widening changed the middle");

            run (0.0f, 1, sideError, middleError, gain);
            expect (sideError < 1.0e-7f && middleError < 1.0e-7f, "a mono channel was changed");
        }

        beginTest ("The reverb decays in its decay time, and goes quiet afterwards");
        {
            for (const auto decay : { 0.8f, 2.5f })
            {
                FdnReverb reverb;
                reverb.prepare (rate);
                ReverbSettings s;
                s.decaySeconds = decay;
                s.dampingHz = 20000.0f;   // damping shortens the highs; leave it out of the measurement
                s.lowCutHz = 20.0f;
                s.preDelayMs = 0.0f;

                const auto length = (int) (rate * decay * 2.0);
                std::vector<float> inL ((size_t) length, 0.0f), inR ((size_t) length, 0.0f), outL ((size_t) length), outR ((size_t) length);
                inL[0] = inR[0] = 1.0f;

                for (int start = 0; start < length; start += 512)
                {
                    const auto n = std::min (512, length - start);
                    reverb.process (inL.data() + start, inR.data() + start, outL.data() + start, outR.data() + start, n, s);
                }

                const auto rt60 = measureRt60 (outL);
                expectWithinAbsoluteError (rt60, (double) decay, decay * 0.15, "decay " + juce::String (decay));
                expect (std::all_of (outL.begin(), outL.end(), [] (float x) { return std::isfinite (x); }));

                // The two sides are different (it's stereo), but not wildly different in level.
                const auto l = rmsDb (outL.data(), length), r = rmsDb (outR.data(), length);
                expect (outL != outR);
                expectWithinAbsoluteError (l, r, 2.0f);

                // Silence in for longer than the tail: exact zeros out.
                std::vector<float> silence ((size_t) length, 0.0f), tailL ((size_t) length), tailR ((size_t) length);

                for (int start = 0; start < length; start += 512)
                    reverb.process (silence.data(), silence.data(), tailL.data() + start, tailR.data() + start,
                                    std::min (512, length - start), s);

                expectEquals (tailL.back(), 0.0f);
            }
        }

        beginTest ("Pre-delay delays the reverb");
        {
            auto onsetMs = [] (float preDelayMs)
            {
                FdnReverb reverb;
                reverb.prepare (rate);
                ReverbSettings s;
                s.preDelayMs = preDelayMs;
                std::vector<float> in (9600, 0.0f), outL (9600), outR (9600);
                in[0] = 1.0f;

                // Let the pre-delay smoothing (which starts at 0) settle first.
                std::vector<float> silence (48000, 0.0f), scratch (48000);
                reverb.process (silence.data(), silence.data(), scratch.data(), scratch.data(), (int) silence.size(), s);
                reverb.process (in.data(), in.data(), outL.data(), outR.data(), (int) in.size(), s);

                const auto first = std::find_if (outL.begin(), outL.end(), [] (float x) { return std::abs (x) > 1.0e-6f; });
                return (double) std::distance (outL.begin(), first) / rate * 1000.0;
            };

            expectLessThan (onsetMs (0.0f), 1.0);
            expectWithinAbsoluteError (onsetMs (50.0f) - onsetMs (0.0f), 50.0, 0.1);
        }
    }
};

static MixerTests mixerTests;

} // namespace lithium
