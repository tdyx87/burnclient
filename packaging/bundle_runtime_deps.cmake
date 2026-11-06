# packaging/bundle_runtime_deps.cmake
#
# Executed at `cmake --install` time via install(SCRIPT ...). It makes every
# SDK package self-contained: every non-system shared library that the
# installed BurningDLL needs is copied into lib/ next to it, patched with a
# relative RPATH, and recorded in DEPENDENCIES.txt.
#
# Only Linux/macOS run this script. On Windows the redistributable MSVC CRT
# DLLs are installed by InstallRequiredSystemLibraries from CMakeLists.txt.
#
# Inputs supplied by the parent install script:
#   CMAKE_INSTALL_PREFIX   honours `cmake --install --prefix ...`
#   BUNDLE_SYSTEM          "Linux" or "Darwin" (set via install(CODE ...))
#
# Design notes:
#   * glibc, the loader, the compiler runtime and OS frameworks are NEVER
#     bundled. Mixing a foreign libc/libstdc++ with the host loader breaks in
#     subtle ways, and they are guaranteed present on supported targets.
#   * Everything else is bundled, including transitive dependencies, so the
#     package works on a machine with no libburn/libisofs installed.
#   * RUNPATH is not inherited by grandchildren in the ELF dependency tree,
#     so every bundled .so gets its own $ORIGIN RPATH via patchelf.

if(NOT DEFINED BUNDLE_SYSTEM)
    message(WARNING "[bundle-deps] BUNDLE_SYSTEM is not set; skipping")
    return()
endif()

set(_prefix "${CMAKE_INSTALL_PREFIX}")
set(_libdir "${_prefix}/lib")

# Windows: the redistributable MSVC CRT DLLs were already installed into bin/
# by InstallRequiredSystemLibraries. There is no ELF/Mach-O dependency walk
# to do here, but the package still gets an accurate manifest.
if(BUNDLE_SYSTEM STREQUAL "Windows")
    set(_manifest
        "Bundled runtime dependencies\n============================\nPlatform : Windows\nPrefix   : ${_prefix}\n\n")
    string(APPEND _manifest
        "These redistributable Visual C++ runtime DLLs are shipped in bin/\n"
        "next to burning_dll.dll, so the SDK runs on a clean machine:\n\n")
    file(GLOB _dlls LIST_DIRECTORIES false "${_prefix}/bin/*.dll")
    foreach(_dll IN LISTS _dlls)
        get_filename_component(_name "${_dll}" NAME)
        if(_name STREQUAL "burning_dll.dll")
            continue()
        endif()
        string(APPEND _manifest "  ${_name}\n")
    endforeach()
    string(APPEND _manifest
        "\nIntentionally NOT bundled (Windows OS components):\n"
        "  IMAPI2 / IMAPI2FS (COM), OLE32, OLEAUT32, SHLWAPI, KERNEL32\n"
        "  Universal CRT: api-ms-win-crt-*.dll, ucrtbase.dll\n\n"
        "Keep the whole bin/ directory together when redistributing.\n")
    file(WRITE "${_prefix}/DEPENDENCIES.txt" "${_manifest}")
    message(STATUS "[bundle-deps] wrote ${_prefix}/DEPENDENCIES.txt")
    return()
endif()

# Basenames matching any of these stay on the host system.
set(_system_patterns
    "^ld-linux" "^linux-vdso" "^libc\\.so" "^libm\\.so" "^libdl\\.so"
    "^librt\\.so" "^libpthread" "^libgcc_s" "^libstdc\\+\\+"
    "^libnsl" "^libutil" "^libresolv" "^libanl" "^libcrypt"
    "^libgomp" "^libatomic" "^libquadmath" "^libasan" "^libubsan"
    "^libtsan" "^liblsan" "^libburning_dll"
    "^libSystem" "^libc\\+\\+" "^libobjc" "^libdyld" "^libunwind")

# Find the shared library that install(TARGETS) just placed in lib/.
file(GLOB _candidates LIST_DIRECTORIES false
     "${_libdir}/libburning_dll.so.*"
     "${_libdir}/libburning_dll*.dylib")
list(LENGTH _candidates _candidate_count)
if(_candidate_count EQUAL 0)
    message(WARNING
        "[bundle-deps] no installed shared library found in ${_libdir}; "
        "the SDK will NOT be self-contained")
    return()
endif()
list(GET _candidates 0 _seed)

set(_taken "")    # basenames already copied
set(_skipped "")  # system basenames intentionally left on the host
set(_report "")   # manifest lines
set(_queue "${_seed}")

# Fixed-point traversal: each newly bundled library is scanned in turn so
# transitive dependencies are picked up too. 12 rounds is far beyond any
# real dependency graph and prevents a pathological infinite loop.
set(_round 0)
while(_queue AND _round LESS 12)
    math(EXPR _round "${_round} + 1")
    set(_next "")
    foreach(_target IN LISTS _queue)
        set(_raw "")
        if(BUNDLE_SYSTEM STREQUAL "Darwin")
            execute_process(COMMAND otool -L "${_target}"
                            OUTPUT_VARIABLE _raw ERROR_QUIET)
        else()
            execute_process(COMMAND ldd "${_target}"
                            OUTPUT_VARIABLE _raw ERROR_QUIET)
        endif()
        string(REPLACE "\n" ";" _lines "${_raw}")

        foreach(_line IN LISTS _lines)
            set(_dep "")
            if(BUNDLE_SYSTEM STREQUAL "Darwin")
                # otool: "\t/path/libfoo.dylib (compatibility version ...)"
                if(_line MATCHES "^[ \t]*([^ \t]+\\.dylib)[ \t]*\\(")
                    set(_dep "${CMAKE_MATCH_1}")
                endif()
                # System frameworks / already-relative install names.
                if(_dep MATCHES "^@|^/usr/lib/|^/System/")
                    set(_dep "")
                endif()
            else()
                # ldd: "\tlibfoo.so.1 => /lib/x86_64-linux-gnu/libfoo.so.1 (0x..)"
                # "=> not found" is deliberately ignored (no absolute path).
                if(_line MATCHES "=>[ \t]+(/[^ \t]+)")
                    set(_dep "${CMAKE_MATCH_1}")
                endif()
            endif()
            if(_dep STREQUAL "")
                continue()
            endif()

            get_filename_component(_name "${_dep}" NAME)
            set(_is_system FALSE)
            foreach(_pattern IN LISTS _system_patterns)
                if(_name MATCHES "${_pattern}")
                    set(_is_system TRUE)
                    break()
                endif()
            endforeach()
            if(_is_system)
                list(APPEND _skipped "${_name}")
                continue()
            endif()

            list(FIND _taken "${_name}" _seen)
            if(NOT _seen EQUAL -1)
                continue()
            endif()

            # ldd reports a SONAME symlink (e.g. libburn.so.4). Resolve it to
            # the real file, then copy those bytes under the exact name the
            # loader will ask for. This avoids dangling symlinks in the SDK.
            get_filename_component(_real "${_dep}" REALPATH)
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${_real}" "${_libdir}/${_name}"
                RESULT_VARIABLE _copy_result ERROR_QUIET)
            if(NOT _copy_result EQUAL 0)
                message(WARNING "[bundle-deps] could not copy ${_dep}")
                continue()
            endif()

            list(APPEND _taken "${_name}")
            list(APPEND _next "${_libdir}/${_name}")
            string(APPEND _report "  ${_name}   <- ${_dep}\n")
            message(STATUS "[bundle-deps] bundled ${_name} <- ${_dep}")
        endforeach()
    endforeach()
    set(_queue "${_next}")
endwhile()

# Give each bundled library its own relative RPATH. This is essential:
# DT_RUNPATH applies only to an object's direct DT_NEEDED entries, so a
# bundled libburn.so would not find a bundled libjte.so without this.
if(_taken)
    if(BUNDLE_SYSTEM STREQUAL "Linux")
        find_program(_patchelf NAMES patchelf)
        if(_patchelf)
            foreach(_name IN LISTS _taken)
                execute_process(
                    COMMAND "${_patchelf}" --set-rpath "$ORIGIN"
                            "${_libdir}/${_name}"
                    RESULT_VARIABLE _patch_result ERROR_QUIET)
                if(NOT _patch_result EQUAL 0)
                    message(WARNING
                        "[bundle-deps] patchelf failed for ${_name}; "
                        "transitive deps may need LD_LIBRARY_PATH")
                endif()
            endforeach()
            message(STATUS "[bundle-deps] set \$ORIGIN RPATH on bundled libs")
        else()
            message(WARNING
                "[bundle-deps] patchelf not found; bundled transitive "
                "libraries may need LD_LIBRARY_PATH on the target")
        endif()
    elseif(BUNDLE_SYSTEM STREQUAL "Darwin")
        find_program(_install_name_tool NAMES install_name_tool)
        if(_install_name_tool)
            foreach(_name IN LISTS _taken)
                execute_process(
                    COMMAND "${_install_name_tool}" -add_rpath "@loader_path"
                            "${_libdir}/${_name}"
                    RESULT_VARIABLE _patch_result ERROR_QUIET)
            endforeach()
        endif()
    endif()
endif()

list(REMOVE_DUPLICATES _skipped)
list(SORT _skipped)

set(_manifest
    "Bundled runtime dependencies\n============================\nPlatform : ${BUNDLE_SYSTEM}\nPrefix   : ${_prefix}\n\n")
if(_taken)
    string(APPEND _manifest
        "These shared libraries are shipped in lib/ next to BurningDLL and\n"
        "are resolved through a relative RPATH. No LD_LIBRARY_PATH or extra\n"
        "package installation is required.\n\n")
    string(APPEND _manifest "${_report}")
else()
    string(APPEND _manifest
        "No third-party shared libraries were required: this platform's\n"
        "backend only uses OS-provided libraries/frameworks.\n")
endif()

string(APPEND _manifest
    "\nIntentionally NOT bundled (guaranteed OS / toolchain components):\n")
foreach(_name IN LISTS _skipped)
    string(APPEND _manifest "  ${_name}\n")
endforeach()

if(BUNDLE_SYSTEM STREQUAL "Linux")
    string(APPEND _manifest
        "\nBuilt against the distro libraries of the CI runner. Keep the\n"
        "whole lib/ directory together; do not mix it with another build.\n")
elseif(BUNDLE_SYSTEM STREQUAL "Darwin")
    string(APPEND _manifest
        "\nmacOS additionally uses the built-in hdiutil / drutil tools.\n")
endif()

file(WRITE "${_prefix}/DEPENDENCIES.txt" "${_manifest}")
message(STATUS "[bundle-deps] wrote ${_prefix}/DEPENDENCIES.txt")
