# Provide the GPMD85Emulator sources (see cmake/Dependencies.cmake) and stage
# the subset we build into core/gpmd85-generated, which is what
# core/CMakeLists.txt compiles.
#
# Only the *machine* is staged: the 8080, the memory maps, the support chips,
# the interfaces and peripherals, SAASound, and the small utility layer they
# share (CommonUtils). GPMD85Emulator's application -- TEmulator, ScreenPMD85,
# SoundDriver, Settings, the Dear ImGui gui/ tree and main.cpp -- is neither
# copied nor compiled; core/pmd85/pmd85_host.cpp stands in for it. So nothing
# upstream is replaced or rewritten, and a newer GPMD85Emulator stages as long
# as the chip classes keep their interfaces.
#
# The machine sources reach SDL only through upstream's globals.h, for a
# scancode enum and a few macros; tools/gpmd85/support/shim/SDL.h stands in
# for SDL on their include path (see that header), so they compile unmodified
# and the core links no SDL of its own.
#
# The copy is done in CMake rather than by a shell script: MSYS2/MinGW
# configures cannot execute a .sh through execute_process ("inappropriate file
# type or format"). Source edits, when there are any, live in
# tools/gpmd85/patch-staged-tree.py (idempotent, anchored, hard-failing),
# because a native Windows python IS directly invocable where a .sh is not.
#
# Staging is automatic: it runs when the staged tree is missing, when the
# source has moved, when the patch script or this file changes, or on demand
# with -DGPMD85_RESTAGE=ON (which is also how to pick up uncommitted edits in a
# working checkout pointed at by GPMD85_SRC).

set(GPMD85_GEN "${CMAKE_SOURCE_DIR}/core/gpmd85-generated")
set(GPMD85_PATCH_SCRIPT "${CMAKE_SOURCE_DIR}/tools/gpmd85/patch-staged-tree.py")

option(GPMD85_RESTAGE "Re-stage GPMD85Emulator sources from the source checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

pmd85_provide_dependency(
  NAME GPMD85Emulator
  PATH third_party/gpmd85
  URL "${GPMD85_URL}"
  COMMIT "${GPMD85_COMMIT}"
  SENTINEL src/ChipCpu8080.cpp
  OVERRIDE GPMD85_SRC
  RESULT GPMD85_DIR)

# The machine sources, by name: a file upstream adds is not built until it is
# listed here and in core/CMakeLists.txt, so a new upstream dependency on the
# application layer shows up as a clear compile error rather than a silent
# pull of the SDL/ImGui app into the core.
set(GPMD85_STAGED_SOURCES
  ChipCpu8080 ChipCpu8080P
  ChipMemory ChipMemory12 ChipMemory2A ChipMemory2AEx ChipMemory3
  ChipMemory3Ex ChipMemoryC2717 ChipMemoryMato
  ChipPIO8255 ChipPIT8253 ChipUSART8251
  IifGPIO IifTape IifTapeMato IifTapePMD85 IifTimer
  Mif85 Mouse602 Pmd32 RomModule RomMegaModule SystemPIO
  TapeBrowser
  CommonUtils)
# Headers with no .cpp of their own.
set(GPMD85_STAGED_HEADERS
  InterruptController.h PeripheralDevice.h macros8080.h)

set(_gpmd85_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${GPMD85_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _gpmd85_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _gpmd85_head)
  set(_gpmd85_head "${GPMD85_DIR}")
endif()
file(SHA256 "${GPMD85_PATCH_SCRIPT}" _gpmd85_patch_hash)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _gpmd85_stage_hash)
set(_gpmd85_want "${_gpmd85_head} ${_gpmd85_patch_hash} ${_gpmd85_stage_hash}")

set(_gpmd85_staged "")
if(EXISTS "${GPMD85_GEN}/.source-info")
  file(READ "${GPMD85_GEN}/.source-info" _gpmd85_staged)
  string(STRIP "${_gpmd85_staged}" _gpmd85_staged)
endif()

set(_gpmd85_sentinel "${GPMD85_GEN}/src/ChipCpu8080.cpp")

if(GPMD85_RESTAGE OR NOT EXISTS "${_gpmd85_sentinel}"
   OR NOT _gpmd85_staged STREQUAL _gpmd85_want)
  message(STATUS "Staging GPMD85Emulator machine sources from ${GPMD85_DIR}")
  file(REMOVE_RECURSE "${GPMD85_GEN}")
  file(MAKE_DIRECTORY "${GPMD85_GEN}/src" "${GPMD85_GEN}/saa" "${GPMD85_GEN}/rom")

  foreach(_f ${GPMD85_STAGED_SOURCES})
    foreach(_ext cpp h)
      if(NOT EXISTS "${GPMD85_DIR}/src/${_f}.${_ext}")
        message(FATAL_ERROR "GPMD85Emulator source ${GPMD85_DIR} is missing src/${_f}.${_ext}")
      endif()
      file(COPY "${GPMD85_DIR}/src/${_f}.${_ext}" DESTINATION "${GPMD85_GEN}/src")
    endforeach()
  endforeach()
  foreach(_h ${GPMD85_STAGED_HEADERS})
    file(COPY "${GPMD85_DIR}/src/${_h}" DESTINATION "${GPMD85_GEN}/src")
  endforeach()
  file(COPY "${GPMD85_DIR}/globals.h" "${GPMD85_DIR}/sigslot.hpp"
       DESTINATION "${GPMD85_GEN}")
  file(GLOB _saa "${GPMD85_DIR}/saa/*.cpp" "${GPMD85_DIR}/saa/*.h"
                 "${GPMD85_DIR}/saa/LICENCE")
  file(COPY ${_saa} DESTINATION "${GPMD85_GEN}/saa")
  # The firmware images: system monitors and the stock ROM modules, embedded
  # by tools/gpmd85/embed-roms.py (see COMPLIANCE.md).
  file(GLOB _roms "${GPMD85_DIR}/rom/*.rom" "${GPMD85_DIR}/rom/*.rmm"
                  "${GPMD85_DIR}/rom/README.md")
  file(COPY ${_roms} DESTINATION "${GPMD85_GEN}/rom")
  file(COPY "${GPMD85_DIR}/LICENSE.md" DESTINATION "${GPMD85_GEN}")

  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${GPMD85_PATCH_SCRIPT}" "${GPMD85_GEN}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    # Leave nothing half-patched behind.
    file(REMOVE_RECURSE "${GPMD85_GEN}")
    message(FATAL_ERROR
      "GPMD85Emulator staging patches failed. The pinned commit "
      "(${GPMD85_COMMIT}) and the anchors in\n  ${GPMD85_PATCH_SCRIPT}\nhave "
      "to agree; a source checkout of a different version is the usual "
      "cause.\n${_out}${_err}")
  endif()

  file(WRITE "${GPMD85_GEN}/.source-info" "${_gpmd85_want}\n")
endif()

message(STATUS "GPMD85Emulator staged from ${GPMD85_DIR}")
