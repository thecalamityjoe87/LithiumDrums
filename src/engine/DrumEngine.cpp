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
    constexpr double pedalShutSeconds = 0.05;      // shutting it: a short fade into the closed sound

    // A drum over its voice limit loses its oldest hit. That hit is mostly
    // tail by then, but a cymbal's tail is still wash you'd hear vanish, so
    // it goes slowly.
    constexpr double pieceLimitFadeSeconds = 0.1;
    constexpr float maxDynamicsDb = 30.0f;         // the most the Dynamics control adds to or takes from a soft hit

    // Width of the crossfade around a zone boundary, as a fraction of the
    // narrower of the two zones.
    constexpr double crossfadeWidth = 0.35;

    // Closing the pedal damps ringing hats that are this much more open
    // than the pedal now is.
    constexpr float pedalDampMargin = 0.4f;
    constexpr float pedalClosed = 0.05f;

    // A pedal sensor's reading wobbles by a step or so even when the foot
    // is still. Changes this small are ignored (hysteresis); a bigger move
    // is taken as it is. So hats resting on a threshold don't flicker
    // between two recordings, and a real move never lags.
    constexpr float pedalDeadBand = 1.5f / 127.0f;

    // Chicks and splashes from the pedal alone: a close counts if the pedal
    // travelled at least this far in the last pedalWindowMs, and a splash is
    // a reopening past splashOpenness within splashWindowMs of a close.
    constexpr float minimumCloseTravel = 0.2f;
    constexpr double pedalWindowMs = 150.0;
    constexpr double splashWindowMs = 250.0;
    constexpr float splashOpenness = 0.3f;

    // Emulated strike positions: how much quieter a hit at the edge is.
    constexpr float edgeLevelDb = -2.5f;

    bool channelMatches (int wanted, int channel) noexcept
    {
        // The on-screen keyboard (channel 0) plays whatever channel is set.
        return wanted == 0 || channel == 0 || wanted == channel;
    }

    float curve (float x, float amount) noexcept
    {
        // An exponent from 1/4 to 4: above 0 the value rises sooner, below 0 later.
        return std::pow (juce::jlimit (0.0f, 1.0f, x), std::pow (4.0f, -juce::jlimit (-1.0f, 1.0f, amount)));
    }

    float calibrate (int value, int low, int high) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (float) (value - low) / (float) std::max (1, high - low));
    }
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
float DrumEngine::latched (const PerChannel& values, int channel) noexcept
{
    if (juce::isPositiveAndNotGreaterThan (channel, 16) && channel > 0 && values[(size_t) channel] >= 0.0f)
        return values[(size_t) channel];

    return values[0];
}

float DrumEngine::pedalOpenness (int value, const ControllerSettings& c) noexcept
{
    const auto v = calibrate (value, c.pedalMin, c.pedalMax);
    return curve (c.pedalInverted ? v : 1.0f - v, c.pedalCurve);
}

float DrumEngine::padPosition (int source, int value, const ControllerSettings& c) noexcept
{
    const auto s = (size_t) source;
    return curve (calibrate (value, c.positionMin[s], c.positionMax[s]), c.positionCurve[s]);
}

float DrumEngine::hatLadderValue (float openness, const ControllerSettings& c) noexcept
{
    // A line through (0, 0), each threshold at the boundary between two
    // standard steps (0.125, 0.375, 0.625, 0.875), and (1, 1).
    constexpr std::array<float, ControllerSettings::numThresholds> boundaries { 0.125f, 0.375f, 0.625f, 0.875f };
    auto x0 = 0.0f, y0 = 0.0f;

    for (size_t i = 0; i <= boundaries.size(); ++i)
    {
        const auto x1 = i < boundaries.size() ? juce::jlimit (x0, 1.0f, c.hatThresholds[i]) : 1.0f;
        const auto y1 = i < boundaries.size() ? boundaries[i] : 1.0f;

        if (openness <= x1)
            return x1 > x0 ? y0 + (y1 - y0) * (openness - x0) / (x1 - x0) : y1;

        x0 = x1;
        y0 = y1;
    }

    return 1.0f;
}

std::pair<std::array<dsp::SvfCoefficients, 2>, float> DrumEngine::positionTone (PieceRole role, float edge, double rate) noexcept
{
    edge = juce::jlimit (0.0f, 1.0f, edge);

    if (edge < 0.01f)
        return { {}, 1.0f };

    // Towards the edge a drum head rings more in its higher modes and less
    // in its fundamental: a low shelf takes body out, a high shelf brings
    // overtones up, and the hit is a little quieter. A ride's bow gets
    // brighter and washier nearer the edge.
    // Hi-hats likewise get brighter and washier from the bell to the edge.
    struct Shape { double lowHz, lowDb, highHz, highDb; };
    const auto shape = role == PieceRole::snare                          ? Shape { 220.0, -7.0, 3000.0, 6.0 }
                     : role == PieceRole::ride || role == PieceRole::hihat ? Shape { 500.0, -3.0, 5000.0, 4.0 }
                                                                          : Shape { 140.0, -6.0, 2200.0, 5.0 };

    return { { dsp::designSvf (dsp::SvfType::lowShelf, shape.lowHz, shape.lowDb * edge, 0.7071, rate),
               dsp::designSvf (dsp::SvfType::highShelf, shape.highHz, shape.highDb * edge, 0.7071, rate) },
             juce::Decibels::decibelsToGain (edgeLevelDb * edge) };
}

void DrumEngine::noteOn (Kit& kit, int note, int velocity, const EngineParams& params, int channel) noexcept
{
    if (! juce::isPositiveAndBelow (note, 128))
        return;

    noteCount = (noteCount + 1) & 0xffff;
    feedback.lastNote.store (std::max (1u, noteCount) << 16 | (std::uint32_t) (velocity & 0x7f) << 8 | (std::uint32_t) note,
                             std::memory_order_relaxed);

    MidiMonitorEvent event;
    event.channel = (std::uint8_t) channel;
    event.number = (std::uint8_t) note;
    event.value = (std::uint8_t) velocity;

    const auto& target = kit.notes.targets[(size_t) note];
    const auto& c = kit.notes.controllers;

    // A note that plays nothing can choke cymbals. One that plays something
    // never does, so a kit's own map that uses 85-95 (Virtuosity's does)
    // doesn't choke the cymbals whenever it plays a snare.
    if (target.articulation < 0)
    {
        for (size_t p = 0; p < kit.pieces.size(); ++p)
        {
            if (params.pieces[p].chokeNote == note)
            {
                chokePiece ((int) p, cymbalChokeSeconds);
                event.detail = MidiMonitorEvent::Detail::choke;
            }
        }

        toMonitor (event);
        return;
    }

    // E-kit notes can follow a controller: a hi-hat hit takes its openness
    // from the pedal, a snare or ride hit its strike position. Until that
    // controller arrives, the note plays its own articulation.
    const auto& art = kit.articulations[(size_t) target.articulation];
    const auto& piece = kit.pieces[(size_t) art.piece];
    Played played { target.articulation };

    if (target.articulation == piece.pedalArticulation)
        pedalNoteSeen = true;   // the module sends its own chicks

    const auto source = (size_t) target.source;
    const auto openness = latched (hatOpenness, channel);
    const auto strike = latched (position[source], channel);

    // A hi-hat hit can follow both: its openness from the pedal, and (with
    // a digital hi-hat) where on the bow it was struck. No kit has hats
    // recorded at several positions, so the position changes the tone.
    std::array<dsp::SvfCoefficients, 2> hatTone {};
    auto hatLevel = 1.0f;
    const auto hatPosition = latched (position[(size_t) PositionSource::hihat], channel);

    if (target.follow == Follow::pedal && hatPosition >= 0.0f && c.positionEnabled[(size_t) PositionSource::hihat])
    {
        std::tie (hatTone, hatLevel) = positionTone (PieceRole::hihat, hatPosition * c.positionEmulation[(size_t) PositionSource::hihat], sampleRate);
        event.position = hatPosition;
    }

    // The map's velocity settings, before anything picks a step or a zone:
    // the pad's edge compensation (for hits that follow the position), the
    // articulation's adjust, the piece's curve and the global velocity. All
    // looked up in the note table. Grooves and clicks on a piece call
    // trigger() directly, so they keep their own velocities. The monitor
    // still shows what came in.
    auto edgeAmount = 0.0f;

    if (target.follow == Follow::position && strike >= 0.0f && c.positionEnabled[source])
        edgeAmount = strike * c.positionVelocity[source];
    else if (target.follow == Follow::pedal && hatPosition >= 0.0f && c.positionEnabled[(size_t) PositionSource::hihat])
        edgeAmount = hatPosition * c.positionVelocity[(size_t) PositionSource::hihat];

    velocity = kit.notes.velocityFor (note, art.piece, velocity, edgeAmount);

    if (target.follow == Follow::pedal && openness >= 0.0f)
    {
        played = playLadder (kit, piece.hatLadder (art.stroke), hatLadderValue (openness, c), velocity, params, hatLevel, hatTone);
        event.detail = MidiMonitorEvent::Detail::pedal;
        event.amount = openness;
    }
    else if (target.follow == Follow::pedal && event.position >= 0.0f)
    {
        trigger (kit, target.articulation, velocity, params, hatLevel, true, hatTone);
    }
    else if (target.follow == Follow::position && strike >= 0.0f && c.positionEnabled[source])
    {
        event.detail = MidiMonitorEvent::Detail::position;
        event.amount = strike;
        played = playPosition (kit, target.articulation, strike, c.positionEmulation[source], velocity, params, event.detail);
    }
    else
    {
        trigger (kit, target.articulation, velocity, params);
    }

    event.articulation = (std::int16_t) played.articulation;
    event.blendedWith = (std::int16_t) played.other;
    event.blend = played.blend;
    toMonitor (event);
}

DrumEngine::Played DrumEngine::playPosition (Kit& kit, int articulation, float strike, float emulation, int velocity,
                                             const EngineParams& params, MidiMonitorEvent::Detail& detail) noexcept
{
    const auto& piece = kit.pieces[(size_t) kit.articulations[(size_t) articulation].piece];

    // Recorded positions: move between them.
    if (piece.positionLadder.numSteps > 1)
        return playLadder (kit, piece.positionLadder, strike, velocity, params);

    // A ride's bow blends towards its edge hit (two real recordings, with
    // an equal-power crossfade); the emulation amount is how much of the
    // edge the far edge of the bow gets.
    if (piece.role == PieceRole::ride && piece.edgeArticulation >= 0)
    {
        const auto w = juce::jlimit (0.0f, 1.0f, strike * emulation);
        detail = MidiMonitorEvent::Detail::rideBlend;

        if (w < 0.02f)
        {
            trigger (kit, articulation, velocity, params);
            return { articulation };
        }

        const auto angle = w * juce::MathConstants<float>::halfPi;
        applyChokes (kit, kit.articulations[(size_t) articulation]);
        applyChokes (kit, kit.articulations[(size_t) piece.edgeArticulation]);
        trigger (kit, piece.edgeArticulation, velocity, params, std::sin (angle), false);
        trigger (kit, articulation, velocity, params, std::cos (angle), false);
        return { articulation, piece.edgeArticulation, w };
    }

    // No recordings to move between: filter this hit (see positionTone()).
    const auto [tone, level] = positionTone (piece.role, strike * emulation, sampleRate);
    detail = MidiMonitorEvent::Detail::emulated;
    trigger (kit, articulation, velocity, params, level, true, tone);
    return { articulation };
}

DrumEngine::Played DrumEngine::playLadder (Kit& kit, const Ladder& ladder, float value, int velocity, const EngineParams& params,
                                           float gainScale, const std::array<dsp::SvfCoefficients, 2>& tone) noexcept
{
    jassert (ladder.numSteps > 0);   // the resolver only sets follow where there are steps

    // The highest step whose range starts at or below the value. Inside its
    // range it plays alone; past it, it crossfades with the next step,
    // until the next step's range begins.
    auto lower = 0;

    while (lower + 1 < ladder.numSteps && value >= ladder.low[(size_t) lower + 1])
        ++lower;

    auto upper = lower;
    auto t = 0.0f;   // 0 = lower alone .. 1 = upper alone

    if (value > ladder.high[(size_t) lower] && lower + 1 < ladder.numSteps)
    {
        upper = lower + 1;
        const auto gap = ladder.low[(size_t) upper] - ladder.high[(size_t) lower];
        t = gap > 1.0e-6f ? juce::jlimit (0.0f, 1.0f, (value - ladder.high[(size_t) lower]) / gap) : 1.0f;
    }

    const auto a = ladder.articulation[(size_t) lower];
    const auto b = ladder.articulation[(size_t) upper];

    if (t <= 0.0f || t >= 1.0f || a == b)
    {
        const auto only = t >= 1.0f ? b : a;
        trigger (kit, only, velocity, params, gainScale, true, tone);
        return { only };
    }

    // Both steps' chokes first, so a closed step (which chokes the open
    // ones) doesn't cut off the more open step it's blended with.
    applyChokes (kit, kit.articulations[(size_t) a]);
    applyChokes (kit, kit.articulations[(size_t) b]);

    // Equal-power, with the louder step last so it's the one the UI shows.
    const auto angle = t * juce::MathConstants<float>::halfPi;
    const auto first = t < 0.5f ? b : a, second = t < 0.5f ? a : b;
    trigger (kit, first, velocity, params, gainScale * (t < 0.5f ? std::sin (angle) : std::cos (angle)), false, tone);
    trigger (kit, second, velocity, params, gainScale * (t < 0.5f ? std::cos (angle) : std::sin (angle)), false, tone);
    return { second, first, t < 0.5f ? t : 1.0f - t };
}

void DrumEngine::controlChange (Kit& kit, int channel, int controller, int value, const EngineParams& params) noexcept
{
    const auto& c = kit.notes.controllers;
    value = juce::jlimit (0, 127, value);

    controllerCount = (controllerCount + 1) & 0xff;
    feedback.lastController.store (std::max (1u, controllerCount) << 24 | (std::uint32_t) (channel & 0x1f) << 16
                                       | (std::uint32_t) (controller & 0x7f) << 8 | (std::uint32_t) value,
                                   std::memory_order_relaxed);

    MidiMonitorEvent event;
    event.kind = MidiMonitorEvent::Kind::controller;
    event.channel = (std::uint8_t) channel;
    event.number = (std::uint8_t) controller;
    event.value = (std::uint8_t) value;

    if (controller == c.hihatPedal)
    {
        event.source = -1;

        if (! channelMatches (c.hihatChannel, channel))
        {
            event.detail = MidiMonitorEvent::Detail::ignored;
            toMonitor (event);
            return;
        }

        // The hysteresis (see pedalDeadBand), except at the very ends, which
        // must be reachable exactly.
        const auto raw = pedalOpenness (value, c);
        const auto slot = (size_t) juce::jlimit (0, 16, channel);
        const auto previous = latched (hatOpenness, channel);
        auto openness = raw;

        if (previous >= 0.0f && raw > 0.002f && raw < 0.998f && std::abs (raw - previous) <= pedalDeadBand)
            openness = previous;

        if (previous >= 0.0f && openness < previous)
            dampHats (kit, hatLadderValue (openness, c));

        hatOpenness[slot] = hatOpenness[0] = openness;
        feedback.hihatOpenness.store (openness, std::memory_order_relaxed);
        feedback.pedalMessages.store (++pedalCount, std::memory_order_relaxed);
        event.detail = MidiMonitorEvent::Detail::pedal;
        event.amount = openness;
        toMonitor (event);

        pedalMoved (kit, previous, openness, channel, params);
        return;
    }

    for (size_t i = 0; i < c.position.size(); ++i)
    {
        if (controller != c.position[i])
            continue;

        event.source = (std::int8_t) i;

        if (! channelMatches (c.positionChannel[i], channel))
        {
            event.detail = MidiMonitorEvent::Detail::ignored;
            continue;
        }

        const auto v = padPosition ((int) i, value, c);
        position[i][(size_t) juce::jlimit (0, 16, channel)] = position[i][0] = v;
        feedback.position[i].store (v, std::memory_order_relaxed);
        event.detail = MidiMonitorEvent::Detail::position;
        event.amount = v;
    }

    toMonitor (event);
}

void DrumEngine::pedalMoved (Kit& kit, float previous, float openness, int channel, const EngineParams& params) noexcept
{
    const auto& c = kit.notes.controllers;
    const auto ms = [this] (std::int64_t samples) { return 1000.0 * (double) samples / sampleRate; };

    pedalHistory[(size_t) pedalHistoryNext] = { now, openness };
    pedalHistoryNext = (pedalHistoryNext + 1) % (int) pedalHistory.size();

    // How fast the hats moved, from the most open (or most closed) point in
    // the last pedalWindowMs to here, in openness per second.
    auto speedSince = [&] (bool closing)
    {
        auto best = openness;
        auto bestTime = now;

        for (const auto& p : pedalHistory)
        {
            if (p.time > now || ms (now - p.time) > pedalWindowMs)
                continue;

            if (closing ? p.openness > best : p.openness < best)
            {
                best = p.openness;
                bestTime = p.time;
            }
        }

        const auto travel = std::abs (best - openness);
        return std::pair { travel, travel / (float) std::max (0.001, ms (now - bestTime) / 1000.0) };
    };

    // The slowest pedal movement that still counts as a stomp, in openness
    // per second: from 10/s (sensitivity 0) down to 1.5/s (sensitivity 1).
    const auto minimumSpeed = juce::jmap (juce::jlimit (0.0f, 1.0f, c.chickSensitivity), 10.0f, 1.5f);

    // Just closed: a chick, if the pedal came down quickly.
    if (previous > pedalClosed && openness <= pedalClosed)
    {
        const auto [travel, speed] = speedSince (true);

        if (travel >= minimumCloseTravel && speed >= minimumSpeed)
        {
            lastClose = now;
            splashArmed = true;

            const auto derive = c.chickFromPedal == ChickMode::always || (c.chickFromPedal == ChickMode::automatic && ! pedalNoteSeen);

            if (derive)
                playPedalHit (kit, false, speed / minimumSpeed, channel, params);
        }
    }

    // Opened again soon after a stomp: a foot splash.
    if (splashArmed && openness >= splashOpenness)
    {
        splashArmed = false;

        if (c.footSplash && ms (now - lastClose) <= splashWindowMs)
            playPedalHit (kit, true, speedSince (false).second / minimumSpeed, channel, params);
    }
}

void DrumEngine::playPedalHit (Kit& kit, bool splash, float speedRatio, int channel, const EngineParams& params) noexcept
{
    // The first hi-hat with that kind of hit.
    for (const auto& piece : kit.pieces)
    {
        const auto articulation = splash ? piece.splashArticulation : piece.pedalArticulation;

        if (articulation < 0)
            continue;

        // Just fast enough is soft; eight times as fast, or more, is full force.
        const auto velocity = juce::jlimit (1, 127, juce::roundToInt (juce::jmap (std::log2 (std::max (1.0f, speedRatio)) / 3.0f,
                                                                                   0.0f, 1.0f, 35.0f, 127.0f)));
        trigger (kit, articulation, velocity, params);

        MidiMonitorEvent event;
        event.kind = splash ? MidiMonitorEvent::Kind::footSplash : MidiMonitorEvent::Kind::pedalChick;
        event.channel = (std::uint8_t) channel;
        event.value = (std::uint8_t) velocity;
        event.articulation = (std::int16_t) articulation;
        toMonitor (event);
        return;
    }
}

void DrumEngine::dampHats (const Kit& kit, float openness) noexcept
{
    // Closing the pedal on ringing hats damps them, like real hats: a voice
    // played well more open than the pedal is now fades out, and once the
    // pedal is shut every open step fades into the closed sound.
    const auto shut = openness < pedalClosed;

    for (auto& v : voices)
    {
        if (! v.isActive() || v.isFadingOut())
            continue;

        const auto stepOpenness = kit.articulations[(size_t) v.getArticulation()].openness;

        if (stepOpenness > (shut ? pedalClosed : openness + pedalDampMargin))
            v.fadeOut (secondsToSamples (shut ? pedalShutSeconds : pedalDampSeconds));
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

void DrumEngine::polyAftertouch (Kit& kit, int note, int pressure, int channel) noexcept
{
    // E-kits (Roland, Alesis) send polyphonic aftertouch when a cymbal edge is grabbed.
    if (! juce::isPositiveAndBelow (note, 128))
        return;

    MidiMonitorEvent event;
    event.kind = MidiMonitorEvent::Kind::aftertouch;
    event.channel = (std::uint8_t) channel;
    event.number = (std::uint8_t) note;
    event.value = (std::uint8_t) pressure;

    if (const auto target = kit.notes.targets[(size_t) note].articulation; target >= 0 && pressure >= 64)
    {
        chokePiece (kit.articulations[(size_t) target].piece, cymbalChokeSeconds);
        event.detail = MidiMonitorEvent::Detail::choke;
        event.articulation = target;
    }

    toMonitor (event);
}

void DrumEngine::trigger (Kit& kit, int articulationIndex, int velocity, const EngineParams& params,
                          float gainScale, bool chokeOthers, const std::array<dsp::SvfCoefficients, 2>& positionTone) noexcept
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
            const auto lowerHit = startVoice (kit, art, articulationIndex, lowerIndex, velocityGain (lower) * std::cos (angle), pitchRatio, piece, positionTone);
            const auto upperHit = startVoice (kit, art, articulationIndex, lowerIndex + 1, velocityGain (upper) * std::sin (angle), pitchRatio, piece, positionTone);

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

    report (zoneIndex, startVoice (kit, art, articulationIndex, zoneIndex, velocityGain (zone), pitchRatio, piece, positionTone));
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
                            float gain, double pitchRatio, const PieceParams& shaping,
                            const std::array<dsp::SvfCoefficients, 2>& positionTone) noexcept
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
    t.positionTone = positionTone;

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
