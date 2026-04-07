#!/bin/bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
LOG_FILE="${LOG_FILE:-test.log}"
LOG_FILE_ABS="$(cd "$(dirname "$LOG_FILE")" && pwd)/$(basename "$LOG_FILE")"

print_log_path() {
    echo "Log file: $LOG_FILE_ABS"
}
trap print_log_path EXIT

trace=1
max_tries=20
vmem_build=0
if [[ "$(uname -s)" == "Linux" ]]; then
    reuseport_default=1
else
    reuseport_default=0
fi
reuseport_build="$reuseport_default"

usage() {
    echo "Usage: $0 [-max-tries <n>] [-trace <0|1>] [-rp <0|1>] [-vmem <0|1>] [-out <file>] <test-source-path>"
    echo "  -rp   Build with GOC_HTTP_REUSEPORT=1 when 1, otherwise 0. Defaults to $reuseport_default on this platform."
    echo "  -vmem Build with LIBGOC_VMEM=ON when 1, OFF when 0. Default: 0."
    echo "  -trace Build with LIBGOC_DEBUG=ON when 1, OFF when 0. Default: 1."
    echo "  -out  Override the default log file path."
    echo "Example: $0 -max-tries 20 -trace 1 -rp 1 -vmem 0 -out custom.log tests/test_p06_thread_pool.c"
    exit 1
}

if [[ $# -lt 1 ]]; then
    usage
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        -max-tries)
            shift
            if [[ $# -eq 0 || ! "$1" =~ ^[0-9]+$ ]]; then
                echo "Error: -max-tries requires a numeric argument."
                usage
            fi
            max_tries="$1"
            if [[ "$max_tries" -le 0 ]]; then
                echo "Error: -max-tries must be greater than 0."
                usage
            fi
            shift
            ;;
        -trace)
            shift
            if [[ $# -eq 0 || ! "$1" =~ ^[01]$ ]]; then
                echo "Error: -trace requires 0 or 1."
                usage
            fi
            trace="$1"
            shift
            ;;
        -rp)
            shift
            if [[ $# -eq 0 || ! "$1" =~ ^[01]$ ]]; then
                echo "Error: -rp requires 0 or 1."
                usage
            fi
            reuseport_build="$1"
            shift
            ;;
        -vmem)
            shift
            if [[ $# -eq 0 || ! "$1" =~ ^[01]$ ]]; then
                echo "Error: -vmem requires 0 or 1."
                usage
            fi
            vmem_build="$1"
            shift
            ;;
        -out)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: -out requires a file path."
                usage
            fi
            LOG_FILE="$1"
            shift
            ;;
        -*)
            echo "Error: Unknown option '$1'."
            usage
            ;;
        *)
            if [[ -n "${test_source:-}" ]]; then
                echo "Error: Multiple test source paths provided."
                usage
            fi
            test_source="$1"
            shift
            ;;
    esac
done

if [[ -z "${test_source:-}" ]]; then
    usage
fi

if [[ ! -f "$test_source" ]]; then
    echo "Test source not found: $test_source"
    exit 1
fi

if [[ "$trace" -eq 1 ]]; then
    build_type="-DLIBGOC_DEBUG=ON"
else
    build_type="-DLIBGOC_DEBUG=OFF"
fi
rp_arg="-DGOC_HTTP_REUSEPORT=$reuseport_build"
vmem_arg="-DLIBGOC_VMEM=$vmem_build"

test_name="$(basename "$test_source" .c)"
binary_path="$BUILD_DIR/$test_name"

echo "Configuring/building in '$BUILD_DIR'..."
cmake -S . -B "$BUILD_DIR" -DGOC_ENABLE_STATS=ON -DLIBGOC_STATIC_DEPENDENCIES=ON $build_type $rp_arg $vmem_arg 
cmake --build "$BUILD_DIR" --target "$test_name"

if [[ ! -x "$binary_path" ]]; then
    echo "Built test binary not found: $binary_path"
    exit 1
fi

echo "-------------------------------------"
echo "| trace: $trace | reuseport: $reuseport_build | vmem: $vmem_build |"
echo "-------------------------------------"
echo "Running '$test_name' in a loop..."

tests_run=0
while true; do
    tests_run=$((tests_run + 1))
    run_start_ts=$(date +"%Y-%m-%d %H:%M:%S.%3N")
    run_start_ms=$(date +%s%3N)
    echo "[$run_start_ts] Run $tests_run/$max_tries: ..."

    if ! "$binary_path" 2>&1 | tee "$LOG_FILE" > /dev/null; then
        exit_code=$?
        run_end_ts=$(date +"%Y-%m-%d %H:%M:%S.%3N")
        run_end_ms=$(date +%s%3N)
        duration_ms=$((run_end_ms - run_start_ms))
        echo "[$run_end_ts] Run $tests_run/$max_tries: FAILED | ${duration_ms}ms | (exit code $exit_code)"
        echo "Test did not pass. Exiting."
        exit "$exit_code"
    fi

    run_end_ts=$(date +"%Y-%m-%d %H:%M:%S.%3N")
    run_end_ms=$(date +%s%3N)
    duration_ms=$((run_end_ms - run_start_ms))
    echo "[$run_end_ts] Run $tests_run/$max_tries: PASSED | ${duration_ms}ms"

    if [[ "$tests_run" -ge "$max_tries" ]]; then
        echo "Completed $tests_run successful tries. Exiting."
        exit 0
    fi
done
