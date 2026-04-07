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

	if stdbuf -oL -eL "$@" >"$pipe_path" 2>&1; then
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

rm -f logs/go.log

i=0
while [ "$i" -lt 3 ]; do
	run_checked logs/go.log make -C go run all=1
	i=$((i + 1))
done

rm -f logs/clojure.log

i=0
while [ "$i" -lt 3 ]; do
	run_checked logs/clojure.log make -C clojure run all=1
	i=$((i + 1))
done

rm -f logs/canary.log

i=0
while [ "$i" -lt 3 ]; do
	run_checked logs/canary.log make -C libgoc build run all=1
	i=$((i + 1))
done

rm -f logs/vmem.log

i=0
while [ "$i" -lt 3 ]; do
	run_checked logs/vmem.log make -C libgoc vmem=1 BUILD_DIR=../../build-bench-vmem build run all=1
	i=$((i + 1))
done
