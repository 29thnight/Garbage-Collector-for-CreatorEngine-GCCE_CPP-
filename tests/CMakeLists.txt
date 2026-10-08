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
    managed_test.cpp
    allocator_test.cpp
)
target_link_libraries(gcce_tests PRIVATE gcce::gcce GTest::gtest_main)

if(MSVC)
    target_compile_options(gcce_tests PRIVATE /W4 /permissive- /bigobj)
else()
    target_compile_options(gcce_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GoogleTest)
gtest_discover_tests(gcce_tests DISCOVERY_MODE PRE_TEST)

# Compile-fail tests: each case must fail to build, and its control build
# (the same file without the offending line) must succeed.
set(GCCE_COMPILE_FAIL_CASES
    new_expression array_new nothrow_new placement_new
    delete_expression unique_ptr make_unmanaged make_twice_managed)
foreach(case IN LISTS GCCE_COMPILE_FAIL_CASES)
    add_library(compile_fail_${case} OBJECT EXCLUDE_FROM_ALL compile_fail/${case}.cpp)
    target_link_libraries(compile_fail_${case} PRIVATE gcce::gcce)
    add_test(NAME CompileFail.${case}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target compile_fail_${case} --config $<CONFIG>)
    set_tests_properties(CompileFail.${case} PROPERTIES WILL_FAIL TRUE RESOURCE_LOCK gcce_build_tree)

    add_library(compile_ok_${case} OBJECT EXCLUDE_FROM_ALL compile_fail/${case}.cpp)
    target_link_libraries(compile_ok_${case} PRIVATE gcce::gcce)
    target_compile_definitions(compile_ok_${case} PRIVATE GCCE_COMPILE_FAIL_CONTROL)
    add_test(NAME CompileFail.${case}.control
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target compile_ok_${case} --config $<CONFIG>)
    set_tests_properties(CompileFail.${case}.control PROPERTIES RESOURCE_LOCK gcce_build_tree)
endforeach()
