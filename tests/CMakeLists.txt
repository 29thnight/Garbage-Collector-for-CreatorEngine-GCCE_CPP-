# GoogleTest: an installed package is used when available, otherwise it is
# fetched. Set FETCHCONTENT_SOURCE_DIR_GOOGLETEST to build from a local copy.
find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(gcce_tests
    refs_test.cpp
    weak_test.cpp
    collection_test.cpp
    containers_test.cpp
    construction_test.cpp
    lifecycle_test.cpp
    incremental_test.cpp
    pacing_test.cpp
    diagnostics_test.cpp
    threading_test.cpp
    shutdown_test.cpp
    scenarios_test.cpp
    stress_test.cpp
)
target_link_libraries(gcce_tests PRIVATE gcce::gcce GTest::gtest_main)

if(MSVC)
    target_compile_options(gcce_tests PRIVATE /W4 /permissive- /bigobj)
else()
    target_compile_options(gcce_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GoogleTest)
gtest_discover_tests(gcce_tests DISCOVERY_MODE PRE_TEST)
