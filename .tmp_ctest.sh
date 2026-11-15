#!/bin/bash
# Full Linux ctest for issue #187. Run inside ~/lane-187.
cd "$HOME/lane-187" || exit 1
ctest --preset dev 2>&1 | tail -40
