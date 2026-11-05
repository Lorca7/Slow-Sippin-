cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
project(SlowSippin VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: use a local checkout (-DSS_JUCE_DIR=/path/to/JUCE) or fetch the pinned release.
set(SS_JUCE_DIR "" CACHE PATH "Path to a JUCE checkout (optional)")
if(SS_JUCE_DIR)
    add_subdirectory(${SS_JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(SS_FORMATS AU VST3 Standalone)
else()
    set(SS_FORMATS VST3 Standalone)
endif()

juce_add_plugin(SlowSippin
    PRODUCT_NAME "Slow Sippin"
    COMPANY_NAME "Slow Sippin"
    BUNDLE_ID eu.slowsippin.plugin
    VERSION 1.0.0
    DESCRIPTION "Extreme time-stretch instrument for textures, pads and drones"
    PLUGIN_MANUFACTURER_CODE Slsp
    PLUGIN_CODE Sls1
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    VST3_CATEGORIES Instrument Synth
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon-1024.png"
    FORMATS ${SS_FORMATS})

target_sources(SlowSippin PRIVATE
    Source/StretchEngine.cpp
    Source/VoiceEngine.cpp
    Source/Offline.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

juce_add_binary_data(SlowSippinFonts SOURCES
    Resources/fonts/Archivo-SemiBold.ttf
    Resources/fonts/Archivo-Bold.ttf
    Resources/fonts/Archivo-ExtraBold.ttf
    Resources/fonts/IBMPlexMono-Light.ttf
    Resources/fonts/IBMPlexMono-Regular.ttf
    Resources/fonts/IBMPlexMono-Medium.ttf)

target_compile_definitions(SlowSippin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SlowSippin
    PRIVATE
        SlowSippinFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Engine unit tests (no JUCE needed)
add_executable(EngineTests Tests/EngineTests.cpp Source/StretchEngine.cpp)
