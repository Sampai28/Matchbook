#!/usr/bin/env bash
#
# Runs the Matchbook benchmark under the conditions the numbers assume.
#
# RUN THIS NATIVELY UNDER WSL2, NOT IN DOCKER.
#
# Containerised timing on a Windows host adds jitter from the Hyper-V scheduler
# and the 9P/virtiofs filesystem layer. At millisecond resolution that is
# invisible; at the tens-of-nanoseconds resolution this harness works in, it is
# the dominant term. Builds and tests belong in Docker; benchmarks do not.
#
#   ./bench/run_bench.sh [extra matchbook-bench args...]

set -euo pipefail

BIN="${MATCHBOOK_BENCH_BIN:-build/matchbook-bench}"
CORE="${MATCHBOOK_BENCH_CORE:-2}"
OUT="${MATCHBOOK_BENCH_OUT:-bench/results}"

if [ ! -x "$BIN" ]; then
  echo "error: $BIN not found or not executable." >&2
  echo "Build it first:" >&2
  echo "  cmake --preset release && cmake --build --preset release" >&2
  exit 1
fi

mkdir -p "$OUT"

# --- environment warnings -------------------------------------------------
# None of these are fatal. They are printed so that a surprising result can be
# explained afterwards rather than puzzled over.

if [ -r /sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_governor ]; then
  gov="$(cat /sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_governor)"
  if [ "$gov" != "performance" ]; then
    echo "[warn] cpu${CORE} governor is '$gov', not 'performance'."
    echo "       Frequency scaling will widen the tail. To pin it (needs root):"
    echo "         sudo cpupower frequency-set -g performance"
  fi
else
  echo "[note] no cpufreq interface visible (normal under WSL2)."
  echo "       The host governs frequency; absolute figures will drift between runs."
fi

if grep -qi microsoft /proc/version 2>/dev/null; then
  echo "[note] running under WSL2. The TSC is passed through from the host, so"
  echo "       timing works, but the hypervisor may steal time occasionally."
  echo "       That shows up as tail outliers, not as a shifted median."
fi

if ! command -v taskset >/dev/null 2>&1; then
  echo "[warn] taskset not found; the run will not be pinned to a core."
  echo "       Install with: sudo apt-get install util-linux"
  PIN=""
else
  PIN="taskset -c ${CORE}"
fi

echo
echo "running: ${PIN} ${BIN} --out ${OUT} $*"
echo

# shellcheck disable=SC2086
${PIN} "${BIN}" --out "${OUT}" "$@"

echo
echo "Results written to ${OUT}/."
echo "Generate the HTML report with:"
echo "  python3 -m venv .venv && . .venv/bin/activate"
echo "  pip install -r tools/requirements.txt"
echo "  python3 tools/make_report.py"
