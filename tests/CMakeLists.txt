add_executable(gcce_tests
    test_main.cpp
    test_basic.cpp
    test_containers.cpp
    test_weak.cpp
    test_construction.cpp
    test_lifecycle.cpp
    test_random.cpp
    test_incremental.cpp
    test_threads_shutdown.cpp
)
target_link_libraries(gcce_tests PRIVATE gcce::gcce)

if(MSVC)
    target_compile_options(gcce_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(gcce_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME gcce_tests COMMAND gcce_tests)
