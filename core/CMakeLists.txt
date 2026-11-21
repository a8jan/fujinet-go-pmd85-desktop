# libpmd85session: the toolkit-agnostic session core. Compiles the staged
# GPMD85Emulator machine sources plus our host (core/pmd85) and the desktop
# session/audio/FujiNet/debugger layers.

set(GP "${CMAKE_SOURCE_DIR}/core/gpmd85-generated")

if(NOT EXISTS "${GP}/src/ChipCpu8080.cpp")
  message(FATAL_ERROR
    "Staged GPMD85Emulator tree is missing. cmake/StageGPMD85.cmake should "
    "have produced it; re-run with -DGPMD85_RESTAGE=ON.")
endif()

find_package(SDL3 CONFIG REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- the staged machine ----------------------------------------------------
# GPMD85Emulator's machine sources (the list cmake/StageGPMD85.cmake stages),
# SAASound, and our host in their place of TEmulator/ScreenPMD85/SoundDriver.
set(GPMD85_MACHINE_SOURCES ${GPMD85_STAGED_SOURCES})
list(TRANSFORM GPMD85_MACHINE_SOURCES PREPEND "${GP}/src/")
list(TRANSFORM GPMD85_MACHINE_SOURCES APPEND ".cpp")
file(GLOB GPMD85_SAA_SOURCES "${GP}/saa/*.cpp")

add_library(gpmd85_core STATIC
  ${GPMD85_MACHINE_SOURCES}
  ${GPMD85_SAA_SOURCES}
  pmd85/pmd85_host.cpp
  pmd85/pmd85_becker.cpp
  pmd85/pmd85_shim.cpp
)
set_target_properties(gpmd85_core PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  POSITION_INDEPENDENT_CODE ON)
# The SDL stand-in goes FIRST, so upstream's #include "SDL.h" finds it and
# never a real SDL a toolchain might have on its default path.
target_include_directories(gpmd85_core BEFORE PUBLIC
  "${CMAKE_SOURCE_DIR}/tools/gpmd85/support/shim")
target_include_directories(gpmd85_core PUBLIC
  "${GP}" "${GP}/src" "${GP}/saa"
  "${CMAKE_CURRENT_SOURCE_DIR}/pmd85"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
# GPMD85Emulator's own warning policy (its CMakeLists: -Wno-deprecated); the
# rest are upstream's to fix, and silencing them only on its sources keeps
# ours visible.
target_compile_options(gpmd85_core PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated>)
set_source_files_properties(${GPMD85_MACHINE_SOURCES} ${GPMD85_SAA_SOURCES}
  PROPERTIES COMPILE_OPTIONS "-w")
# Ours: -Wall, less one that misfires on upstream's classes -- they have
# virtual functions but no virtual destructor, and the host deletes each
# through its own concrete type exactly as upstream's TEmulator does.
target_compile_options(gpmd85_core PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wno-delete-non-virtual-dtor>)
if(WIN32)
  target_link_libraries(gpmd85_core PUBLIC ws2_32)
endif()
# std::filesystem (Pmd32, CommonUtils) is in libstdc++ proper from GCC 9.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(gpmd85_core PUBLIC stdc++fs)
endif()

# ---- the embedded firmware ---------------------------------------------------
file(GLOB PMD85_EMBED_IMAGES
  "${GP}/rom/*.rom"
  "${GP}/rom/*.rmm")
list(APPEND PMD85_EMBED_IMAGES
  "${CMAKE_SOURCE_DIR}/tools/gpmd85/roms/fujinet-config.rmm")
set(PMD85_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
add_custom_command(
  OUTPUT "${PMD85_ROMS_C}"
  COMMAND "${Python3_EXECUTABLE}" -I
          "${CMAKE_SOURCE_DIR}/tools/gpmd85/embed-roms.py"
          "${PMD85_ROMS_C}" ${PMD85_EMBED_IMAGES}
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gpmd85/embed-roms.py"
          ${PMD85_EMBED_IMAGES}
  COMMENT "Embedding the PMD 85 firmware images"
  VERBATIM)

# ---- the session --------------------------------------------------------------
add_library(pmd85session STATIC
  src/session.c
  src/settings.c
  src/paths.c
  src/input_map.c
  src/audio_sdl.c
  src/gamepad_sdl.c
  src/joy_input.c
  src/pad_assign.c
  src/fujinet_runtime.c
  debugger/debugger.c
  debugger/i8080dasm.c
  debugger/expr.c
  debugger/symtab.c
  debugger/monitor_symbols.c
  debugger/memtools.c
  debugger/dwtrace.c
  debugger/prompt.c
  "${PMD85_ROMS_C}"
)
target_include_directories(pmd85session
  PUBLIC include pmd85
  PRIVATE src debugger "${CMAKE_CURRENT_BINARY_DIR}"
)
target_compile_options(pmd85session PRIVATE -Wall -Wextra -Wno-format-truncation)
target_compile_definitions(pmd85session PRIVATE
  _GNU_SOURCE
  # The dev fallback for the FujiNet runtime, so a git checkout runs without
  # an install step; and the install locations, baked in.
  PMD85_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  PMD85_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-pmd85"
  PMD85_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-pmd85"
)
target_link_libraries(pmd85session PUBLIC
  gpmd85_core
  SDL3::SDL3
  Threads::Threads
)
if(WIN32)
  target_link_libraries(pmd85session PUBLIC ws2_32 winmm)
else()
  target_link_libraries(pmd85session PUBLIC dl m)
endif()

# ---- tests ---------------------------------------------------------------------
# Boot-dependent tests skip (77) rather than fail when they cannot boot.

add_executable(input_map_test tests/input_map_test.c)
target_link_libraries(input_map_test pmd85session)
add_test(NAME input_map COMMAND input_map_test)

add_executable(hostport_test tests/hostport_test.c)
target_link_libraries(hostport_test pmd85session)
add_test(NAME hostport COMMAND hostport_test)

add_executable(dasm_test tests/dasm_test.c debugger/i8080dasm.c)
target_include_directories(dasm_test PRIVATE include)
add_test(NAME dasm COMMAND dasm_test "${GPMD85_DIR}")
set_tests_properties(dasm PROPERTIES SKIP_RETURN_CODE 77)

add_executable(tape_test tests/tape_test.c)
target_link_libraries(tape_test pmd85session)
target_include_directories(tape_test PRIVATE src)
add_test(NAME tape COMMAND tape_test)
set_tests_properties(tape PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke pmd85session)
target_include_directories(boot_smoke PRIVATE src)
add_test(NAME boot_smoke COMMAND boot_smoke)
set_tests_properties(boot_smoke PROPERTIES SKIP_RETURN_CODE 77)

add_executable(becker_test tests/becker_test.cpp)
target_link_libraries(becker_test pmd85session)
target_include_directories(becker_test PRIVATE src)
add_test(NAME becker COMMAND becker_test)
set_tests_properties(becker PROPERTIES SKIP_RETURN_CODE 77)

add_executable(session_test tests/session_test.c)
target_link_libraries(session_test pmd85session)
add_test(NAME session COMMAND session_test)
set_tests_properties(session PROPERTIES SKIP_RETURN_CODE 77)

add_executable(debug_test tests/debug_test.c)
target_link_libraries(debug_test pmd85session)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

add_executable(drivewire_smoke tests/drivewire_smoke.c)
target_link_libraries(drivewire_smoke pmd85session)
target_compile_definitions(drivewire_smoke PRIVATE
  PMD85_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out")
add_test(NAME drivewire_smoke COMMAND drivewire_smoke)
set_tests_properties(drivewire_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_executable(rebuild_test tests/rebuild_test.c)
target_link_libraries(rebuild_test pmd85session)
target_include_directories(rebuild_test PRIVATE src)
add_test(NAME rebuild COMMAND rebuild_test)
add_executable(fujinet_restart_test tests/fujinet_restart_test.c)
target_link_libraries(fujinet_restart_test pmd85session)
target_compile_definitions(fujinet_restart_test PRIVATE
  PMD85_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out")
add_test(NAME fujinet_restart COMMAND fujinet_restart_test)
set_tests_properties(fujinet_restart PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
add_executable(joystick_test tests/joystick_test.c)
target_link_libraries(joystick_test pmd85session)
target_include_directories(joystick_test PRIVATE src)
add_test(NAME joystick COMMAND joystick_test)
add_executable(pad_assign_test tests/pad_assign_test.c src/pad_assign.c)
target_include_directories(pad_assign_test PRIVATE src)
add_test(NAME pad_assign COMMAND pad_assign_test)
add_executable(gamepad_test tests/gamepad_test.c)
target_link_libraries(gamepad_test pmd85session)
add_test(NAME gamepad COMMAND gamepad_test)
set_tests_properties(gamepad PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_executable(mouse_test tests/mouse_test.c)
target_link_libraries(mouse_test pmd85session)
target_include_directories(mouse_test PRIVATE src)
add_test(NAME mouse COMMAND mouse_test)
add_executable(pmd32_test tests/pmd32_test.c)
target_link_libraries(pmd32_test pmd85session)
target_include_directories(pmd32_test PRIVATE src)
add_test(NAME pmd32 COMMAND pmd32_test)
set_tests_properties(pmd32 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
