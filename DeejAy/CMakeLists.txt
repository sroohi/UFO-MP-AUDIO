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
    engine/dj/Engine.cpp)
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
    add_executable(deejay_tests tests/EngineTests.cpp)
    target_link_libraries(deejay_tests PRIVATE deejay_engine)
    add_test(NAME engine COMMAND deejay_tests)
endif()
