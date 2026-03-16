cmake_minimum_required(VERSION 3.16)
project(libgoc VERSION 0.1.0 LANGUAGES C)

# ---------------------------------------------------------------------------
# C standard
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Default build type
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type specified — defaulting to RelWithDebInfo")
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
# libgoc requires a thread-aware build of Boehm GC.  The plain bdw-gc package
# resolves to libgc on some Debian/Ubuntu systems compiled without
# --enable-threads; linking it while compiling with -DGC_THREADS causes
# GC_INIT() / GC_allow_register_threads() to misbehave or segfault at runtime.
# bdw-gc-threaded (libgc-threaded) is the explicitly thread-safe variant and is
# the only acceptable dependency.  If it is not present, fail loudly so the
# developer installs the correct package rather than silently linking a broken
# build.
#
# IMPORTED_TARGET is required so that the linker search path (-L) from the
# .pc file is embedded in the PkgConfig::bdw-gc-threaded imported target and
# propagated to all consumers.  Without it, pkg_check_modules populates only
# BDWGC_LIBRARIES with bare library names (e.g. -lgc) and the linker never
# learns the non-default install prefix, causing "cannot find -lgc" at link
# time when Boehm GC is installed outside the system library paths.
pkg_check_modules(BDWGC REQUIRED IMPORTED_TARGET bdw-gc-threaded)

# ---------------------------------------------------------------------------
# Source list
# ---------------------------------------------------------------------------
set(GOC_SOURCES
    src/gc.c
    src/pool.c
    src/loop.c
    src/channel.c
    src/fiber.c
    src/alts.c
    src/timeout.c
    src/minicoro.c
)

# ---------------------------------------------------------------------------
# Phase test sources — discover every tests/test_*.c automatically
# ---------------------------------------------------------------------------
file(GLOB PHASE_TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.c)

set(PHASE_TESTS "")
foreach(src IN LISTS PHASE_TEST_SOURCES)
    get_filename_component(phase ${src} NAME_WE)   # e.g. test_p1_foundation
    list(APPEND PHASE_TESTS ${phase})
    set(${phase}_SOURCE ${src})
endforeach()

# ---------------------------------------------------------------------------
# Helper: apply common compile options to a goc library variant
# ---------------------------------------------------------------------------
function(configure_goc_target target_name extra_flags)
    target_include_directories(${target_name} PUBLIC
        include
        vendor/minicoro
    )
    target_compile_definitions(${target_name} PUBLIC
        # Required for Boehm GC multi-thread support:
        # exposes GC_register_my_thread / GC_unregister_my_thread /
        # GC_allow_register_threads in gc.h.
        GC_THREADS
        # Required for POSIX extensions (pthread_rwlock_t et al.) when
        # compiling with -std=c11, which disables _POSIX_SOURCE by default.
        _GNU_SOURCE
    )
    target_link_libraries(${target_name} PUBLIC
        PkgConfig::LIBUV
        PkgConfig::BDWGC
        Threads::Threads
    )
    target_compile_options(${target_name} PRIVATE
        -Wall -Wextra
        ${extra_flags}
    )
endfunction()

# ---------------------------------------------------------------------------
# Helper: register one phase test executable against a goc library variant
# ---------------------------------------------------------------------------
function(add_phase_test phase_name goc_lib extra_flags suffix)
    set(exe_name "${phase_name}${suffix}")
    add_executable(${exe_name} ${${phase_name}_SOURCE})
    target_link_libraries(${exe_name} PRIVATE ${goc_lib})
    target_compile_options(${exe_name} PRIVATE -Wall -Wextra ${extra_flags})
    add_test(NAME ${exe_name} COMMAND ${exe_name})
endfunction()

# ---------------------------------------------------------------------------
# Default static library
# ---------------------------------------------------------------------------
add_library(goc STATIC ${GOC_SOURCES})
configure_goc_target(goc "")

# ---------------------------------------------------------------------------
# Optional: shared library  (-DLIBGOC_SHARED=ON)
# ---------------------------------------------------------------------------
option(LIBGOC_SHARED "Build libgoc as a shared library as well" OFF)

if(LIBGOC_SHARED)
    add_library(goc_shared SHARED ${GOC_SOURCES})
    configure_goc_target(goc_shared "")
    set_target_properties(goc_shared PROPERTIES
        OUTPUT_NAME goc
        VERSION     ${PROJECT_VERSION}
        SOVERSION   ${PROJECT_VERSION_MAJOR}
    )
endif()

# ---------------------------------------------------------------------------
# Phase test executables (default build)
# Compile definitions and include paths propagate transitively from goc (PUBLIC).
# ---------------------------------------------------------------------------
enable_testing()

foreach(phase IN LISTS PHASE_TESTS)
    add_phase_test(${phase} goc "" "")
endforeach()

# ---------------------------------------------------------------------------
# Optional: AddressSanitizer  (-DLIBGOC_ASAN=ON)
# ---------------------------------------------------------------------------
option(LIBGOC_ASAN "Build with AddressSanitizer" OFF)
option(LIBGOC_TSAN "Build with ThreadSanitizer" OFF)
option(LIBGOC_COVERAGE "Build with lcov coverage" OFF)

if(LIBGOC_ASAN AND LIBGOC_TSAN)
    message(FATAL_ERROR "LIBGOC_ASAN and LIBGOC_TSAN are mutually exclusive.")
endif()

if(LIBGOC_ASAN AND LIBGOC_COVERAGE)
    message(FATAL_ERROR "LIBGOC_ASAN and LIBGOC_COVERAGE should not be combined.")
endif()

if(LIBGOC_TSAN AND LIBGOC_COVERAGE)
    message(FATAL_ERROR "LIBGOC_TSAN and LIBGOC_COVERAGE should not be combined.")
endif()

if(LIBGOC_ASAN)
    set(ASAN_FLAGS -fsanitize=address -fno-omit-frame-pointer -g)

    add_library(goc_asan STATIC ${GOC_SOURCES})
    configure_goc_target(goc_asan "${ASAN_FLAGS}")
    target_link_options(goc_asan PUBLIC -fsanitize=address)

    foreach(phase IN LISTS PHASE_TESTS)
        add_phase_test(${phase} goc_asan "${ASAN_FLAGS}" "_asan")
        target_link_options("${phase}_asan" PRIVATE -fsanitize=address)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Optional: ThreadSanitizer  (-DLIBGOC_TSAN=ON)
# ---------------------------------------------------------------------------
if(LIBGOC_TSAN)
    set(TSAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g)

    add_library(goc_tsan STATIC ${GOC_SOURCES})
    configure_goc_target(goc_tsan "${TSAN_FLAGS}")
    target_link_options(goc_tsan PUBLIC -fsanitize=thread)

    foreach(phase IN LISTS PHASE_TESTS)
        add_phase_test(${phase} goc_tsan "${TSAN_FLAGS}" "_tsan")
        target_link_options("${phase}_tsan" PRIVATE -fsanitize=thread)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Optional: Coverage  (-DLIBGOC_COVERAGE=ON)
# Builds a separate goc_coverage variant rather than mutating the base goc
# target, consistent with the ASAN/TSAN pattern above.
# ---------------------------------------------------------------------------
if(LIBGOC_COVERAGE)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "LIBGOC_COVERAGE is incompatible with Release build type.")
    endif()

    find_program(LCOV lcov REQUIRED)
    find_program(GENHTML genhtml REQUIRED)

    set(COVERAGE_FLAGS --coverage -fprofile-arcs -ftest-coverage -g -O0)

    add_library(goc_coverage STATIC ${GOC_SOURCES})
    configure_goc_target(goc_coverage "${COVERAGE_FLAGS}")
    target_link_options(goc_coverage PUBLIC --coverage)

    set(COVERAGE_TEST_TARGETS "")
    foreach(phase IN LISTS PHASE_TESTS)
        add_phase_test(${phase} goc_coverage "${COVERAGE_FLAGS}" "_coverage")
        target_link_options("${phase}_coverage" PRIVATE --coverage)
        list(APPEND COVERAGE_TEST_TARGETS "${phase}_coverage")
    endforeach()

    add_custom_target(coverage
        COMMAND ${LCOV} --capture --directory . --output-file coverage.info
        COMMAND ${LCOV} --remove coverage.info '/usr/*' '*/vendor/*' --output-file coverage.info
        COMMAND ${GENHTML} coverage.info --output-directory coverage_html
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS ${COVERAGE_TEST_TARGETS}
        COMMENT "Generating HTML coverage report in coverage_html/"
    )
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

# Static library and public header
install(TARGETS goc
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/goc.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Shared library (only when LIBGOC_SHARED=ON)
if(LIBGOC_SHARED)
    install(TARGETS goc_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # .dll on Windows
    )
endif()

# pkg-config file: expand @-variables and write into the build tree, then install
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/libgoc.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/libgoc.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libgoc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
