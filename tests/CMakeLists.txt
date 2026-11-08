enable_testing()

add_executable(test_burning_utility
    test_burning_utility.cpp
)

target_link_libraries(test_burning_utility PRIVATE BurningDLL Threads::Threads)

target_include_directories(test_burning_utility PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    # The tests use the portable fopen/remove spelling on purpose.
    target_compile_definitions(test_burning_utility PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

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

# The ABI check links against the library exactly like an external caller
# would and checks that the exported surface matches the public header.
add_executable(test_abi
    test_abi.cpp
)

target_link_libraries(test_abi PRIVATE BurningDLL Threads::Threads)

target_include_directories(test_abi PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_definitions(test_abi PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_test(NAME abi_consistency
    COMMAND test_abi
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_abi>
)

set_tests_properties(abi_consistency PROPERTIES
    TIMEOUT 60
    LABELS "unit;abi"
)

# The web console is exercised over real HTTP against a running server, so
# it is registered as a test and the server binary is passed to it.
add_executable(test_webui
    test_webui.cpp
)

target_link_libraries(test_webui PRIVATE BurningDLL Threads::Threads)

target_include_directories(test_webui PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    # The tests use the portable fopen/remove spelling on purpose.
    target_compile_definitions(test_webui PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(test_v3_capabilities PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(hw_check PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(TARGET burn_webui)
    add_dependencies(test_webui burn_webui)
    add_test(NAME webui_console
        COMMAND test_webui $<TARGET_FILE:burn_webui>
        WORKING_DIRECTORY $<TARGET_FILE_DIR:test_webui>
    )
    set_tests_properties(webui_console PROPERTIES
        TIMEOUT 180
        LABELS "unit;ui"
    )
endif()
