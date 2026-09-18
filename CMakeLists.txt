cmake_minimum_required(VERSION 3.28...3.30)

# ---------------------------------------------------------------------------
# SEIInjector - OBS Studio plugin: per-frame real-time stamps in pushed video.
#
# Build modes
#   CI / by default (SEI_TIMESTAMP_BOOTSTRAP_OBS=ON):
#       Keep the official template platform setup and find_package(libobs).
#       macOS/Windows bootstrap pinned SDKs; Linux uses the installed SDK.
#   Local dev (SEI_TIMESTAMP_BOOTSTRAP_OBS=OFF):
#       Prefer an exported OBS SDK package; allow explicit manual SDK paths.
#       See docs/BUILDING.md for platform examples and ABI requirements.
# ---------------------------------------------------------------------------
option(
  SEI_TIMESTAMP_BOOTSTRAP_OBS
  "Download/build libobs + obs-deps from the pinned obs-studio source (CI). \
Set OFF to link against an already-installed OBS for local dev."
  ON
)
option(SEI_TIMESTAMP_BUILD_TESTS "Build core and encoder reliability tests" ON)
option(
  SEI_TIMESTAMP_DYNAMIC_FFMPEG
  "Unsupported legacy option; ON is rejected. FFmpeg must be linked directly."
  OFF
)

# Reject before bootstrap: an invalid legacy configuration must not download
# dependencies or increment the source-tree build number.
if(SEI_TIMESTAMP_DYNAMIC_FFMPEG)
  message(
    FATAL_ERROR
    "SEI_TIMESTAMP_DYNAMIC_FFMPEG is not implemented and is disabled. "
    "Use OFF (direct linking) and rebuild against the target OBS/FFmpeg SDK. "
    "A binary is not compatible with arbitrary FFmpeg major versions."
  )
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The template platform modules handle the full OBS SDK bootstrap (libobs build,
# Xcode/bundle packaging for macOS, installer generation). For CI we use all of
# it; for local dev (SEI_TIMESTAMP_BOOTSTRAP_OBS=OFF) we keep a minimal setup so
# a plain Makefile/Ninja build against an installed OBS just works.
if(SEI_TIMESTAMP_BOOTSTRAP_OBS)
  include(compilerconfig)
  include(defaults)
  include(helpers)
  set(_TEMPLATE_HELPERS ON)
else()
  set(_TEMPLATE_HELPERS OFF)
endif()

# ---------------------------------------------------------------------------
# libobs discovery
# ---------------------------------------------------------------------------
if(SEI_TIMESTAMP_BOOTSTRAP_OBS)
  # CI mode: preserve template SDK discovery (bootstrapped on macOS/Windows,
  # installed development package on Linux).
  find_package(libobs REQUIRED)
  set(_OBS_TARGET OBS::libobs)
else()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/local-libobs.cmake")
  set(_OBS_TARGET OBS::libobs)
endif()

# ---------------------------------------------------------------------------
# FFmpeg discovery (avcodec + avutil).
#   Prefer pkg-config (Homebrew/macOS, apt/Ubuntu); fall back to the prebuilt
#   obs-deps bundle (Windows CI) on CMAKE_PREFIX_PATH.
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/ffmpeg-dependencies.cmake")

# ---------------------------------------------------------------------------
# plugin
# ---------------------------------------------------------------------------
set(
  PLUGIN_SOURCES
  src/sei-timestamp-plugin.c
  src/sei-timestamp-encoder.c
  src/sei-payload.c
  src/h26x-util.c
  src/realtime-clock.c
  src/pts-timeline.c
  src/packet-pump.c
)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PLUGIN_SOURCES})
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE SEI_TIMESTAMP_VERSION="${_version}")

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# All discovery paths expose imported targets, including manual SDKs.
# The public FFmpeg structs require headers and libraries from the same ABI.
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${_OBS_TARGET} SEI::FFmpeg)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Threads::Threads)
endif()

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32)
endif()

if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /W3)
else()
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

if(_TEMPLATE_HELPERS)
  # CI: full template packaging (bundle / installer generation / layout).
  set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
else()
  # Local dev: minimal OBS plugin layout (flat shared object, no bundle).
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES PREFIX "" OUTPUT_NAME ${_name} LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin"
  )
  include(GNUInstallDirs)
  install(TARGETS ${CMAKE_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
  install(DIRECTORY data/ DESTINATION "${CMAKE_INSTALL_DATADIR}/obs-plugins/sei-timestamp")
endif()

# ---------------------------------------------------------------------------
# standalone core tests (no OBS / FFmpeg)
# ---------------------------------------------------------------------------
if(SEI_TIMESTAMP_BUILD_TESTS)
  enable_testing()
  add_executable(sei_core_tests tests/test_sei.c src/sei-payload.c src/h26x-util.c)
  target_include_directories(sei_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME sei_core_tests COMMAND sei_core_tests)
  add_executable(
    sei_reliability_tests
    tests/test_reliability.c src/pts-timeline.c src/packet-pump.c src/h26x-util.c src/sei-payload.c
  )
  target_include_directories(sei_reliability_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(sei_reliability_tests PRIVATE SEI::FFmpeg)
  add_test(NAME sei_reliability_tests COMMAND sei_reliability_tests)
  add_executable(sei_codec_delay_tests tests/test_codec_delay.c src/pts-timeline.c src/packet-pump.c src/h26x-util.c)
  target_include_directories(sei_codec_delay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(sei_codec_delay_tests PRIVATE SEI::FFmpeg)
  add_test(NAME sei_codec_delay_tests COMMAND sei_codec_delay_tests)
  set_tests_properties(sei_codec_delay_tests PROPERTIES SKIP_RETURN_CODE 77)
  if(UNIX)
    add_executable(sei_clock_tests tests/test_clock.c)
    target_link_libraries(sei_clock_tests PRIVATE Threads::Threads)
    add_test(NAME sei_clock_tests COMMAND sei_clock_tests)
  endif()
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME sei_dependency_discovery
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dependency_discovery.py"
    )
    add_test(NAME sei_gateway_tests COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gateway.py")
  endif()
endif()

message(STATUS "=================================================")
message(STATUS "SEI Timestamp plugin: ${_name} v${_version}")
message(STATUS "  Bootstrap libobs : ${SEI_TIMESTAMP_BOOTSTRAP_OBS}")
message(STATUS "  Tests            : ${SEI_TIMESTAMP_BUILD_TESTS}")
message(STATUS "=================================================")
