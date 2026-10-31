#include "DrumEngine.h"

#include <cmath>
#include <limits>

namespace lithium
{

namespace
{
    constexpr double hiHatChokeSeconds = 0.03;     // closing the hats
    constexpr double cymbalChokeSeconds = 0.07;    // grabbing a cymbal
    constexpr double stealFadeSeconds = 0.005;     // stolen voices are quiet already
    constexpr double decayHoldSeconds = 0.012;     // the decay control never touches the attack

    // Width of the crossfade around a zone boundary, as a fraction of the
    // narrower of the two zones.
    constexpr double crossfadeWidth = 0.35;
}

DrumEngine::DrumEngine (EngineFeedback& f) : feedback (f)
{
    prepare (sampleRate);   // so the engine is usable even if a host renders before preparing
}

void DrumEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    // The resampler may consume up to maxPitchRatio source frames per output frame.
    scratch.setSize (maxChannelsPerMic, maxSliceSize * Voice::maxPitchRatio + 8);
    envelope.assign ((size_t) maxSliceSize, 0.0f);

    context.scratch = { scratch.getWritePointer (0), scratch.getWritePointer (1) };
    context.envelope = envelope.data();
    context.decayHoldSamples = secondsToSamples (decayHoldSeconds);
}

int DrumEngine::secondsToSamples (double seconds) const noexcept
{
    return std::max (1, (int) std::lround (seconds * sampleRate));
}

//==============================================================================
void DrumEngine::noteOn (Kit& kit, int note, int velocity, const EngineParams& params) noexcept
{
    if (! juce::isPositiveAndBelow (note, 128))
        return;

    for (size_t p = 0; p < kit.pieces.size(); ++p)
        if (params.pieces[p].chokeNote == note)
            chokePiece ((int) p, cymbalChokeSeconds);

    if (const auto target = kit.noteMap[(size_t) note].articulation; target >= 0)
        trigger (kit, target, velocity, params);
}

void DrumEngine::polyAftertouch (Kit& kit, int note, int pressure) noexcept
{
    // E-kits (Roland, Alesis) send polyphonic aftertouch when a cymbal edge is grabbed.
    if (! juce::isPositiveAndBelow (note, 128) || pressure < 64)
        return;

    if (const auto target = kit.noteMap[(size_t) note].articulation; target >= 0)
        chokePiece (kit.articulations[(size_t) target].piece, cymbalChokeSeconds);
}

void DrumEngine::trigger (Kit& kit, int articulationIndex, int velocity, const EngineParams& params) noexcept
{
    const auto& art = kit.articulations[(size_t) articulationIndex];
    velocity = juce::jlimit (1, 127, velocity);

    feedback.pieceHits[(size_t) art.piece].fetch_add (1, std::memory_order_relaxed);

    if (art.chokesMask != 0)
        for (auto& v : voices)
            if (v.isActive() && (v.getChokeGroups() & art.chokesMask) != 0)
                v.fadeOut (secondsToSamples (hiHatChokeSeconds));

    const auto& piece = params.pieces[(size_t) art.piece];
    const auto pitchRatio = std::exp2 (piece.tuneSemitones / 12.0) * kit.sampleRate / sampleRate;

    // Within one zone, velocity still scales the level a little (by the zone's
    // velocityRangeDb), so the response is continuous instead of stepping at
    // zone boundaries.
    auto velocityGain = [velocity] (const Zone& z)
    {
        const auto t = z.velocityHigh > z.velocityLow
                         ? juce::jlimit (0.0f, 1.0f, (float) (velocity - z.velocityLow) / (float) (z.velocityHigh - z.velocityLow))
                         : 1.0f;
        return z.gain * juce::Decibels::decibelsToGain (-z.velocityRangeDb * (1.0f - t));
    };

    const auto zoneIndex = findZone (kit, art, velocity);
    const auto& zone = kit.zones[(size_t) zoneIndex];

    if (params.velocityCrossfade && art.numZones > 1)
    {
        // A velocity near the boundary between two zones plays both, with an
        // equal-power crossfade, so there is no audible step between zones.
        auto tryCrossfade = [&] (int lowerIndex) -> bool
        {
            const auto& lower = kit.zones[(size_t) lowerIndex];
            const auto& upper = kit.zones[(size_t) lowerIndex + 1];
            const auto boundary = 0.5 * (lower.velocityHigh + upper.velocityLow);
            const auto narrower = std::min (lower.velocityHigh - lower.velocityLow, upper.velocityHigh - upper.velocityLow) + 1;
            const auto width = std::max (2.0, crossfadeWidth * narrower);
            const auto t = ((double) velocity - (boundary - 0.5 * width)) / width;

            if (t <= 0.0 || t >= 1.0)
                return false;

            const auto angle = (float) t * juce::MathConstants<float>::halfPi;
            startVoice (kit, art, articulationIndex, lowerIndex, velocityGain (lower) * std::cos (angle), pitchRatio);
            startVoice (kit, art, articulationIndex, lowerIndex + 1, velocityGain (upper) * std::sin (angle), pitchRatio);
            return true;
        };

        const auto rel = zoneIndex - art.firstZone;

        if (rel + 1 < art.numZones && tryCrossfade (zoneIndex))
            return;

        if (rel > 0 && tryCrossfade (zoneIndex - 1))
            return;
    }

    startVoice (kit, art, articulationIndex, zoneIndex, velocityGain (zone), pitchRatio);
}

int DrumEngine::findZone (const Kit& kit, const Articulation& art, int velocity) const noexcept
{
    // Velocities in a gap between zones use the nearest zone.
    auto best = art.firstZone;
    auto bestDistance = std::numeric_limits<int>::max();

    for (int i = art.firstZone; i < art.firstZone + art.numZones; ++i)
    {
        const auto& l = kit.zones[(size_t) i];

        if (velocity >= l.velocityLow && velocity <= l.velocityHigh)
            return i;

        const auto distance = velocity < l.velocityLow ? l.velocityLow - velocity : velocity - l.velocityHigh;

        if (distance < bestDistance)
        {
            best = i;
            bestDistance = distance;
        }
    }

    return best;
}

void DrumEngine::startVoice (Kit& kit, const Articulation& art, int articulationIndex, int zoneIndex,
                             float gain, double pitchRatio) noexcept
{
    auto* voice = allocateVoice (art.piece);

    if (voice == nullptr)
        return;

    VoiceTrigger t;
    t.articulation = articulationIndex;
    t.hit = kit.nextRoundRobin (zoneIndex, random);
    t.gain = gain;
    t.pitchRatio = pitchRatio;

    // A voice found by allocateVoice() is never active, so start() has
    // nothing to close and doesn't need the old kit.
    voice->start (kit, art.piece, art.chokeGroupMask, t, nextSerial++);
}

Voice* DrumEngine::allocateVoice (int piece) noexcept
{
    // Voice stealing, in order:
    //  1. A piece may only have maxVoicesPerPiece voices: fast rolls on one
    //     drum fade out that drum's oldest voice rather than eating the pool.
    //  2. Above voiceStealThreshold active voices, fade out the quietest voice
    //     overall. The steal threshold leaves headroom below the pool size so
    //     stolen voices can fade out (no click) while new hits start in the
    //     spare slots.
    //  3. Only if the pool is completely full (very unusual) is the new hit
    //     dropped.
    int active = 0, activeForPiece = 0;
    Voice* oldestForPiece = nullptr;
    Voice* quietest = nullptr;
    Voice* freeVoice = nullptr;

    for (auto& v : voices)
    {
        if (! v.isActive())
        {
            if (freeVoice == nullptr)
                freeVoice = &v;

            continue;
        }

        ++active;

        if (v.isFadingOut())
            continue;     // already on its way out

        if (v.getPiece() == piece)
        {
            ++activeForPiece;

            if (oldestForPiece == nullptr || v.getSerial() < oldestForPiece->getSerial())
                oldestForPiece = &v;
        }

        if (quietest == nullptr || v.getStealScore() < quietest->getStealScore())
            quietest = &v;
    }

    if (activeForPiece >= maxVoicesPerPiece && oldestForPiece != nullptr)
        oldestForPiece->fadeOut (secondsToSamples (stealFadeSeconds * 4));
    else if (active >= voiceStealThreshold && quietest != nullptr)
        quietest->fadeOut (secondsToSamples (stealFadeSeconds));

    return freeVoice;
}

void DrumEngine::chokePiece (int piece, double seconds) noexcept
{
    for (auto& v : voices)
        if (v.isActive() && v.getPiece() == piece)
            v.fadeOut (secondsToSamples (seconds));
}

//==============================================================================
void DrumEngine::updateRenderContext (const Kit& kit, const EngineParams& params) noexcept
{
    const auto numPieces = (int) kit.pieces.size();
    const auto anySolo = params.anyPieceSoloed (numPieces);

    for (size_t i = 0; i < (size_t) numPieces; ++i)
    {
        const auto& pp = params.pieces[i];
        const auto audible = ! pp.mute && (! anySolo || pp.solo);
        const auto level = audible ? pp.volume : 0.0f;

        for (size_t r = 0; r < (size_t) numMicRoles; ++r)
            context.roleGains[i][r] = level * pp.sends[r];

        // Decay 1.0 leaves the sample untouched. Below that, an exponential
        // decay starts after a short hold, with a time constant from 30 ms
        // (decay 0) up to 4 s (just below 1).
        if (pp.decay >= 0.999f)
        {
            context.decayCoefficient[i] = 1.0f;
        }
        else
        {
            const auto tau = 0.03 * std::pow (4.0 / 0.03, (double) pp.decay);
            context.decayCoefficient[i] = (float) std::exp (-1.0 / (tau * sampleRate));
        }
    }
}

void DrumEngine::render (Kit& kit, juce::AudioBuffer<float>& micBus, int startSample, int numSamples,
                         const EngineParams& params) noexcept
{
    updateRenderContext (kit, params);

    for (int offset = 0; offset < numSamples; offset += maxSliceSize)
    {
        const auto slice = std::min (maxSliceSize, numSamples - offset);

        for (auto& v : voices)
            if (v.isActive())
                v.render (kit, micBus, startSample + offset, slice, context);
    }

    feedback.activeVoices.store (getNumActiveVoices(), std::memory_order_relaxed);
}

void DrumEngine::fadeOutAll (double seconds) noexcept
{
    for (auto& v : voices)
        if (v.isActive())
            v.fadeOut (secondsToSamples (seconds));
}

void DrumEngine::stopAll (Kit& kit) noexcept
{
    for (auto& v : voices)
        if (v.isActive())
            v.stop (kit);

    feedback.activeVoices.store (0, std::memory_order_relaxed);
}

int DrumEngine::getNumActiveVoices() const noexcept
{
    int n = 0;

    for (const auto& v : voices)
        n += v.isActive() ? 1 : 0;

    return n;
}

} // namespace lithium
