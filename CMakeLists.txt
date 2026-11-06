cmake_minimum_required(VERSION 3.10)
project(BurningDLL VERSION 1.0.0 LANGUAGES CXX)

set(PROJECT_NAME "BurningDLL")
set(PROJECT_DESCRIPTION "Cross-platform burning utility DLL for Visual Studio 2017")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/unix_platform.cpp
    src/mac_platform.cpp
)

set(HEADERS
    src/burning_utility.h
)

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

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
    # IMAPI2 is part of Windows SDK, no extra lib needed
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
add_subdirectory(tests)

# Example CLI tool
add_executable(burn_cli examples/burn_cli.cpp)
target_link_libraries(burn_cli PRIVATE ${PROJECT_NAME})
target_include_directories(burn_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# These are part of the downloadable SDK: the CLI lands in bin/ next to the
# shared library, and the samples show how to consume the C/C++/C# APIs.
install(TARGETS burn_cli RUNTIME DESTINATION bin)
install(FILES
    examples/c_example.c
    examples/cpp_example.cpp
    examples/csharp_example.cs
    DESTINATION examples
)

if(MSVC)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
endif()

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

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
