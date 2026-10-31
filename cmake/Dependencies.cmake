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

FetchContent_MakeAvailable(JUCE clap_juce_extensions)
