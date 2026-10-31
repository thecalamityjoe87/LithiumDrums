#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <utility>

namespace lithium::dsp
{

/** Left and right gains for a pan position (-1..1).

    Mono sources: constant-power pan, scaled so the centre is at unity gain.
    Stereo sources: a balance control, which keeps the stereo image intact. */
inline std::pair<float, float> panGains (float pan, int numChannels) noexcept
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);

    if (numChannels == 1)
    {
        const auto angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        return { std::cos (angle) * juce::MathConstants<float>::sqrt2,
                 std::sin (angle) * juce::MathConstants<float>::sqrt2 };
    }

    return { pan > 0.0f ? 1.0f - pan : 1.0f,
             pan < 0.0f ? 1.0f + pan : 1.0f };
}

} // namespace lithium::dsp
