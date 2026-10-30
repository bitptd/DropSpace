add_library(dropspace_core STATIC
  src/core/CachePlanner.cpp
)
target_include_directories(dropspace_core PUBLIC src)

if(DROPSPACE_BUILD_TESTS)
  add_executable(cache_planner_tests tests/cache_planner_tests.cpp)
  target_link_libraries(cache_planner_tests PRIVATE dropspace_core)
  add_test(NAME cache_planner_tests COMMAND cache_planner_tests)

  if(WIN32)
    add_executable(windows_virtual_file_tests
      tests/windows_virtual_file_tests.cpp
      src/platform/windows/VirtualFileDrag.cpp
      src/platform/windows/VirtualFileDrag.h
    )
    target_include_directories(windows_virtual_file_tests PRIVATE src)
    target_compile_definitions(windows_virtual_file_tests PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(windows_virtual_file_tests PRIVATE ole32 shell32 uuid)
    add_test(NAME windows_virtual_file_tests COMMAND windows_virtual_file_tests)
  endif()
endif()

if(DROPSPACE_BUILD_CLIENT)
  if(APPLE)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core CorePrivate Concurrent Gui Widgets Network Sql)
    find_package(ZLIB REQUIRED)
  else()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Concurrent Gui Widgets Network Sql ZlibPrivate)
  endif()
  qt_standard_project_setup()

  qt_add_executable(dropspace_client WIN32
    src/main.cpp
    src/core/FolderPacker.cpp
    src/core/FolderPacker.h
    src/core/ApiClient.cpp
    src/core/ApiClient.h
    src/core/SafeExtractor.cpp
    src/core/SafeExtractor.h
    src/storage/ClientDatabase.cpp
    src/storage/ClientDatabase.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
  )
  qt_add_resources(dropspace_client dropspace_assets
    PREFIX "/"
    FILES resources/icon.jpg
  )
  set_target_properties(dropspace_client PROPERTIES OUTPUT_NAME "DropSpace")
  target_link_libraries(dropspace_client PRIVATE
    dropspace_core
    Qt6::Core Qt6::CorePrivate Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Sql
  )
  if(APPLE)
    target_link_libraries(dropspace_client PRIVATE ZLIB::ZLIB)
  else()
    target_link_libraries(dropspace_client PRIVATE Qt6::ZlibPrivate)
  endif()

  if(DROPSPACE_BUILD_TESTS)
    add_executable(folder_packer_tests
      tests/folder_packer_tests.cpp
      src/core/FolderPacker.cpp
      src/core/FolderPacker.h
    )
    target_include_directories(folder_packer_tests PRIVATE src)
    target_link_libraries(folder_packer_tests PRIVATE Qt6::Core Qt6::CorePrivate)
    add_test(NAME folder_packer_tests COMMAND folder_packer_tests)

    add_executable(safe_extractor_tests
      tests/safe_extractor_tests.cpp
      src/core/FolderPacker.cpp src/core/FolderPacker.h
      src/core/SafeExtractor.cpp src/core/SafeExtractor.h
    )
    target_include_directories(safe_extractor_tests PRIVATE src)
    target_link_libraries(safe_extractor_tests PRIVATE
      Qt6::Core Qt6::CorePrivate)
    if(APPLE)
      target_link_libraries(safe_extractor_tests PRIVATE ZLIB::ZLIB)
    else()
      target_link_libraries(safe_extractor_tests PRIVATE Qt6::ZlibPrivate)
    endif()
    add_test(NAME safe_extractor_tests COMMAND safe_extractor_tests)

    if(WIN32)
      add_executable(api_client_integration
        tests/api_client_integration.cpp
        src/core/ApiClient.cpp src/core/ApiClient.h
        src/core/FolderPacker.cpp src/core/FolderPacker.h
        src/core/SafeExtractor.cpp src/core/SafeExtractor.h
        src/storage/ClientDatabase.cpp src/storage/ClientDatabase.h
        src/platform/windows/CredentialStore.cpp src/platform/windows/CredentialStore.h
      )
      target_include_directories(api_client_integration PRIVATE src)
      target_compile_definitions(api_client_integration PRIVATE UNICODE _UNICODE NOMINMAX)
      target_link_libraries(api_client_integration PRIVATE
        dropspace_core Qt6::Core Qt6::CorePrivate Qt6::Concurrent Qt6::Network Qt6::Sql
        Qt6::ZlibPrivate advapi32)
    endif()
  endif()

  if(WIN32)
    set_property(SOURCE resources/dropspace.rc APPEND PROPERTY
      OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
    target_sources(dropspace_client PRIVATE
      resources/dropspace.rc
      src/platform/windows/VirtualFileDrag.cpp
      src/platform/windows/VirtualFileDrag.h
      src/platform/windows/CredentialStore.cpp
      src/platform/windows/CredentialStore.h
    )
    target_compile_definitions(dropspace_client PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(dropspace_client PRIVATE advapi32 ole32 shell32 uuid)
  endif()

  if(APPLE)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    target_sources(dropspace_client PRIVATE
      src/platform/macos/CredentialStore.h
      src/platform/macos/CredentialStore.mm
      src/platform/macos/FilePromiseDrag.h
      src/platform/macos/FilePromiseDrag.mm
    )
    set(MAC_ICONSET "${CMAKE_CURRENT_BINARY_DIR}/DropSpace.iconset")
    set(MAC_ICNS "${CMAKE_CURRENT_BINARY_DIR}/DropSpace.icns")
    set(MAC_ICON_MASTER "${CMAKE_CURRENT_BINARY_DIR}/DropSpace-icon-master.png")
    add_custom_command(
      OUTPUT "${MAC_ICNS}"
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${MAC_ICONSET}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${MAC_ICONSET}"
      COMMAND /usr/bin/sips -s format png "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg"
              --out "${MAC_ICON_MASTER}"
      COMMAND /usr/bin/sips -z 16 16 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_16x16.png"
      COMMAND /usr/bin/sips -z 32 32 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_16x16@2x.png"
      COMMAND /usr/bin/sips -z 32 32 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_32x32.png"
      COMMAND /usr/bin/sips -z 64 64 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_32x32@2x.png"
      COMMAND /usr/bin/sips -z 128 128 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_128x128.png"
      COMMAND /usr/bin/sips -z 256 256 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_128x128@2x.png"
      COMMAND /usr/bin/sips -z 256 256 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_256x256.png"
      COMMAND /usr/bin/sips -z 512 512 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_256x256@2x.png"
      COMMAND /usr/bin/sips -z 512 512 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_512x512.png"
      COMMAND /usr/bin/sips -z 1024 1024 "${MAC_ICON_MASTER}"
              --out "${MAC_ICONSET}/icon_512x512@2x.png"
      COMMAND /usr/bin/iconutil -c icns "${MAC_ICONSET}" -o "${MAC_ICNS}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg"
      VERBATIM)
    set_source_files_properties("${MAC_ICNS}" PROPERTIES
      GENERATED TRUE
      MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(dropspace_client PRIVATE "${MAC_ICNS}")
    set_source_files_properties(
      src/platform/macos/CredentialStore.mm
      src/platform/macos/FilePromiseDrag.mm
      PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
    )
    target_link_libraries(dropspace_client PRIVATE
      ${APPKIT_FRAMEWORK}
      ${FOUNDATION_FRAMEWORK}
      ${SECURITY_FRAMEWORK}
      ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}
    )
    set_target_properties(dropspace_client PROPERTIES
      MACOSX_BUNDLE TRUE
      MACOSX_BUNDLE_BUNDLE_NAME "DropSpace"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.dropspace.client"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.5"
      MACOSX_BUNDLE_BUNDLE_VERSION "1"
      MACOSX_BUNDLE_ICON_FILE "DropSpace.icns"
    )
  endif()

  install(TARGETS dropspace_client
    RUNTIME DESTINATION .
    BUNDLE DESTINATION .)
endif()

