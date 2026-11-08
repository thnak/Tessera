#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Runs the Android unit/null-GPU test executables on the emulator started by the CI action.
# The action executes its `script` input line by line, so keep the logic in a real shell script.

set -euo pipefail

test_dir=/data/local/tmp/tessera
adb shell rm -rf "$test_dir"
adb shell mkdir -p "$test_dir"

for binary in build/android-tests/tests/unit/*_tests build/android-tests/tests/gpu/*_tests; do
    adb push "$binary" "$test_dir/"
done
adb shell chmod +x "$test_dir/*_tests"

# Font tests need a fixture path that does not exist in the device sandbox; the host platforms
# cover them.
adb shell 'cd /data/local/tmp/tessera && fail=0; for t in *_tests; do case "$t" in font_face_tests|freetype_metrics_tests|glyph_raster_tests|glyph_atlas_tests|harfbuzz_shaper_tests|text_render_tests) continue;; esac; echo "== $t"; ./$t || fail=1; done; exit $fail'
