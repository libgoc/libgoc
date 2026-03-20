.PHONY: run run-all

# Number of threads in the core.async go-block dispatch pool.
# Analogous to GOMAXPROCS (Go) and GOC_POOL_THREADS (libgoc).
POOL_SIZE ?= 8

run:
	clojure -J-Dclojure.core.async.pool-size=$(POOL_SIZE) -M -m bench.core

run-all:
	@for pool in 1 2 4 8; do \
		echo "=== Pool Size: $$pool ==="; \
		$(MAKE) --no-print-directory run POOL_SIZE=$$pool; \
		echo; \
	done
