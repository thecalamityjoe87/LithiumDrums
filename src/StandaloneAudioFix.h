#pragma once

namespace lithium
{

// In the standalone app on Linux, makes opening a direct ALSA device wait out
// PipeWire/WirePlumber still holding it, instead of failing with "no channels".
// Does nothing in a plugin host, or on other platforms.
void installStandaloneAudioFix();

}
