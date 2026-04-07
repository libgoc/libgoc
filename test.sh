#!/usr/bin/env bash

set -u
set -o pipefail

BUILD_DIR="${BUILD_DIR:-build}"
LOG_FILE="${LOG_FILE:-test.log}"
WATCH_MODE="${WATCH:-0}"
DEBUG_BUILD="0"
VMEM="0"
if [[ "$(uname -s)" == "Linux" ]]; then
    REUSEPORT_DEFAULT=1
else
    REUSEPORT_DEFAULT=0
fi
REUSEPORT="$REUSEPORT_DEFAULT"

usage() {
    echo "Usage: $0 [-dbg <0|1>] [-rp <0|1>] [-vmem <0|1>]"
    echo "  -dbg  Build with LIBGOC_DEBUG=ON when 1, OFF when 0. Default: 0."
    echo "  -rp   Build with GOC_HTTP_REUSEPORT=1 when 1, otherwise 0. Default: $REUSEPORT_DEFAULT."
    echo "  -vmem Build with LIBGOC_VMEM=ON when 1, OFF when 0. Default: 0."
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -dbg)
            shift
            if [[ $# -eq 0 || ! "$1" =~ ^[01]$ ]]; then
                echo "Error: -dbg requires 0 or 1."
                usage
            fi
            DEBUG_BUILD="$1"
            shift
            ;;
        -rp)
            shift
            if [[ $# -eq 0 || ! "$1" =~ ^[01]$ ]]; then
                echo "Error: -rp requires 0 or 1."
                usage
            fi
            REUSEPORT="$1"
            shift
            ;;
        -vmem)
            shift
            if [[ $# -eq 0 || ! "$1" =~ ^[01]$ ]]; then
                echo "Error: -vmem requires 0 or 1."
                usage
            fi
            VMEM="$1"
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Error: Unknown option '$1'."
            usage
            ;;
    esac
done

# Truncate and stream all output (stdout+stderr) to console and log file.
exec > >(tee "$LOG_FILE") 2>&1

FAILED_TESTS=()

configure_build() {
    cmake -S . -B "$BUILD_DIR" \
        -DGOC_ENABLE_STATS=ON \
        -DLIBGOC_STATIC_DEPENDENCIES=ON \
        -DLIBGOC_DEBUG="$DEBUG_BUILD" \
        -DGOC_HTTP_REUSEPORT="$REUSEPORT" \
        -DLIBGOC_VMEM="$VMEM"
}

build_project() {
    cmake --build "$BUILD_DIR"
}

collect_failed_tests() {
    local out_file="$1"
    mapfile -t FAILED_TESTS < <(
        sed -n 's/^[[:space:]]*[0-9][0-9]* - \([^[:space:]]*\).*/\1/p' "$out_file"
    )
}

run_all_tests() {
    local out_file
    out_file="$(mktemp)"
    if ctest --test-dir "$BUILD_DIR" --output-on-failure | tee "$out_file"; then
        FAILED_TESTS=()
        rm -f "$out_file"
        return 0
    fi
    collect_failed_tests "$out_file"
    rm -f "$out_file"
    return 1
}

join_failed_regex() {
    local regex=""
    local t
    for t in "${FAILED_TESTS[@]}"; do
        if [[ -n "$regex" ]]; then
            regex+="|"
        fi
        regex+="$t"
    done
    printf '%s' "$regex"
}

run_failed_tests() {
    if [[ ${#FAILED_TESTS[@]} -eq 0 ]]; then
        echo "No recorded failed tests; running full suite."
        run_all_tests
        return $?
    fi

    local regex
    regex="$(join_failed_regex)"
    local out_file
    out_file="$(mktemp)"

    echo "Re-running failed tests: ${FAILED_TESTS[*]}"
    if ctest --test-dir "$BUILD_DIR" --output-on-failure -R "^(${regex})$" | tee "$out_file"; then
        FAILED_TESTS=()
        rm -f "$out_file"
        return 0
    fi

    collect_failed_tests "$out_file"
    rm -f "$out_file"
    return 1
}

watch_signature() {
    find src include tests -type f \( -name '*.c' -o -name '*.h' \) -print \
        | LC_ALL=C sort \
        | while IFS= read -r f; do
              cksum "$f"
          done \
        | cksum \
        | awk '{print $1":"$2}'
}

run_cycle() {
    configure_build && build_project
    run_all_tests
    return $?
}

debug_status="OFF"
if [[ "$DEBUG_BUILD" == "1" ]]; then
    debug_status="ON"
fi

echo "== libgoc test runner =="
echo "Build dir : $BUILD_DIR"
echo "Log file  : $LOG_FILE"
echo "Debug     : LIBGOC_DEBUG=$debug_status"
echo "Reuseport : GOC_HTTP_REUSEPORT=$REUSEPORT"
echo "VMEM      : LIBGOC_VMEM=$VMEM"
echo "Watch mode: $WATCH_MODE"

last_status=0
if ! run_cycle; then
    last_status=$?
fi

if [[ "$WATCH_MODE" == "1" ]]; then
    echo "Watch mode enabled: waiting for source/header changes under src/, include/, tests/."
    prev_sig="$(watch_signature)"

    while true; do
        sleep 1
        cur_sig="$(watch_signature)"
        if [[ "$cur_sig" == "$prev_sig" ]]; then
            continue
        fi

        prev_sig="$cur_sig"
        echo
        echo "== Change detected; rebuilding and running tests =="
        : > "$LOG_FILE"

        if ! (configure_build && build_project); then
            echo "Build failed; waiting for next change..."
            last_status=1
            continue
        fi

        if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
            if run_failed_tests; then
                last_status=0
            else
                last_status=1
            fi
        else
            if run_all_tests; then
                last_status=0
            else
                last_status=1
            fi
        fi
    done
fi

exit "$last_status"
