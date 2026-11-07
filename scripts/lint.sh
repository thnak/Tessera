#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-build}"
clang_tidy="${CLANG_TIDY:-clang-tidy}"

if ! command -v "$clang_tidy" >/dev/null 2>&1; then
    echo "clang-tidy not found (set CLANG_TIDY)" >&2
    exit 1
fi

if [[ ! -f "$root/$build_dir/compile_commands.json" ]]; then
    echo "compile_commands.json not found in $build_dir (use the Ninja preset)" >&2
    exit 1
fi

failed=0
count=0
while IFS= read -r -d '' file; do
    count=$((count + 1))
    if ! "$clang_tidy" -p "$root/$build_dir" --quiet "$file"; then
        failed=$((failed + 1))
    fi
done < <(find "$root" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
    ! -path '*/build/*' ! -path '*/third_party/stb/*' \
    ! -path '*/third_party/volk/*' ! -path '*/third_party/vma/*' \
    ! -path '*/.git/*' -print0)

if (( failed > 0 )); then
    echo "$failed file(s) reported clang-tidy issues" >&2
    exit 1
fi

echo "clang-tidy checked $count file(s)."
