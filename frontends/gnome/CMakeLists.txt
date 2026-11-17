# GNOME frontend: GTK4 + libadwaita (the FujiNet web UI opens in the system
# browser). Found-or-skipped so lean machines can still build the core / KDE
# side.

find_package(PkgConfig REQUIRED)
# GTK 4.16 for GdkMemoryTextureBuilder (the display reuses its GPU texture
# frame to frame).
pkg_check_modules(ADW IMPORTED_TARGET libadwaita-1>=1.5 gtk4>=4.16)

if(NOT ADW_FOUND)
  message(STATUS "GNOME frontend: libadwaita/gtk4 not found -- skipping")
  return()
endif()

set(GNOME_APP_ID online.fujinet.go.pmd85.gnome)

add_executable(fujinet-go-pmd85-gnome
  main.c
  window.c
  display.c
  prefs.c
  fujilog.c
  debugger/dbg_window.c
)
target_link_libraries(fujinet-go-pmd85-gnome PRIVATE pmd85session PkgConfig::ADW)
target_include_directories(fujinet-go-pmd85-gnome PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(fujinet-go-pmd85-gnome PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-pmd85-gnome PRIVATE
  PMD85_APP_ID="${GNOME_APP_ID}"
  # Uninstalled runs fall back to the in-tree icon theme (pmd85_icon_name).
  PMD85_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

install(TARGETS fujinet-go-pmd85-gnome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${GNOME_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${GNOME_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Catch a malformed launcher at build time rather than after installing.
find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_gnome
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${GNOME_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_gnome
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${GNOME_APP_ID}.metainfo.xml")
endif()
