# Direct FFmpeg linking only. Explicit SDK selection never falls back to a
# potentially unrelated host pkg-config installation.
include_guard(GLOBAL)
set(FFMPEG_SDK_PREFIX "" CACHE PATH "Matching FFmpeg development SDK prefix (include/ and lib/)")
add_library(SEI::FFmpeg INTERFACE IMPORTED)

if(NOT FFMPEG_SDK_PREFIX)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(SEI_AVCODEC QUIET IMPORTED_TARGET libavcodec)
    pkg_check_modules(SEI_AVUTIL QUIET IMPORTED_TARGET libavutil)
  endif()
endif()

if(NOT FFMPEG_SDK_PREFIX AND TARGET PkgConfig::SEI_AVCODEC AND TARGET PkgConfig::SEI_AVUTIL)
  set_property(TARGET SEI::FFmpeg PROPERTY INTERFACE_LINK_LIBRARIES
    "PkgConfig::SEI_AVCODEC;PkgConfig::SEI_AVUTIL")
  message(STATUS "FFmpeg: pkg-config avcodec=${SEI_AVCODEC_VERSION}, avutil=${SEI_AVUTIL_VERSION}")
  message(STATUS "FFmpeg: include=${SEI_AVCODEC_INCLUDE_DIRS};${SEI_AVUTIL_INCLUDE_DIRS}; libraries=${SEI_AVCODEC_LINK_LIBRARIES};${SEI_AVUTIL_LINK_LIBRARIES}")
else()
  # Preserve the obs-deps fallback on CMAKE_PREFIX_PATH; make the same SDK
  # layout available to local builds. NO_CACHE avoids stale results when the
  # explicitly selected SDK changes between configurations.
  if(FFMPEG_SDK_PREFIX)
    set(_ffmpeg_prefixes "${FFMPEG_SDK_PREFIX}")
  else()
    set(_ffmpeg_prefixes ${CMAKE_PREFIX_PATH})
  endif()
  foreach(_component IN ITEMS avcodec avutil)
    if(_component STREQUAL avcodec)
      set(_header libavcodec/avcodec.h)
    else()
      set(_header libavutil/avutil.h)
    endif()
    find_path(_sei_${_component}_include NAMES "${_header}"
      PATHS ${_ffmpeg_prefixes} PATH_SUFFIXES include NO_DEFAULT_PATH NO_CACHE)
    find_library(_sei_${_component}_library NAMES ${_component}
      PATHS ${_ffmpeg_prefixes} PATH_SUFFIXES lib lib64 NO_DEFAULT_PATH NO_CACHE)
    if(NOT _sei_${_component}_include OR NOT _sei_${_component}_library)
      message(FATAL_ERROR
        "FFmpeg ${_component} development headers/library not found. "
        "Set FFMPEG_SDK_PREFIX to the FFmpeg SDK matching the target OBS, "
        "or provide libavcodec and libavutil via pkg-config. "
        "CI also searches the bootstrapped obs-deps on CMAKE_PREFIX_PATH. "
        "Runtime DLLs alone are not a development SDK.")
    endif()
    # UNKNOWN supports Unix shared libraries and Windows import libraries.
    # On Windows the matching DLLs must be supplied by the target OBS runtime.
    add_library(SEI::${_component} UNKNOWN IMPORTED)
    set_target_properties(SEI::${_component} PROPERTIES
      IMPORTED_LOCATION "${_sei_${_component}_library}"
      INTERFACE_INCLUDE_DIRECTORIES "${_sei_${_component}_include}")
    set_property(TARGET SEI::FFmpeg APPEND PROPERTY INTERFACE_LINK_LIBRARIES SEI::${_component})
    message(STATUS "FFmpeg ${_component}: include=${_sei_${_component}_include}; library=${_sei_${_component}_library}")
  endforeach()
endif()
message(STATUS "FFmpeg: direct linking; compile-time ABI versions are recorded in the plugin load log. Rebuild and validate for each target OBS/FFmpeg ABI.")
