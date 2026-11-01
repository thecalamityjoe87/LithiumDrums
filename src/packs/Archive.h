#pragma once

#include <juce_core/juce_core.h>

#include <functional>

// Unpacking downloaded archives: .zip (including Zip64, for archives and
// files over 4 GB), .tar, and .7z (including .tar.7z).
//
// JUCE has a zip reader, but it doesn't read Zip64, which the larger kits
// need (CrocellKit's zip is 5.6 GB), so zips are read here, with JUCE's zlib
// doing the inflating. 7z uses the LZMA SDK (public domain, from 7-Zip). An
// archive that is one LZMA or LZMA2 stream, like the Salamander Drumkit's
// .tar.7z, is decoded as it's read, straight into the tar reader and onto
// disk, so a 500 MB tar never sits in memory.
//
// Every entry is checked before it's written: absolute paths, "..", and
// links are refused, so an archive can only write inside its destination.

namespace lithium::packs
{

/** Called now and then with how far it has got, 0..1. Returning false
    cancels, and extract() fails with "Cancelled". */
using Progress = std::function<bool (double)>;

/** Unpacks an archive into a folder (created if needed), going by its
    extension: .zip, .tar, .7z (a .tar inside a .7z is unpacked too). */
juce::Result extract (const juce::File& archive, const juce::File& destination, const Progress& = {});

/** Whether extract() can unpack a file of this name. */
bool isArchive (const juce::File&);

} // namespace lithium::packs
