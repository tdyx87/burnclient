enable_testing()

add_executable(test_burning_utility
    test_burning_utility.cpp
)

target_link_libraries(test_burning_utility PRIVATE BurningDLL Threads::Threads)

target_include_directories(test_burning_utility PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME burning_utility_tests
    COMMAND test_burning_utility
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_burning_utility>
)

set_tests_properties(burning_utility_tests PROPERTIES
    TIMEOUT 60
    LABELS "unit"
)
