#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Runs the Android unit/null-GPU test executables on the emulator started by the CI action.
# The action executes its `script` input line by line, so keep the logic in a real shell script.
#
# The emulator runner can start this script before the adb server/device is reachable; every adb
# call then fails with "Unable to connect to adb daemon on port: 5037". The readiness handshake
# below (server restart, wait-for-device, boot-completed) is bounded and reports diagnostics.

set -euo pipefail

log() { printf '[android-emulator] %s\n' "$*"; }

# Resolve the SDK/platform-tools even when the action did not export them into this shell.
sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/usr/local/lib/android/sdk}}"
if [[ -d "$sdk_root/platform-tools" ]]; then
    export ANDROID_SDK_ROOT="$sdk_root"
    export ANDROID_HOME="$sdk_root"
    export PATH="$sdk_root/platform-tools:$PATH"
fi

ADB_START_ATTEMPTS="${ADB_START_ATTEMPTS:-10}"
ADB_BOOT_TIMEOUT="${ADB_BOOT_TIMEOUT:-600}"
ADB_STEP_TIMEOUT="${ADB_STEP_TIMEOUT:-60}"
ADB_PUSH_TIMEOUT="${ADB_PUSH_TIMEOUT:-180}"
TEST_TIMEOUT="${TEST_TIMEOUT:-1200}"

if ! command -v adb >/dev/null 2>&1; then
    log "ERROR: adb is not on PATH (ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-unset})"
    exit 1
fi

adb_run() { timeout "$ADB_STEP_TIMEOUT" adb "$@"; }

dump_diagnostics() {
    log "adb: $(command -v adb)"
    log "ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-unset} ANDROID_HOME=${ANDROID_HOME:-unset}"
    timeout 15 adb devices -l || true
    timeout 15 adb shell getprop sys.boot_completed || true
    timeout 15 adb shell getprop dev.bootcomplete || true
    timeout 15 adb shell service check package || true
}

die() {
    log "ERROR: $*"
    dump_diagnostics
    exit 1
}

start_adb_server() {
    log "restarting the adb server"
    timeout 15 adb kill-server >/dev/null 2>&1 || true
    local attempt=1
    local output=""
    while (( attempt <= ADB_START_ATTEMPTS )); do
        if output="$(timeout 20 adb start-server 2>&1)"; then
            log "adb server started (attempt ${attempt})"
            return 0
        fi
        log "adb start-server attempt ${attempt} failed: ${output//$'\n'/ }"
        sleep 2
        attempt=$((attempt + 1))
    done
    die "adb server did not start after ${ADB_START_ATTEMPTS} attempts"
}

wait_for_device() {
    local attempt=1
    while (( attempt <= 30 )); do
        if timeout 20 adb wait-for-device shell true >/dev/null 2>&1; then
            log "emulator device is connected (attempt ${attempt})"
            return 0
        fi
        sleep 2
        attempt=$((attempt + 1))
    done
    die "no emulator device became available within the wait-for-device retries"
}

wait_for_boot() {
    local deadline=$((SECONDS + ADB_BOOT_TIMEOUT))
    local booted=""
    local devboot=""
    local package=""
    while (( SECONDS < deadline )); do
        booted="$(timeout 15 adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r\n ' || true)"
        devboot="$(timeout 15 adb shell getprop dev.bootcomplete 2>/dev/null | tr -d '\r\n ' || true)"
        package="$(timeout 15 adb shell service check package 2>/dev/null | tr -d '\r' || true)"
        if [[ "$booted" == "1" && ( "$devboot" == "1" || "$package" == *found* ) ]]; then
            log "emulator boot completed (sys.boot_completed=${booted} dev.bootcomplete=${devboot:-unset})"
            return 0
        fi
        sleep 5
    done
    die "emulator did not finish booting within ${ADB_BOOT_TIMEOUT}s (sys.boot_completed=${booted:-?}, dev.bootcomplete=${devboot:-?}, package=${package:-?})"
}

start_adb_server
wait_for_device
wait_for_boot
adb_run shell getprop ro.build.version.sdk || true

test_dir=/data/local/tmp/tessera
adb_run shell rm -rf "$test_dir"
adb_run shell mkdir -p "$test_dir"

pushed=0
for binary in build/android-tests/tests/unit/*_tests build/android-tests/tests/gpu/*_tests; do
    [[ -f "$binary" ]] || die "test binary not found: $binary"
    timeout "$ADB_PUSH_TIMEOUT" adb push "$binary" "$test_dir/"
    pushed=$((pushed + 1))
done
[[ "$pushed" -gt 0 ]] || die "no test executables found under build/android-tests/tests"

adb_run shell chmod +x "$test_dir/*_tests"

# Font tests need a fixture path; the IR golden and the IR and rules layering tests read repo files.
# None of these exist in the device sandbox; the host platforms cover them.
if ! timeout "$TEST_TIMEOUT" adb shell 'cd /data/local/tmp/tessera && fail=0; for t in *_tests; do case "$t" in font_face_tests|freetype_metrics_tests|glyph_raster_tests|glyph_atlas_tests|harfbuzz_shaper_tests|text_render_tests|ir_json_tests|ir_layering_tests|rules_layering_tests) continue;; esac; echo "== $t"; ./$t || fail=1; done; exit $fail'; then
    dump_diagnostics
    exit 1
fi

log "all Android test executables passed on the emulator"
