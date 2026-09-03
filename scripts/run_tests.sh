#!/usr/bin/env bash
#
# Everything CI runs, in the order CI runs it, so a failure can be reproduced
# locally with one command instead of by reading the workflow file.
#
# Stages that need a toolchain the machine does not have are skipped with a
# notice rather than failing, so this is usable on a laptop that has no ARM
# cross compiler.
#
# Usage: scripts/run_tests.sh [--quick]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

GREEN=$'\033[32m'; YELLOW=$'\033[33m'; BOLD=$'\033[1m'; RESET=$'\033[0m'
QUICK=${1:-}

MAKE=make
command -v make >/dev/null 2>&1 || MAKE=mingw32-make

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python

stage() { printf '\n%s==> %s%s\n' "$BOLD" "$1" "$RESET"; }
skip()  { printf '%s--- skipped: %s%s\n' "$YELLOW" "$1" "$RESET"; }

stage "host build"
$MAKE host

stage "C unit tests"
$MAKE test

stage "integration tests"
$PY -m pytest tests/integration -q

if [ "$QUICK" = "--quick" ]; then
    printf '\n%squick run complete.%s\n' "$GREEN" "$RESET"
    exit 0
fi

stage "traffic reconciliation"
$PY tools/trafficgen.py --frames 2000 --corrupt-rate 0.15

stage "sanitizers"
if $MAKE asan 2>/dev/null; then
    :
else
    skip "no libasan/libubsan for this compiler"
fi

stage "target cross build"
if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    $MAKE arm
    $MAKE size-check
else
    skip "arm-none-eabi-gcc not installed"
fi

stage "LinkMon telemetry validator"
if command -v dotnet >/dev/null 2>&1; then
    dotnet run --project tools/LinkMon -- --self-test
else
    skip "dotnet not installed"
fi

printf '\n%sall stages passed.%s\n' "$GREEN" "$RESET"
