cmake_minimum_required(VERSION 3.20)
project(gcce VERSION 0.1.0 LANGUAGES CXX)

option(GCCE_BUILD_SHARED "Build the GC runtime as a shared library (DLL)" OFF)
option(GCCE_BUILD_TESTS "Build the test suite" ON)
option(GCCE_THREAD_CHECKS "Report registry and graph changes off the owner thread" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep executables next to the runtime DLL on Windows.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(GCCE_BUILD_SHARED)
    add_library(gcce SHARED src/domain.cpp)
    target_compile_definitions(gcce PUBLIC GCCE_SHARED PRIVATE GCCE_BUILDING)
    set_target_properties(gcce PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
else()
    add_library(gcce STATIC src/domain.cpp)
endif()
add_library(gcce::gcce ALIAS gcce)

target_include_directories(gcce PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(GCCE_THREAD_CHECKS)
    target_compile_definitions(gcce PUBLIC GC_THREAD_CHECKS=1)
else()
    target_compile_definitions(gcce PUBLIC GC_THREAD_CHECKS=0)
endif()

if(MSVC)
    target_compile_options(gcce PRIVATE /W4 /permissive-)
else()
    target_compile_options(gcce PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(GCCE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
