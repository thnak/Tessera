// SPDX-License-Identifier: MIT

#include "tessera/runtime/config.hpp"

#include <cmath>
#include <cstddef>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using tessera::DirtyMode;
using tessera::FramePacing;
using tessera::parse_runtime_config;
using tessera::PowerPolicy;
using tessera::RefreshMode;
using tessera::ResizeMode;
using tessera::RuntimeConfig;
using tessera::RuntimeConfigError;
using tessera::RuntimeConfigErrorCode;
using tessera::VsyncMode;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "runtime_config: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected) {
    return std::abs(value - expected) <= 1.0e-9;
}

std::expected<RuntimeConfig, RuntimeConfigError> parse_one(std::string_view field,
                                                           std::string_view literal) {
    return parse_runtime_config("{\"runtime\":{\"" + std::string(field) +
                                "\":" + std::string(literal) + "}}");
}

// The documented section 3 defaults, written out positionally in struct order. This is the
// drift guard: changing a default in the header without changing the spec fails the comparisons
// that use it.
constexpr RuntimeConfig kDocumentedDefaults{60.0,
                                            0,
                                            VsyncMode::On,
                                            RefreshMode::Auto,
                                            0,
                                            FramePacing::Auto,
                                            2,
                                            false,
                                            250.0,
                                            5,
                                            1.0f,
                                            true,
                                            PowerPolicy::Adaptive,
                                            true,
                                            0.6f,
                                            0.0,
                                            true,
                                            false,
                                            0,
                                            ResizeMode::Adaptive,
                                            16.0,
                                            16.0,
                                            1,
                                            4,
                                            0,
                                            1,
                                            4096,
                                            2.0,
                                            false,
                                            DirtyMode::Auto,
                                            true,
                                            16,
                                            0.6f,
                                            64,
                                            32,
                                            64,
                                            2,
                                            true,
                                            0.0625f,
                                            false};

bool expect_defaults(const RuntimeConfig& config) {
    return expect(config == kDocumentedDefaults, "documented defaults");
}

// Readers for the numeric fields, in the same order as kNumericBoundaries below. A reordered or
// missing entry surfaces as a failed value assertion, never a silent wrong-field check.
constexpr double (*kNumericReaders[])(const RuntimeConfig&) = {
    [](const RuntimeConfig& c) { return static_cast<double>(c.fixed_timestep_hz); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.max_fps); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.preferred_refresh_rate); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.latency_frames); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.max_frame_delta_ms); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.max_updates_per_frame); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.render_scale); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.min_render_scale); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.frame_budget_ms); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.background_max_fps); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.resize_debounce_ms); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.swapchain_recreate_delay_ms); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.max_pending_resizes); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.layout_observation_max_rounds); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.async_worker_threads); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.async_io_threads); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.async_queue_capacity); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.async_main_thread_budget_ms); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.max_damage_rects); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.full_redraw_threshold); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.layer_cache_budget_mb); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.draw_list_cache_budget_mb); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.view_transition_max_mb); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.gpu_frames_in_flight); },
    [](const RuntimeConfig& c) { return static_cast<double>(c.interpolation_epsilon_px); },
};

double numeric_field(const RuntimeConfig& config, std::size_t index) {
    return kNumericReaders[index](config);
}

struct NumericBoundary {
    const char* field;
    const char* min_literal;
    const char* max_literal;
    const char* below_literal;
    const char* above_literal;
    double min_value;
    double max_value;
    bool clamps;
    // Extra `"key":value` members needed to satisfy a cross-field invariant (`minRenderScale <=
    // renderScale`): a config that lowers renderScale below the default 0.6 must lower
    // minRenderScale too, or it is a CrossField error by design.
    const char* companion = nullptr;
};

// One row per documented numeric field (section 3 table): min, max, just below min and just above
// max. Only renderScale and layoutObservationMaxRounds carry the table's "Out-of-range values
// clamp" wording; every other row must error.
constexpr NumericBoundary kNumericBoundaries[] = {
    {"fixedTimestepHz", "1", "1000", "0.5", "1000.5", 1.0, 1000.0, false},
    {"maxFps", "0", "1000", "-1", "1001", 0.0, 1000.0, false},
    {"preferredRefreshRate", "0", "1000", "-1", "1001", 0.0, 1000.0, false},
    {"latencyFrames", "1", "3", "0", "4", 1.0, 3.0, false},
    {"maxFrameDeltaMs", "1", "5000", "0.5", "5000.5", 1.0, 5000.0, false},
    {"maxUpdatesPerFrame", "1", "16", "0", "17", 1.0, 16.0, false},
    {"renderScale", "0.25", "4", "0.1", "4.5", 0.25, 4.0, true, "\"minRenderScale\":0.25"},
    {"minRenderScale", "0.25", "1", "0.1", "1.5", 0.25, 1.0, false},
    {"frameBudgetMs", "0", "1000", "-0.5", "1000.5", 0.0, 1000.0, false},
    {"backgroundMaxFps", "0", "1000", "-1", "1001", 0.0, 1000.0, false},
    {"resizeDebounceMs", "0", "1000", "-0.5", "1000.5", 0.0, 1000.0, false},
    {"swapchainRecreateDelayMs", "0", "1000", "-0.5", "1000.5", 0.0, 1000.0, false},
    {"maxPendingResizes", "1", "16", "0", "17", 1.0, 16.0, false},
    {"layoutObservationMaxRounds", "1", "16", "0", "17", 1.0, 16.0, true},
    {"asyncWorkerThreads", "0", "256", "-1", "257", 0.0, 256.0, false},
    {"asyncIoThreads", "1", "16", "0", "17", 1.0, 16.0, false},
    {"asyncQueueCapacity", "64", "1048576", "63", "1048577", 64.0, 1048576.0, false},
    {"asyncMainThreadBudgetMs", "0", "16", "-0.5", "16.5", 0.0, 16.0, false},
    {"maxDamageRects", "1", "64", "0", "65", 1.0, 64.0, false},
    {"fullRedrawThreshold", "0", "1", "-0.5", "1.5", 0.0, 1.0, false},
    {"layerCacheBudgetMb", "0", "4096", "-1", "4097", 0.0, 4096.0, false},
    {"drawListCacheBudgetMb", "0", "4096", "-1", "4097", 0.0, 4096.0, false},
    {"viewTransitionMaxMb", "0", "4096", "-1", "4097", 0.0, 4096.0, false},
    {"gpuFramesInFlight", "1", "3", "0", "4", 1.0, 3.0, false},
    {"interpolationEpsilonPx", "0", "4", "-0.5", "4.5", 0.0, 4.0, false},
};

std::expected<RuntimeConfig, RuntimeConfigError> parse_boundary(const NumericBoundary& boundary,
                                                                std::string_view literal) {
    std::string document =
        "{\"runtime\":{\"" + std::string(boundary.field) + "\":" + std::string(literal);
    if (boundary.companion != nullptr) {
        document += ",";
        document += boundary.companion;
    }
    document += "}}";
    return parse_runtime_config(document);
}

bool expect_boundary_error(const std::expected<RuntimeConfig, RuntimeConfigError>& result,
                           const NumericBoundary& boundary) {
    return expect(!result.has_value(), std::string(boundary.field) + ": expected an error") &&
           expect(result.error().code == RuntimeConfigErrorCode::OutOfRange,
                  std::string(boundary.field) + ": expected OutOfRange") &&
           expect(result.error().field == boundary.field,
                  std::string(boundary.field) + ": error names the field");
}

bool check_numeric_boundaries() {
    std::size_t index = 0;
    for (const NumericBoundary& boundary : kNumericBoundaries) {
        const auto min_parsed = parse_boundary(boundary, boundary.min_literal);
        if (!expect(min_parsed.has_value() &&
                        near(numeric_field(*min_parsed, index), boundary.min_value),
                    std::string(boundary.field) + ": minimum accepted")) {
            return false;
        }
        const auto max_parsed = parse_boundary(boundary, boundary.max_literal);
        if (!expect(max_parsed.has_value() &&
                        near(numeric_field(*max_parsed, index), boundary.max_value),
                    std::string(boundary.field) + ": maximum accepted")) {
            return false;
        }
        const auto below = parse_boundary(boundary, boundary.below_literal);
        const auto above = parse_boundary(boundary, boundary.above_literal);
        if (boundary.clamps) {
            if (!expect(below.has_value() && near(numeric_field(*below, index), boundary.min_value),
                        std::string(boundary.field) + ": below minimum clamps") ||
                !expect(above.has_value() && near(numeric_field(*above, index), boundary.max_value),
                        std::string(boundary.field) + ": above maximum clamps")) {
                return false;
            }
        } else if (!expect_boundary_error(below, boundary) ||
                   !expect_boundary_error(above, boundary)) {
            return false;
        }
        ++index;
    }
    // The disjoint 0-or-24..1000 range has an extra hole between the two documented spans.
    const auto hole = parse_one("preferredRefreshRate", "23");
    return expect(!hole.has_value() && hole.error().code == RuntimeConfigErrorCode::OutOfRange,
                  "preferredRefreshRate: 1..23 is out of range");
}

bool check_enum_values() {
    const auto check = [](std::string_view field, const char* literal, auto member, auto expected) {
        const auto parsed = parse_one(field, literal);
        return expect(parsed.has_value() && (*parsed).*member == expected,
                      std::string(field) + ": " + literal);
    };
    const bool ok =
        check("vsync", "\"off\"", &RuntimeConfig::vsync, VsyncMode::Off) &&
        check("vsync", "\"on\"", &RuntimeConfig::vsync, VsyncMode::On) &&
        check("vsync", "\"adaptive\"", &RuntimeConfig::vsync, VsyncMode::Adaptive) &&
        check("vsync", "\"mailbox\"", &RuntimeConfig::vsync, VsyncMode::Mailbox) &&
        check("dynamicRefreshRate", "\"off\"", &RuntimeConfig::dynamic_refresh_rate,
              RefreshMode::Off) &&
        check("dynamicRefreshRate", "\"auto\"", &RuntimeConfig::dynamic_refresh_rate,
              RefreshMode::Auto) &&
        check("dynamicRefreshRate", "\"preferred\"", &RuntimeConfig::dynamic_refresh_rate,
              RefreshMode::Preferred) &&
        check("framePacing", "\"auto\"", &RuntimeConfig::frame_pacing, FramePacing::Auto) &&
        check("framePacing", "\"smooth\"", &RuntimeConfig::frame_pacing, FramePacing::Smooth) &&
        check("framePacing", "\"low-latency\"", &RuntimeConfig::frame_pacing,
              FramePacing::LowLatency) &&
        check("framePacing", "\"power-save\"", &RuntimeConfig::frame_pacing,
              FramePacing::PowerSave) &&
        check("powerPolicy", "\"adaptive\"", &RuntimeConfig::power_policy, PowerPolicy::Adaptive) &&
        check("powerPolicy", "\"power-save\"", &RuntimeConfig::power_policy,
              PowerPolicy::PowerSave) &&
        check("powerPolicy", "\"balanced\"", &RuntimeConfig::power_policy, PowerPolicy::Balanced) &&
        check("powerPolicy", "\"performance\"", &RuntimeConfig::power_policy,
              PowerPolicy::Performance) &&
        check("powerPolicy", "\"benchmark\"", &RuntimeConfig::power_policy,
              PowerPolicy::Benchmark) &&
        check("dirtyRendering", "\"auto\"", &RuntimeConfig::dirty_rendering, DirtyMode::Auto) &&
        check("dirtyRendering", "\"on\"", &RuntimeConfig::dirty_rendering, DirtyMode::On) &&
        check("dirtyRendering", "\"off\"", &RuntimeConfig::dirty_rendering, DirtyMode::Off) &&
        check("resizeMode", "\"live\"", &RuntimeConfig::resize_mode, ResizeMode::Live) &&
        check("resizeMode", "\"deferred\"", &RuntimeConfig::resize_mode, ResizeMode::Deferred) &&
        check("resizeMode", "\"adaptive\"", &RuntimeConfig::resize_mode, ResizeMode::Adaptive);
    if (!ok) {
        return false;
    }
    const auto unknown = parse_one("vsync", "\"sometimes\"");
    if (!expect(!unknown.has_value() &&
                    unknown.error().code == RuntimeConfigErrorCode::InvalidValue,
                "unknown vsync is InvalidValue") ||
        !expect(unknown.error().field == "vsync", "unknown vsync names the field")) {
        return false;
    }
    const std::string& message = unknown.error().message;
    return expect(message.find("off") != std::string::npos &&
                      message.find("on") != std::string::npos &&
                      message.find("adaptive") != std::string::npos &&
                      message.find("mailbox") != std::string::npos,
                  "unknown vsync message lists the allowed set");
}

bool check_defaults() {
    const auto empty = parse_runtime_config("{}");
    const auto empty_runtime = parse_runtime_config(R"({"runtime":{}})");
    const auto missing = parse_runtime_config(R"({"app":{"namespace":"demo"},"build":{}})");
    const auto null_runtime = parse_runtime_config(R"({"runtime":null})");
    return expect(empty.has_value() && expect_defaults(*empty), "empty object is defaults") &&
           expect(empty_runtime.has_value() && expect_defaults(*empty_runtime),
                  "empty runtime object is defaults") &&
           expect(missing.has_value() && expect_defaults(*missing),
                  "missing runtime is defaults") &&
           expect(null_runtime.has_value() && expect_defaults(*null_runtime),
                  "null runtime is defaults");
}

bool check_section_2_sample() {
    // Verbatim values from docs/specs/runtime-config-v1.md section 2, kept in table order.
    const auto sample = parse_runtime_config(R"({"runtime":{
        "fixedTimestepHz":60,"maxFps":0,"vsync":"on","dynamicRefreshRate":"auto",
        "preferredRefreshRate":0,"framePacing":"auto","latencyFrames":2,"allowTearing":false,
        "maxFrameDeltaMs":250,"maxUpdatesPerFrame":5,"renderScale":1.0,"renderOnDemand":true,
        "powerPolicy":"adaptive","adaptiveQuality":true,"minRenderScale":0.6,
        "frameBudgetMs":0,"pauseWhenOccluded":true,"pauseWhenUnfocused":false,
        "backgroundMaxFps":0,"resizeMode":"adaptive","resizeDebounceMs":16,
        "swapchainRecreateDelayMs":16,"maxPendingResizes":1,"layoutObservationMaxRounds":4,
        "asyncWorkerThreads":0,"asyncIoThreads":1,"asyncQueueCapacity":4096,
        "asyncMainThreadBudgetMs":2.0,"asyncTraceTasks":false,"dirtyRendering":"auto",
        "damageRects":true,"maxDamageRects":16,"fullRedrawThreshold":0.6,
        "layerCacheBudgetMb":64,"drawListCacheBudgetMb":32,"viewTransitionMaxMb":64,
        "gpuFramesInFlight":2,"animationInterpolation":true,"deterministic":false}})");
    if (!expect(sample.has_value(), "section 2 sample parses") || !expect_defaults(*sample)) {
        return false;
    }
    // A non-default override still reads through the same path (the sample itself is all defaults).
    const auto overridden = parse_one("maxFps", "120");
    return expect(overridden.has_value() && overridden->max_fps == 120,
                  "an explicit value overrides the default");
}

bool check_errors() {
    const auto non_finite = parse_one("fixedTimestepHz", "1e400");
    const auto negative_non_finite = parse_one("asyncMainThreadBudgetMs", "-1e400");
    if (!expect(!non_finite.has_value() &&
                    non_finite.error().code == RuntimeConfigErrorCode::NonFinite &&
                    non_finite.error().field == "fixedTimestepHz",
                "1e400 is NonFinite") ||
        !expect(!negative_non_finite.has_value() &&
                    negative_non_finite.error().code == RuntimeConfigErrorCode::NonFinite,
                "-1e400 is NonFinite")) {
        return false;
    }

    struct TypeCase {
        const char* field;
        const char* literal;
    };
    constexpr TypeCase kTypeCases[] = {
        {"maxFps", "60.0"}, {"maxFps", "60.5"}, {"maxFps", "\"60\""},       {"maxFps", "true"},
        {"maxFps", "null"}, {"vsync", "1"},     {"renderScale", "\"1.0\""}, {"damageRects", "1"},
    };
    for (const TypeCase& test : kTypeCases) {
        const auto parsed = parse_one(test.field, test.literal);
        if (!expect(!parsed.has_value() &&
                        parsed.error().code == RuntimeConfigErrorCode::InvalidType &&
                        parsed.error().field == test.field,
                    std::string(test.field) + ": wrong JSON type is InvalidType")) {
            return false;
        }
    }

    const auto not_object = parse_runtime_config(R"({"runtime":[]})");
    const auto array_document = parse_runtime_config("[]");
    if (!expect(!not_object.has_value() &&
                    not_object.error().code == RuntimeConfigErrorCode::InvalidType &&
                    not_object.error().field == "runtime",
                "runtime must be an object") ||
        !expect(!array_document.has_value() &&
                    array_document.error().code == RuntimeConfigErrorCode::InvalidType,
                "document must be an object")) {
        return false;
    }

    const auto unexpected_end = parse_runtime_config(R"({"runtime": {)");
    const auto trailing = parse_runtime_config(R"({"runtime":{}}x)");
    const auto duplicate = parse_runtime_config(R"({"runtime":{"maxFps":1,"maxFps":2}})");
    const auto trailing_comma = parse_runtime_config(R"({"runtime":{"maxFps":1,}})");
    if (!expect(!unexpected_end.has_value() &&
                    unexpected_end.error().code == RuntimeConfigErrorCode::MalformedJson &&
                    unexpected_end.error().offset == 13,
                "unexpected end reports its offset") ||
        !expect(!trailing.has_value() &&
                    trailing.error().code == RuntimeConfigErrorCode::MalformedJson &&
                    trailing.error().offset == 14,
                "trailing content reports its offset") ||
        !expect(!duplicate.has_value() &&
                    duplicate.error().code == RuntimeConfigErrorCode::MalformedJson &&
                    duplicate.error().offset == 23,
                "duplicate key reports the second key") ||
        !expect(!trailing_comma.has_value() &&
                    trailing_comma.error().code == RuntimeConfigErrorCode::MalformedJson &&
                    trailing_comma.error().offset == 23,
                "trailing comma is malformed")) {
        return false;
    }

    const auto inverted =
        parse_runtime_config(R"({"runtime":{"renderScale":0.5,"minRenderScale":0.75}})");
    if (!expect(!inverted.has_value() &&
                    inverted.error().code == RuntimeConfigErrorCode::CrossField &&
                    inverted.error().field == "minRenderScale",
                "inverted scale relationship is CrossField")) {
        return false;
    }
    const std::string& message = inverted.error().message;
    if (!expect(message.find("minRenderScale") != std::string::npos &&
                    message.find("renderScale") != std::string::npos,
                "cross-field message names both fields")) {
        return false;
    }
    const auto equal =
        parse_runtime_config(R"({"runtime":{"renderScale":0.5,"minRenderScale":0.5}})");
    const auto clamped_pair =
        parse_runtime_config(R"({"runtime":{"renderScale":10,"minRenderScale":1}})");
    return expect(equal.has_value() && clamped_pair.has_value() &&
                      near(clamped_pair->render_scale, 4.0),
                  "equal and clamped scale pairs are valid");
}

bool check_unknown_keys() {
    // Section 4 rule 11 is a strict schema: an unknown key is an error, not a warning. The first
    // unknown key in document order wins, so a document with two is reported at the first.
    const auto parsed = parse_runtime_config(
        R"({"app":{"namespace":"demo"},"runtime":{"futureKey":true,"maxFps":30,
           "nestedFuture":{"a":[1,2,{"b":null}]}}})");
    if (!expect(!parsed.has_value(), "an unknown runtime key is rejected") ||
        !expect(parsed.error().code == RuntimeConfigErrorCode::UnknownField,
                "an unknown runtime key is UnknownField") ||
        !expect(parsed.error().field == "futureKey",
                "the first unknown key in document order wins") ||
        !expect(parsed.error().message.find("futureKey") != std::string::npos,
                "the error names the key") ||
        !expect(parsed.error().offset > 0, "the error carries the key offset")) {
        return false;
    }
    const auto single = parse_runtime_config(R"({"runtime":{"futureKey":1}})");
    return expect(!single.has_value() &&
                      single.error().code == RuntimeConfigErrorCode::UnknownField,
                  "a single unknown key is rejected the same way");
}

bool check_determinism() {
    constexpr std::string_view kDocument =
        R"({"runtime":{"maxFps":120,"vsync":"adaptive","renderScale":0.75,"minRenderScale":0.5,
           "layoutObservationMaxRounds":99,"pauseWhenOccluded":false,
           "animationInterpolation":false}})";
    const auto first = parse_runtime_config(kDocument);
    const auto second = parse_runtime_config(kDocument);
    if (!expect(first.has_value() && second.has_value(), "determinism parse succeeds") ||
        !expect(*first == *second, "two parses produce the same struct") ||
        !expect(near(first->render_scale, 0.75) && first->layout_observation_max_rounds == 16 &&
                    !first->pause_when_occluded && !first->animation_interpolation,
                "overrides and clamps apply before comparison")) {
        return false;
    }
    const auto first_error = parse_runtime_config(R"({"runtime":{"maxFps":-1}})");
    const auto second_error = parse_runtime_config(R"({"runtime":{"maxFps":-1}})");
    return expect(!first_error.has_value() && !second_error.has_value() &&
                      first_error.error() == second_error.error(),
                  "the error path is deterministic");
}

} // namespace

int main() {
    // The returned model owns no allocation, so a parsed struct can be copied and compared without
    // touching the heap; this is also what "identical struct" means for determinism.
    static_assert(std::is_trivially_copyable_v<RuntimeConfig>);

    if (!check_section_2_sample() || !check_defaults() || !check_numeric_boundaries() ||
        !check_enum_values() || !check_errors() || !check_unknown_keys() || !check_determinism()) {
        return 1;
    }
    std::cout << "runtime_config: ok\n";
    return 0;
}
