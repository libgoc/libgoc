.PHONY: run build clean

threads ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
all ?= 0

run:
	if [ "$(all)" = "1" ]; then \
		for pool in 1 2 4 8; do \
			echo "=== Pool Size: $$pool ==="; \
			echo "GOMAXPROCS=$$pool"; \
			GOMAXPROCS=$$pool go run .; \
			echo; \
		done; \
	else \
		echo "GOMAXPROCS=$(threads)"; \
		GOMAXPROCS=$(threads) go run .; \
	fi

build:
	go build -ldflags="-s -w" -o bench-go .

clean:
	rm -f bench-go
