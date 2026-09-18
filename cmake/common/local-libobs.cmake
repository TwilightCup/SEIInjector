# Local SDK discovery only. CI keeps the template's libobs discovery unchanged.
include_guard(GLOBAL)

set(OBS_SDK_PREFIX "" CACHE PATH "OBS SDK installation prefix containing libobsConfig.cmake")
if(OBS_SDK_PREFIX)
  list(PREPEND CMAKE_PREFIX_PATH "${OBS_SDK_PREFIX}")
endif()
find_package(libobs CONFIG QUIET)
if(TARGET OBS::libobs)
  message(STATUS "OBS: exported SDK target OBS::libobs (${libobs_DIR}; version=${libobs_VERSION})")
  return()
endif()
if(libobs_FOUND)
  message(FATAL_ERROR "The libobs package at ${libobs_DIR} did not export OBS::libobs. Use a matching OBS SDK.")
endif()

# Manual fallback for SDKs without an exported CMake package. CMake chooses
# platform library prefixes/suffixes, including Apple frameworks and MSVC .lib.
set(OBS_LIB_DIR "" CACHE PATH "Manual libobs library or framework search directory")
set(OBS_RUNTIME_DIR "" CACHE PATH "Manual Windows OBS DLL directory")
find_path(OBS_INCLUDE_DIR NAMES obs-module.h
  HINTS "${OBS_SDK_PREFIX}" "${CMAKE_SOURCE_DIR}/obs-studio-master/libobs"
  PATH_SUFFIXES include/obs include libobs)
find_library(OBS_LIBRARY NAMES obs libobs
  HINTS "${OBS_LIB_DIR}" "${OBS_SDK_PREFIX}"
  PATHS /Applications/OBS.app/Contents/Frameworks
  PATH_SUFFIXES lib lib64 bin/64bit Frameworks)
if(NOT OBS_INCLUDE_DIR OR NOT EXISTS "${OBS_INCLUDE_DIR}/obs-module.h" OR NOT OBS_LIBRARY)
  message(FATAL_ERROR
    "Local libobs SDK not found. Set OBS_SDK_PREFIX or CMAKE_PREFIX_PATH to an SDK "
    "exporting OBS::libobs (or set libobs_DIR to its CMake package directory). "
    "Manual fallback requires OBS_INCLUDE_DIR and OBS_LIB_DIR/OBS_LIBRARY; "
    "use a framework on macOS, a development .so on Linux, or an import .lib on Windows. "
    "Headers, generated obsconfig.h and libraries must belong to the target OBS SDK.")
endif()

if(WIN32)
  find_file(OBS_RUNTIME_LIBRARY NAMES obs.dll libobs.dll
    HINTS "${OBS_RUNTIME_DIR}" "${OBS_LIB_DIR}" "${OBS_SDK_PREFIX}"
    PATH_SUFFIXES bin bin/64bit)
  if(NOT OBS_RUNTIME_LIBRARY)
    message(FATAL_ERROR "Manual Windows SDK requires obs.dll/libobs.dll. Set OBS_RUNTIME_DIR or OBS_RUNTIME_LIBRARY, and OBS_LIBRARY to its matching import library.")
  endif()
  add_library(OBS::libobs SHARED IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES
    IMPORTED_IMPLIB "${OBS_LIBRARY}" IMPORTED_LOCATION "${OBS_RUNTIME_LIBRARY}")
else()
  add_library(OBS::libobs UNKNOWN IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${OBS_LIBRARY}")
endif()
set_target_properties(OBS::libobs PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}")

# These are only needed with bare source headers; exported SDK targets own
# their transitive include directories. Never inject them into the package path.
set(OBS_CONFIG_DIR "" CACHE PATH "Directory containing the target SDK's generated obsconfig.h")
set(SIMDE_INCLUDE_DIR "" CACHE PATH "Directory containing simde headers for manual source-header builds")
foreach(_extra_include IN ITEMS OBS_CONFIG_DIR SIMDE_INCLUDE_DIR)
  if(${_extra_include})
    if(NOT IS_DIRECTORY "${${_extra_include}}")
      message(FATAL_ERROR "${_extra_include} does not exist: ${${_extra_include}}")
    endif()
    set_property(TARGET OBS::libobs APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${${_extra_include}}")
  endif()
endforeach()
# The repository's optional _obscfg helper requires this definition.
if(APPLE AND OBS_CONFIG_DIR STREQUAL "${CMAKE_SOURCE_DIR}/_obscfg")
  set_property(TARGET OBS::libobs APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
    "OBS_INSTALL_PREFIX=\"@executable_path/..\"")
endif()
message(STATUS "OBS: manual SDK headers=${OBS_INCLUDE_DIR}; library=${OBS_LIBRARY}; runtime=${OBS_RUNTIME_LIBRARY}")
message(STATUS "OBS: manual SDK version is recorded from compile-time headers in the plugin load log")
