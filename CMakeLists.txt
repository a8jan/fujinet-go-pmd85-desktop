cmake_minimum_required(VERSION 3.20)
# The version lives here and nowhere else: the About boxes, the macOS bundle
# and the release workflow all take it from PROJECT_VERSION.
project(fujinet-go-pmd85-desktop VERSION 0.1.0 LANGUAGES C CXX)

add_compile_definitions(PMD85_VERSION_STRING="${PROJECT_VERSION}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Which native frontend(s) to build. Each is found-or-skipped so a lean
# machine can still build the other one (or just the core + tests).
set(FRONTEND "all" CACHE STRING
    "Frontend to build: gnome, kde, macos, windows, all, none")

# Included here, at the top of the tree, so every subdirectory sees the
# *variables* it defines: GNUInstallDirs caches CMAKE_INSTALL_DATADIR as an
# empty string and only sets its non-cache fallback (share) in the including
# scope. Including it in a subdirectory leaves the frontends with an empty
# DATADIR -- and "${CMAKE_INSTALL_DATADIR}/applications" then resolves to the
# absolute path /applications, outside the prefix.
include(GNUInstallDirs)

include(cmake/Dependencies.cmake)
include(cmake/StageGPMD85.cmake)

enable_testing()
add_subdirectory(core)

# The FujiNet runtime: built from the pinned firmware sources as part of the
# normal build. Set up before the frontends so the macOS bundle can depend on
# the library it packs.
if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/FujiNetRuntime.cmake")
  include(cmake/FujiNetRuntime.cmake)
endif()

# Each frontend is native to one platform; with FRONTEND=all (the default)
# only the host's viable frontends are considered. add_frontend skips a
# directory that has no CMakeLists.txt yet; naming it explicitly is an error.
function(add_frontend name)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/frontends/${name}/CMakeLists.txt")
    if(FRONTEND STREQUAL name)
      message(FATAL_ERROR "The ${name} frontend is not implemented yet.")
    endif()
    message(STATUS "${name} frontend: not implemented yet -- skipping")
    return()
  endif()
  add_subdirectory("frontends/${name}")
endfunction()

if(FRONTEND STREQUAL "gnome" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(gnome)
endif()
if(FRONTEND STREQUAL "kde" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(kde)
endif()
if(FRONTEND STREQUAL "macos" OR (FRONTEND STREQUAL "all" AND APPLE))
  add_frontend(macos)
endif()
if(FRONTEND STREQUAL "windows" OR (FRONTEND STREQUAL "all" AND WIN32))
  add_frontend(windows)
endif()

# Application icons (the FujiNet Go mark in PMD 85 colours, rendered by
# tools/icons/make-icons.py). Desktop environments look the icon up by the
# desktop-entry id, so the art is installed under each frontend's id, at
# every hicolor size. Only for frontends actually built here.
set(PMD85_ICON_SIZES 16 24 32 48 64 128 192 256 512)
set(PMD85_ICON_TARGETS_AND_IDS
  fujinet-go-pmd85-gnome online.fujinet.go.pmd85.gnome
  fujinet-go-pmd85-kde online.fujinet.go.pmd85.kde)
list(LENGTH PMD85_ICON_TARGETS_AND_IDS _pmd85_icon_n)
math(EXPR _pmd85_icon_last "${_pmd85_icon_n} - 1")
foreach(_i RANGE 0 ${_pmd85_icon_last} 2)
  list(GET PMD85_ICON_TARGETS_AND_IDS ${_i} _target)
  math(EXPR _j "${_i} + 1")
  list(GET PMD85_ICON_TARGETS_AND_IDS ${_j} _id)
  if(TARGET ${_target})
    foreach(_size IN LISTS PMD85_ICON_SIZES)
      install(FILES data/icons/hicolor/${_size}x${_size}/apps/fujinet-go-pmd85.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
              RENAME ${_id}.png)
    endforeach()
  endif()
endforeach()
