#pragma once

#include "Kit.h"

#include <array>

// A plain snapshot of every parameter the engine and mixer need, read once
// per audio block from the APVTS atomics. Passing a struct around keeps the
// DSP code independent of JUCE's parameter classes, and makes the engine easy
// to drive from tests.
//
// Pieces and mics are slots: slot n is the loaded kit's nth piece or mic.

namespace lithium
{

struct PieceParams
{
    float volume = 1.0f;                       // linear gain
    float tuneSemitones = 0.0f;
    float decay = 1.0f;                        // 0..1, 1 = the sample's natural length
    bool mute = false;
    bool solo = false;
    std::array<float, numMicRoles> sends { 1.0f, 1.0f, 1.0f };   // close, overheads, room
    int chokeNote = -1;                        // a note that chokes this piece, -1 = none
};

enum class MicOutput : int { main = 0, direct = 1 };

struct MicParams
{
    // Both relative to the kit's own default fader and pan, so 0 dB / centre
    // means "as the kit was mixed".
    float volume = 1.0f;                       // linear gain on top of the kit's default
    float pan = 0.0f;                          // added to the kit's default pan
    bool mute = false;
    bool solo = false;
    MicOutput output = MicOutput::main;
};

struct EngineParams
{
    std::array<PieceParams, maxPieces> pieces;
    std::array<MicParams, maxMics> mics;
    float masterGain = 1.0f;
    bool velocityCrossfade = true;

    bool anyPieceSoloed (int numPieces) const noexcept
    {
        for (int i = 0; i < numPieces; ++i)
            if (pieces[(size_t) i].solo)
                return true;

        return false;
    }

    /** Neutral settings for a kit: what its pieces and mics play like before
        anyone touches a control. */
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
