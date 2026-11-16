cmake_minimum_required(VERSION 3.20)
project(tokkel VERSION 0.1.0 LANGUAGES C CXX)

# Standalone (not pulled in through add_subdirectory)?
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(TOKKEL_STANDALONE ON)
else()
  set(TOKKEL_STANDALONE OFF)
endif()

option(TOKKEL_BUILD_EXAMPLES "Build the tokkel examples" ${TOKKEL_STANDALONE})
option(TOKKEL_BUILD_TESTS "Build the tokkel tests" ${TOKKEL_STANDALONE})

set(TOKKEL_MINIAUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio")
set(TOKKEL_STK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/stk")

if(NOT EXISTS "${TOKKEL_MINIAUDIO_DIR}/miniaudio.h" OR NOT EXISTS "${TOKKEL_STK_DIR}/include/Stk.h")
  message(FATAL_ERROR "tokkel: vendor submodules are missing, run `git submodule update --init --recursive`")
endif()

# ---------------------------------------------------------------------------
# miniaudio: devices, mixing and decoding. Built from its single source file
# rather than its own CMake project, which adds options and targets we don't use.
# ---------------------------------------------------------------------------
add_library(tokkel_miniaudio STATIC "${TOKKEL_MINIAUDIO_DIR}/miniaudio.c")
target_include_directories(tokkel_miniaudio PUBLIC "${TOKKEL_MINIAUDIO_DIR}")
set_target_properties(tokkel_miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  find_package(Threads REQUIRED)
  target_link_libraries(tokkel_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
elseif(IOS)
  # miniaudio's iOS backend uses AVAudioSession, so it has to be compiled as
  # Objective-C there.
  set_source_files_properties("${TOKKEL_MINIAUDIO_DIR}/miniaudio.c" PROPERTIES
    COMPILE_FLAGS "-x objective-c")
  target_link_libraries(tokkel_miniaudio PUBLIC
    "-framework CoreFoundation" "-framework AudioToolbox" "-framework AVFoundation")
elseif(APPLE)
  target_link_libraries(tokkel_miniaudio PUBLIC
    "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

# ---------------------------------------------------------------------------
# STK: synthesis and DSP only. The device, MIDI, socket and thread classes are
# left out: miniaudio owns the audio device, so STK is never built "realtime"
# (no __STK_REALTIME__, no RtAudio, no ALSA/JACK dependency).
# ---------------------------------------------------------------------------
file(GLOB TOKKEL_STK_SOURCES "${TOKKEL_STK_DIR}/src/*.cpp")
list(FILTER TOKKEL_STK_SOURCES EXCLUDE REGEX
  "/(RtAudio|RtMidi|RtWvIn|RtWvOut|InetWvIn|InetWvOut|Socket|TcpClient|TcpServer|UdpSocket|Thread|Mutex|Messager)\\.cpp$")

add_library(tokkel_stk STATIC ${TOKKEL_STK_SOURCES})
target_include_directories(tokkel_stk SYSTEM PUBLIC "${TOKKEL_STK_DIR}/include")
target_compile_features(tokkel_stk PUBLIC cxx_std_17)
set_target_properties(tokkel_stk PROPERTIES POSITION_INDEPENDENT_CODE ON)
# STK reads its raw wave files as big-endian and swaps on little-endian hosts.
if(CMAKE_CXX_BYTE_ORDER STREQUAL "LITTLE_ENDIAN" OR EMSCRIPTEN)
  target_compile_definitions(tokkel_stk PUBLIC __LITTLE_ENDIAN__)
endif()
if(MSVC)
  target_compile_options(tokkel_stk PRIVATE /w)
  target_compile_definitions(tokkel_stk PUBLIC _USE_MATH_DEFINES NOMINMAX)
else()
  target_compile_options(tokkel_stk PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# tokkel
# ---------------------------------------------------------------------------
add_library(tokkel STATIC
  src/bass_synth.cpp
  src/cello_synth.cpp
  src/drum_synth.cpp
  src/engine.cpp
  src/guitar_synth.cpp
  src/piano_synth.cpp
  src/bell_synth.cpp
  src/sampler.cpp
  src/synth.cpp
)
add_library(tokkel::tokkel ALIAS tokkel)
target_include_directories(tokkel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(tokkel PUBLIC cxx_std_17)
set_target_properties(tokkel PROPERTIES POSITION_INDEPENDENT_CODE ON)
# miniaudio stays an implementation detail; STK is part of the public API
# (tokkel/stk_source.hpp wraps STK generators and instruments).
target_link_libraries(tokkel PUBLIC tokkel_stk PRIVATE tokkel_miniaudio)
if(NOT MSVC)
  target_compile_options(tokkel PRIVATE -Wall -Wextra)
endif()

if(TOKKEL_BUILD_EXAMPLES)
  add_executable(tokkel_sine examples/sine.cpp)
  target_link_libraries(tokkel_sine PRIVATE tokkel::tokkel)
endif()

if(TOKKEL_BUILD_TESTS)
  enable_testing()
  add_executable(tokkel_offline_test tests/offline_render.cpp)
  target_link_libraries(tokkel_offline_test PRIVATE tokkel::tokkel)
  add_test(NAME offline_render COMMAND tokkel_offline_test)
endif()
