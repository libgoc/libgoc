.PHONY: run run-all build clean

run:
	go run .

run-all:
	@for pool in 1 2 4 8; do \
		echo "=== Pool Size: $$pool ==="; \
		echo "GOMAXPROCS=$$pool"; \
		GOMAXPROCS=$$pool go run .; \
		echo; \
	done

build:
	go build -ldflags="-s -w" -o bench-go .

clean:
	rm -f bench-go
