cmake_minimum_required(VERSION 3.24)

project(DropSpace VERSION 0.1.0 LANGUAGES CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DROPSPACE_BUILD_CLIENT "Build the Qt desktop client" ON)
option(DROPSPACE_BUILD_TESTS "Build native unit tests" ON)

if(NOT WIN32 AND NOT APPLE)
  message(FATAL_ERROR "DropSpace phase one supports Windows and macOS clients only")
endif()

enable_testing()
add_subdirectory(client)

