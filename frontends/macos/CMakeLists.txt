# macOS frontend: native AppKit (Objective-C) over the shared session core.
# Built only on Apple hosts; verified by the macOS CI job.
#
# From fujinet-go-coco-desktop's frontends/macos.

if(NOT APPLE)
  return()
endif()

enable_language(OBJC)

# Same rendered art as the other frontends (tools/icons/make-icons.py), packed
# as .icns. Listing it as a source with a MACOSX_PACKAGE_LOCATION is what makes
# CMake copy it into Contents/Resources.
set(MACOS_ICON "${CMAKE_SOURCE_DIR}/data/icons/fujinet-go-pmd85.icns")
set_source_files_properties(${MACOS_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

add_executable(fujinet-go-pmd85-macos MACOSX_BUNDLE
  main.m
  AppDelegate.m
  DebuggerWindow.m
  DisplayView.m
  KeyForward.m
  SettingsWindow.m
  TapeWindow.m
  ${MACOS_ICON}
)
set_target_properties(fujinet-go-pmd85-macos PROPERTIES
  OUTPUT_NAME "FujiNet Go PMD 85"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_ICON_FILE "fujinet-go-pmd85.icns"
)
target_link_libraries(fujinet-go-pmd85-macos PRIVATE
  pmd85session
  "-framework Cocoa"
  "-framework CoreVideo"
  "-framework QuartzCore"
  "-framework Metal"        # the picture is a texture on a CAMetalLayer
  "-framework UniformTypeIdentifiers"
)
target_include_directories(fujinet-go-pmd85-macos PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(fujinet-go-pmd85-macos PRIVATE -Wall -Wextra -fobjc-arc)

# Pack the FujiNet runtime into the bundle: the dylib into Contents/Frameworks,
# the pristine runtime tree into Contents/Resources/fujinet (main.m hands both
# paths to the session). The runtime is built by this same build
# (cmake/FujiNetRuntime.cmake), so wait for it.
if(TARGET fujinet-runtime)
  add_dependencies(fujinet-go-pmd85-macos fujinet-runtime)
endif()
if(TARGET fujinet-runtime OR EXISTS "${FUJINET_OUT}/libfujinet.dylib")
  add_custom_command(TARGET fujinet-go-pmd85-macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-pmd85-macos>/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/libfujinet.dylib"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-pmd85-macos>/Frameworks/"
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/fnconfig.ini"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-pmd85-macos>/Resources/fujinet/fnconfig.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/data"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-pmd85-macos>/Resources/fujinet/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/SD"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-pmd85-macos>/Resources/fujinet/SD"
    COMMENT "Bundling the FujiNet runtime into the app"
  )
else()
  message(STATUS "macOS frontend: FujiNet runtime disabled; the app will run without FujiNet")
endif()
