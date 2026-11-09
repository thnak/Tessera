#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
clang_format="${CLANG_FORMAT:-clang-format}"

if ! command -v "$clang_format" >/dev/null 2>&1; then
    echo "clang-format not found (set CLANG_FORMAT)" >&2
    exit 1
fi

# Warn (but do not fail) when the formatter differs from the pinned version; the format-version gate
# in scripts/gates.py is the enforcement point.
version_gate="$root/scripts/check_format_version.py"
if command -v python3 >/dev/null 2>&1 && [[ -f "$version_gate" ]]; then
    if ! CLANG_FORMAT="$clang_format" python3 -B "$version_gate" >/dev/null 2>&1; then
        echo "warning: clang-format does not match tools/format-version.txt;" >&2
        echo "         run: python3 -B scripts/check_format_version.py" >&2
    fi
fi

check=0
if [[ "${1:-}" == "--check" ]]; then
    check=1
fi

failed=0
count=0
while IFS= read -r -d '' file; do
    count=$((count + 1))
    if (( check )); then
        if ! "$clang_format" --dry-run --Werror --style=file "$file"; then
            failed=$((failed + 1))
        fi
    else
        if ! "$clang_format" -i --style=file "$file"; then
            failed=$((failed + 1))
        fi
    fi
done < <(find "$root" -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' -o -name '*.cxx' \) \
    ! -path '*/build/*' ! -path '*/tests/golden/*' ! -path '*/third_party/stb/*' \
    ! -path '*/third_party/volk/*' ! -path '*/third_party/vma/*' \
    ! -path '*/third_party/yoga/*' \
    ! -path '*/.git/*' -print0)

if (( failed > 0 )); then
    echo "$failed file(s) failed formatting" >&2
    exit 1
fi

echo "Formatted/checked $count file(s)."
