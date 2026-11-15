#!/bin/bash
# Linux verification for issue #187 on the build server. Run inside ~/lane-187.
set -uo pipefail
cd "$HOME/lane-187" || exit 1
cmake --build --preset dev --parallel 24 2>&1 | tail -60
exit "${PIPESTATUS[0]}"
