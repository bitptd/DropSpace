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
  find_package(Qt6 6.8 REQUIRED COMPONENTS Core Concurrent Gui Widgets Network Sql WebSockets ZlibPrivate)
  qt_standard_project_setup()

  qt_add_executable(dropspace_client WIN32
    src/main.cpp
    src/core/FolderPacker.cpp
    src/core/FolderPacker.h
    src/core/ApiClient.cpp
    src/core/ApiClient.h
    src/core/ClipboardController.cpp
    src/core/ClipboardController.h
    src/core/SafeExtractor.cpp
    src/core/SafeExtractor.h
    src/storage/ClientDatabase.cpp
    src/storage/ClientDatabase.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ClipboardHistoryDialog.cpp
    src/ui/ClipboardHistoryDialog.h
  )
  qt_add_resources(dropspace_client dropspace_assets
    PREFIX "/"
    FILES resources/icon.jpg
  )
  set_target_properties(dropspace_client PROPERTIES OUTPUT_NAME "DropSpace")
  target_link_libraries(dropspace_client PRIVATE
    dropspace_core
    Qt6::Core Qt6::CorePrivate Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Sql Qt6::WebSockets
    Qt6::ZlibPrivate
  )

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
      Qt6::Core Qt6::CorePrivate Qt6::ZlibPrivate)
    add_test(NAME safe_extractor_tests COMMAND safe_extractor_tests)

    if(WIN32)
      add_executable(windows_clipboard_integration
        tests/windows_clipboard_integration.cpp
        src/platform/windows/WindowsClipboard.cpp
        src/platform/windows/WindowsClipboard.h
      )
      target_include_directories(windows_clipboard_integration PRIVATE src)
      target_compile_definitions(windows_clipboard_integration PRIVATE UNICODE _UNICODE NOMINMAX)
      target_link_libraries(windows_clipboard_integration PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets ole32)
      add_test(NAME windows_clipboard_integration COMMAND windows_clipboard_integration)

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
      src/platform/windows/WindowsClipboard.cpp
      src/platform/windows/WindowsClipboard.h
    )
    target_compile_definitions(dropspace_client PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(dropspace_client PRIVATE advapi32 ole32 shell32 uuid)
  endif()

  install(TARGETS dropspace_client RUNTIME DESTINATION .)
endif()

