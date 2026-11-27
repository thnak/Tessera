// SPDX-License-Identifier: MIT
//
// Glass runtime-config keys (issue #244, docs/specs/runtime-config-v1.md section 3):
// `glassMaxBlurRadius`, `glassMaxBlurTargets` and `glassMaxBackdropArea` parse, default and
// validate with the same strict rules as every other field.

#include "tessera/runtime/config.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::parse_runtime_config;
using tessera::RuntimeConfig;
using tessera::RuntimeConfigErrorCode;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glass_config: " << message << "\n";
    }
    return condition;
}

bool same_defaults(const RuntimeConfig& config) {
    return config.glass_max_blur_radius == 64.0f && config.glass_max_blur_targets == 4 &&
           config.glass_max_backdrop_area == 4'000'000;
}

bool rejects(std::string_view json, const char* field, RuntimeConfigErrorCode code) {
    const auto parsed = parse_runtime_config(json);
    return !parsed.has_value() && parsed.error().field == field && parsed.error().code == code;
}

} // namespace

int main() {
    bool ok = true;

    const auto defaults = parse_runtime_config("{}");
    ok = expect(defaults.has_value() && same_defaults(*defaults),
                "the documented defaults are 64 / 4 / 4,000,000") &&
         ok;

    const auto configured =
        parse_runtime_config(R"({"runtime":{"glassMaxBlurRadius":32,"glassMaxBlurTargets":2,)"
                             R"("glassMaxBackdropArea":1000000}})");
    ok = expect(configured.has_value(), "the glass keys parse") && ok;
    if (configured.has_value()) {
        ok = expect(configured->glass_max_blur_radius == 32.0f, "glassMaxBlurRadius is read") && ok;
        ok = expect(configured->glass_max_blur_targets == 2, "glassMaxBlurTargets is read") && ok;
        ok = expect(configured->glass_max_backdrop_area == 1'000'000,
                    "glassMaxBackdropArea is read") &&
             ok;
    }

    // 0 is legal for every key: a zero radius, zero targets or a zero area only bounds cost.
    const auto zero = parse_runtime_config(
        R"({"runtime":{"glassMaxBlurRadius":0,"glassMaxBlurTargets":0,"glassMaxBackdropArea":0}})");
    ok = expect(zero.has_value() && zero->glass_max_blur_targets == 0,
                "zero disables the bounded resources instead of being an error") &&
         ok;

    ok = expect(rejects(R"({"runtime":{"glassMaxBlurRadius":300}})", "glassMaxBlurRadius",
                        RuntimeConfigErrorCode::OutOfRange),
                "a radius over the range is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"glassMaxBlurRadius":-1}})", "glassMaxBlurRadius",
                        RuntimeConfigErrorCode::OutOfRange),
                "a negative radius is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"glassMaxBlurTargets":17}})", "glassMaxBlurTargets",
                        RuntimeConfigErrorCode::OutOfRange),
                "too many blur targets is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"glassMaxBlurTargets":1.5}})", "glassMaxBlurTargets",
                        RuntimeConfigErrorCode::InvalidType),
                "a fractional blur-target count is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"glassMaxBackdropArea":-1}})", "glassMaxBackdropArea",
                        RuntimeConfigErrorCode::OutOfRange),
                "a negative backdrop area is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"glassMaxBlurRadius":"64"}})", "glassMaxBlurRadius",
                        RuntimeConfigErrorCode::InvalidType),
                "a string radius is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"glassMaxBlurTarget":4}})", "glassMaxBlurTarget",
                        RuntimeConfigErrorCode::UnknownField),
                "a misspelled key is still an unknown field") &&
         ok;

    // The offscreen pool memory keys: defaults match the engine's old fixed values.
    ok = expect(defaults.has_value() && defaults->offscreen_max_mb == 256 &&
                    defaults->offscreen_idle_seconds == 2.0,
                "offscreenMaxMb / offscreenIdleSeconds default to 256 / 2") &&
         ok;
    const auto pool =
        parse_runtime_config(R"({"runtime":{"offscreenMaxMb":512,"offscreenIdleSeconds":0.5}})");
    ok = expect(pool.has_value() && pool->offscreen_max_mb == 512 &&
                    pool->offscreen_idle_seconds == 0.5,
                "the offscreen pool keys are read") &&
         ok;
    const auto pool_edges =
        parse_runtime_config(R"({"runtime":{"offscreenMaxMb":4096,"offscreenIdleSeconds":0}})");
    ok = expect(pool_edges.has_value() && pool_edges->offscreen_idle_seconds == 0.0,
                "the range edges are accepted") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"offscreenMaxMb":0}})", "offscreenMaxMb",
                        RuntimeConfigErrorCode::OutOfRange),
                "a zero byte cap is an error (it would mean unbounded)") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"offscreenMaxMb":4097}})", "offscreenMaxMb",
                        RuntimeConfigErrorCode::OutOfRange),
                "a byte cap over 4096 MiB is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"offscreenMaxMb":1.5}})", "offscreenMaxMb",
                        RuntimeConfigErrorCode::InvalidType),
                "a fractional byte cap is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"offscreenIdleSeconds":-1}})", "offscreenIdleSeconds",
                        RuntimeConfigErrorCode::OutOfRange),
                "a negative idle time is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"offscreenIdleSeconds":3601}})", "offscreenIdleSeconds",
                        RuntimeConfigErrorCode::OutOfRange),
                "an idle time over an hour is an error") &&
         ok;
    ok = expect(rejects(R"({"runtime":{"offscreenIdleSeconds":"2"}})", "offscreenIdleSeconds",
                        RuntimeConfigErrorCode::InvalidType),
                "a string idle time is an error") &&
         ok;

    if (!ok) {
        std::cerr << "glass_config: FAILED\n";
        return 1;
    }
    std::cout << "glass_config: ok\n";
    return 0;
}
