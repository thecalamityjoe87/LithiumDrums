#pragma once

#include "Kit.h"
#include "dsp/ChannelChain.h"
#include "dsp/RoomReverb.h"

#include <array>

// A plain snapshot of every parameter the engine and mixer need, read once
// per audio block from the APVTS atomics. Passing a struct around keeps the
// DSP code independent of JUCE's parameter classes, and makes the engine easy
// to drive from tests.
//
// Pieces and mics are slots: slot n is the loaded kit's nth piece or mic.

namespace lithium
{

// Where a mixer channel goes: the main stereo out (0), or one of the
// stereo aux outs (1 to numAuxOuts) for mixing in the host.
inline constexpr int numAuxOuts = 16;
inline constexpr int mainOutput = 0;

/** A mixer channel: its processing, and where it goes after its fader. */
struct ChannelParams
{
    ChannelSettings chain;
    float reverbSend = 0.0f;                   // linear, post-fader
    int output = mainOutput;
};

struct PieceParams
{
    // The drum as a whole: its hits through every mic.
    float volume = 1.0f;                       // linear gain
    bool mute = false;
    bool solo = false;
    std::array<float, numMicRoles> sends { 1.0f, 1.0f, 1.0f };   // close, overheads, room
    int chokeNote = -1;                        // a note that chokes this piece, -1 = none

    // Shaping each hit.
    float dynamics = 0.0f;                     // -1..1: how much more (or less) velocity changes the level
    float tuneSemitones = 0.0f;
    float pitchEnvSemitones = 0.0f;            // the pitch starts this far off and glides back
    float pitchEnvMs = 60.0f;                  // ... over about this long
    float toneHighPassHz = 20.0f;              // at 20 Hz or below: off
    float toneLowPassHz = 20000.0f;            // at 20 kHz or above: off
    float attackMs = 0.0f;                     // fade-in at the start of each hit
    float holdMs = 12.0f;                      // before the decay starts
    float decay = 1.0f;                        // 0..1, 1 = the sample's natural length

    // The drum's own mixer channel, which its close mics feed.
    ChannelParams channel;
    float faderGain = 1.0f;                    // linear
    float pan = 0.0f;                          // -1..1, a balance on the channel's stereo image

    static constexpr float toneOffHz = 20.0f, toneOpenHz = 20000.0f;
};

struct MicParams
{
    // Both relative to the kit's own default fader and pan, so 0 dB / centre
    // means "as the kit was mixed". For a close mic, these trim it inside
    // the drum channels it feeds; for an overhead or room mic, they are its
    // own channel's fader and pan.
    float volume = 1.0f;                       // linear gain on top of the kit's default
    float pan = 0.0f;                          // added to the kit's default pan
    bool mute = false;
    bool solo = false;

    ChannelParams channel;                     // overhead and room mics only
};

struct ReverbParams
{
    ReverbSettings room;
    float returnGain = 1.0f;                   // linear
    bool mute = false;
    int output = mainOutput;
};

struct EngineParams
{
    std::array<PieceParams, maxPieces> pieces;
    std::array<MicParams, maxMics> mics;
    ReverbParams reverb;
    float masterGain = 1.0f;
    bool velocityCrossfade = true;

    bool anyPieceSoloed (int numPieces) const noexcept
    {
        for (int i = 0; i < numPieces; ++i)
            if (pieces[(size_t) i].solo)
                return true;

        return false;
    }

    bool anyMicSoloed (int numMics) const noexcept
    {
        for (int i = 0; i < numMics; ++i)
            if (mics[(size_t) i].solo)
                return true;

        return false;
    }

    /** Neutral settings for a kit: what its pieces and mics play like before
        anyone touches a control. The reverb bus isn't part of the kit, so it
        keeps its settings. */
    void resetForKit (const Kit& kit) noexcept
    {
        for (size_t i = 0; i < pieces.size(); ++i)
        {
            pieces[i] = {};
            pieces[i].chokeNote = i < kit.pieces.size() ? kit.pieces[i].defaultChokeNote : -1;
        }

        for (auto& m : mics)
            m = {};
    }
};

} // namespace lithium
