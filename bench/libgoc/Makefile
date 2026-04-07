.PHONY: all run build clean

ROOT_DIR := $(abspath ../..)
BUILD_DIR := $(ROOT_DIR)/build-bench
BENCH_BIN := bench-libgoc
PKG_CONFIG ?= pkg-config
trace ?= 0
trace_arg := OFF
ifeq ($(trace),1)
trace_arg := ON
endif
rp ?= 1
rp_arg := OFF
ifeq ($(rp),1)
rp_arg := ON
endif
vmem ?= 0
vmem_arg := OFF
ifeq ($(vmem),1)
vmem_arg := ON
endif
stats ?= 0
stats_arg := OFF
ifeq ($(stats),1)
stats_arg := ON
endif
http_only ?= 0
threads ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
all ?= 0

GOC_ENABLE_STATS ?= $(stats)

CFLAGS := -std=c11 -O2 -DNDEBUG -DGC_THREADS -D_GNU_SOURCE
CFLAGS += -I$(ROOT_DIR)/include -I$(ROOT_DIR)/src -I$(ROOT_DIR)/vendor/minicoro
CFLAGS += $(shell PKG_CONFIG_ALL_STATIC=1 $(PKG_CONFIG) --cflags --static libuv bdw-gc-threaded)
LIBUV_LIBDIR := $(shell PKG_CONFIG_ALL_STATIC=1 $(PKG_CONFIG) --variable=libdir libuv)
BDWGC_LIBDIR := $(shell PKG_CONFIG_ALL_STATIC=1 $(PKG_CONFIG) --variable=libdir bdw-gc-threaded)
LDLIBS := -L$(LIBUV_LIBDIR) -l:libuv.a -L$(BDWGC_LIBDIR) -l:libgc.a $(shell PKG_CONFIG_ALL_STATIC=1 $(PKG_CONFIG) --static --libs libuv bdw-gc-threaded | sed -E 's/-L[^ ]+//g; s/-luv//g; s/-lgc//g')

ifeq ($(trace),1)
CFLAGS += -DLIBGOC_DEBUG
endif

ifeq ($(GOC_ENABLE_STATS),1)
CFLAGS += -DGOC_ENABLE_STATS
endif

all: build

# Always run cmake --build so that library API changes in src/ are picked up
# even when build-bench/libgoc.a already exists from an older build.
# cmake --build is incremental, so it only recompiles changed source files.
#
# Default mode: canary (vmem mode when vmem=1).
# For vmem mode, set vmem=1 (and optionally BUILD_DIR):
#   make vmem=1 BUILD_DIR=../../build-bench-vmem build run
build: bench.c
	cmake -S $(ROOT_DIR) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release \
	    -DLIBGOC_VMEM=$(vmem_arg) \
	    -DLIBGOC_DEBUG=$(trace_arg) \
	    -DGOC_HTTP_REUSEPORT=$(rp_arg) \
	    -DGOC_ENABLE_STATS=$(stats_arg)
	cmake --build $(BUILD_DIR) --target goc
	$(CC) $(CFLAGS) $< $(BUILD_DIR)/libgoc.a $(LDLIBS) -o $(BENCH_BIN)

run: build
ifeq ($(all),1)
	@for pool in 1 2 4 8; do \
		echo "=== Pool Size: $$pool ==="; \
		echo "GOC_POOL_THREADS=$$pool"; \
		GOC_BENCH_HTTP_ONLY=$(http_only) GOC_POOL_THREADS=$$pool ./$(BENCH_BIN); \
		echo; \
	done
else
	@echo "GOC_POOL_THREADS=$(threads)"
	@GOC_BENCH_HTTP_ONLY=$(http_only) GOC_POOL_THREADS=$(threads) ./$(BENCH_BIN)
endif

clean:
	rm -f $(BENCH_BIN)
	rm -rf $(BUILD_DIR)
