# Matchbook — convenience wrappers around CMake, Docker and the Python tooling.
#
# NOTHING HERE HAS BEEN RUN. Every target is written to be correct on inspection
# and is unverified. See docs/BUILD_NOTES.md before the first build.
#
# Two distinct workflows, deliberately separate:
#
#   BUILD AND TEST IN DOCKER (make docker-build, make docker-test)
#     Reproducible, and pins gcc 13 to match WSL's g++ 13.3.0.
#
#   BENCHMARK NATIVELY IN WSL2 (make bench)
#     Containerised timing on a Windows host carries jitter from the Hyper-V
#     scheduler and the filesystem shim. At millisecond resolution that is
#     invisible; at the tens-of-nanoseconds resolution this harness works in, it
#     is the dominant term.

SHELL       := /bin/bash
BUILD_DIR   := build
PRESET      := release
PYTHON      ?= python3
FLOW        ?= /tmp/matchbook_flow.csv
FLOW_COUNT  ?= 50000
FLOW_SEED   ?= 42
ENGINES     := v0 v1 v2 v3

.DEFAULT_GOAL := help

## help: list targets
help:
	@echo "Matchbook targets:"
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/## /  /'
	@echo ""
	@echo "Status: generated but never compiled. Start with docs/BUILD_NOTES.md."

## build: configure and compile (release)
build:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET) -j

## test: run the Catch2 suite via CTest
test: build
	ctest --preset $(PRESET)

## diff-test: differential oracle — Python reference vs all four C++ engines
diff-test: build
	@echo "==> generating $(FLOW_COUNT) operations (seed $(FLOW_SEED))"
	$(PYTHON) tools/flowgen.py --count $(FLOW_COUNT) --seed $(FLOW_SEED) --out $(FLOW)
	@echo "==> running the Python reference matcher (this is the slow one)"
	$(PYTHON) tools/reference_matcher.py --input $(FLOW) --output /tmp/mb_ref.log \
		--final-book /tmp/mb_ref.book
	@set -e; for v in $(ENGINES); do \
		echo "==> replaying through $$v"; \
		./$(BUILD_DIR)/matchbook-replay --engine $$v --input $(FLOW) \
			--output /tmp/mb_$$v.log --final-book /tmp/mb_$$v.book; \
		echo "==> diffing $$v against the reference"; \
		$(PYTHON) tools/validate_fills.py /tmp/mb_ref.log /tmp/mb_$$v.log; \
		diff -u /tmp/mb_ref.book /tmp/mb_$$v.book > /dev/null \
			|| { echo "FINAL BOOK STATE DIFFERS for $$v"; \
			     diff -u /tmp/mb_ref.book /tmp/mb_$$v.book | head -40; exit 1; }; \
	done
	@echo ""
	@echo "All four engines match the Python reference on events and final state."

## replay-diff: determinism — the fixture must replay identically every time
replay-diff: build
	@set -e; for v in $(ENGINES); do \
		./$(BUILD_DIR)/matchbook-replay --engine $$v \
			--input tests/fixtures/recorded_stream.csv --output /tmp/mb_fix_$$v.log; \
		./$(BUILD_DIR)/matchbook-replay --engine $$v \
			--input tests/fixtures/recorded_stream.csv --output /tmp/mb_fix_$${v}_again.log; \
		cmp -s /tmp/mb_fix_$$v.log /tmp/mb_fix_$${v}_again.log \
			|| { echo "$$v is NOT deterministic across runs"; exit 1; }; \
	done
	@set -e; for v in v1 v2 v3; do \
		cmp -s /tmp/mb_fix_v0.log /tmp/mb_fix_$$v.log \
			|| { echo "$$v diverges from v0:"; \
			     $(PYTHON) tools/validate_fills.py /tmp/mb_fix_v0.log /tmp/mb_fix_$$v.log; \
			     exit 1; }; \
	done
	@echo "Replay is byte-identical across runs and across all four engines."

## fuzz: run the validation-layer fuzz target
fuzz: build
	./$(BUILD_DIR)/matchbook-fuzz --iterations 5000

## bench: run the benchmark harness NATIVELY (never in Docker)
bench: build
	@if [ -f /.dockerenv ]; then \
		echo "ERROR: you are inside a container."; \
		echo "Benchmarks must run natively under WSL2 — containerised timing on a"; \
		echo "Windows host is too noisy for nanosecond-resolution measurement."; \
		exit 1; \
	fi
	bash bench/run_bench.sh

## report: build bench/report.html from bench/results/
report:
	$(PYTHON) tools/make_report.py

## serve: run the HTTP server and ladder viewer on :8080
serve: build
	./$(BUILD_DIR)/matchbook-server --port 8080 --web web --engine v3

## docker-build: build the image (gcc 13, matching WSL's g++ 13.3.0)
docker-build:
	docker build -f docker/Dockerfile -t matchbook:latest .

## docker-test: build and run the full test suite inside the container
docker-test:
	docker build -f docker/Dockerfile --target test -t matchbook:test .
	docker run --rm matchbook:test

## docker-serve: run the server in a container on :8080
docker-serve: docker-build
	docker run --rm -p 8080:8080 matchbook:latest

## asan: build and test under AddressSanitizer and UBSan
asan:
	cmake --preset asan
	cmake --build --preset asan -j
	ctest --preset asan

## clean: remove all build directories
clean:
	rm -rf build build-debug build-asan build-native

## clean-results: remove benchmark results and the generated report
clean-results:
	rm -f bench/results/*.txt bench/report.html

.PHONY: help build test diff-test replay-diff fuzz bench report serve \
        docker-build docker-test docker-serve asan clean clean-results
