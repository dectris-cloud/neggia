# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.6)
project(neggia VERSION 1.3.1)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
  set(BUILD_TESTING OFF CACHE BOOL "")
else()
  set(BUILD_TESTING ON CACHE BOOL "")
endif()
include(CTest)

set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(DEBUG_PARSING "Print debug information during parsing of HDF5" OFF)
if(DEBUG_PARSING)
  add_definitions(-DDEBUG_PARSING)
endif()

add_subdirectory(third_party)

include_directories(src)

add_subdirectory(bin)
add_subdirectory(src)
