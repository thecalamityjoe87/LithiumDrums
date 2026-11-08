cmake_minimum_required(VERSION 3.25)

project(LithiumDrums VERSION 0.9.3 LANGUAGES C CXX)

if(APPLE)
    # JUCE's macOS sources are Objective-C++.
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LITHIUM_BUILD_TESTS "Build the headless engine tests" ON)
option(LITHIUM_COPY_PLUGINS "Copy built plugins to the user plugin folders (~/.vst3, ~/.clap)" OFF)

# Keep builds from running the machine out of memory. Ninja otherwise runs a
# compile per core, and JUCE's module files take 1-2 GB each to compile, so a
# full rebuild on a machine with many cores and modest RAM can freeze it.
# These pools allow one compile per 2.5 GB of RAM (and two links at a time).
# Other generators ignore them.
cmake_host_system_information(RESULT lithium_ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
math(EXPR lithium_compile_jobs "${lithium_ram_mb} / 2560")
if(lithium_compile_jobs LESS 1)
    set(lithium_compile_jobs 1)
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS lithium_compile=${lithium_compile_jobs} lithium_link=2)
set(CMAKE_JOB_POOL_COMPILE lithium_compile)
set(CMAKE_JOB_POOL_LINK lithium_link)

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
    COMPANY_COPYRIGHT           "Copyright (C) 2026 ${LITHIUM_AUTHOR}. GNU AGPLv3."
    COMPANY_WEBSITE             "${LITHIUM_WEBSITE}"
    BUNDLE_ID                   "${LITHIUM_BUNDLE_ID}"
    PLUGIN_MANUFACTURER_CODE    "${LITHIUM_MANUFACTURER_CODE}"
    PLUGIN_CODE                 "${LITHIUM_PLUGIN_CODE}"
    FORMATS                     ${lithium_formats}
    ICON_BIG                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/icon-1024.png"
    ICON_SMALL                  "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/icon-256.png"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    NEEDS_CURL                  TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Instrument Drum Sampler
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD     ${LITHIUM_COPY_PLUGINS})

clap_juce_extensions_plugin(TARGET LithiumDrums
    CLAP_ID       "${LITHIUM_CLAP_ID}"
    CLAP_FEATURES instrument drum sampler stereo)

target_sources(LithiumDrums PRIVATE
    src/PluginProcessor.cpp
    src/StandaloneAudioFix.cpp
    src/PluginEditor.cpp
    src/Parameters.cpp
    src/Settings.cpp
    src/KitLibrary.cpp
    src/MidiMapLibrary.cpp
    src/Presets.cpp
    src/FxPresets.cpp
    src/importer/Importer.cpp
    src/importer/KitWriter.cpp
    src/importer/DrumGizmoImporter.cpp
    src/importer/SfzParser.cpp
    src/importer/SfzImporter.cpp
    src/importer/SampleFolderImporter.cpp
    src/grooves/GrooveLibrary.cpp
    src/grooves/GrooveMidi.cpp
    src/grooves/GrooveImporter.cpp
    src/packs/Archive.cpp
    src/packs/PackInstaller.cpp
    src/engine/KitManifest.cpp
    src/engine/MidiMap.cpp
    src/engine/MidiMapping.cpp
    src/engine/InMemorySampleSource.cpp
    src/engine/StreamCache.cpp
    src/engine/StreamingSampleSource.cpp
    src/engine/KitLoader.cpp
    src/engine/PieceSwap.cpp
    src/engine/KitSlots.cpp
    src/engine/Voice.cpp
    src/engine/DrumEngine.cpp
    src/engine/GroovePlayer.cpp
    src/engine/IrLoader.cpp
    src/engine/MicEmulation.cpp
    src/engine/Mixer.cpp
    src/engine/dsp/ChannelEq.cpp
    src/engine/dsp/Compressor.cpp
    src/engine/dsp/ChannelChain.cpp
    src/engine/dsp/FdnReverb.cpp
    src/engine/dsp/ConvolutionReverb.cpp
    src/engine/dsp/TempoDelay.cpp
    src/engine/dsp/MasterChain.cpp
    src/ui/LithiumLookAndFeel.cpp
    src/ui/MainView.cpp
    src/ui/HeaderBar.cpp
    src/ui/Logo.cpp
    src/ui/KitPage.cpp
    src/ui/KitStage.cpp
    src/ui/KitArt.cpp
    src/ui/ModelPicker.cpp
    src/ui/PresetPanel.cpp
    src/ui/SlotGrid.cpp
    src/ui/Controls.cpp
    src/ui/EqGraph.cpp
    src/ui/ChannelModules.cpp
    src/ui/MicsModule.cpp
    src/ui/ZoneMap.cpp
    src/ui/EditPage.cpp
    src/ui/MixerPage.cpp
    src/ui/FxPage.cpp
    src/ui/KitBrowser.cpp
    src/ui/InfoPanels.cpp
    src/ui/MidiPanel.cpp
    src/ui/EdrumsView.cpp
    src/ui/VelocityCurveEditor.cpp
    src/ui/MidiKeyboard.cpp
    src/ui/GroovesPage.cpp)

target_include_directories(LithiumDrums PRIVATE src)

# Each kit's own finish of the drums (assets/kit-finishes.json): stage layers
# and slot pictures, finish-<kit id>-*.png. Picked up as they're added.
file(GLOB lithium_finish_images CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/kit-renders/finish-*.png")

# The fonts on their own, so the Linux installer can use them without
# carrying the kit renders.
juce_add_binary_data(LithiumFonts
    NAMESPACE   LithiumFonts
    HEADER_NAME LithiumFonts.h
    SOURCES
        assets/fonts/Barlow-Regular.ttf
        assets/fonts/Barlow-Medium.ttf
        assets/fonts/Barlow-SemiBold.ttf
        assets/fonts/BarlowCondensed-Medium.ttf
        assets/fonts/BarlowCondensed-SemiBold.ttf
        assets/fonts/BarlowCondensed-Bold.ttf
        assets/fonts/IBMPlexMono-Regular.ttf
        assets/fonts/IBMPlexMono-Medium.ttf
)

juce_add_binary_data(LithiumAssets
    NAMESPACE   LithiumAssets
    HEADER_NAME LithiumAssets.h
    SOURCES
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
        assets/kit-renders/crash2.png
        assets/kit-renders/ride.png
        assets/kit-renders/china.png
        assets/kit-renders/splash.png
        assets/kit-renders/slot-kick.png
        assets/kit-renders/slot-snare.png
        assets/kit-renders/slot-hihat.png
        assets/kit-renders/slot-tom1.png
        assets/kit-renders/slot-tom2.png
        assets/kit-renders/slot-floor.png
        assets/kit-renders/slot-crash.png
        assets/kit-renders/slot-crash2.png
        assets/kit-renders/slot-ride.png
        assets/kit-renders/slot-china.png
        assets/kit-renders/slot-cowbell.png
        assets/kit-renders/slot-chimes.png
        assets/kit-renders/slot-gong.png
        assets/kit-renders/slot-splash.png
        assets/kit-renders/slot-tambourine.png
        assets/kit-renders/slot-maraca.png
        assets/kit-renders/slot-shaker.png
        assets/kit-renders/slot-claves.png
        assets/kit-renders/slot-belltree.png
        assets/kit-renders/slot-bucket.png
        assets/kit-renders/slot-stack.png
        assets/kit-renders/slot-snap.png
        assets/kit-renders/slot-clap.png
        assets/kit-renders/slot-stomp.png
        assets/kit-renders/slot-stickclick.png
        assets/kit-renders/slot-cajon.png
        assets/kit-renders/slot-conga-small.png
        assets/kit-renders/slot-conga-large.png
        assets/mic-renders/mics.json
        assets/mic-renders/mics-close.png
        assets/mic-renders/mics-kick.png
        assets/mic-renders/mics-cymbal.png
        assets/mic-renders/mics-overheads.png
        assets/mic-renders/mics-room.png
        assets/kit-finishes.json
        ${lithium_finish_images}
        assets/midi-maps/lithium-standard.json
        assets/midi-maps/general-midi.json
        assets/midi-maps/roland-vdrums.json
        assets/midi-maps/alesis.json
        assets/midi-maps/drumit-five.json
        assets/midi-maps/efnote.json
        assets/midi-maps/lemon-t950.json
        assets/midi-maps/millenium-mps1000.json
        assets/midi-maps/yamaha-dtx402.json
        assets/packs/catalog.json
        assets/irs/irs.json
        assets/irs/studio.flac
        assets/irs/seminar-room.flac
        assets/irs/tiled-room.flac
        assets/irs/hall.flac
        assets/irs/plate.flac)

# The importer's built-in mappings for known kits.
juce_add_binary_data(LithiumImportMaps
    NAMESPACE   LithiumImportMaps
    HEADER_NAME LithiumImportMaps.h
    SOURCES
        assets/import-maps/DRSKit.json
        assets/import-maps/MuldjordKit.json
        assets/import-maps/AVLDrumkits.json
        assets/import-maps/SalamanderDrumkit.json
        assets/import-maps/VirtuosityDrums.json
        assets/import-maps/CrocellKit.json
        assets/import-maps/BigRustyDrums.json
        assets/import-maps/SMDrums.json
        assets/import-maps/NakedDrums.json
        assets/import-maps/OpenSourceDrumKit.json
        assets/import-maps/Tchimera.json)

target_compile_definitions(LithiumDrums
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=0
        JUCE_USE_MP3AUDIOFORMAT=0
        JUCE_USE_LAME_AUDIO_FORMAT=0
        JUCE_USE_WINDOWS_MEDIA_FORMAT=0
        # JACK only on Linux: macOS and Windows have no JACK headers on a
        # normal system (and use Core Audio / WASAPI and ASIO instead).
        $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>
        LITHIUM_VERSION_STRING="${PROJECT_VERSION}"
        LITHIUM_AUTHOR_STRING="${LITHIUM_AUTHOR}"
        LITHIUM_COMPANY_STRING="${LITHIUM_COMPANY_NAME}"
        LITHIUM_WEBSITE_STRING="${LITHIUM_WEBSITE}")

target_link_libraries(LithiumDrums
    PRIVATE
        LithiumAssets
        LithiumFonts
        LithiumImportMaps
        LithiumLzma
    PUBLIC
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Kit and groove importer (DrumGizmo, SFZ, and the Groove MIDI Dataset)

# A small command-line tool. It compiles the importer and settings sources
# itself (rather than linking the plugin), so it only needs the core audio
# modules. KitManifest is there so the importer can check what it wrote by
# parsing it back.
# It also downloads and installs packs (--install), hence curl.
juce_add_console_app(LithiumImport PRODUCT_NAME "LithiumImport" NEEDS_CURL TRUE)
target_sources(LithiumImport PRIVATE
    tools/importer/ImportMain.cpp
    src/importer/Importer.cpp
    src/importer/KitWriter.cpp
    src/importer/DrumGizmoImporter.cpp
    src/importer/SfzParser.cpp
    src/importer/SfzImporter.cpp
    src/importer/SampleFolderImporter.cpp
    src/grooves/GrooveLibrary.cpp
    src/grooves/GrooveImporter.cpp
    src/packs/Archive.cpp
    src/packs/PackInstaller.cpp
    src/engine/KitManifest.cpp
    src/Settings.cpp)
target_include_directories(LithiumImport PRIVATE src)
target_compile_definitions(LithiumImport PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=0)
target_link_libraries(LithiumImport PRIVATE
    LithiumImportMaps
    LithiumAssets
    LithiumLzma
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
# Icon and installers

# Writes the app icon and the installers' artwork (assets/icon/) from the
# logo code the header draws with. Like the renders, its output is committed;
# run scripts/make-icons.sh by hand after changing the logo.
juce_add_console_app(LithiumIconGen PRODUCT_NAME "LithiumIconGen")
target_sources(LithiumIconGen PRIVATE tools/icongen/IconGenerator.cpp src/ui/Logo.cpp)
target_include_directories(LithiumIconGen PRIVATE src)
target_compile_definitions(LithiumIconGen PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(LithiumIconGen PRIVATE
    LithiumFonts
    juce::juce_graphics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# The graphical installer in the Linux download ("Install Lithium Drums").
# Windows and macOS use their own installers (packaging/).
if(UNIX AND NOT APPLE)
    juce_add_gui_app(LithiumDrumsInstaller
        PRODUCT_NAME "Install Lithium Drums"
        COMPANY_NAME "${LITHIUM_COMPANY_NAME}"
        BUNDLE_ID    "${LITHIUM_BUNDLE_ID}.installer"
        VERSION      "${PROJECT_VERSION}")
    target_sources(LithiumDrumsInstaller PRIVATE
        tools/installer/InstallerMain.cpp
        tools/installer/Installation.cpp
        src/ui/Logo.cpp
        src/ui/LithiumLookAndFeel.cpp)
    target_include_directories(LithiumDrumsInstaller PRIVATE src)
    target_compile_definitions(LithiumDrumsInstaller PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        LITHIUM_VERSION_STRING="${PROJECT_VERSION}")
    target_link_libraries(LithiumDrumsInstaller PRIVATE
        LithiumFonts
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()

# ---------------------------------------------------------------------------
# Room reverb impulse responses

# Synthesizes the built-in IRs and converts measured ones (assets/irs/).
# Like the room backdrop, its output is committed; run it by hand.
juce_add_console_app(LithiumIrGen PRODUCT_NAME "LithiumIrGen")
target_sources(LithiumIrGen PRIVATE tools/irgen/IrGenerator.cpp)
target_compile_definitions(LithiumIrGen PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=0)
target_link_libraries(LithiumIrGen PRIVATE
    juce::juce_audio_formats
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Factory presets

# The presets are built from assets/presets/recipes.json by LithiumPresetGen,
# which plays each one through the real plugin (so it links the shared code,
# like the tests) and needs the kits installed. Its output is committed, so a
# normal build only compiles it.
add_executable(LithiumPresetGen tools/presetgen/PresetGen.cpp)
target_include_directories(LithiumPresetGen PRIVATE src)
target_link_libraries(LithiumPresetGen PRIVATE LithiumDrums)

# Where Settings::getFactoryPresetsFolder() looks on Linux.
if(UNIX AND NOT APPLE)
    install(DIRECTORY assets/presets/ DESTINATION share/lithium-drums/presets
            FILES_MATCHING PATTERN "*.lithiumpreset")
endif()

# ---------------------------------------------------------------------------
# Tests

if(LITHIUM_BUILD_TESTS)
    # The tests link the plugin's shared-code library (the processor, engine
    # and UI, compiled once) rather than recompiling the sources.
    add_executable(LithiumDrumsTests tests/TestMain.cpp tests/EngineTests.cpp tests/MixerTests.cpp tests/MidiTests.cpp tests/GrooveTests.cpp tests/PackTests.cpp tests/SwapTests.cpp tests/FxTests.cpp tests/EmulationTests.cpp tests/EmulationCompare.cpp)
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
