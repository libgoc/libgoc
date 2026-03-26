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
option(LIBGOC_SHARED      "Build a shared library (libgoc.so / .dylib / .dll)" OFF)
option(LIBGOC_VMEM        "Use minicoro virtual memory allocator (off by default; canary-protected stacks are the default)" OFF)
option(LIBGOC_ASAN        "Build an instrumented copy of goc with AddressSanitizer" OFF)
option(LIBGOC_TSAN        "Build an instrumented copy of goc with ThreadSanitizer" OFF)
option(LIBGOC_COVERAGE    "Instrument goc and tests for code coverage" OFF)
option(GOC_ENABLE_STATS   "Enable telemetry / statistics collection (goc_stats)" OFF)
set(LIBGOC_STACK_SIZE "0" CACHE STRING "Default fiber stack size in bytes (0 = let minicoro decide)")

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
    src/mutex.c
    src/minicoro.c
    src/wsdq.c
    src/goc_array.c
    src/goc_io.c
)

if(GOC_ENABLE_STATS)
    set(GOC_SOURCES ${GOC_SOURCES} src/goc_stats.c)
endif()

# ---------------------------------------------------------------------------
# Helper: apply common compile options to a goc library target
# ---------------------------------------------------------------------------
function(goc_configure_target target_name)
    target_include_directories(${target_name}
        PUBLIC  include
        PRIVATE src
                vendor/minicoro
    )
    # GC_THREADS: enable bdwgc multi-thread support on all platforms.
    # GC_PTHREADS: on POSIX this activates gc_pthread_redirects.h so that
    # GC_pthread_create / GC_pthread_join are declared and defined.  On
    # Windows, bdwgc (MSYS2 UCRT64) is compiled with Win32 threads and does
    # not provide those symbols, so GC_PTHREADS must be omitted there;
    # Windows threads are registered with the GC manually via
    # GC_register_my_thread / GC_unregister_my_thread instead.
    if(WIN32)
        target_compile_definitions(${target_name} PRIVATE GC_THREADS)
    else()
        target_compile_definitions(${target_name} PRIVATE GC_THREADS GC_PTHREADS)
    endif()
    target_link_libraries(${target_name}
        PUBLIC
            PkgConfig::LIBUV
            PkgConfig::BDWGC
    )
endfunction()

# minicoro.c must NOT be compiled with -DGC_THREADS (the TLS variable
# mco_current_co causes a SIGSEGV during Boehm GC's thread-startup walk);
# it MUST be compiled with -DMCO_ZERO_MEMORY so that minicoro zeroes its
# push/pop storage on pop, preventing stale GC-managed pointers from keeping
# objects alive longer than they should be under Boehm's conservative scan.
# Set globally before any add_library so it applies to all targets that
# include this source file. Does not require TARGET_DIRECTORY (CMake 3.26+).
if(LIBGOC_VMEM)
    set_source_files_properties(src/minicoro.c PROPERTIES COMPILE_OPTIONS "-UGC_THREADS;-UGC_PTHREADS;-DMCO_USE_VMEM_ALLOCATOR;-DMCO_ZERO_MEMORY")
else()
    set_source_files_properties(src/minicoro.c PROPERTIES COMPILE_OPTIONS "-UGC_THREADS;-UGC_PTHREADS;-DMCO_ZERO_MEMORY")
endif()

# ---------------------------------------------------------------------------
# Primary static library: goc
# ---------------------------------------------------------------------------
add_library(goc STATIC ${GOC_SOURCES})
goc_configure_target(goc)

# Pass virtual memory flag, stats flag, and stack size to source code
if(LIBGOC_VMEM)
    target_compile_definitions(goc PRIVATE LIBGOC_VMEM_ENABLED)
endif()
if(GOC_ENABLE_STATS)
    target_compile_definitions(goc PRIVATE GOC_ENABLE_STATS)
endif()
target_compile_definitions(goc PRIVATE LIBGOC_STACK_SIZE=${LIBGOC_STACK_SIZE})

# ---------------------------------------------------------------------------
# Optional shared library: goc_shared  (-DLIBGOC_SHARED=ON)
# ---------------------------------------------------------------------------
if(LIBGOC_SHARED)
    add_library(goc_shared SHARED ${GOC_SOURCES})
    goc_configure_target(goc_shared)
    set_target_properties(goc_shared PROPERTIES OUTPUT_NAME goc)
    if(LIBGOC_VMEM)
        target_compile_definitions(goc_shared PRIVATE LIBGOC_VMEM_ENABLED)
    endif()
    if(GOC_ENABLE_STATS)
        target_compile_definitions(goc_shared PRIVATE GOC_ENABLE_STATS)
    endif()
    target_compile_definitions(goc_shared PRIVATE LIBGOC_STACK_SIZE=${LIBGOC_STACK_SIZE})
endif()

# ---------------------------------------------------------------------------
# Optional: AddressSanitizer  (-DLIBGOC_ASAN=ON)
# ---------------------------------------------------------------------------
if(LIBGOC_ASAN)
    add_library(goc_asan STATIC ${GOC_SOURCES})
    goc_configure_target(goc_asan)
    target_compile_options(goc_asan PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(goc_asan INTERFACE -fsanitize=address)
    if(LIBGOC_VMEM)
        target_compile_definitions(goc_asan PRIVATE LIBGOC_VMEM_ENABLED)
    endif()
    if(GOC_ENABLE_STATS)
        target_compile_definitions(goc_asan PRIVATE GOC_ENABLE_STATS)
    endif()
    target_compile_definitions(goc_asan PRIVATE LIBGOC_STACK_SIZE=${LIBGOC_STACK_SIZE})
endif()

# ---------------------------------------------------------------------------
# Optional: ThreadSanitizer  (-DLIBGOC_TSAN=ON)
# ---------------------------------------------------------------------------
if(LIBGOC_TSAN)
    add_library(goc_tsan STATIC ${GOC_SOURCES})
    goc_configure_target(goc_tsan)
    target_compile_options(goc_tsan PRIVATE -fsanitize=thread)
    target_link_options(goc_tsan INTERFACE -fsanitize=thread)
    if(LIBGOC_VMEM)
        target_compile_definitions(goc_tsan PRIVATE LIBGOC_VMEM_ENABLED)
    endif()
    if(GOC_ENABLE_STATS)
        target_compile_definitions(goc_tsan PRIVATE GOC_ENABLE_STATS)
    endif()
    target_compile_definitions(goc_tsan PRIVATE LIBGOC_STACK_SIZE=${LIBGOC_STACK_SIZE})
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
file(GLOB TEST_SOURCES "tests/test_p*.c" "tests/test_goc_*.c")

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

    # Tests always run with GOC_ENABLE_STATS active so that stats APIs are
    # available unconditionally (no #ifdef guards needed in test sources).
    # When GOC_ENABLE_STATS is OFF the regular 'goc' library has the
    # instrumentation macros as no-ops, so build a dedicated variant with
    # the flag enabled and link all tests against it instead.
    if(NOT GOC_ENABLE_STATS)
        if(NOT TARGET goc_stats_instrumented)
            add_library(goc_stats_instrumented STATIC ${GOC_SOURCES} src/goc_stats.c)
            goc_configure_target(goc_stats_instrumented)
            target_compile_definitions(goc_stats_instrumented PRIVATE
                GOC_ENABLE_STATS LIBGOC_STACK_SIZE=${LIBGOC_STACK_SIZE})
            if(LIBGOC_VMEM)
                target_compile_definitions(goc_stats_instrumented PRIVATE LIBGOC_VMEM_ENABLED)
            endif()
        endif()
        set(GOC_LINK_TARGET goc_stats_instrumented)
    endif()

    add_executable(${test_name} ${test_src} tests/test_harness.c)

    target_include_directories(${test_name} PRIVATE include src tests vendor/minicoro)

    target_link_libraries(${test_name} PRIVATE
        ${GOC_LINK_TARGET}
        PkgConfig::LIBUV
        PkgConfig::BDWGC
    )

    # Propagate the vmem mode flag so tests can conditionally skip behaviour
    # that only applies to canary mode (e.g. P8.1 stack-overflow detection).
    if(LIBGOC_VMEM)
        target_compile_definitions(${test_name} PRIVATE LIBGOC_VMEM_ENABLED)
    endif()
    # Stats always enabled for tests (see goc_stats_instrumented above).
    target_compile_definitions(${test_name} PRIVATE GOC_ENABLE_STATS)
    target_compile_definitions(${test_name} PRIVATE LIBGOC_STACK_SIZE=${LIBGOC_STACK_SIZE})

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
install(FILES include/goc.h include/goc_io.h include/goc_array.h include/goc_stats.h
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
