.PHONY: run clean

threads ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
all ?= 0

# Number of threads in the core.async go-block dispatch pool.
# Analogous to GOMAXPROCS (Go) and GOC_POOL_THREADS (libgoc).

run:
	if [ "$(all)" = "1" ]; then \
		for pool in 1 2 4 8; do \
			echo "=== Pool Size: $$pool ==="; \
			clojure -J-Dclojure.core.async.pool-size=$$pool -M -m bench.core; \
			echo; \
		done; \
	else \
		echo "threads=$(threads)"; \
		clojure -J-Dclojure.core.async.pool-size=$(threads) -M -m bench.core; \
	fi
