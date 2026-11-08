#include "MicEmulation.h"

#include "IrLoader.h"

#include <cmath>

namespace lithium
{

namespace emulation
{

Position positionOf (std::optional<StageSlot> slot, PieceRole role, int extra) noexcept
{
    // The stage slots: where assets/blender-kit.py puts each piece (the
    // drum's head, or the cymbal's centre). The camera there looks from the
    // front, so x is to the listener's right, as the kits' own pans have it
    // (the hi-hat panned right).
    if (slot.has_value())
    {
        switch (*slot)
        {
            case StageSlot::kick:     return { 0.0f, 0.15f, 0.31f };
            case StageSlot::snare:    return { 0.36f, 0.42f, 0.58f };
            case StageSlot::hihat:    return { 0.68f, 0.42f, 0.93f };
            case StageSlot::tom1:     return { 0.165f, 0.10f, 0.84f };
            case StageSlot::tom2:     return { -0.185f, 0.10f, 0.86f };
            case StageSlot::floorTom: return { -0.60f, 0.42f, 0.40f };
            case StageSlot::crash:    return { 0.34f, 0.04f, 1.28f };
            case StageSlot::crash2:   return { -0.36f, 0.06f, 1.28f };
            case StageSlot::ride:     return { -0.66f, 0.10f, 1.06f };
            case StageSlot::china:    return { -0.90f, 0.36f, 1.26f };
            case StageSlot::splash:   return { 0.62f, 0.20f, 1.08f };
        }
    }

    // Pieces with no slot: where a drummer would most likely put them.
    const auto k = (float) extra;

    switch (role)
    {
        case PieceRole::kick:     return { -0.25f - 0.1f * k, 0.15f, 0.31f };      // a second kick, left of the first
        case PieceRole::snare:    return { 0.62f, 0.10f, 0.72f };                   // a side snare by the hi-hat
        case PieceRole::hihat:    return { 0.50f, 0.05f, 1.00f };                   // an X-hat
        case PieceRole::tom:      return { k < 1.0f ? -0.45f : 0.45f, 0.12f, 0.82f };
        case PieceRole::floorTom: return { -0.95f - 0.25f * k, 0.50f, 0.42f };
        case PieceRole::crash:    return { k < 1.0f ? 0.0f : 0.85f, 0.0f, 1.38f };
        case PieceRole::ride:     return { -0.45f, 0.35f, 1.00f };
        case PieceRole::china:    return { 0.85f, 0.25f, 1.30f };
        case PieceRole::other:    break;
    }

    // Percussion: spread along the front of the kit.
    const auto spread = std::fmod (k, 5.0f) - 2.0f;
    return { 0.18f * spread, -0.35f, 0.90f };
}

std::array<Position, 2> micPositions (MicRole role) noexcept
{
    // Overheads: a spaced pair 70 cm apart, 1.5 m up (about 70 cm over the
    // toms), centred on the snare as engineers usually set them, a little in
    // front of the drummer. Room: a wide pair 2.6 m in front of the kit.
    if (role == MicRole::room)
        return {{ { -1.4f, -2.6f, 1.4f }, { 1.4f, -2.6f, 1.4f } }};

    return {{ { -0.1f, 0.35f, 1.5f }, { 0.6f, 0.35f, 1.5f } }};
}

namespace
{
    // The emulated mics' own level (their channel's 0 dB): set so that, at
    // the kit's own mix, they sit where real ones do. Measured against
    // DRSKit's and MuldjordKit's (whose rooms are 10 dB apart: this is
    // between the two).
    float emulatedLevelDb (MicRole role) noexcept
    {
        return role == MicRole::room ? 13.0f : 2.0f;
    }
}

void addEmulatedMics (Kit& kit, KitInfo& info, bool evenIfRecorded)
{
    // Every piece's place on the kit.
    std::array<int, numPieceRoles> extras {};

    for (size_t p = 0; p < kit.pieces.size(); ++p)
    {
        auto& piece = kit.pieces[p];
        const auto slot = info.slotOf ((int) p);
        piece.position = positionOf (slot, piece.role, slot.has_value() ? 0 : extras[(size_t) piece.role]++);
    }

    // Which mics hear each piece: its close mics, or else the overheads.
    std::array<bool, numMicRoles> hasRole {};

    for (int m = 0; m < kit.numMics; ++m)
        hasRole[(size_t) kit.mics[(size_t) m].role] = true;

    // How loud a mic has a piece, in the kit's mix: the energy of the start
    // of the loudest hit of its loudest articulation, at the mic's level. 0
    // if the mic didn't record the piece; a tiny positive number if it did
    // but its audio can't be read (the mic isn't loaded), so it can still be chosen.
    constexpr int window = 4096;
    juce::AudioBuffer<float> scratch (maxChannelsPerMic, window);

    auto loudness = [&] (const PieceData& piece, int mic)
    {
        double loudest = 0.0;
        auto any = false;

        for (int a = piece.firstArticulation; a < piece.firstArticulation + piece.numArticulations; ++a)
        {
            const auto& art = kit.articulations[(size_t) a];

            if (art.numZones == 0)
                continue;

            const auto& zone = kit.zones[(size_t) (art.firstZone + art.numZones - 1)];   // sorted by velocity
            const auto id = kit.getSample (zone.firstHit, mic);

            if (id == invalidSampleId)
                continue;

            any = true;

            if (kit.samples == nullptr)
                continue;

            scratch.clear();
            const auto frames = kit.samples->peekStart (id, scratch.getArrayOfWritePointers(), window);
            double energy = 0.0;

            for (int ch = 0; ch < scratch.getNumChannels(); ++ch)
                for (int i = 0; i < frames; ++i)
                    energy += (double) scratch.getSample (ch, i) * scratch.getSample (ch, i);

            loudest = std::max (loudest, energy);
        }

        const auto gain = (double) kit.mics[(size_t) mic].defaultGain;
        return any ? std::max (1.0e-30, loudest * gain * gain) : 0.0;
    };

    const auto numPieces = kit.pieces.size();
    std::vector<std::array<double, maxMics>> heard (numPieces);   // [piece][mic]
    std::array<double, maxMics> loudestOnMic {};

    for (size_t p = 0; p < numPieces; ++p)
    {
        for (int m = 0; m < kit.numMics; ++m)
        {
            heard[p][(size_t) m] = loudness (kit.pieces[p], m);
            loudestOnMic[(size_t) m] = std::max (loudestOnMic[(size_t) m], heard[p][(size_t) m]);
        }
    }

    // A close mic is one of a piece's own only if the piece is about as loud
    // in it as the loudest piece it has: within 12 dB. A crash that only
    // leaks into the hi-hat's mic is heard through the overheads, not
    // through the hi-hat's mic.
    constexpr double ownMicRange = 0.063;   // -12 dB, as energy

    info.emulationSources = 0;

    for (size_t p = 0; p < numPieces; ++p)
    {
        auto& piece = kit.pieces[p];
        piece.emulationSources = 0;

        for (const auto role : { MicRole::close, MicRole::overheads })
        {
            auto best = 0.0;

            for (int m = 0; m < kit.numMics; ++m)
            {
                const auto l = heard[p][(size_t) m];

                if (kit.mics[(size_t) m].role != role || l <= 0.0)
                    continue;

                if (role == MicRole::close && l < loudestOnMic[(size_t) m] * ownMicRange)
                    continue;

                if (l > best)
                {
                    best = l;
                    piece.emulationSources = 1u << m;
                }
            }

            if (piece.emulationSources != 0)
                break;
        }

        info.emulationSources |= piece.emulationSources;
    }

    if (info.emulationSources == 0)
        return;

    // A slot for each emulated pair, after the kit's own mics.
    auto nextSlot = kit.numMics;

    for (const auto role : { MicRole::overheads, MicRole::room })
    {
        if (hasRole[(size_t) role] && ! evenIfRecorded)
            continue;

        if (nextSlot >= maxMics || kit.numBusChannels + 2 > maxMicBusChannels)
            break;

        auto& mic = kit.mics[(size_t) nextSlot];
        mic = {};
        mic.defaultGain = juce::Decibels::decibelsToGain (emulatedLevelDb (role));
        mic.numChannels = 2;
        mic.role = role;
        mic.busChannel = kit.numBusChannels;
        mic.direct = false;
        mic.emulated = true;
        kit.numBusChannels += 2;
        kit.emulatedMic[(size_t) role] = nextSlot;

        KitMicInfo micInfo;
        micInfo.id = role == MicRole::room ? "emulated_room" : "emulated_overheads";
        micInfo.name = role == MicRole::room ? "Room" : "Overheads";
        micInfo.numChannels = 2;
        micInfo.defaultDb = emulatedLevelDb (role);
        micInfo.role = role;
        micInfo.direct = false;
        micInfo.emulated = true;
        info.mics.push_back (micInfo);
        info.emulatedMic[(size_t) role] = nextSlot;

        ++nextSlot;
    }
}

int defaultRoomFor (const juce::String& kitId)
{
    // Big kits recorded for a big sound get the bigger, livelier room; the
    // rest a small studio.
    juce::String room = "studio";

    for (const auto* big : { "basalt-garnet", "big-rusty" })
        if (kitId.startsWith (big))
            room = "seminar-room";

    const auto& all = irs::builtIn();

    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].id == room)
            return (int) i;

    return 0;
}

juce::StringArray roomNames()
{
    juce::StringArray names;

    for (const auto& ir : irs::builtIn())
        names.add (ir.name);

    return names;
}

} // namespace emulation

//==============================================================================
namespace
{
    float distance (const emulation::Position& a, const emulation::Position& b) noexcept
    {
        const auto dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt (dx * dx + dy * dy + dz * dz);
    }

    // How the overheads hear a close-miked piece: the close mic's low end
    // (proximity effect, a mic inside the kick) shelved down by this much.
    struct Shelf { float hz, db; };

    // A cymbal's close mic also picks up the drums around it, which the
    // overheads hear from the drums themselves.
    Shelf overheadShelf (PieceRole role) noexcept
    {
        switch (role)
        {
            case PieceRole::kick:     return { 200.0f, -14.0f };
            case PieceRole::snare:    return { 220.0f, -4.0f };
            case PieceRole::tom:      return { 180.0f, -6.0f };
            case PieceRole::floorTom: return { 150.0f, -6.0f };
            case PieceRole::hihat:    return { 300.0f, -8.0f };
            case PieceRole::ride:
            case PieceRole::crash:
            case PieceRole::china:    return { 250.0f, -10.0f };
            case PieceRole::other:    return { 200.0f, -3.0f };
        }

        return { 200.0f, 0.0f };
    }

    // A kit's close mics are mixed hotter than the drums sound from a few
    // feet away: a kick, hi-hat or ride is pushed up close, so a mic over
    // the kit hears them much quieter, relative to the snare, than the close
    // mix has them. A close-miked piece goes into the emulated mics this
    // much quieter (dB). Measured against DRSKit's and MuldjordKit's real
    // overheads and room (LithiumDrumsTests --emulation-compare).
    float closeMixCorrectionDb (PieceRole role) noexcept
    {
        switch (role)
        {
            case PieceRole::kick:     return -5.0f;
            case PieceRole::snare:    return -3.0f;
            case PieceRole::hihat:    return -13.0f;
            case PieceRole::tom:      return -6.0f;
            case PieceRole::floorTom: return -9.0f;
            case PieceRole::ride:     return -12.0f;
            case PieceRole::crash:
            case PieceRole::china:    return -6.0f;
            case PieceRole::other:    return -3.0f;
        }

        return 0.0f;
    }

    // Each pair's level at this distance is 1 (0 dB): the pieces nearer
    // than it come out louder, those further away quieter.
    constexpr std::array<float, 2> referenceDistance { 1.0f, 3.0f };   // overheads, room

    // How level falls with distance: (reference / distance) to this power.
    // A point source falls as 1 / distance (1), but overheads are directional
    // mics aimed down at the kit, so a cymbal right next to one mic isn't as
    // much louder in it as the distances alone say. Real pairs (DRSKit,
    // MuldjordKit) put cymbals 1-6 dB to one side, not 5-8.
    constexpr std::array<float, 2> distanceLaw { 0.6f, 1.0f };

    // The room hears some pieces differently from the overheads (dB, on top
    // of the close-mix correction): a little less of the kick, snare and
    // ride, a little more of the floor tom.
    float roomCorrectionDb (PieceRole role) noexcept
    {
        switch (role)
        {
            case PieceRole::kick:     return -2.0f;
            case PieceRole::snare:    return -2.0f;
            case PieceRole::tom:      return -1.0f;
            case PieceRole::floorTom: return 2.0f;
            case PieceRole::ride:     return -3.0f;
            case PieceRole::hihat:
            case PieceRole::crash:
            case PieceRole::china:
            case PieceRole::other:    break;
        }

        return 0.0f;
    }

    // How much of the sound the floor reflects back up to the overheads.
    constexpr float floorReflection = 0.45f;

    // The room's convolution: everything below this is left out, so a close
    // kick mic's sub doesn't turn the room to mud.
    const ConvolutionSettings roomSettings { 0.0f, 40.0f, 16000.0f, 1.0f };
}

void MicEmulator::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    const auto length = juce::nextPowerOfTwo ((int) std::ceil (maxDelaySeconds * sampleRate) + maxBlockSize + 1);
    ringMask = length - 1;

    for (auto& line : lines)
        for (auto& ring : line.ring)
            ring.assign ((size_t) length, 0.0f);

    roomIn.setSize (2, maxBlockSize);
    room.prepare (sampleRate, maxBlockSize);
    pathsFor = nullptr;
    reset();
}

void MicEmulator::reset() noexcept
{
    for (auto& line : lines)
    {
        for (auto& ring : line.ring)
            std::fill (ring.begin(), ring.end(), 0.0f);

        line.shelfState.reset();
        line.quiet = ringMask + 1;   // drained: nothing to read
        line.lastSend = {};
    }

    writePosition = 0;
    wasOn = {};
    room.reset();
}

void MicEmulator::updatePaths (const Kit& kit) noexcept
{
    pathsFor = &kit;
    pathsForSerial = kit.serial;
    const auto maxDelay = (int) (maxDelaySeconds * sampleRate);

    for (size_t p = 0; p < kit.pieces.size() && p < paths.size(); ++p)
    {
        const auto& piece = kit.pieces[p];
        auto& path = paths[p];

        for (const auto role : { MicRole::overheads, MicRole::room })
        {
            const auto pair = (size_t) pairIndex (role);
            const auto mics = emulation::micPositions (role);

            for (size_t side = 0; side < 2; ++side)
            {
                // The direct sound, and (for the overheads) the floor's
                // reflection: the path to the mic's mirror image under the floor.
                auto mirrored = mics[side];
                mirrored[2] = -mirrored[2];

                const auto direct = std::max (0.05f, distance (piece.position, mics[side]));
                const auto reflected = std::max (0.05f, distance (piece.position, mirrored));
                const auto reference = referenceDistance[pair];

                auto delayOf = [&] (float metres)
                {
                    return std::min (maxDelay, (int) std::lround (metres / emulation::speedOfSound * sampleRate));
                };

                const auto law = distanceLaw[pair];
                path.taps[pair][side][0] = { delayOf (direct), std::pow (reference / direct, law) };
                path.taps[pair][side][1] = role == MicRole::overheads
                                               ? Tap { delayOf (reflected), floorReflection * std::pow (reference / reflected, law) }
                                               : Tap {};   // the room's IR has its own reflections
            }
        }

        // Heard through its close mics, or (with none) through the overheads,
        // which already hear it from a distance.
        auto closeSourced = false;

        for (int m = 0; m < kit.numMics; ++m)
            closeSourced = closeSourced || (((piece.emulationSources >> m) & 1u) != 0 && kit.mics[(size_t) m].role == MicRole::close);

        for (const auto role : { MicRole::overheads, MicRole::room })
        {
            const auto db = (closeSourced ? closeMixCorrectionDb (piece.role) : 0.0f)
                            + (closeSourced && role == MicRole::room ? roomCorrectionDb (piece.role) : 0.0f);

            // Emulated overheads hear only close-miked pieces: one heard
            // through the overheads is already in the kit's own (that only
            // happens when comparing the two, see addEmulatedMics()).
            const auto correction = role == MicRole::overheads && ! closeSourced ? 0.0f : juce::Decibels::decibelsToGain (db);

            for (auto& side : path.taps[(size_t) pairIndex (role)])
                for (auto& tap : side)
                    tap.gain *= correction;
        }

        const auto shelf = closeSourced ? overheadShelf (piece.role) : Shelf { 200.0f, 0.0f };
        path.shelf = juce::exactlyEqual (shelf.db, 0.0f)
                         ? dsp::SvfCoefficients {}
                         : dsp::designSvf (dsp::SvfType::lowShelf, shelf.hz, shelf.db, 0.7071, sampleRate);
    }
}

void MicEmulator::process (const Kit& kit, const EngineParams& params, const juce::AudioBuffer<float>& feed,
                           juce::AudioBuffer<float>& micBus, int numSamples) noexcept
{
    const std::array<bool, numPairs> on { params.isEmulating (kit, MicRole::overheads), params.isEmulating (kit, MicRole::room) };

    // Switched off, a pair forgets its tail, so switching it back on doesn't
    // play a moment of what it heard before.
    for (int pair = 0; pair < numPairs; ++pair)
    {
        if (wasOn[(size_t) pair] && ! on[(size_t) pair])
        {
            for (auto& line : lines)
            {
                std::fill (line.ring[(size_t) pair].begin(), line.ring[(size_t) pair].end(), 0.0f);
                line.lastSend[(size_t) pair] = 0.0f;
            }

            if (pair == 1)
                room.reset();
        }

        wasOn[(size_t) pair] = on[(size_t) pair];
    }

    if (! on[0] && ! on[1])
        return;

    if (&kit != pathsFor || kit.serial != pathsForSerial)
        updatePaths (kit);

    jassert (numSamples <= roomIn.getNumSamples());
    const auto ringLength = ringMask + 1;
    std::array<std::array<float*, 2>, numPairs> out {};

    for (const auto role : { MicRole::overheads, MicRole::room })
    {
        const auto pair = (size_t) pairIndex (role);

        if (on[pair])
        {
            const auto& mic = kit.mics[(size_t) kit.emulatedMic[(size_t) role]];
            out[pair] = role == MicRole::room
                            ? std::array<float*, 2> { roomIn.getWritePointer (0), roomIn.getWritePointer (1) }
                            : std::array<float*, 2> { micBus.getWritePointer (mic.busChannel), micBus.getWritePointer (mic.busChannel + 1) };
        }
    }

    if (on[1])
        roomIn.clear (0, numSamples);

    const auto numPieces = std::min ((int) kit.pieces.size(), maxPieces);

    for (int p = 0; p < numPieces; ++p)
    {
        auto& line = lines[(size_t) p];
        const auto& path = paths[(size_t) p];
        const auto* x = feed.getReadPointer (p);

        // A piece that has been silent for longer than its line holds has
        // nothing left to play: skip it (its line is all zeros by now).
        const auto range = juce::FloatVectorOperations::findMinAndMax (x, numSamples);
        const auto silent = std::max (range.getEnd(), -range.getStart()) < 1.0e-9f;
        line.quiet = silent ? std::min (line.quiet + numSamples, ringLength * 2) : 0;

        if (silent && line.quiet > ringLength)
            continue;

        // Into the lines: shelved for the overheads, as it is for the room.
        for (int i = 0; i < numSamples; ++i)
        {
            const auto w = (writePosition + i) & ringMask;

            if (on[0])
                line.ring[0][(size_t) w] = line.shelfState.process (path.shelf, x[i]);

            if (on[1])
                line.ring[1][(size_t) w] = x[i];
        }

        // Out of them: each tap at its delay, ramping the piece's send (its
        // Overheads or Room knob) across the block.
        for (int pair = 0; pair < numPairs; ++pair)
        {
            if (! on[(size_t) pair])
                continue;

            const auto role = pair == 0 ? MicRole::overheads : MicRole::room;
            const auto send = params.pieces[(size_t) p].sends[(size_t) role];
            const auto startSend = line.lastSend[(size_t) pair];
            const auto step = (send - startSend) / (float) numSamples;
            line.lastSend[(size_t) pair] = send;

            if (juce::exactlyEqual (send, 0.0f) && juce::exactlyEqual (startSend, 0.0f))
                continue;

            const auto* ring = line.ring[(size_t) pair].data();

            for (size_t side = 0; side < 2; ++side)
            {
                auto* o = out[(size_t) pair][side];

                for (const auto& tap : path.taps[(size_t) pair][side])
                {
                    if (juce::exactlyEqual (tap.gain, 0.0f))
                        continue;

                    const auto start = writePosition - tap.delay;

                    for (int i = 0; i < numSamples; ++i)
                        o[i] += ring[(start + i) & ringMask] * tap.gain * (startSend + step * (float) (i + 1));
                }
            }
        }
    }

    writePosition = (writePosition + numSamples) & ringMask;

    // The room pair hears the room.
    if (on[1])
    {
        const auto& mic = kit.mics[(size_t) kit.emulatedMic[(size_t) MicRole::room]];
        room.process (roomIn.getReadPointer (0), roomIn.getReadPointer (1),
                      micBus.getWritePointer (mic.busChannel), micBus.getWritePointer (mic.busChannel + 1),
                      numSamples, roomSettings);
    }
}

} // namespace lithium
