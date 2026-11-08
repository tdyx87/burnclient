cmake_minimum_required(VERSION 3.10)
project(BurningDLL VERSION 1.0.1 LANGUAGES C CXX)

set(PROJECT_NAME "BurningDLL")
set(PROJECT_DESCRIPTION "Cross-platform burning utility DLL for Visual Studio 2017")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The job engine runs work on std::thread; the unit test also uses threads.
find_package(Threads REQUIRED)

if(MSVC)
    message(STATUS "Configuring for Visual Studio")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /EHsc /permissive- /wd4100 /wd4189")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /INCREMENTAL:NO")
    set(CMAKE_WIN32_EXECUTABLE FALSE)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    set(CMAKE_DEBUG_POSTFIX "_d")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "Configuring for GCC/Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic -std=c++17")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
    endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Each platform .cpp is internally guarded by preprocessor checks, so all
# of them are compiled everywhere; exactly one provides CreatePlatformImpl().
set(SOURCES
    src/burning_utility.cpp
    src/windows_platform.cpp
    src/windows_features.cpp
    src/unix_platform.cpp
    src/unix_features.cpp
    src/mac_platform.cpp
    src/platform_defaults.cpp
    src/burn_internal.h
    src/media_format.h
    src/windows_features.h
    src/unix_features.h
)

set(HEADERS
    src/burning_utility.h
)

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "burning_dll"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${HEADERS}"
    CXX_VISIBILITY_PRESET "hidden"
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BUILDING_DLL _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${PROJECT_NAME} PRIVATE wbemuuid ole32 oleaut32 shlwapi)
    # IMAPI2 is part of Windows SDK, no extra lib needed.

    # The DLL is built with the dynamic CRT (/MD), so MSVCP140.dll,
    # VCRUNTIME140.dll and friends are real runtime dependencies. Bundle the
    # redistributable ones into bin/ so a clean machine can run the SDK.
    # UCRT (api-ms-win-crt-*.dll) and IMAPI2/COM are OS components and are
    # deliberately NOT redistributed.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin)
        message(STATUS "Bundling MSVC runtimes: ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
    endif()
elseif(APPLE)
    # libburn has no Darwin backend (no os-darwin.h upstream) and Homebrew
    # ships no libburn/libisoburn formula. mac_platform.cpp uses the native
    # hdiutil/drutil tools instead, so there are no third-party dependencies.
    message(STATUS "Configuring for macOS (native hdiutil/drutil backend)")
elseif(UNIX)
    # Linux / FreeBSD / Solaris: libburn + libisofs (libisoburn is not used).
    # __linux__ / __APPLE__ are compiler-predefined; do not redefine them.
    message(STATUS "Configuring for Unix (libburn/libisofs)")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBBURN REQUIRED libburn-1)
    pkg_check_modules(LIBISOFS REQUIRED libisofs-1)
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBBURN_INCLUDE_DIRS} ${LIBISOFS_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread ${LIBBURN_LIBRARIES} ${LIBISOFS_LIBRARIES})
endif()

# The installed shared library lives in lib/ while the CLI lives in bin/.
# Without an INSTALL_RPATH, CMake strips the build-tree RPATH on install and
# burn_cli can no longer find libburning_dll.so. $ORIGIN on the library also
# lets it find the bundled libburn/libisofs .so files in the same directory.
if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_RPATH "@loader_path")
    set(BURN_EXE_INSTALL_RPATH "@loader_path/../lib")
elseif(UNIX)
    set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_RPATH "$ORIGIN")
    set(BURN_EXE_INSTALL_RPATH "$ORIGIN/../lib")
endif()

install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include
)

# The public header is installed by the PUBLIC_HEADER rule above; do not
# install it a second time here.

# A short manifest is generated at configure time so that every downloadable
# package states its version and target platform.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/README.txt.in
    ${CMAKE_CURRENT_BINARY_DIR}/packaging/README.txt
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/packaging/README.txt DESTINATION .)

enable_testing()

# Example CLI tool
add_executable(burn_cli examples/burn_cli.cpp)
target_link_libraries(burn_cli PRIVATE ${PROJECT_NAME})
target_include_directories(burn_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(BURN_EXE_INSTALL_RPATH)
    set_target_properties(burn_cli PROPERTIES INSTALL_RPATH "${BURN_EXE_INSTALL_RPATH}")
endif()
if(MSVC)
    # The CLI uses the portable getenv spelling on purpose.
    target_compile_definitions(burn_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Build the plain-C samples too. This is a compile-time guard that the public
# header really is consumable from C (no unguarded C++ headers / types).
add_executable(c_example examples/c_example.c)
target_link_libraries(c_example PRIVATE ${PROJECT_NAME})
target_include_directories(c_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(job_example examples/job_example.c)
target_link_libraries(job_example PRIVATE ${PROJECT_NAME})
target_include_directories(job_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Local web console. It needs no GUI toolchain, so it builds everywhere the
# library builds, and it is driven by the automated test suite.
add_executable(burn_webui examples/burn_webui.cpp)
target_link_libraries(burn_webui PRIVATE ${PROJECT_NAME} Threads::Threads)
target_include_directories(burn_webui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(BURN_EXE_INSTALL_RPATH)
    set_target_properties(burn_webui PROPERTIES INSTALL_RPATH "${BURN_EXE_INSTALL_RPATH}")
endif()
# The page is written in Chinese; MSVC must read the file as UTF-8 instead of
# the system code page, or C4819 warns and the literals can be mangled.
if(MSVC)
    target_compile_options(burn_webui PRIVATE /utf-8)
endif()

# The tests come last: the console test drives the burn_webui binary, so
# that target must already exist when the tests directory is processed.
add_subdirectory(tests)

# These are part of the downloadable SDK: the CLI lands in bin/ next to the
# shared library, and the samples show how to consume the C/C++/C# APIs.
install(TARGETS burn_cli burn_webui RUNTIME DESTINATION bin)
install(FILES
    examples/c_example.c
    examples/cpp_example.cpp
    examples/csharp_example.cs
    examples/job_example.c
    DESTINATION examples
)

if(MSVC)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
endif()

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

# The diagnostic job needs no optical drive, so it is safe to run in CI.
# It exercises Start/GetStatus/Cancel/Wait/Release from plain C.
add_test(NAME job_example_selftest COMMAND job_example)
set_tests_properties(job_example_selftest PROPERTIES TIMEOUT 30 LABELS "job")
add_test(NAME job_example_cancel COMMAND job_example cancel)
set_tests_properties(job_example_cancel PROPERTIES TIMEOUT 30 LABELS "job")

# Hardware integration test: burns a generated ISO to the first writable
# recorder. Self-skips (exit 0) when no writable burner exists, so CI stays
# green; on machines with a real or virtual burner it runs a full
# create-image + async-burn + status-machine pass against the hardware.
add_test(NAME burn_integration COMMAND burn_cli burn-test)
set_tests_properties(burn_integration PROPERTIES TIMEOUT 300 LABELS "integration")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Generator: ${CMAKE_GENERATOR}")
message(STATUS "========================================")

# Runs LAST so every installed file is already on disk. On Linux/macOS it
# resolves and bundles third-party .so/.dylib files into lib/; on every
# platform it writes DEPENDENCIES.txt describing what is (and is not) shipped.
# Windows CRT DLLs are installed above by InstallRequiredSystemLibraries.
install(CODE "set(BUNDLE_SYSTEM \"${CMAKE_SYSTEM_NAME}\")")
install(SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/packaging/bundle_runtime_deps.cmake)
