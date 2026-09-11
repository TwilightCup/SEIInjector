cmake_minimum_required(VERSION 3.28...3.30)

# ---------------------------------------------------------------------------
# SEIInjector - OBS Studio plugin: per-frame real-time stamps in pushed video.
#
# Build modes
#   CI / by default (SEI_TIMESTAMP_BOOTSTRAP_OBS=ON):
#       The official obs-plugintemplate bootstrap downloads the pinned
#       obs-studio source + prebuilt obs-deps, compiles libobs, and links via
#       find_package(libobs). Used by the GitHub Actions pipeline.
#   Local dev (SEI_TIMESTAMP_BOOTSTRAP_OBS=OFF):
#       Skips the SDK bootstrap and links against an already-installed OBS
#       (e.g. /Applications/OBS.app) using libobs headers you provide. Fast
#       iteration path; keep _simde/_obscfg helpers (see below).
# ---------------------------------------------------------------------------
option(
  SEI_TIMESTAMP_BOOTSTRAP_OBS
  "Download/build libobs + obs-deps from the pinned obs-studio source (CI). \
Set OFF to link against an already-installed OBS for local dev."
  ON
)
option(SEI_TIMESTAMP_BUILD_TESTS "Build the standalone core-logic tests (no OBS / FFmpeg needed)" ON)
option(
  SEI_TIMESTAMP_DYNAMIC_FFMPEG
  "Resolve FFmpeg symbols at runtime from the host OBS instead of linking a specific libavcodec."
  OFF
)

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
  # CI mode: libobs was built by the bootstrap into .deps and exported as a
  # CMake package on CMAKE_PREFIX_PATH.
  find_package(libobs REQUIRED)
  set(_OBS_TARGET OBS::libobs)
else()
  # Local dev mode: point OBS_INCLUDE_DIR at a libobs source checkout and let
  # the framework/runtime library be discovered from an installed OBS.
  if(NOT DEFINED OBS_INCLUDE_DIR)
    set(OBS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/libobs" CACHE PATH "OBS libobs include directory")
  endif()

  set(_OBS_LIB_LOCATION "")
  set(_OBS_FRAMEWORK_DEFAULT "/Applications/OBS.app/Contents/Frameworks/libobs.framework")
  if(EXISTS "${_OBS_FRAMEWORK_DEFAULT}/libobs")
    set(_OBS_LIB_LOCATION "${_OBS_FRAMEWORK_DEFAULT}/libobs")
    message(STATUS "OBS: using installed libobs.framework at ${_OBS_FRAMEWORK_DEFAULT}")
  elseif(NOT DEFINED OBS_LIB_DIR)
    message(
      FATAL_ERROR
      "OBS::libobs not found. Set OBS_INCLUDE_DIR + OBS_LIB_DIR (or link an "
      "installed OBS.app framework) for local dev, or build with "
      "SEI_TIMESTAMP_BOOTSTRAP_OBS=ON."
    )
  else()
    set(_OBS_LIB_LOCATION "${OBS_LIB_DIR}/libobs.dylib")
  endif()

  add_library(OBS::libobs SHARED IMPORTED)
  set_target_properties(
    OBS::libobs
    PROPERTIES IMPORTED_LOCATION "${_OBS_LIB_LOCATION}" INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}"
  )
  set(_OBS_TARGET OBS::libobs)

  # libobs graphics headers pull in SIMDe (vendored locally for the app-only
  # dev path; CI gets it from obs-deps) ...
  if(NOT DEFINED SIMDE_INCLUDE_DIR)
    set(SIMDE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/_simde" CACHE PATH "SIMDe include directory")
  endif()
  # ... and a configure-generated obsconfig.h that isn't in a bare checkout.
  if(NOT DEFINED OBS_CONFIG_DIR)
    set(OBS_CONFIG_DIR "${CMAKE_SOURCE_DIR}/_obscfg" CACHE PATH "Directory containing a generated libobs obsconfig.h")
  endif()
endif()

# ---------------------------------------------------------------------------
# FFmpeg discovery (avcodec + avutil).
#   Prefer pkg-config (Homebrew/macOS, apt/Ubuntu); fall back to the prebuilt
#   obs-deps bundle (Windows CI) on CMAKE_PREFIX_PATH.
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
set(_FFMPEG_OK FALSE)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavcodec libavutil)
  if(TARGET PkgConfig::FFMPEG)
    message(STATUS "FFmpeg: found via pkg-config")
    set(_FFMPEG_OK TRUE)
  endif()
endif()
if(NOT _FFMPEG_OK AND SEI_TIMESTAMP_BOOTSTRAP_OBS)
  find_path(
    FFMPEG_INCLUDE_DIR_FF
    NAMES libavcodec/avcodec.h
    PATHS ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
  )
  find_library(AVCODEC_LIB NAMES avcodec PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib NO_DEFAULT_PATH)
  find_library(AVUTIL_LIB NAMES avutil PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib NO_DEFAULT_PATH)
  if(FFMPEG_INCLUDE_DIR_FF AND AVCODEC_LIB AND AVUTIL_LIB)
    message(STATUS "FFmpeg: found via obs-deps (${FFMPEG_INCLUDE_DIR_FF})")
    set(_FFMPEG_OK TRUE)
  endif()
endif()

if(NOT _FFMPEG_OK)
  message(
    FATAL_ERROR
    "FFmpeg (libavcodec + libavutil) not found. Install pkg-config dev "
    "packages (macOS: brew install ffmpeg; Ubuntu: libavcodec-dev "
    "libavutil-dev) or provide them via the obs-deps bundle (CI/Windows)."
  )
endif()

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
)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PLUGIN_SOURCES})
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE SEI_TIMESTAMP_VERSION="${_version}")

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${_OBS_TARGET})

# Headers are always needed: the encoder reads/writes AVFrame / AVPacket public
# fields and the small leading AVCodecContext config fields, all compiled
# against these headers (the runtime major is gated in ffmpeg-bind.c).
if(TARGET PkgConfig::FFMPEG)
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
else()
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIR_FF})
endif()

# Optional static FFmpeg link (local single-OBS dev / harnesses). Default OFF in
# favour of runtime resolution so one binary rides multiple OBS versions.
if(NOT SEI_TIMESTAMP_DYNAMIC_FFMPEG)
  if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::FFMPEG)
  else()
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${AVCODEC_LIB} ${AVUTIL_LIB})
  endif()
endif()

# The runtime loader uses dlopen/dlsym on ELF; Mach-O and Windows provide them
# in their base runtimes.
if(NOT WIN32 AND NOT APPLE AND SEI_TIMESTAMP_DYNAMIC_FFMPEG)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()

if(NOT SEI_TIMESTAMP_BOOTSTRAP_OBS)
  # Local dev against an installed OBS: vendor SIMDe + a generated
  # obsconfig.h, and let the plugin resolve app-relative paths at runtime.
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SIMDE_INCLUDE_DIR} ${OBS_CONFIG_DIR})
  if(APPLE)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE OBS_INSTALL_PREFIX="@executable_path/..")
  endif()
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
endif()

message(STATUS "=================================================")
message(STATUS "SEI Timestamp plugin: ${_name} v${_version}")
message(STATUS "  Bootstrap libobs : ${SEI_TIMESTAMP_BOOTSTRAP_OBS}")
message(STATUS "  Tests            : ${SEI_TIMESTAMP_BUILD_TESTS}")
message(STATUS "=================================================")
