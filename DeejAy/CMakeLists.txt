cmake_minimum_required(VERSION 3.22)

# DEEJ-AY by MP Audio — native macOS DJ software.
# The engine (engine/) is plain C++20 with no JUCE dependency and builds anywhere; the app (app/) needs
# JUCE 9 and is built for macOS (Linux is supported as a compile-check target).

set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version (Ventura)")

project(DeejAy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DEEJAY_BUILD_APP   "Build the JUCE application"                   ON)
option(DEEJAY_BUILD_TESTS "Build the engine test executable"             ON)
option(DEEJAY_RTSAN       "Enable Clang RealtimeSanitizer (clang >= 20)" OFF)
set(DEEJAY_JUCE_DIR "" CACHE PATH "Path to a JUCE checkout (9.0.2 or later). Empty = fetch JUCE 9.0.4")
set(DEEJAY_JUCE_TAG "9.0.4" CACHE STRING "JUCE git tag to fetch when DEEJAY_JUCE_DIR is empty")

# ---------------------------------------------------------------------------------------------
# engine: real-time core, analysis, library, controller mapping
# ---------------------------------------------------------------------------------------------
add_library(deejay_engine STATIC
    engine/dj/Json.cpp
    engine/dj/Effects.cpp
    engine/dj/ColorFx.cpp
    engine/dj/Mixer.cpp
    engine/dj/Deck.cpp
    engine/dj/Sampler.cpp
    engine/dj/Recorder.cpp
    engine/dj/Performer.cpp
    engine/dj/Engine.cpp
    engine/dj/analysis/Analysis.cpp
    engine/dj/analysis/StemSeparator.cpp
    engine/dj/library/Tags.cpp
    engine/dj/library/Library.cpp
    engine/dj/library/Samples.cpp
    engine/dj/library/StreamLinks.cpp
    engine/dj/midi/Flx10.cpp)
target_include_directories(deejay_engine PUBLIC engine)
target_compile_features(deejay_engine PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(deejay_engine PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(deejay_engine PRIVATE /W4)
else()
    target_compile_options(deejay_engine PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wno-sign-conversion)
endif()
if(DEEJAY_RTSAN)
    target_compile_options(deejay_engine PUBLIC -fsanitize=realtime)
    target_link_options(deejay_engine PUBLIC -fsanitize=realtime)
endif()

# ---------------------------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------------------------
if(DEEJAY_BUILD_TESTS)
    enable_testing()
    add_executable(deejay_tests tests/EngineTests.cpp tests/AnalysisTests.cpp tests/LibraryTests.cpp tests/ControllerTests.cpp
        tests/PerformerTests.cpp)
    target_link_libraries(deejay_tests PRIVATE deejay_engine)
    add_test(NAME engine COMMAND deejay_tests)
endif()

# ---------------------------------------------------------------------------------------------
# app
# ---------------------------------------------------------------------------------------------
if(DEEJAY_BUILD_APP)
    if(DEEJAY_JUCE_DIR)
        add_subdirectory(${DEEJAY_JUCE_DIR} ${CMAKE_BINARY_DIR}/juce EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(juce
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG        ${DEEJAY_JUCE_TAG}
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE)
        FetchContent_MakeAvailable(juce)
    endif()

    juce_add_gui_app(DeejAy
        PRODUCT_NAME "DEEJ-AY"
        COMPANY_NAME "MP Audio"
        COMPANY_COPYRIGHT "MP Audio"
        BUNDLE_ID com.mpaudio.deejay
        VERSION ${PROJECT_VERSION}
        ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon-1024.png
        HARDENED_RUNTIME_ENABLED TRUE
        HARDENED_RUNTIME_OPTIONS "com.apple.security.device.audio-input"
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "DEEJ-AY uses your DJ controller's audio interface."
        PLIST_TO_MERGE "<plist><dict><key>LSApplicationCategoryType</key><string>public.app-category.music</string><key>NSHighResolutionCapable</key><true/></dict></plist>")

    juce_generate_juce_header(DeejAy)

    target_sources(DeejAy PRIVATE
        app/Main.cpp
        app/AppModel.cpp
        app/AudioHost.cpp
        app/MidiHost.cpp
        app/WaveformView.cpp
        app/DeckView.cpp
        app/MixerView.cpp
        app/FxView.cpp
        app/SamplerView.cpp
        app/LibraryView.cpp
        app/SettingsView.cpp
        app/MainComponent.cpp)
    target_include_directories(DeejAy PRIVATE app)

    target_compile_definitions(DeejAy PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DeejAy,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DeejAy,JUCE_VERSION>")

    target_link_libraries(DeejAy PRIVATE
        deejay_engine
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_gui_extra
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endif()
