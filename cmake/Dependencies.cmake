# Third-party code, pulled in with FetchContent.
#
# Why FetchContent rather than git submodules: the versions are pinned right
# here in CMake, a fresh clone builds with a single configure step (no
# "git submodule update --init --recursive" to forget), and it works the same
# whether or not the source tree is a git checkout. The CMake presets point
# FETCHCONTENT_BASE_DIR at build/_deps, so Debug and Release share one download.
#
# To build offline or against a local JUCE checkout, configure with
#   -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE

include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.4
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    # SYSTEM marks JUCE's headers as system headers, so our strict warning
    # flags apply to our code and not to JUCE's.
    SYSTEM)

# clap-juce-extensions has no release tags, so it is pinned to a commit.
# Its own clap and clap-helpers submodules are fetched recursively.
FetchContent_Declare(clap_juce_extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        7adee3a1bd4684d4caa5601100e364abccff4b4d
    GIT_PROGRESS   TRUE
    SYSTEM)

# The LZMA SDK, for unpacking .7z downloads (the Salamander Drumkit is only
# published as one). It is the C part of 7-Zip, by Igor Pavlov, and in the
# public domain. Only its sources are fetched (the repository has no CMake
# build of its own; SOURCE_SUBDIR points at nothing so none is looked for),
# and the few decoder files needed are built into a small library below.
FetchContent_Declare(lzma_sdk
    GIT_REPOSITORY https://github.com/ip7z/7zip.git
    GIT_TAG        26.04
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  no-cmake-build)

FetchContent_MakeAvailable(JUCE clap_juce_extensions lzma_sdk)

set(lzma_c "${lzma_sdk_SOURCE_DIR}/C")
add_library(LithiumLzma STATIC
    ${lzma_c}/7zAlloc.c ${lzma_c}/7zArcIn.c ${lzma_c}/7zBuf.c ${lzma_c}/7zCrc.c ${lzma_c}/7zCrcOpt.c
    ${lzma_c}/7zDec.c ${lzma_c}/7zStream.c ${lzma_c}/Alloc.c ${lzma_c}/Bcj2.c ${lzma_c}/Bra.c ${lzma_c}/Bra86.c
    ${lzma_c}/BraIA64.c ${lzma_c}/CpuArch.c ${lzma_c}/Delta.c ${lzma_c}/Lzma2Dec.c ${lzma_c}/LzmaDec.c
    ${lzma_c}/Ppmd7.c ${lzma_c}/Ppmd7Dec.c)
target_include_directories(LithiumLzma SYSTEM PUBLIC "${lzma_c}")
set_target_properties(LithiumLzma PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
