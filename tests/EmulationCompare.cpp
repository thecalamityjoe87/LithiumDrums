// --emulation-compare <kit folder> [--out <folder>]: how the emulated mics
// compare with real ones.
//
// Loads a kit that has its own overheads and room (DRSKit, MuldjordKit)
// with emulated ones as well (KitLoadOptions::emulateRecordedMics), plays
// each piece once, and prints, for the real and the emulated pair side by
// side: the level, where in the spectrum the energy is, how late it
// arrives after the close mics, the stereo image, and (for the room) how
// long it rings. With --out, also writes a short pattern through each pair
// as WAV files to listen to.
//
// For a kit with no overheads or room (Basalt Onyx), it writes only the
// WAVs: the close mics alone, and with the emulated pairs.

#include "engine/DrumEngine.h"
#include "engine/IrLoader.h"
#include "engine/KitLoader.h"
#include "engine/Mixer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <iostream>
#include <thread>

namespace
{
    using namespace lithium;

    constexpr int blockSize = 256;

    struct Pair
    {
        juce::String name;
        int slot = -1;
    };

    /** The audio of one pair, rendered. */
    using Stereo = juce::AudioBuffer<float>;

    float rmsDb (const Stereo& b, int from, int length)
    {
        double sum = 0.0;
        length = std::min (length, b.getNumSamples() - from);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = from; i < from + length; ++i)
                sum += (double) b.getSample (ch, i) * b.getSample (ch, i);

        return juce::Decibels::gainToDecibels ((float) std::sqrt (sum / std::max (1, length * b.getNumChannels())), -120.0f);
    }

    /** The first sample whose level reaches -30 dB below the peak (of both sides). */
    int onset (const Stereo& b)
    {
        auto peak = 0.0f;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            peak = std::max (peak, b.getMagnitude (ch, 0, b.getNumSamples()));

        const auto threshold = peak * 0.0316f;

        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                if (std::abs (b.getSample (ch, i)) >= threshold)
                    return i;

        return -1;
    }

    /** Energy per octave band (63 Hz to 16 kHz), in dB relative to the
        loudest band, over the first half second. */
    std::array<float, 9> bands (const Stereo& b, double rate)
    {
        constexpr int order = 15;
        constexpr int size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) size * 2, 0.0f);
        std::array<double, 9> energy {};
        const auto length = std::min ({ size, b.getNumSamples(), (int) (0.5 * rate) });

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            std::fill (data.begin(), data.end(), 0.0f);

            for (int i = 0; i < length; ++i)
                data[(size_t) i] = b.getSample (ch, i);

            fft.performFrequencyOnlyForwardTransform (data.data());

            for (int k = 1; k < size / 2; ++k)
            {
                const auto hz = k * rate / size;
                const auto band = (int) std::floor (std::log2 (hz / 63.0) + 0.5);

                if (juce::isPositiveAndBelow (band, 9))
                    energy[(size_t) band] += (double) data[(size_t) k] * data[(size_t) k];
            }
        }

        const auto loudest = *std::max_element (energy.begin(), energy.end());
        std::array<float, 9> db {};

        for (size_t i = 0; i < db.size(); ++i)
            db[i] = (float) (10.0 * std::log10 (std::max (1.0e-12, energy[i] / std::max (1.0e-12, loudest))));

        return db;
    }

    /** Left minus right, in dB, and the lag (ms, + = right later) where they correlate best. */
    std::pair<float, float> image (const Stereo& b, double rate)
    {
        const auto n = std::min (b.getNumSamples(), (int) (0.1 * rate));
        double l = 0.0, r = 0.0;

        for (int i = 0; i < n; ++i)
        {
            l += (double) b.getSample (0, i) * b.getSample (0, i);
            r += (double) b.getSample (1, i) * b.getSample (1, i);
        }

        const auto maxLag = (int) (0.003 * rate);
        auto bestLag = 0;
        auto best = -1.0e30;

        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            double c = 0.0;

            for (int i = maxLag; i < n - maxLag; ++i)
                c += (double) b.getSample (0, i) * b.getSample (1, i + lag);

            if (c > best)
            {
                best = c;
                bestLag = lag;
            }
        }

        return { (float) (10.0 * std::log10 (std::max (1.0e-20, l) / std::max (1.0e-20, r))), (float) (1000.0 * bestLag / rate) };
    }

    /** How long the level takes to fall 30 dB below its loudest 10 ms, in seconds. */
    float ringTime (const Stereo& b, double rate)
    {
        const auto window = (int) (0.01 * rate);
        std::vector<float> levels;

        for (int i = 0; i + window <= b.getNumSamples(); i += window)
            levels.push_back (rmsDb (b, i, window));

        if (levels.empty())
            return 0.0f;

        const auto peakAt = (size_t) std::distance (levels.begin(), std::max_element (levels.begin(), levels.end()));

        for (auto i = peakAt; i < levels.size(); ++i)
            if (levels[i] < levels[peakAt] - 30.0f)
                return (float) ((double) (i - peakAt) * window / rate);

        return (float) ((double) (levels.size() - peakAt) * window / rate);
    }

    struct Rig
    {
        std::unique_ptr<Kit> kit;
        KitInfo info;
        DrumEngine engine;
        Mixer mixer;
        EngineParams params;
        double rate = 48000.0;
        std::unique_ptr<IrLoader> roomLoader;

        explicit Rig (EngineFeedback& feedback) : engine (feedback) {}

        bool load (const juce::File& folder, bool emulateRecorded)
        {
            KitLoadOptions options;
            options.emulateRecordedMics = emulateRecorded;
            juce::String error;
            kit = KitLoader::loadKit (folder, MidiMap::kitDefault(), info, error, options);

            if (kit == nullptr)
            {
                std::cerr << "Couldn't load " << folder.getFullPathName() << ": " << error << std::endl;
                return false;
            }

            rate = kit->sampleRate;
            engine.prepare (rate);
            mixer.prepare (rate, blockSize);
            params.resetForKit (*kit);
            params.velocityCrossfade = false;

            roomLoader = std::make_unique<IrLoader> (mixer.getEmulatedRoom());
            roomLoader->request ({ emulation::defaultRoomFor (info.id), {}, 1.0f });

            while (roomLoader->getStatus().loading || roomLoader->getStatus().serial == 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (10));

            // Let the Convolution swap its new IR in.
            juce::AudioBuffer<float> out (2, blockSize);
            std::this_thread::sleep_for (std::chrono::milliseconds (300));
            render ({}, (int) rate / 4, {}, out);
            return true;
        }

        /** Renders hits (articulation, velocity, sample) for numSamples, into
            one stereo buffer per mic slot asked for (the mic bus after the
            mixer, so after each channel's chain, which is flat here), and the
            main out. A slot of -2 means the close mics (the drum channels, summed). */
        void render (const std::vector<std::tuple<int, int, int>>& hits, int numSamples,
                     const std::vector<int>& slots, juce::AudioBuffer<float>& mainOut,
                     std::vector<Stereo>* perSlot = nullptr)
        {
            mainOut.setSize (2, numSamples);
            mainOut.clear();

            if (perSlot != nullptr)
            {
                perSlot->assign (slots.size(), Stereo (2, numSamples));

                for (auto& b : *perSlot)
                    b.clear();
            }

            size_t next = 0;

            for (int start = 0; start < numSamples; start += blockSize)
            {
                const auto n = std::min (blockSize, numSamples - start);

                for (; next < hits.size() && std::get<2> (hits[next]) < start + n; ++next)
                    engine.trigger (*kit, std::get<0> (hits[next]), std::get<1> (hits[next]), params);

                mixer.clearBuses (n);
                engine.render (*kit, mixer.getBuses(), 0, n, params);

                // The drum channels before the mixer works on them in place.
                if (perSlot != nullptr)
                {
                    for (size_t s = 0; s < slots.size(); ++s)
                    {
                        if (slots[s] != -2)
                            continue;

                        for (int p = 0; p < (int) kit->pieces.size(); ++p)
                            for (int ch = 0; ch < 2; ++ch)
                                (*perSlot)[s].addFrom (ch, start, mixer.getPieceBus(), p * 2 + ch, 0, n);
                    }
                }

                mixer.process (*kit, n, params, { mainOut.getWritePointer (0, start), mainOut.getWritePointer (1, start) }, {});

                if (perSlot != nullptr)
                {
                    for (size_t s = 0; s < slots.size(); ++s)
                    {
                        if (slots[s] < 0)
                            continue;

                        // At the kit's own level for the mic (an emulated mic's is its calibration).
                        const auto& mic = kit->mics[(size_t) slots[s]];

                        for (int ch = 0; ch < 2; ++ch)
                            (*perSlot)[s].copyFrom (ch, start, mixer.getMicBus().getReadPointer (mic.busChannel + std::min (ch, mic.numChannels - 1)),
                                                    n, std::abs (mic.defaultGain));
                    }
                }
            }
        }
    };

    int firstMicOf (const KitInfo& info, MicRole role, bool emulated)
    {
        for (size_t m = 0; m < info.mics.size(); ++m)
            if (info.mics[m].role == role && info.mics[m].emulated == emulated)
                return (int) m;

        return -1;
    }

    void writeWav (const juce::File& file, const juce::AudioBuffer<float>& b, double rate)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;

        if (std::unique_ptr<juce::OutputStream> stream = file.createOutputStream())
        {
            auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24);

            if (auto writer = wav.createWriterFor (stream, options))
                writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
    }

    /** A two-bar pattern over the kit's main pieces: kick, snare, hats, a tom fill, a crash and the ride. */
    std::vector<std::tuple<int, int, int>> pattern (const Kit& kit, const KitInfo& info, double rate)
    {
        auto pieceOf = [&] (PieceRole role, int nth = 0) -> int
        {
            for (size_t p = 0; p < info.pieces.size(); ++p)
                if (info.pieces[p].role == role && nth-- == 0)
                    return kit.pieces[p].firstArticulation;

            return -1;
        };

        const auto kick = pieceOf (PieceRole::kick), snare = pieceOf (PieceRole::snare), hat = pieceOf (PieceRole::hihat);
        const auto tom1 = pieceOf (PieceRole::tom), tom2 = pieceOf (PieceRole::tom, 1), floor = pieceOf (PieceRole::floorTom);
        const auto crash = pieceOf (PieceRole::crash), ride = pieceOf (PieceRole::ride);
        const auto eighth = 60.0 / 100.0 / 2.0 * rate;   // 100 bpm
        std::vector<std::tuple<int, int, int>> hits;

        auto add = [&] (int articulation, int velocity, double eighths)
        {
            if (articulation >= 0)
                hits.emplace_back (articulation, velocity, (int) (eighths * eighth));
        };

        for (int bar = 0; bar < 2; ++bar)
        {
            const auto b = bar * 8.0;

            for (int i = 0; i < 8; ++i)
                add (bar == 0 ? hat : ride, i % 2 == 0 ? 100 : 70, b + i);

            add (kick, 110, b);
            add (kick, 100, b + 3);
            add (kick, 110, b + 4);
            add (snare, 110, b + 2);
            add (snare, 115, b + 6);
        }

        add (crash, 115, 0.0);
        add (tom1, 110, 16.0);
        add (tom1, 100, 16.5);
        add (tom2, 110, 17.0);
        add (tom2, 100, 17.5);
        add (floor, 115, 18.0);
        add (floor, 105, 18.5);
        add (kick, 120, 20.0);
        add (crash, 120, 20.0);

        std::sort (hits.begin(), hits.end(), [] (const auto& a, const auto& b) { return std::get<2> (a) < std::get<2> (b); });
        return hits;
    }
}

int emulationCompare (const juce::StringArray& args)
{
    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--emulation-compare") + 1]);
    const auto out = args.contains ("--out") ? juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--out") + 1])
                                             : juce::File();

    EngineFeedback feedback;
    Rig rig (feedback);

    if (! rig.load (folder, true))
        return 1;

    const auto& info = rig.info;
    const auto rate = rig.rate;
    const Pair realOh { "real overheads", firstMicOf (info, MicRole::overheads, false) };
    const Pair emuOh { "emulated overheads", firstMicOf (info, MicRole::overheads, true) };
    const Pair realRoom { "real room", firstMicOf (info, MicRole::room, false) };
    const Pair emuRoom { "emulated room", firstMicOf (info, MicRole::room, true) };
    rig.params.emulate[(size_t) MicRole::overheads] = rig.params.emulate[(size_t) MicRole::room] = true;

    std::cout << info.credits.name << " at " << rate << " Hz; emulated room: "
              << irs::builtIn()[(size_t) emulation::defaultRoomFor (info.id)].name << std::endl;

    const auto compare = realOh.slot >= 0 && emuOh.slot >= 0;
    juce::AudioBuffer<float> mainOut;

    // A Convolution swaps a new IR in during process(), which the room only
    // calls once something reaches it: one hit first, so the room is ready.
    for (const auto& piece : rig.kit->pieces)
    {
        if (piece.emulationSources != 0)
        {
            rig.render ({ { piece.firstArticulation, 110, 0 } }, (int) (3.0 * rate), {}, mainOut);
            break;
        }
    }

    // Emulated minus real level, per pair, over the pieces heard through close mics.
    std::array<std::vector<float>, 2> differences;

    if (compare)
    {
        const std::vector<int> slots { -2, realOh.slot, emuOh.slot, realRoom.slot, emuRoom.slot };
        std::cout << "\nPer piece, one hit at velocity 110: level (dB RMS over 0.5 s), arrival after the close mics (ms),"
                     "\nleft-right level (dB) and time (ms, + = right later), room ring time (s to fall 30 dB), and"
                     "\nthe spectrum (dB per octave, 63 Hz to 16 kHz, 0 = the loudest band)." << std::endl;

        for (size_t p = 0; p < info.pieces.size(); ++p)
        {
            const auto& piece = rig.kit->pieces[p];

            if (piece.emulationSources == 0)
                continue;

            if (args.contains ("--piece") && info.pieces[p].name != args[args.indexOf ("--piece") + 1])
                continue;

            std::vector<Stereo> pairs;
            rig.render ({ { piece.firstArticulation, 110, 0 } }, (int) (2.5 * rate), slots, mainOut, &pairs);
            const auto closeOnset = onset (pairs[0]);
            auto closeRecorded = false;

            for (int m = 0; m < rig.kit->numMics; ++m)
                closeRecorded = closeRecorded || (((piece.emulationSources >> m) & 1u) != 0 && rig.kit->mics[(size_t) m].role == MicRole::close);

            juce::String source;

            for (int m = 0; m < rig.kit->numMics; ++m)
                if (((piece.emulationSources >> m) & 1u) != 0)
                    source << info.mics[(size_t) m].name;

            std::cout << "\n" << info.pieces[p].name << " (heard through " << source << ")"
                      << (closeRecorded ? "" : " (no close mic: heard through the overheads)") << std::endl;

            for (size_t i = 1; i < pairs.size(); ++i)
            {
                const auto& pair = i == 1 ? realOh : i == 2 ? emuOh : i == 3 ? realRoom : emuRoom;

                if (pair.slot < 0)
                    continue;

                const auto& b = pairs[i];
                const auto [lr, lag] = image (b, rate);

                if ((i == 2 || i == 4) && closeRecorded && pairs[i - 1].getNumSamples() > 0 && (i == 2 ? realOh.slot : realRoom.slot) >= 0)
                    differences[i == 2 ? 0 : 1].push_back (rmsDb (b, 0, (int) (0.5 * rate)) - rmsDb (pairs[i - 1], 0, (int) (0.5 * rate)));
                const auto spectrum = bands (b, rate);
                juce::String line;
                line << "  " << pair.name.paddedRight (' ', 19) << juce::String (rmsDb (b, 0, (int) (0.5 * rate)), 1).paddedLeft (' ', 6) << " dB"
                     << juce::String (1000.0 * (onset (b) - closeOnset) / rate, 1).paddedLeft (' ', 7) << " ms"
                     << juce::String (lr, 1).paddedLeft (' ', 6) << " dB" << juce::String (lag, 2).paddedLeft (' ', 6) << " ms";

                if (i >= 3)
                    line << juce::String (ringTime (b, rate), 2).paddedLeft (' ', 6) << " s";
                else
                    line << "        ";

                line << "  |";

                for (auto d : spectrum)
                    line << juce::String (juce::roundToInt (d)).paddedLeft (' ', 4);

                std::cout << line << std::endl;
            }
        }
    }

    for (size_t i = 0; i < differences.size(); ++i)
    {
        if (differences[i].empty())
            continue;

        auto sum = 0.0f;
        juce::String list;

        for (auto d : differences[i])
        {
            sum += d;
            list << juce::String (d, 1) << " ";
        }

        std::cout << "\nEmulated " << (i == 0 ? "overheads" : "room") << " minus real, close-miked pieces: mean "
                  << juce::String (sum / (float) differences[i].size(), 1) << " dB (" << list.trim() << ")" << std::endl;
    }

    if (out != juce::File())
    {
        out.createDirectory();
        const auto hits = pattern (*rig.kit, info, rate);
        const auto length = (int) (std::get<2> (hits.back()) + 3.0 * rate);
        const auto stem = juce::File::createLegalFileName (info.credits.name);
        std::vector<Stereo> pairs;
        const std::vector<int> slots { -2, realOh.slot, emuOh.slot, realRoom.slot, emuRoom.slot };
        rig.render (hits, length, slots, mainOut, &pairs);
        const std::array<const char*, 5> names { "close", "overheads-real", "overheads-emulated", "room-real", "room-emulated" };

        for (size_t i = 0; i < pairs.size(); ++i)
            if (i == 0 || slots[i] >= 0)
                writeWav (out.getChildFile (stem + " - " + names[i] + ".wav"), pairs[i], rate);

        // Whole mixes: close + each kind of overheads and room, at the kit's own mic levels.
        auto mix = [&] (bool emulated)
        {
            Stereo b (2, length);
            b.makeCopyOf (pairs[0]);

            for (const auto& [slot, index] : { std::pair { emulated ? emuOh.slot : realOh.slot, emulated ? 2 : 1 },
                                               std::pair { emulated ? emuRoom.slot : realRoom.slot, emulated ? 4 : 3 } })
            {
                if (slot < 0)
                    continue;

                for (int ch = 0; ch < 2; ++ch)
                    b.addFrom (ch, 0, pairs[(size_t) index], ch, 0, length);
            }

            return b;
        };

        if (realOh.slot >= 0 || realRoom.slot >= 0)
            writeWav (out.getChildFile (stem + " - mix-real.wav"), mix (false), rate);

        writeWav (out.getChildFile (stem + " - mix-emulated.wav"), mix (true), rate);
        std::cout << "\nWrote the pattern to " << out.getFullPathName() << std::endl;
    }

    return 0;
}
