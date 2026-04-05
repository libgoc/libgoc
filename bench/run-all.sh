#! /usr/bin/sh

set -eu

script_dir=$(
    CDPATH= cd -- "$(dirname -- "$0")" && pwd
)
cd "$script_dir"

run_checked() {
	log_file=$1
	shift

	pipe_dir=$(mktemp -d)
	pipe_path=$pipe_dir/out
	mkfifo "$pipe_path"
	tee -a "$log_file" <"$pipe_path" &
	tee_pid=$!

	if "$@" >"$pipe_path" 2>&1; then
		cmd_status=0
	else
		cmd_status=$?
	fi

	wait "$tee_pid"
	rm -f "$pipe_path"
	rmdir "$pipe_dir"

	if [ "$cmd_status" -ne 0 ]; then
		exit "$cmd_status"
	fi
}

find logs -type f -name '*.log' -exec truncate -s 0 {} +

i=0
while [ "$i" -lt 3 ]; do
	run_checked logs/go.log make -C go run-all
	i=$((i + 1))
done

i=0
while [ "$i" -lt 3 ]; do
	run_checked logs/clojure.log make -C clojure run-all
	i=$((i + 1))
done

i=0
while [ "$i" -lt 3 ]; do
	run_checked logs/canary.log make -C libgoc build run-all
	i=$((i + 1))
done

i=0
while [ "$i" -lt 3 ]; do
	run_checked logs/vmem.log make -C libgoc LIBGOC_VMEM=ON BUILD_DIR=../../build-bench-vmem build run-all
	i=$((i + 1))
done