#!/usr/bin/env bash
# Runs clang-tidy over project sources using a build directory's compile_commands.json.
# Usage: scripts/lint.sh [build-dir]   (default: build/dev; configure it with Clang for best results)
set -euo pipefail

cd "$(dirname "$0")/.."
build_dir="${1:-build/dev}"
runner="${RUN_CLANG_TIDY:-$(command -v run-clang-tidy-18 || command -v run-clang-tidy)}"

# Anchored at the repo root so fetched dependencies under build/_deps are never analysed.
"$runner" -quiet -p "$build_dir" "^$PWD/(src|tests)/"
