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
option(LIBGOC_VMEM        "Use minicoro virtual memory allocator (off by default; canary-protected stacks are the default)" OFF)
option(LIBGOC_ASAN        "Build an instrumented copy of goc with AddressSanitizer" OFF)
option(LIBGOC_TSAN        "Build an instrumented copy of goc with ThreadSanitizer" OFF)
option(LIBGOC_COVERAGE    "Instrument goc and tests for code coverage" OFF)
option(LIBGOC_STATIC_DEPENDENCIES "Link libuv and bdwgc statically" ON)
option(LIBGOC_SERVER      "Build the goc_http HTTP/1.1 library" ON)
option(GOC_ENABLE_STATS   "Enable telemetry / statistics collection (goc_stats)" OFF)
option(LIBGOC_DEBUG       "Enable compile-time GOC_DBG diagnostic logging" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(GOC_HTTP_REUSEPORT_DEFAULT ON)
else()
    set(GOC_HTTP_REUSEPORT_DEFAULT OFF)
endif()
option(GOC_HTTP_REUSEPORT  "Compile-time override for HTTP SO_REUSEPORT support" ${GOC_HTTP_REUSEPORT_DEFAULT})
set(LIBGOC_STACK_SIZE "0" CACHE STRING "Default fiber stack size in bytes (0 = let minicoro decide)")

# ---------------------------------------------------------------------------
# Mutual-exclusion guards
# ---------------------------------------------------------------------------
if(LIBGOC_SHARED)
    message(FATAL_ERROR "Shared library builds are no longer supported. Remove -DLIBGOC_SHARED or build libgoc as a static library only.")
endif()

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

if(LIBGOC_STATIC_DEPENDENCIES)
    # Force pkg-config to resolve static dependencies for libuv and bdwgc.
    # This is the intended build mode for libgoc.a: no runtime shared
    # dependency on libuv or Boehm GC.
    set(ENV{PKG_CONFIG_ALL_STATIC} 1)
    message(STATUS "Static dependency linking enabled for libuv and bdw-gc.")
else()
    message(STATUS "Static dependency linking disabled; pkg-config may select shared libs.")
endif()

pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
pkg_check_modules(BDWGC REQUIRED IMPORTED_TARGET bdw-gc-threaded)

if(LIBGOC_STATIC_DEPENDENCIES)
    function(goc_find_static_pkg_archive pkg_name archive_name archive_var libs_var)
        execute_process(COMMAND pkg-config --variable=libdir ${pkg_name}
                        RESULT_VARIABLE PKG_LIBDIR_RESULT
                        OUTPUT_VARIABLE PKG_LIBDIR
                        ERROR_VARIABLE PKG_LIBDIR_ERROR
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_STRIP_TRAILING_WHITESPACE)
        if(NOT PKG_LIBDIR_RESULT EQUAL 0)
            message(FATAL_ERROR "Could not resolve ${pkg_name} libdir: ${PKG_LIBDIR_ERROR}")
        endif()

        execute_process(COMMAND pkg-config --static --libs ${pkg_name}
                        RESULT_VARIABLE PKG_STATIC_RESULT
                        OUTPUT_VARIABLE PKG_STATIC_LIBS
                        ERROR_VARIABLE PKG_STATIC_ERROR
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_STRIP_TRAILING_WHITESPACE)
        if(NOT PKG_STATIC_RESULT EQUAL 0)
            message(FATAL_ERROR "Static ${pkg_name} dependency check failed: ${PKG_STATIC_ERROR}")
        endif()

        set(CANDIDATE_LIBDIRS ${PKG_LIBDIR})
        string(REGEX MATCHALL "-L[^ ]+" PKG_LIB_DIR_FLAGS "${PKG_STATIC_LIBS}")
        foreach(lib_dir_flag IN LISTS PKG_LIB_DIR_FLAGS)
            string(REGEX REPLACE "^-L" "" libdir "${lib_dir_flag}")
            list(APPEND CANDIDATE_LIBDIRS "${libdir}")
        endforeach()
        list(REMOVE_DUPLICATES CANDIDATE_LIBDIRS)

        set(FOUND_ARCHIVE "")
        string(REGEX MATCHALL "[^ ]*${archive_name}" ARCHIVE_PATHS "${PKG_STATIC_LIBS}")
        foreach(cand IN LISTS ARCHIVE_PATHS)
            if(EXISTS "${cand}")
                set(FOUND_ARCHIVE "${cand}")
                break()
            endif()
        endforeach()

        foreach(libdir IN LISTS CANDIDATE_LIBDIRS)
            if(FOUND_ARCHIVE)
                break()
            endif()
            string(CONCAT candidate "${libdir}/" "${archive_name}")
            if(EXISTS "${candidate}")
                set(FOUND_ARCHIVE "${candidate}")
            endif()
        endforeach()

        if(NOT FOUND_ARCHIVE)
            message(FATAL_ERROR "Could not find static ${pkg_name} archive ${archive_name} in any of: ${CANDIDATE_LIBDIRS}")
        endif()

        set(${archive_var} "${FOUND_ARCHIVE}" PARENT_SCOPE)
        set(${libs_var} "${PKG_STATIC_LIBS}" PARENT_SCOPE)
    endfunction()

    goc_find_static_pkg_archive(libuv libuv.a LIBUV_STATIC_ARCHIVE LIBUV_STATIC_LIBS)
    goc_find_static_pkg_archive(bdw-gc-threaded libgc.a BDWGC_STATIC_ARCHIVE BDWGC_STATIC_LIBS)

    string(REPLACE "-luv" "${LIBUV_STATIC_ARCHIVE}" LIBUV_STATIC_LINK_FLAGS_TMP "${LIBUV_STATIC_LIBS}")
    string(REPLACE "-lgc" "${BDWGC_STATIC_ARCHIVE}" LIBUV_STATIC_LINK_FLAGS_TMP "${LIBUV_STATIC_LINK_FLAGS_TMP}")
    string(REPLACE "-lgc" "${BDWGC_STATIC_ARCHIVE}" BDWGC_STATIC_LINK_FLAGS_TMP "${BDWGC_STATIC_LIBS}")
    separate_arguments(LIBUV_STATIC_LINK_FLAGS UNIX_COMMAND "${LIBUV_STATIC_LINK_FLAGS_TMP}")
    separate_arguments(BDWGC_STATIC_LINK_FLAGS UNIX_COMMAND "${BDWGC_STATIC_LINK_FLAGS_TMP}")
endif()

# picohttpparser is vendored under vendor/picohttpparser — no external package needed.

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
    src/goc_debug.c
)

if(GOC_ENABLE_STATS)
    set(GOC_SOURCES ${GOC_SOURCES} src/goc_stats.c)
endif()

if(LIBGOC_SERVER)
    set(GOC_SOURCES ${GOC_SOURCES}
        src/goc_http.c
        vendor/picohttpparser/picohttpparser.c
    )
endif()

# ---------------------------------------------------------------------------
# Helper: apply common compile options to a goc library target
# ---------------------------------------------------------------------------
function(goc_configure_target target_name)
    target_include_directories(${target_name}
        PUBLIC  include
        PRIVATE src
                vendor/minicoro
                vendor/picohttpparser
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

    if(LIBGOC_STATIC_DEPENDENCIES)
        target_include_directories(${target_name} PUBLIC ${LIBUV_INCLUDE_DIRS} ${BDWGC_INCLUDE_DIRS})
        target_link_libraries(${target_name} PUBLIC ${LIBUV_STATIC_LINK_FLAGS} ${BDWGC_STATIC_LINK_FLAGS})
    else()
        target_link_libraries(${target_name}
            PUBLIC
                PkgConfig::LIBUV
                PkgConfig::BDWGC
        )
    endif()
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
if(LIBGOC_DEBUG)
    target_compile_definitions(goc PRIVATE LIBGOC_DEBUG)
endif()
if(GOC_HTTP_REUSEPORT)
    target_compile_definitions(goc PRIVATE GOC_HTTP_REUSEPORT)
else()
    target_compile_definitions(goc PRIVATE GOC_HTTP_REUSEPORT=0)
endif()
target_compile_definitions(goc PRIVATE LIBGOC_STACK_SIZE=${LIBGOC_STACK_SIZE})

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
    if(LIBGOC_DEBUG)
        target_compile_definitions(goc_asan PRIVATE LIBGOC_DEBUG)
    endif()
    if(GOC_HTTP_REUSEPORT)
        target_compile_definitions(goc_asan PRIVATE GOC_HTTP_REUSEPORT)
    else()
        target_compile_definitions(goc_asan PRIVATE GOC_HTTP_REUSEPORT=0)
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
    if(LIBGOC_DEBUG)
        target_compile_definitions(goc_tsan PRIVATE LIBGOC_DEBUG)
    endif()
    if(GOC_HTTP_REUSEPORT)
        target_compile_definitions(goc_tsan PRIVATE GOC_HTTP_REUSEPORT)
    else()
        target_compile_definitions(goc_tsan PRIVATE GOC_HTTP_REUSEPORT=0)
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

# Exclude the HTTP test when LIBGOC_SERVER is disabled.
if(NOT LIBGOC_SERVER)
    list(FILTER TEST_SOURCES EXCLUDE REGEX "test_p11_http\.c$")
endif()

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
            if(LIBGOC_DEBUG)
                target_compile_definitions(goc_stats_instrumented PRIVATE LIBGOC_DEBUG)
            endif()
            if(GOC_HTTP_REUSEPORT)
                target_compile_definitions(goc_stats_instrumented PRIVATE GOC_HTTP_REUSEPORT)
            else()
                target_compile_definitions(goc_stats_instrumented PRIVATE GOC_HTTP_REUSEPORT=0)
            endif()
        endif()
        set(GOC_LINK_TARGET goc_stats_instrumented)
    endif()

    add_executable(${test_name} ${test_src} tests/test_harness.c)

    target_include_directories(${test_name} PRIVATE include src tests vendor/minicoro)

    if(LIBGOC_STATIC_DEPENDENCIES)
        target_link_libraries(${test_name} PRIVATE ${GOC_LINK_TARGET})
    else()
        target_link_libraries(${test_name} PRIVATE
            ${GOC_LINK_TARGET}
            PkgConfig::LIBUV
            PkgConfig::BDWGC
        )
    endif()

    # Propagate the vmem mode flag so tests can conditionally skip behaviour
    # that only applies to canary mode (e.g. P8.1 stack-overflow detection).
    if(LIBGOC_VMEM)
        target_compile_definitions(${test_name} PRIVATE LIBGOC_VMEM_ENABLED)
    endif()
    # Stats always enabled for tests (see goc_stats_instrumented above).
    target_compile_definitions(${test_name} PRIVATE GOC_ENABLE_STATS)
    target_compile_definitions(${test_name} PRIVATE LIBGOC_STACK_SIZE=${LIBGOC_STACK_SIZE})
    if(LIBGOC_DEBUG)
        target_compile_definitions(${test_name} PRIVATE LIBGOC_DEBUG)
    endif()

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Some test suites are significantly heavier than the others and regularly
# exceed the default ctest --timeout on slow CI runners (e.g. Windows, vmem, macOS).
# Override the per-test timeout to give them enough headroom.
set_tests_properties(test_goc_stats             PROPERTIES TIMEOUT 180)
set_tests_properties(test_p11_http              PROPERTIES TIMEOUT 540)
set_tests_properties(test_p03_channel_io        PROPERTIES TIMEOUT 180)
set_tests_properties(test_p05_select_timeout    PROPERTIES TIMEOUT 300)

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
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS goc
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/goc.h include/goc_io.h include/goc_array.h include/goc_stats.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
if(LIBGOC_SERVER)
    install(FILES include/goc_http.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

# pkg-config file
configure_file(libgoc.pc.in ${CMAKE_BINARY_DIR}/libgoc.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/libgoc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
