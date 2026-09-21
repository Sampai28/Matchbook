# Matchbook build and test image.
#
# Base is gcc:13, NOT gcc:14, so the containerised compiler matches the native
# g++ 13.3.0 in WSL2. A code generation difference between the container and the
# host would mean the tests validate a different binary from the one being
# benchmarked, which quietly defeats the purpose of having both.
#
# BUILD AND TEST HERE. DO NOT BENCHMARK HERE. Containerised timing on a Windows
# host inherits jitter from the Hyper-V scheduler and the filesystem shim; at
# the tens-of-nanoseconds resolution the harness works in, that jitter is the
# dominant term. Run bench/run_bench.sh natively under WSL2 instead.

# --------------------------------------------------------------------------
# Stage 1: build
# --------------------------------------------------------------------------
FROM gcc:13 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake \
        ninja-build \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the build definitions first so that editing a source file does not
# invalidate the layer that fetched Catch2 and cpp-httplib.
COPY CMakeLists.txt CMakePresets.json ./
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/
COPY bench/ ./bench/

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    && cmake --build build -j"$(nproc)"

# --------------------------------------------------------------------------
# Stage 2: test — `docker build --target test` runs the suite
# --------------------------------------------------------------------------
FROM build AS test

COPY tools/ ./tools/

# python3 for the differential oracle. No pip install: flowgen.py,
# reference_matcher.py and validate_fills.py are standard-library only, and only
# the report generator needs plotly, which is not part of testing.
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 \
    && rm -rf /var/lib/apt/lists/*

CMD set -e; \
    echo "=== unit tests ==="; \
    ctest --test-dir build --output-on-failure; \
    echo "=== replay determinism ==="; \
    for v in v0 v1 v2 v3; do \
        ./build/matchbook-replay --engine $v \
            --input tests/fixtures/recorded_stream.csv --output /tmp/$v.log; \
    done; \
    for v in v1 v2 v3; do \
        cmp /tmp/v0.log /tmp/$v.log || { \
            echo "$v diverges from v0"; \
            python3 tools/validate_fills.py /tmp/v0.log /tmp/$v.log; exit 1; }; \
    done; \
    echo "=== differential oracle (10k operations) ==="; \
    python3 tools/flowgen.py --count 10000 --seed 42 --out /tmp/flow.csv; \
    python3 tools/reference_matcher.py --input /tmp/flow.csv --output /tmp/ref.log; \
    for v in v0 v1 v2 v3; do \
        ./build/matchbook-replay --engine $v --input /tmp/flow.csv --output /tmp/eng_$v.log; \
        python3 tools/validate_fills.py /tmp/ref.log /tmp/eng_$v.log; \
    done; \
    echo "=== fuzz (short) ==="; \
    ./build/matchbook-fuzz --iterations 500; \
    echo "ALL CHECKS PASSED"

# --------------------------------------------------------------------------
# Stage 3: runtime — just the server and the static viewer
# --------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

# cpp-httplib feature-detects brotli, zlib and OpenSSL at configure time and
# links whatever it finds in the gcc:13 build image. The slim runtime has none
# of them, so without this the binary dies at startup with
#   error while loading shared libraries: libbrotlicommon.so.1
# which reads like a build problem and is really a missing runtime package.
#
# `ldd build/matchbook-server` in the build stage is the way to check this list
# after changing anything about how httplib is configured.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        ca-certificates \
        libbrotli1 \
        zlib1g \
        libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --shell /usr/sbin/nologin matchbook

WORKDIR /app

# GCC 13's libstdc++, copied from the build image.
#
# Debian bookworm ships GCC 12's, which stops at GLIBCXX_3.4.30. A binary
# compiled by gcc:13 needs 3.4.31 and 3.4.32, so the stock one fails at load
# with "version `GLIBCXX_3.4.32' not found" — a message that points at the
# runtime image while the cause is the compiler choice.
#
# Copying the library beats pinning the build to GCC 12: the whole reason the
# build image is gcc:13 is to match WSL2's native g++ 13.3.0, so that the
# tested binary and the benchmarked binary come from the same compiler.
COPY --from=build /usr/local/lib64/libstdc++.so.6* /usr/local/lib64/
ENV LD_LIBRARY_PATH=/usr/local/lib64

COPY --from=build /src/build/matchbook-server /app/matchbook-server
COPY web/ /app/web/

USER matchbook
EXPOSE 8080

# The engine is single-threaded and the server runs one worker. Scaling this
# container horizontally gives you N independent books, not one shared book.
ENTRYPOINT ["/app/matchbook-server", "--host", "0.0.0.0", "--port", "8080", "--web", "/app/web"]
