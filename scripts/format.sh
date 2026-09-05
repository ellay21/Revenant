#!/usr/bin/env bash
# Formats all C++ sources with the pinned clang-format, or checks them with --check.
set -euo pipefail

readonly version="18.1.8"
cd "$(dirname "$0")/.."

if [[ -n "${CLANG_FORMAT:-}" ]]; then
  read -r -a formatter <<<"$CLANG_FORMAT"
elif command -v clang-format >/dev/null && clang-format --version | grep -q "version ${version}"; then
  formatter=(clang-format)
else
  formatter=(pipx run --spec "clang-format==${version}" clang-format)
fi

mapfile -t files < <(git ls-files --cached --others --exclude-standard -- '*.hpp' '*.cpp')
[[ ${#files[@]} -eq 0 ]] && exit 0

if [[ "${1:-}" == "--check" ]]; then
  "${formatter[@]}" --dry-run --Werror "${files[@]}"
else
  "${formatter[@]}" -i "${files[@]}"
fi
