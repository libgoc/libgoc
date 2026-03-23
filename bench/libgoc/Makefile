.PHONY: all run run-all build clean

ROOT_DIR := $(abspath ../..)
BUILD_DIR := $(ROOT_DIR)/build-bench
BENCH_BIN := bench-libgoc
PKG_CONFIG ?= pkg-config
LIBGOC_VMEM ?= OFF
LIBGOC_STATS ?= OFF

CFLAGS := -std=c11 -O2 -DNDEBUG -DGC_THREADS -D_GNU_SOURCE
CFLAGS += -I$(ROOT_DIR)/include -I$(ROOT_DIR)/src -I$(ROOT_DIR)/vendor/minicoro
CFLAGS += $(shell $(PKG_CONFIG) --cflags libuv bdw-gc-threaded)
LDLIBS := $(shell $(PKG_CONFIG) --libs libuv bdw-gc-threaded)

all: build

# Always run cmake --build so that library API changes in src/ are picked up
# even when build-bench/libgoc.a already exists from an older build.
# cmake --build is incremental, so it only recompiles changed source files.
#
# Default mode: canary (LIBGOC_VMEM=OFF).
# For vmem mode, set LIBGOC_VMEM=ON (and optionally BUILD_DIR):
#   make LIBGOC_VMEM=ON BUILD_DIR=../../build-bench-vmem build run-all
build: bench.c
	cmake -S $(ROOT_DIR) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DLIBGOC_VMEM=$(LIBGOC_VMEM) -DLIBGOC_STATS=$(LIBGOC_STATS)
	cmake --build $(BUILD_DIR) --target goc
	$(CC) $(CFLAGS) $< $(BUILD_DIR)/libgoc.a $(LDLIBS) -o $(BENCH_BIN)

run: build
	./$(BENCH_BIN)

run-all: build
	@for pool in 1 2 4 8; do \
		echo "=== Pool Size: $$pool ==="; \
		echo "GOC_POOL_THREADS=$$pool"; \
		GOC_POOL_THREADS=$$pool ./$(BENCH_BIN); \
		echo; \
	done

clean:
	rm -f $(BENCH_BIN)
	rm -rf $(BUILD_DIR)
