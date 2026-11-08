#!/usr/bin/env python3
"""Writes THIRD-PARTY-NOTICES.txt: the copyright notice and licence text of
every third-party component compiled into Lithium Drums or shipped with it.

MIT, BSD, zlib, Apache, ISC, libpng, IJG and OFL licences all ask for their
notice and licence text to go along with binary copies, so the packaging
scripts put this file next to the plugins. It is generated from the licence
files of the dependencies CMake fetched, so it follows their versions.

Usage: scripts/third-party-notices.py <deps folder> <output file>
       (the deps folder is build/<preset>/_deps or build/_deps)
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (name, what it does in Lithium Drums, licence, licence file). Paths are
# relative to the deps folder unless they start with "assets/".
JUCE = "juce-src/modules/"
COMPONENTS = [
    ("JUCE", "application and plugin framework", "GNU AGPLv3 (see LICENSE)",
     None),
    ("VST3 SDK (Steinberg)", "the VST3 plugin format", "MIT",
     JUCE + "juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt"),
    ("AudioUnitSDK (Apple)", "the AU plugin format, macOS only", "Apache-2.0",
     JUCE + "juce_audio_plugin_client/AU/AudioUnitSDK/LICENSE.txt"),
    ("CLAP", "the CLAP plugin format", "MIT",
     "clap_juce_extensions-src/clap-libs/clap/LICENSE"),
    ("clap-helpers", "the CLAP plugin format", "MIT",
     "clap_juce_extensions-src/clap-libs/clap-helpers/LICENSE"),
    ("clap-juce-extensions", "the CLAP plugin format", "MIT",
     "clap_juce_extensions-src/LICENSE.md"),
    ("LZMA SDK (Igor Pavlov)", "unpacking .7z kit downloads", "public domain",
     None),
    ("FLAC", "reading kit audio", "BSD-3-Clause",
     JUCE + "juce_audio_formats/codecs/flac/Flac Licence.txt"),
    ("libogg", "Ogg container support in JUCE", "BSD-3-Clause",
     JUCE + "juce_audio_formats/codecs/ogg/COPYING"),
    ("Opus", "Opus audio support in JUCE", "BSD-3-Clause",
     JUCE + "juce_audio_formats/codecs/opus/opus/COPYING"),
    ("opusfile", "Opus audio support in JUCE", "BSD-3-Clause",
     JUCE + "juce_audio_formats/codecs/opus/opusfile/COPYING"),
    ("libopusenc", "Opus audio support in JUCE", "BSD-3-Clause",
     JUCE + "juce_audio_formats/codecs/opus/libopusenc/COPYING"),
    ("zlib", "compression", "Zlib",
     JUCE + "juce_core/zip/zlib/LICENSE"),
    ("HarfBuzz", "text shaping", "MIT (modern variant)",
     JUCE + "juce_graphics/fonts/harfbuzz/COPYING"),
    ("SheenBidi", "bidirectional text", "Apache-2.0",
     JUCE + "juce_graphics/unicode/sheenbidi/LICENSE"),
    ("libpng", "PNG images", "libpng-2.0",
     JUCE + "juce_graphics/image_formats/pnglib/LICENSE"),
    ("libjpeg (Independent JPEG Group)", "JPEG images", "IJG",
     JUCE + "juce_graphics/image_formats/jpglib/README"),
    ("libwebp", "WebP images", "BSD-3-Clause",
     JUCE + "juce_graphics/image_formats/libwebp/COPYING"),
    ("LunaSVG", "SVG drawing", "MIT",
     JUCE + "juce_graphics/drawables/lunasvg/LICENSE"),
    ("PlutoVG", "SVG drawing", "MIT",
     JUCE + "juce_graphics/drawables/lunasvg/plutovg/LICENSE"),
    ("Barlow and Barlow Condensed fonts", "interface text", "SIL OFL 1.1",
     "assets/fonts/OFL-Barlow.txt"),
    ("IBM Plex Mono font", "interface numbers", "SIL OFL 1.1",
     "assets/fonts/OFL-IBMPlexMono.txt"),
]

LZMA_NOTE = ("The LZMA SDK's 7z decoder files used here (C/7z*.c, LzmaDec.c, Lzma2Dec.c, Ppmd7*.c, Bcj2.c, Bra*.c, "
             "Delta.c, Alloc.c, CpuArch.c) each state that they are in the public domain.\n")

IR_NOTE = """The Room reverb's "Seminar Room" impulse response is from HOMULA-RIR
(doi:10.5281/zenodo.10479726) by Federico Miotello, Paolo Ostan, Mirco Pezzoli,
Luca Comanducci, Alberto Bernardini, Fabio Antonacci and Augusto Sarti
(Politecnico di Milano), under CC BY 4.0
(https://creativecommons.org/licenses/by/4.0/). Lithium Drums combines two of
its microphones into a stereo pair, trims and fades it, lowers the direct sound
and high-passes it. The other impulse responses are synthesized and CC0.

The drum kits and grooves are not part of this program. Lithium Drums
downloads them from their authors on request; the About panel shows each
one's credits and licence.
"""


def extract_ijg(text: str) -> str:
    """The IJG README is long; its LEGAL ISSUES section is the licence."""
    start = text.find("LEGAL ISSUES\n============")
    end = text.find("\nREFERENCES\n==========", start)
    return text[start:end].strip() + "\n" if start >= 0 and end > start else text


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    deps = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2])
    rule = "=" * 78 + "\n"
    parts = ["Lithium Drums: third-party notices\n", rule,
             "Lithium Drums is Copyright (C) 2026 Isaac Joseph and is licensed under the\n"
             "GNU Affero General Public License, version 3 (see LICENSE). It includes\n"
             "the following third-party components.\n\n"]

    for name, use, licence, _ in COMPONENTS:
        parts.append(f"  {name}: {use} ({licence})\n")

    parts += ["\n", rule, IR_NOTE]
    missing = []

    for name, _, licence, path in COMPONENTS:
        if path is None:
            if name.startswith("LZMA"):
                parts += ["\n", rule, f"{name}\n\n", LZMA_NOTE]
            continue

        file = ROOT / path if path.startswith("assets/") else deps / path
        if not file.is_file():
            missing.append(str(file))
            continue

        text = file.read_text(encoding="utf-8", errors="replace")
        if name.startswith("libjpeg"):
            text = extract_ijg(text)
        parts += ["\n", rule, f"{name} ({licence})\n\n", text.rstrip() + "\n"]

    if missing:
        print("Licence files not found (configure the build first):\n  " + "\n  ".join(missing), file=sys.stderr)
        return 1

    out.write_text("".join(parts), encoding="utf-8")
    print(f"Wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
