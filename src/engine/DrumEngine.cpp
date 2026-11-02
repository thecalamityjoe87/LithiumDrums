#include "DrumEngine.h"

#include "dsp/Pan.h"

#include <cmath>
#include <limits>

namespace lithium
{

namespace
{
    constexpr double hiHatChokeSeconds = 0.03;     // closing the hats
    constexpr double cymbalChokeSeconds = 0.07;    // grabbing a cymbal
    constexpr double stealFadeSeconds = 0.005;     // stolen voices are quiet already
    constexpr double pedalDampSeconds = 0.08;      // partly closing the pedal on ringing hats

    // A drum over its voice limit loses its oldest hit. That hit is mostly
    // tail by then, but a cymbal's tail is still wash you'd hear vanish, so
    // it goes slowly.
    constexpr double pieceLimitFadeSeconds = 0.1;
    constexpr float maxDynamicsDb = 30.0f;         // the most the Dynamics control adds to or takes from a soft hit

    // Width of the crossfade around a zone boundary, as a fraction of the
    // narrower of the two zones.
    constexpr double crossfadeWidth = 0.35;

    // Between two openness or position steps, the middle of the gap (this
    // fraction of it) crossfades from one to the other; either side plays
    // the nearer step alone. Mostly playing real recordings keeps the sound
    // natural; the crossfade means a slowly moving pedal never jumps.
    constexpr float stepCrossfadeWidth = 0.3f;

    // Closing the pedal damps ringing hats that are this much more open
    // than the pedal now is.
    constexpr float pedalDampMargin = 0.4f;
    constexpr float pedalClosed = 0.05f;
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
    streamOut.setSize (maxChannelsPerMic, maxSliceSize);
    envelope.assign ((size_t) maxSliceSize, 0.0f);

    context.scratch = { scratch.getWritePointer (0), scratch.getWritePointer (1) };
    context.streamOut = { streamOut.getWritePointer (0), streamOut.getWritePointer (1) };
    context.envelope = envelope.data();
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

    noteCount = (noteCount + 1) & 0xffff;
    feedback.lastNote.store (std::max (1u, noteCount) << 16 | (std::uint32_t) (velocity & 0x7f) << 8 | (std::uint32_t) note,
                             std::memory_order_relaxed);

    const auto& target = kit.notes.targets[(size_t) note];

    // A note that plays nothing can choke cymbals. One that plays something
    // never does, so a kit's own map that uses 85-95 (Virtuosity's does)
    // doesn't choke the cymbals whenever it plays a snare.
    if (target.articulation < 0)
    {
        for (size_t p = 0; p < kit.pieces.size(); ++p)
            if (params.pieces[p].chokeNote == note)
                chokePiece ((int) p, cymbalChokeSeconds);

        return;
    }

    // E-kit notes can follow a controller: a hi-hat hit takes its openness
    // from the pedal, a snare or ride hit its strike position. Until that
    // controller arrives, the note plays its own articulation.
    const auto& art = kit.articulations[(size_t) target.articulation];
    const auto& piece = kit.pieces[(size_t) art.piece];

    if (target.follow == Follow::pedal && hatOpenness >= 0.0f)
        playLadder (kit, piece.hatLadder (art.stroke), hatOpenness, velocity, params);
    else if (target.follow == Follow::position && position[(size_t) target.source] >= 0.0f)
        playLadder (kit, piece.positionLadder, position[(size_t) target.source], velocity, params);
    else
        trigger (kit, target.articulation, velocity, params);
}

void DrumEngine::playLadder (Kit& kit, const Ladder& ladder, float value, int velocity, const EngineParams& params) noexcept
{
    jassert (ladder.numSteps > 0);   // the resolver only sets follow where there are steps
    const auto n = ladder.numSteps;
    auto lower = 0, upper = 0;
    auto t = 0.0f;   // 0 = lower alone .. 1 = upper alone

    if (value >= ladder.value[(size_t) n - 1])
    {
        lower = upper = n - 1;
    }
    else if (value > ladder.value[0])
    {
        while (lower + 2 < n && value >= ladder.value[(size_t) lower + 1])
            ++lower;

        upper = lower + 1;
        const auto x = (value - ladder.value[(size_t) lower]) / (ladder.value[(size_t) upper] - ladder.value[(size_t) lower]);
        t = juce::jlimit (0.0f, 1.0f, (x - 0.5f * (1.0f - stepCrossfadeWidth)) / stepCrossfadeWidth);
    }

    const auto a = ladder.articulation[(size_t) lower];
    const auto b = ladder.articulation[(size_t) upper];

    if (t <= 0.0f || t >= 1.0f || a == b)
    {
        trigger (kit, t >= 1.0f ? b : a, velocity, params);
        return;
    }

    // Both steps' chokes first, so a closed step (which chokes the open
    // ones) doesn't cut off the more open step it's blended with.
    applyChokes (kit, kit.articulations[(size_t) a]);
    applyChokes (kit, kit.articulations[(size_t) b]);

    // Equal-power, with the louder step last so it's the one the UI shows.
    const auto angle = t * juce::MathConstants<float>::halfPi;
    const auto first = t < 0.5f ? b : a, second = t < 0.5f ? a : b;
    trigger (kit, first, velocity, params, t < 0.5f ? std::sin (angle) : std::cos (angle), false);
    trigger (kit, second, velocity, params, t < 0.5f ? std::cos (angle) : std::sin (angle), false);
}

void DrumEngine::controlChange (Kit& kit, int controller, int value) noexcept
{
    const auto& c = kit.notes.controllers;
    const auto v = (float) juce::jlimit (0, 127, value) / 127.0f;

    if (controller == c.hihatPedal)
    {
        const auto openness = c.pedalInverted ? v : 1.0f - v;

        if (hatOpenness >= 0.0f && openness < hatOpenness)
            dampHats (kit, openness);

        hatOpenness = openness;
        feedback.hihatOpenness.store (openness, std::memory_order_relaxed);
    }

    for (size_t i = 0; i < c.position.size(); ++i)
    {
        if (controller == c.position[i])
        {
            position[i] = v;
            feedback.position[i].store (v, std::memory_order_relaxed);
        }
    }
}

void DrumEngine::dampHats (const Kit& kit, float openness) noexcept
{
    // Closing the pedal on ringing hats damps them, like real hats: a voice
    // played well more open than the pedal is now fades out, and once the
    // pedal is shut every open step does, as fast as a closed hit chokes them.
    const auto shut = openness < pedalClosed;

    for (auto& v : voices)
    {
        if (! v.isActive() || v.isFadingOut())
            continue;

        const auto stepOpenness = kit.articulations[(size_t) v.getArticulation()].openness;

        if (stepOpenness > (shut ? pedalClosed : openness + pedalDampMargin))
            v.fadeOut (secondsToSamples (shut ? hiHatChokeSeconds : pedalDampSeconds));
    }
}

void DrumEngine::applyChokes (const Kit& kit, const Articulation& art) noexcept
{
    if (art.chokesMask == 0)
        return;

    for (auto& v : voices)
    {
        if (! v.isActive() || (v.getChokeGroups() & art.chokesMask) == 0)
            continue;

        if (v.getPiece() == art.piece)
        {
            // How a hit treats its own piece depends on what the piece is (see SelfChoke).
            if (art.selfChoke == SelfChoke::none)
                continue;

            if (art.selfChoke == SelfChoke::moreOpen && kit.articulations[(size_t) v.getArticulation()].hatLevel <= art.hatLevel)
                continue;
        }

        v.fadeOut (secondsToSamples (hiHatChokeSeconds));
    }
}

void DrumEngine::polyAftertouch (Kit& kit, int note, int pressure) noexcept
{
    // E-kits (Roland, Alesis) send polyphonic aftertouch when a cymbal edge is grabbed.
    if (! juce::isPositiveAndBelow (note, 128) || pressure < 64)
        return;

    if (const auto target = kit.notes.targets[(size_t) note].articulation; target >= 0)
        chokePiece (kit.articulations[(size_t) target].piece, cymbalChokeSeconds);
}

void DrumEngine::trigger (Kit& kit, int articulationIndex, int velocity, const EngineParams& params,
                          float gainScale, bool chokeOthers) noexcept
{
    const auto& art = kit.articulations[(size_t) articulationIndex];
    velocity = juce::jlimit (1, 127, velocity);

    // Tells the UI what played. With a velocity crossfade, that's the louder of the two zones.
    auto report = [&] (int zoneIndex, int roundRobin)
    {
        const auto p = (size_t) art.piece;
        feedback.lastHit[p].store (LastHit { articulationIndex, zoneIndex - art.firstZone, std::max (0, roundRobin), velocity }.pack(),
                                   std::memory_order_relaxed);
        feedback.pieceHits[p].fetch_add (1, std::memory_order_release);
    };

    if (chokeOthers)
        applyChokes (kit, art);

    // The Tune parameter is added to the kit's (or preset's) own tuning of the piece.
    const auto& piece = params.pieces[(size_t) art.piece];
    const auto semitones = piece.tuneSemitones + kit.pieces[(size_t) art.piece].defaultTune;
    // The piece's own sample rate: a piece swapped in from another kit may have been recorded at another.
    const auto pitchRatio = std::exp2 (semitones / 12.0) * kit.pieces[(size_t) art.piece].sampleRate / sampleRate;

    // Within one zone, velocity still scales the level a little (by the zone's
    // velocityRangeDb), so the response is continuous instead of stepping at
    // zone boundaries.
    //
    // Dynamics then widens (or narrows) the whole range: a soft hit is
    // turned down (or up) by up to maxDynamicsDb, in proportion to how soft it is.
    const auto dynamicsDb = piece.dynamics * maxDynamicsDb * juce::jlimit (-1.0f, 0.0f, std::log10 ((float) velocity / 127.0f) / 2.1f);
    const auto dynamicsGain = juce::Decibels::decibelsToGain (dynamicsDb) * gainScale;

    auto velocityGain = [velocity, dynamicsGain] (const Zone& z)
    {
        const auto t = z.velocityHigh > z.velocityLow
                         ? juce::jlimit (0.0f, 1.0f, (float) (velocity - z.velocityLow) / (float) (z.velocityHigh - z.velocityLow))
                         : 1.0f;
        return z.gain * dynamicsGain * juce::Decibels::decibelsToGain (-z.velocityRangeDb * (1.0f - t));
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
            const auto lowerHit = startVoice (kit, art, articulationIndex, lowerIndex, velocityGain (lower) * std::cos (angle), pitchRatio, piece);
            const auto upperHit = startVoice (kit, art, articulationIndex, lowerIndex + 1, velocityGain (upper) * std::sin (angle), pitchRatio, piece);

            if (t < 0.5)
                report (lowerIndex, lowerHit);
            else
                report (lowerIndex + 1, upperHit);

            return true;
        };

        const auto rel = zoneIndex - art.firstZone;

        if (rel + 1 < art.numZones && tryCrossfade (zoneIndex))
            return;

        if (rel > 0 && tryCrossfade (zoneIndex - 1))
            return;
    }

    report (zoneIndex, startVoice (kit, art, articulationIndex, zoneIndex, velocityGain (zone), pitchRatio, piece));
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

int DrumEngine::startVoice (Kit& kit, const Articulation& art, int articulationIndex, int zoneIndex,
                            float gain, double pitchRatio, const PieceParams& shaping) noexcept
{
    auto* voice = allocateVoice (art.piece);

    if (voice == nullptr)
        return -1;

    VoiceTrigger t;
    t.articulation = articulationIndex;
    t.hit = kit.nextRoundRobin (zoneIndex, random);
    t.gain = gain;
    t.pitchRatio = pitchRatio;
    t.attackSamples = (int) std::lround (shaping.attackMs * 0.001 * sampleRate);
    t.holdSamples = (int) std::lround (shaping.holdMs * 0.001 * sampleRate);

    // The pitch envelope falls to 5% of its start (three time constants) over its time.
    if (std::abs (shaping.pitchEnvSemitones) > 0.001f)
    {
        t.pitchEnvSemitones = shaping.pitchEnvSemitones;
        t.pitchEnvDecay = (float) std::exp (-3.0 / (std::max (1.0, (double) shaping.pitchEnvMs) * 0.001 * sampleRate));
    }

    // A voice found by allocateVoice() is never active, so start() has
    // nothing to close and doesn't need the old kit.
    voice->start (kit, art.piece, art.chokeGroupMask, t, nextSerial++);
    return t.hit - kit.zones[(size_t) zoneIndex].firstHit;
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
        oldestForPiece->fadeOut (secondsToSamples (pieceLimitFadeSeconds));
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
    const auto anyMicSolo = params.anyMicSoloed (kit.numMics);

    // The close mics' trims and pans, once per mic rather than per piece.
    std::array<std::array<float, 2>, maxMics> micGains {};

    for (int m = 0; m < kit.numMics; ++m)
    {
        const auto& mic = kit.mics[(size_t) m];
        const auto& mp = params.mics[(size_t) m];

        if (! mic.direct || mp.mute || (anyMicSolo && ! mp.solo))
            continue;

        const auto [l, r] = dsp::panGains (mic.defaultPan + mp.pan, mic.numChannels);
        micGains[(size_t) m] = { mic.defaultGain * mp.volume * l, mic.defaultGain * mp.volume * r };
    }

    for (size_t i = 0; i < (size_t) numPieces; ++i)
    {
        const auto& pp = params.pieces[i];
        const auto& kitPiece = kit.pieces[i];
        const auto audible = ! pp.mute && (! anySolo || pp.solo);
        const auto level = audible ? pp.volume * kitPiece.defaultGain : 0.0f;
        const auto decay = pp.decay * kitPiece.defaultDecay;

        for (size_t r = 0; r < (size_t) numMicRoles; ++r)
            context.roleGains[i][r] = level * pp.sends[r];

        for (int m = 0; m < kit.numMics; ++m)
        {
            const auto send = level * pp.sends[(size_t) kit.mics[(size_t) m].role];
            context.directGains[i][(size_t) m] = { send * micGains[(size_t) m][0], send * micGains[(size_t) m][1] };
        }

        // Decay 1.0 leaves the sample untouched. Below that, an exponential
        // decay starts after the hold, with a time constant from 30 ms
        // (decay 0) up to 4 s (just below 1).
        if (decay >= 0.999f)
        {
            context.decayCoefficient[i] = 1.0f;
        }
        else
        {
            const auto tau = 0.03 * std::pow (4.0 / 0.03, (double) decay);
            context.decayCoefficient[i] = (float) std::exp (-1.0 / (tau * sampleRate));
        }

        // The tone filters: pass-through (and skipped) at their ends.
        context.toneHighPass[i] = pp.toneHighPassHz > PieceParams::toneOffHz + 0.5f
                                    ? dsp::designSvf (dsp::SvfType::highPass, pp.toneHighPassHz, 0.0, 0.7071, sampleRate)
                                    : dsp::SvfCoefficients {};
        context.toneLowPass[i] = pp.toneLowPassHz < PieceParams::toneOpenHz - 1.0f
                                   ? dsp::designSvf (dsp::SvfType::lowPass, pp.toneLowPassHz, 0.0, 0.7071, sampleRate)
                                   : dsp::SvfCoefficients {};
    }
}

void DrumEngine::render (Kit& kit, VoiceBuses buses, int startSample, int numSamples,
                         const EngineParams& params) noexcept
{
    updateRenderContext (kit, params);

    for (int offset = 0; offset < numSamples; offset += maxSliceSize)
    {
        const auto slice = std::min (maxSliceSize, numSamples - offset);

        for (auto& v : voices)
            if (v.isActive())
                v.render (kit, buses, startSample + offset, slice, context);
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
