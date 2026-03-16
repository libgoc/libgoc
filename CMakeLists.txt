cmake_minimum_required(VERSION 3.20)
project(libgoc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
option(LIBGOC_SHARED   "Build a shared library (libgoc.so / .dylib / .dll)" OFF)
option(LIBGOC_ASAN     "Build an instrumented copy of goc with AddressSanitizer" OFF)
option(LIBGOC_TSAN     "Build an instrumented copy of goc with ThreadSanitizer" OFF)
option(LIBGOC_COVERAGE "Instrument goc and tests for code coverage" OFF)

# ---------------------------------------------------------------------------
# Mutual-exclusion guards
# ---------------------------------------------------------------------------
if(LIBGOC_ASAN AND LIBGOC_TSAN)
    message(FATAL_ERROR "LIBGOC_ASAN and LIBGOC_TSAN are mutually exclusive. "
                        "Configure them in separate build directories.")
endif()
if(LIBGOC_COVERAGE AND LIBGOC_ASAN)
    message(FATAL_ERROR "LIBGOC_COVERAGE and LIBGOC_ASAN are mutually exclusive.")
endif()
if(LIBGOC_COVERAGE AND LIBGOC_TSAN)
    message(FATAL_ERROR "LIBGOC_COVERAGE and LIBGOC_TSAN are mutually exclusive.")
endif()

# ---------------------------------------------------------------------------
# Dependencies — pkg-config only, no fallback
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
pkg_check_modules(BDWGC REQUIRED IMPORTED_TARGET bdw-gc-threaded)

# ---------------------------------------------------------------------------
# Source lists
# ---------------------------------------------------------------------------
set(GOC_SOURCES
    src/alts.c
    src/timeout.c
    src/gc.c
    src/loop.c
    src/pool.c
    src/fiber.c
    src/channel.c
    src/minicoro.c
)

# ---------------------------------------------------------------------------
# Helper: apply common compile options to a goc library target
# ---------------------------------------------------------------------------
function(goc_configure_target target_name)
    target_include_directories(${target_name}
        PUBLIC  include
        PRIVATE src
                vendor/minicoro
    )
    target_compile_definitions(${target_name} PRIVATE GC_THREADS)
    target_link_libraries(${target_name}
        PUBLIC
            PkgConfig::LIBUV
            PkgConfig::BDWGC
    )
endfunction()

# minicoro.c must NOT be compiled with -DGC_THREADS.
# Set globally before any add_library so it applies to all targets that
# include this source file. Does not require TARGET_DIRECTORY (CMake 3.26+).
set_source_files_properties(src/minicoro.c PROPERTIES COMPILE_OPTIONS "-UGC_THREADS")

# ---------------------------------------------------------------------------
# Primary static library: goc
# ---------------------------------------------------------------------------
add_library(goc STATIC ${GOC_SOURCES})
goc_configure_target(goc)

# ---------------------------------------------------------------------------
# Optional shared library: goc_shared  (-DLIBGOC_SHARED=ON)
# ---------------------------------------------------------------------------
if(LIBGOC_SHARED)
    add_library(goc_shared SHARED ${GOC_SOURCES})
    goc_configure_target(goc_shared)
    set_target_properties(goc_shared PROPERTIES OUTPUT_NAME goc)
endif()

# ---------------------------------------------------------------------------
# Optional: AddressSanitizer  (-DLIBGOC_ASAN=ON)
# ---------------------------------------------------------------------------
if(LIBGOC_ASAN)
    add_library(goc_asan STATIC ${GOC_SOURCES})
    goc_configure_target(goc_asan)
    target_compile_options(goc_asan PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(goc_asan INTERFACE -fsanitize=address)
endif()

# ---------------------------------------------------------------------------
# Optional: ThreadSanitizer  (-DLIBGOC_TSAN=ON)
# ---------------------------------------------------------------------------
if(LIBGOC_TSAN)
    add_library(goc_tsan STATIC ${GOC_SOURCES})
    goc_configure_target(goc_tsan)
    target_compile_options(goc_tsan PRIVATE -fsanitize=thread)
    target_link_options(goc_tsan INTERFACE -fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# Optional: Coverage  (-DLIBGOC_COVERAGE=ON)
# ---------------------------------------------------------------------------
if(LIBGOC_COVERAGE)
    target_compile_options(goc PRIVATE --coverage -fprofile-arcs -ftest-coverage)
    target_link_options(goc INTERFACE --coverage)
endif()

# ---------------------------------------------------------------------------
# Test executables
# ---------------------------------------------------------------------------
enable_testing()

# Discover test source files (requires cmake_path — CMake >= 3.20)
file(GLOB TEST_SOURCES "tests/test_p*.c")

foreach(test_src IN LISTS TEST_SOURCES)
    cmake_path(GET test_src STEM test_name)

    # Choose which goc variant to link against
    if(LIBGOC_ASAN)
        set(GOC_LINK_TARGET goc_asan)
    elseif(LIBGOC_TSAN)
        set(GOC_LINK_TARGET goc_tsan)
    else()
        set(GOC_LINK_TARGET goc)
    endif()

    add_executable(${test_name} ${test_src})

    target_include_directories(${test_name} PRIVATE include src tests vendor/minicoro)

    # Wrap libgoc.a in a rescan group so ld resolves cross-archive references
    # regardless of object-file ordering (Linux ld single-pass limitation).
    # $<LINK_GROUP:RESCAN,...> (CMake >= 3.24) is intentionally avoided to
    # preserve the stated minimum of CMake 3.20.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${test_name} PRIVATE
            "LINKER:--start-group"
            "$<TARGET_FILE:${GOC_LINK_TARGET}>"
            "LINKER:--end-group"
        )
    endif()
    target_link_libraries(${test_name} PRIVATE
        ${GOC_LINK_TARGET}
        PkgConfig::LIBUV
        PkgConfig::BDWGC
    )

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ---------------------------------------------------------------------------
# Coverage target (requires lcov + genhtml)
# ---------------------------------------------------------------------------
if(LIBGOC_COVERAGE)
    find_program(LCOV lcov)
    find_program(GENHTML genhtml)
    if(LCOV AND GENHTML)
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${LCOV}
                --capture
                --directory ${CMAKE_BINARY_DIR}
                --output-file ${CMAKE_BINARY_DIR}/coverage.info
                --exclude "*/tests/*"
                --exclude "/usr/*"
                --exclude "*/vendor/*"
            COMMAND ${GENHTML}
                ${CMAKE_BINARY_DIR}/coverage.info
                --output-directory ${CMAKE_BINARY_DIR}/coverage_html
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running tests and generating HTML coverage report -> coverage_html/index.html"
        )
    else()
        message(WARNING "lcov/genhtml not found — 'coverage' target will not be available.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install rules (used with -DLIBGOC_SHARED=ON)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS goc
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/goc.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
if(LIBGOC_SHARED)
    install(TARGETS goc_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# pkg-config file
configure_file(libgoc.pc.in ${CMAKE_BINARY_DIR}/libgoc.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/libgoc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
