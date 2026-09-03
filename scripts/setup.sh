#!/usr/bin/env bash
#
# Prepares a machine to build and test OptiLink.
#
# Reports what is present and what is missing rather than installing things
# behind your back, except for the Python test dependencies, which go into a
# virtual environment inside the repo where they cannot disturb anything else.
#
# Usage: scripts/setup.sh [--venv]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RESET=$'\033[0m'
missing_required=0

have() { command -v "$1" >/dev/null 2>&1; }

check() {
    local tool="$1" required="$2" purpose="$3"
    if have "$tool"; then
        printf '  %sok%s   %-22s %s\n' "$GREEN" "$RESET" "$tool" "$(command -v "$tool")"
    elif [ "$required" = required ]; then
        printf '  %smiss%s %-22s %s\n' "$RED" "$RESET" "$tool" "$purpose"
        missing_required=$((missing_required + 1))
    else
        printf '  %sskip%s %-22s %s\n' "$YELLOW" "$RESET" "$tool" "$purpose"
    fi
}

echo "toolchain:"
check "${CC:-gcc}"       required "host build and unit tests"
check make               required "build driver"
check python3            optional "test harness (python also accepted)"
check python             optional "test harness"
check arm-none-eabi-gcc  optional "cross build for the target; host tests run without it"
check arm-none-eabi-size optional "footprint report"
check dotnet             optional "the LinkMon telemetry validator"

if [ "$missing_required" -gt 0 ]; then
    echo
    echo "${RED}$missing_required required tool(s) missing.${RESET}"
    echo "Debian/Ubuntu:  sudo apt-get install build-essential gcc-arm-none-eabi python3-pip"
    echo "macOS:          brew install make python && brew install --cask gcc-arm-embedded"
    exit 1
fi

PY=python3
have python3 || PY=python

if [ "${1:-}" = "--venv" ]; then
    echo
    echo "creating .venv and installing test dependencies"
    "$PY" -m venv .venv
    # shellcheck disable=SC1091
    if [ -f .venv/bin/activate ]; then . .venv/bin/activate; else . .venv/Scripts/activate; fi
    python -m pip install --quiet --upgrade pip
    python -m pip install --quiet -r requirements.txt
    echo "activate it with: . .venv/bin/activate"
else
    echo
    echo "installing test dependencies for $PY"
    "$PY" -m pip install --quiet --user -r requirements.txt || {
        echo "${YELLOW}pip install failed; re-run with --venv to use a virtual environment${RESET}"
    }
fi

echo
echo "${GREEN}ready.${RESET}  make test && make pytest"
