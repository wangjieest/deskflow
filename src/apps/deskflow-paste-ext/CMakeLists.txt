# SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
# SPDX-License-Identifier: MIT

# Finder Sync Extension for macOS deferred paste
# Builds as a .appex bundle embedded in the main app's PlugIns/

set(EXT_TARGET DeskflowPaste)

add_library(${EXT_TARGET} MODULE
  FinderSync.h
  FinderSync.m
)

set_target_properties(${EXT_TARGET} PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION appex
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.autodeskflow.autodeskflow.paste-ext"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

target_link_libraries(${EXT_TARGET} PRIVATE
  "-framework Foundation"
  "-framework Cocoa"
  "-framework FinderSync"
)

# Sign with entitlements (required for sandbox + FinderSync registration)
if(APPLE_CODESIGN_DEV)
  add_custom_command(TARGET ${EXT_TARGET} POST_BUILD
    COMMAND codesign --force --sign "${APPLE_CODESIGN_DEV}"
      --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/DeskflowPaste.entitlements"
      "$<TARGET_BUNDLE_DIR:${EXT_TARGET}>"
    COMMENT "Signing ${EXT_TARGET}.appex with entitlements"
  )
endif()

# Embed the .appex into the main app bundle's PlugIns directory
set(PLUGINS_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/AutoDeskflow.app/Contents/PlugIns")

add_custom_command(TARGET ${EXT_TARGET} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGINS_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "$<TARGET_BUNDLE_DIR:${EXT_TARGET}>"
    "${PLUGINS_DIR}/${EXT_TARGET}.appex"
  COMMENT "Embedding ${EXT_TARGET}.appex into AutoDeskflow.app/Contents/PlugIns/"
)

# Install the extension inside the app bundle
if(BUILD_OSX_BUNDLE)
  install(
    DIRECTORY "$<TARGET_BUNDLE_DIR:${EXT_TARGET}>/"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/../PlugIns/${EXT_TARGET}.appex"
  )
endif()
