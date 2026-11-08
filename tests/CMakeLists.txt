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

# Version 3 capability tests: the drive-free parts (RAW PQ layout, UDF
# anchor, El Torito catalog) are verified at byte level, and the refusal
# contract for unsupported backends is asserted explicitly.
add_executable(test_v3_capabilities
    test_v3_capabilities.cpp
)

target_link_libraries(test_v3_capabilities PRIVATE BurningDLL Threads::Threads)

target_include_directories(test_v3_capabilities PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME v3_capability_tests
    COMMAND test_v3_capabilities
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_v3_capabilities>
)

set_tests_properties(v3_capability_tests PROPERTIES
    TIMEOUT 180
    LABELS "unit;v3"
)

# Hardware checklist driver: built but deliberately NOT registered with CTest,
# because it needs a real recorder and writable media. Run it by hand on the
# target machine; see docs/HARDWARE_TEST_CHECKLIST.md.
add_executable(hw_check
    test_hardware_checklist.cpp
)

target_link_libraries(hw_check PRIVATE BurningDLL Threads::Threads)

target_include_directories(hw_check PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
