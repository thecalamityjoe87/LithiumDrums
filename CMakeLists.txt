cmake_minimum_required(VERSION 3.25)

project(LithiumDrums VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    # JUCE's macOS sources are Objective-C++.
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LITHIUM_BUILD_TESTS "Build the headless engine tests" ON)
option(LITHIUM_COPY_PLUGINS "Copy built plugins to the user plugin folders (~/.vst3, ~/.clap)" OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

include(cmake/PluginIdentity.cmake)
include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# Plugin

set(lithium_formats VST3 Standalone)
if(APPLE)
    list(APPEND lithium_formats AU)
endif()

juce_add_plugin(LithiumDrums
    PRODUCT_NAME                "${LITHIUM_PRODUCT_NAME}"
    COMPANY_NAME                "${LITHIUM_COMPANY_NAME}"
    BUNDLE_ID                   "${LITHIUM_BUNDLE_ID}"
    PLUGIN_MANUFACTURER_CODE    "${LITHIUM_MANUFACTURER_CODE}"
    PLUGIN_CODE                 "${LITHIUM_PLUGIN_CODE}"
    FORMATS                     ${lithium_formats}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Instrument Drum Sampler
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD     ${LITHIUM_COPY_PLUGINS})

clap_juce_extensions_plugin(TARGET LithiumDrums
    CLAP_ID       "${LITHIUM_CLAP_ID}"
    CLAP_FEATURES instrument drum sampler stereo)

target_sources(LithiumDrums PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/Parameters.cpp
    src/Settings.cpp
    src/KitLibrary.cpp
    src/importer/DrumGizmoImporter.cpp
    src/engine/KitManifest.cpp
    src/engine/MidiMap.cpp
    src/engine/InMemorySampleSource.cpp
    src/engine/KitLoader.cpp
    src/engine/Voice.cpp
    src/engine/DrumEngine.cpp
    src/engine/Mixer.cpp
    src/ui/LithiumLookAndFeel.cpp
    src/ui/MainView.cpp
    src/ui/HeaderBar.cpp
    src/ui/KitPage.cpp
    src/ui/KitStage.cpp
    src/ui/PieceStrip.cpp
    src/ui/InfoPanels.cpp)

target_include_directories(LithiumDrums PRIVATE src)

juce_add_binary_data(LithiumAssets
    NAMESPACE   LithiumAssets
    HEADER_NAME LithiumAssets.h
    SOURCES
        assets/fonts/Barlow-Regular.ttf
        assets/fonts/Barlow-Medium.ttf
        assets/fonts/Barlow-SemiBold.ttf
        assets/fonts/BarlowCondensed-Medium.ttf
        assets/fonts/BarlowCondensed-SemiBold.ttf
        assets/fonts/BarlowCondensed-Bold.ttf
        assets/fonts/IBMPlexMono-Regular.ttf
        assets/fonts/IBMPlexMono-Medium.ttf
        assets/kit-renders/layout.json
        assets/kit-renders/room.png
        assets/kit-renders/base.png
        assets/kit-renders/kick.png
        assets/kit-renders/snare.png
        assets/kit-renders/hihat.png
        assets/kit-renders/tom1.png
        assets/kit-renders/tom2.png
        assets/kit-renders/floor.png
        assets/kit-renders/crash.png
        assets/kit-renders/ride.png
        assets/kit-renders/china.png
        assets/midi-maps/general-midi.json)

# The importer's built-in mappings for known DrumGizmo kits.
juce_add_binary_data(LithiumImportMaps
    NAMESPACE   LithiumImportMaps
    HEADER_NAME LithiumImportMaps.h
    SOURCES
        assets/import-maps/DRSKit.json
        assets/import-maps/MuldjordKit.json)

target_compile_definitions(LithiumDrums
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=0
        JUCE_USE_MP3AUDIOFORMAT=0
        JUCE_USE_LAME_AUDIO_FORMAT=0
        JUCE_USE_WINDOWS_MEDIA_FORMAT=0
        JUCE_JACK=1
        LITHIUM_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(LithiumDrums
    PRIVATE
        LithiumAssets
        LithiumImportMaps
    PUBLIC
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# DrumGizmo importer

# A small command-line tool. It compiles the importer and settings sources
# itself (rather than linking the plugin), so it only needs the core audio
# modules.
juce_add_console_app(LithiumImport PRODUCT_NAME "LithiumImport")
target_sources(LithiumImport PRIVATE
    tools/importer/ImportMain.cpp
    src/importer/DrumGizmoImporter.cpp
    src/Settings.cpp)
target_include_directories(LithiumImport PRIVATE src)
target_compile_definitions(LithiumImport PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=0)
target_link_libraries(LithiumImport PRIVATE
    LithiumImportMaps
    juce::juce_audio_formats
    juce::juce_data_structures
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Synthetic test kit

# A small, deterministic kit for the automated tests, so they don't need the
# real (multi-gigabyte) kits. The plugin never loads it by itself. The
# generator only uses the low-level audio modules, so it stays small.
juce_add_console_app(LithiumKitGen PRODUCT_NAME "LithiumKitGen")
target_sources(LithiumKitGen PRIVATE tools/kitgen/KitGenerator.cpp)
target_compile_definitions(LithiumKitGen PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=0)
target_link_libraries(LithiumKitGen PRIVATE
    juce::juce_audio_formats
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# In a folder of its own, so the tests can use that folder as their kits folder.
set(LITHIUM_TEST_KITS_FOLDER "${CMAKE_BINARY_DIR}/test-kits")
set(LITHIUM_TEST_KIT_DIR "${LITHIUM_TEST_KITS_FOLDER}/LithiumTestKit")

# kit.json is written last, so its presence means a complete kit. The command
# re-runs only when the generator's source changes, not on every relink.
add_custom_command(
    OUTPUT  "${LITHIUM_TEST_KIT_DIR}/kit.json"
    COMMAND LithiumKitGen "${LITHIUM_TEST_KIT_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/kitgen/KitGenerator.cpp"
    COMMENT "Generating the synthetic test kit in ${LITHIUM_TEST_KIT_DIR}"
    VERBATIM)
add_custom_target(LithiumTestKit DEPENDS "${LITHIUM_TEST_KIT_DIR}/kit.json")

# ---------------------------------------------------------------------------
# Room backdrop

# Paints assets/kit-renders/room.png. Like the Blender renders, its output is
# committed, so the build only compiles the tool; run it by hand after editing it.
juce_add_console_app(LithiumRoomGen PRODUCT_NAME "LithiumRoomGen")
target_sources(LithiumRoomGen PRIVATE tools/roomgen/RoomGenerator.cpp)
target_compile_definitions(LithiumRoomGen PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(LithiumRoomGen PRIVATE
    juce::juce_graphics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Tests

if(LITHIUM_BUILD_TESTS)
    # The tests link the plugin's shared-code library (the processor, engine
    # and UI, compiled once) rather than recompiling the sources.
    add_executable(LithiumDrumsTests tests/TestMain.cpp tests/EngineTests.cpp)
    target_include_directories(LithiumDrumsTests PRIVATE src)
    target_compile_definitions(LithiumDrumsTests PRIVATE
        LITHIUM_TEST_KIT_DIR="${LITHIUM_TEST_KIT_DIR}"
        LITHIUM_TEST_KITS_FOLDER="${LITHIUM_TEST_KITS_FOLDER}")
    target_link_libraries(LithiumDrumsTests PRIVATE LithiumDrums)
    add_dependencies(LithiumDrumsTests LithiumTestKit)

    # Hosts the built VST3 bundle through JUCE's own VST3 hosting code.
    juce_add_console_app(LithiumHostSmokeTest PRODUCT_NAME "LithiumHostSmokeTest")
    target_sources(LithiumHostSmokeTest PRIVATE tests/HostSmokeTest.cpp)
    target_compile_definitions(LithiumHostSmokeTest PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        LITHIUM_TEST_KITS_FOLDER="${LITHIUM_TEST_KITS_FOLDER}"
        LITHIUM_VST3_PATH="$<TARGET_GENEX_EVAL:LithiumDrums_VST3,$<TARGET_PROPERTY:LithiumDrums_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
    target_link_libraries(LithiumHostSmokeTest PRIVATE
        juce::juce_audio_processors
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    add_dependencies(LithiumHostSmokeTest LithiumDrums_VST3 LithiumTestKit)
endif()
