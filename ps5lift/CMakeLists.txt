cmake_minimum_required(VERSION 3.22)
project(anyps5_lift LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build and register tests" ON)

find_package(LLVM REQUIRED CONFIG)
if(LLVM_VERSION_MAJOR LESS 18)
    message(FATAL_ERROR "LLVM 18 or newer is required, found ${LLVM_PACKAGE_VERSION}")
endif()
message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
if(APPLE)
    set(PS5LIFT_LIBCLANG ${LLVM_LIBRARY_DIR}/libclang.dylib CACHE INTERNAL "")
    if(NOT EXISTS ${PS5LIFT_LIBCLANG})
        message(FATAL_ERROR "The bridges parse AnyPS5's sources with the libclang of the LLVM ps5lift builds against, and ${LLVM_DIR} has none at ${PS5LIFT_LIBCLANG}")
    endif()
endif()

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/zydis ${CMAKE_CURRENT_BINARY_DIR}/zydis EXCLUDE_FROM_ALL)

set(ANYPS5_RELINKER ${CMAKE_CURRENT_SOURCE_DIR}/../core/relinker)
add_library(anyps5_elf STATIC
        ${ANYPS5_RELINKER}/relinker/src/parsing/ElfReader.cpp
        ${ANYPS5_RELINKER}/relinker/src/analysis/UnusedNidFilter/EhFrameReader.cpp)
target_include_directories(anyps5_elf PUBLIC
        ${ANYPS5_RELINKER}/relinker/include
        ${ANYPS5_RELINKER}/domain/include
        ${ANYPS5_RELINKER}/elfpatcher/include
        ${ANYPS5_RELINKER}/io/include)

add_subdirectory(lifter)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
