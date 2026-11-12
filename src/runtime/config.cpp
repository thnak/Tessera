// SPDX-License-Identifier: MIT

// Loader and validator for the `runtime` object of tessera.json
// (docs/specs/runtime-config-v1.md sections 3, 4 and 6.1). Model only: nothing here is wired into
// the window, loop or frame pipeline yet.

#include "config_json.hpp"
#include "tessera/runtime/config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tessera {
namespace {

namespace json = runtime::internal;

// A project file nests `runtime` one level below the document root; unknown members may carry a
// small object/array. Anything deeper is rejected as malformed rather than consuming arbitrary
// stack for a config file.
constexpr std::size_t kMaxDocumentDepth = 16;

constexpr std::array<std::string_view, 39> kKnownKeys = {
    "fixedTimestepHz",
    "maxFps",
    "vsync",
    "dynamicRefreshRate",
    "preferredRefreshRate",
    "framePacing",
    "latencyFrames",
    "allowTearing",
    "maxFrameDeltaMs",
    "maxUpdatesPerFrame",
    "renderScale",
    "renderOnDemand",
    "powerPolicy",
    "adaptiveQuality",
    "minRenderScale",
    "frameBudgetMs",
    "pauseWhenOccluded",
    "pauseWhenUnfocused",
    "backgroundMaxFps",
    "resizeMode",
    "resizeDebounceMs",
    "swapchainRecreateDelayMs",
    "maxPendingResizes",
    "layoutObservationMaxRounds",
    "asyncWorkerThreads",
    "asyncIoThreads",
    "asyncQueueCapacity",
    "asyncMainThreadBudgetMs",
    "asyncTraceTasks",
    "dirtyRendering",
    "damageRects",
    "maxDamageRects",
    "fullRedrawThreshold",
    "layerCacheBudgetMb",
    "drawListCacheBudgetMb",
    "viewTransitionMaxMb",
    "gpuFramesInFlight",
    "animationInterpolation",
    "deterministic",
};

// `format_number`/`quote` keep diagnostics locale-independent: `std::to_chars` never consults the
// global locale, unlike `snprintf`/`strtod`.
template<typename Number> std::string format_number(Number value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc()) {
        return "?";
    }
    return std::string(buffer, result.ptr);
}

std::string quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('`');
    out += text;
    out.push_back('`');
    return out;
}

RuntimeConfigError type_error(std::string_view key, const json::JsonValue& value,
                              std::string_view expected) {
    return RuntimeConfigError{RuntimeConfigErrorCode::InvalidType, std::string(key),
                              quote(key) + " must be " + std::string(expected), value.offset};
}

RuntimeConfigError non_finite_error(std::string_view key, const json::JsonValue& value) {
    return RuntimeConfigError{RuntimeConfigErrorCode::NonFinite, std::string(key),
                              quote(key) + " must be a finite number", value.offset};
}

RuntimeConfigError range_error(std::string_view key, const json::JsonValue& value, double min,
                               double max) {
    return RuntimeConfigError{RuntimeConfigErrorCode::OutOfRange, std::string(key),
                              quote(key) + " must be in " + format_number(min) + ".." +
                                  format_number(max) + " (got " + format_number(value.number) + ")",
                              value.offset};
}

RuntimeConfigError preferred_refresh_rate_error(const json::JsonValue& value) {
    return RuntimeConfigError{RuntimeConfigErrorCode::OutOfRange, "preferredRefreshRate",
                              "`preferredRefreshRate` must be 0 or in 24..1000 (got " +
                                  format_number(value.number) + ")",
                              value.offset};
}

template<typename Enum> struct EnumName {
    std::string_view name;
    Enum value;
};

constexpr EnumName<VsyncMode> kVsyncModes[] = {
    {"off", VsyncMode::Off},
    {"on", VsyncMode::On},
    {"adaptive", VsyncMode::Adaptive},
    {"mailbox", VsyncMode::Mailbox},
};

constexpr EnumName<RefreshMode> kRefreshModes[] = {
    {"off", RefreshMode::Off},
    {"auto", RefreshMode::Auto},
    {"preferred", RefreshMode::Preferred},
};

constexpr EnumName<FramePacing> kFramePacingModes[] = {
    {"auto", FramePacing::Auto},
    {"smooth", FramePacing::Smooth},
    {"low-latency", FramePacing::LowLatency},
    {"power-save", FramePacing::PowerSave},
};

constexpr EnumName<DirtyMode> kDirtyModes[] = {
    {"auto", DirtyMode::Auto},
    {"on", DirtyMode::On},
    {"off", DirtyMode::Off},
};

constexpr EnumName<PowerPolicy> kPowerPolicies[] = {
    {"adaptive", PowerPolicy::Adaptive},   {"power-save", PowerPolicy::PowerSave},
    {"balanced", PowerPolicy::Balanced},   {"performance", PowerPolicy::Performance},
    {"benchmark", PowerPolicy::Benchmark},
};

constexpr EnumName<ResizeMode> kResizeModes[] = {
    {"live", ResizeMode::Live},
    {"deferred", ResizeMode::Deferred},
    {"adaptive", ResizeMode::Adaptive},
};

// Reads members of the `runtime` object into a RuntimeConfig. A member that is absent keeps the
// struct default; the first rejection latches and every later read is a no-op, so the error names
// the first offending field in table order. Integer fields reject a JSON number with a fraction or
// exponent even when its value is integral (`60.0` and `1e2` are not integers here).
class Reader {
public:
    explicit Reader(const json::JsonValue& object)
        : object_(object) {}

    [[nodiscard]] bool failed() const {
        return error_.has_value();
    }
    [[nodiscard]] RuntimeConfigError take_error() {
        return std::move(*error_);
    }

    void number(std::string_view key, double& out, double min, double max, bool clamp = false) {
        const json::JsonValue* value = member(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_number()) {
            set(type_error(key, *value, "a number"));
            return;
        }
        if (value->non_finite) {
            set(non_finite_error(key, *value));
            return;
        }
        double parsed = value->number;
        if (parsed < min || parsed > max) {
            if (!clamp) {
                set(range_error(key, *value, min, max));
                return;
            }
            parsed = std::clamp(parsed, min, max);
        }
        out = parsed;
    }

    void number(std::string_view key, float& out, double min, double max, bool clamp = false) {
        double parsed = static_cast<double>(out);
        number(key, parsed, min, max, clamp);
        if (!failed()) {
            out = static_cast<float>(parsed);
        }
    }

    void integer(std::string_view key, std::uint32_t& out, std::uint32_t min, std::uint32_t max,
                 bool clamp = false) {
        const json::JsonValue* value = member(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_number()) {
            set(type_error(key, *value, "an integer"));
            return;
        }
        if (value->non_finite) {
            set(non_finite_error(key, *value));
            return;
        }
        if (!value->exact_integer) {
            set(type_error(key, *value, "an integer (no fraction or exponent)"));
            return;
        }
        const double parsed = value->number;
        if (parsed < static_cast<double>(min) || parsed > static_cast<double>(max)) {
            if (!clamp) {
                set(range_error(key, *value, static_cast<double>(min), static_cast<double>(max)));
                return;
            }
            out = parsed < static_cast<double>(min) ? min : max;
            return;
        }
        out = static_cast<std::uint32_t>(parsed);
    }

    void boolean(std::string_view key, bool& out) {
        const json::JsonValue* value = member(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_bool()) {
            set(type_error(key, *value, "a boolean"));
            return;
        }
        out = value->boolean;
    }

    template<typename Enum, std::size_t N>
    void enumeration(std::string_view key, Enum& out, const EnumName<Enum> (&names)[N]) {
        const json::JsonValue* value = member(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_string()) {
            set(type_error(key, *value, "a string"));
            return;
        }
        const std::string_view text = value->string;
        for (const EnumName<Enum>& entry : names) {
            if (entry.name == text) {
                out = entry.value;
                return;
            }
        }
        std::string allowed;
        for (std::size_t index = 0; index < N; ++index) {
            if (index != 0) {
                allowed += ", ";
            }
            allowed += quote(names[index].name);
        }
        set(RuntimeConfigError{RuntimeConfigErrorCode::InvalidValue, std::string(key),
                               quote(key) + " must be one of " + allowed, value->offset});
    }

    // `preferredRefreshRate` is the one numeric field whose range is disjoint: 0 or 24..1000.
    void preferred_refresh_rate(std::uint32_t& out) {
        const json::JsonValue* value = member("preferredRefreshRate");
        if (value == nullptr) {
            return;
        }
        if (!value->is_number()) {
            set(type_error("preferredRefreshRate", *value, "an integer"));
            return;
        }
        if (value->non_finite) {
            set(non_finite_error("preferredRefreshRate", *value));
            return;
        }
        if (!value->exact_integer) {
            set(type_error("preferredRefreshRate", *value, "an integer (no fraction or exponent)"));
            return;
        }
        const double parsed = value->number;
        if (parsed != 0.0 && (parsed < 24.0 || parsed > 1000.0)) {
            set(preferred_refresh_rate_error(*value));
            return;
        }
        out = static_cast<std::uint32_t>(parsed);
    }

private:
    const json::JsonValue* member(std::string_view key) {
        if (failed()) {
            return nullptr;
        }
        return object_.find(key);
    }

    void set(RuntimeConfigError error) {
        if (!error_) {
            error_ = std::move(error);
        }
    }

    const json::JsonValue& object_;
    std::optional<RuntimeConfigError> error_;
};

// Section 4 rule 11 is a strict schema: the first key that is not a runtime-config field is an
// error, in document order.
std::optional<RuntimeConfigError> find_unknown_key(const json::JsonValue& runtime) {
    for (const auto& member : runtime.object) {
        if (std::find(kKnownKeys.begin(), kKnownKeys.end(), std::string_view(member.first)) !=
            kKnownKeys.end()) {
            continue;
        }
        return RuntimeConfigError{RuntimeConfigErrorCode::UnknownField, member.first,
                                  quote(member.first) + " is not a runtime-config field",
                                  member.second.offset};
    }
    return std::nullopt;
}

std::expected<RuntimeConfig, RuntimeConfigError>
validate_runtime_object(const json::JsonValue& runtime) {
    if (std::optional<RuntimeConfigError> unknown = find_unknown_key(runtime)) {
        return std::unexpected(std::move(*unknown));
    }

    RuntimeConfig config;
    Reader reader(runtime);

    reader.number("fixedTimestepHz", config.fixed_timestep_hz, 1.0, 1000.0);
    reader.integer("maxFps", config.max_fps, 0, 1000);
    reader.enumeration("vsync", config.vsync, kVsyncModes);
    reader.enumeration("dynamicRefreshRate", config.dynamic_refresh_rate, kRefreshModes);
    reader.preferred_refresh_rate(config.preferred_refresh_rate);
    reader.enumeration("framePacing", config.frame_pacing, kFramePacingModes);
    reader.integer("latencyFrames", config.latency_frames, 1, 3);
    reader.boolean("allowTearing", config.allow_tearing);
    reader.number("maxFrameDeltaMs", config.max_frame_delta_ms, 1.0, 5000.0);
    reader.integer("maxUpdatesPerFrame", config.max_updates_per_frame, 1, 16);
    // Section 4 rule 7: renderScale clamps; section 3's "out-of-range values clamp" note is the
    // other explicit clamp (layoutObservationMaxRounds).
    reader.number("renderScale", config.render_scale, 0.25, 4.0, true);
    reader.boolean("renderOnDemand", config.render_on_demand);
    reader.enumeration("powerPolicy", config.power_policy, kPowerPolicies);
    reader.boolean("adaptiveQuality", config.adaptive_quality);
    reader.number("minRenderScale", config.min_render_scale, 0.25, 1.0);
    reader.number("frameBudgetMs", config.frame_budget_ms, 0.0, 1000.0);
    reader.boolean("pauseWhenOccluded", config.pause_when_occluded);
    reader.boolean("pauseWhenUnfocused", config.pause_when_unfocused);
    reader.integer("backgroundMaxFps", config.background_max_fps, 0, 1000);
    reader.enumeration("resizeMode", config.resize_mode, kResizeModes);
    reader.number("resizeDebounceMs", config.resize_debounce_ms, 0.0, 1000.0);
    reader.number("swapchainRecreateDelayMs", config.swapchain_recreate_delay_ms, 0.0, 1000.0);
    reader.integer("maxPendingResizes", config.max_pending_resizes, 1, 16);
    reader.integer("layoutObservationMaxRounds", config.layout_observation_max_rounds, 1, 16, true);
    reader.integer("asyncWorkerThreads", config.async_worker_threads, 0, 256);
    reader.integer("asyncIoThreads", config.async_io_threads, 1, 16);
    reader.integer("asyncQueueCapacity", config.async_queue_capacity, 64, 1048576);
    reader.number("asyncMainThreadBudgetMs", config.async_main_thread_budget_ms, 0.0, 16.0);
    reader.boolean("asyncTraceTasks", config.async_trace_tasks);
    reader.enumeration("dirtyRendering", config.dirty_rendering, kDirtyModes);
    reader.boolean("damageRects", config.damage_rects);
    reader.integer("maxDamageRects", config.max_damage_rects, 1, 64);
    reader.number("fullRedrawThreshold", config.full_redraw_threshold, 0.0, 1.0);
    reader.integer("layerCacheBudgetMb", config.layer_cache_budget_mb, 0, 4096);
    reader.integer("drawListCacheBudgetMb", config.draw_list_cache_budget_mb, 0, 4096);
    reader.integer("viewTransitionMaxMb", config.view_transition_max_mb, 0, 4096);
    reader.integer("gpuFramesInFlight", config.gpu_frames_in_flight, 1, 3);
    reader.boolean("animationInterpolation", config.animation_interpolation);
    reader.boolean("deterministic", config.deterministic);

    if (reader.failed()) {
        return std::unexpected(reader.take_error());
    }

    // Section 4 rule 13. Compared after clamping, so `renderScale=10` (clamped to 4) plus
    // `minRenderScale=1` is valid, while a genuinely inverted pair is an error naming both fields.
    if (config.min_render_scale > config.render_scale) {
        const json::JsonValue* min_member = runtime.find("minRenderScale");
        const json::JsonValue* scale_member = runtime.find("renderScale");
        const std::size_t offset = min_member != nullptr
                                       ? min_member->offset
                                       : (scale_member != nullptr ? scale_member->offset : 0);
        return std::unexpected(RuntimeConfigError{
            RuntimeConfigErrorCode::CrossField, "minRenderScale",
            "`minRenderScale` (" + format_number(config.min_render_scale) +
                ") must not exceed `renderScale` (" + format_number(config.render_scale) + ")",
            offset});
    }
    return config;
}

std::string_view describe(json::JsonErrorCode code) {
    switch (code) {
    case json::JsonErrorCode::UnexpectedEnd:
        return "unexpected end of input";
    case json::JsonErrorCode::UnexpectedCharacter:
        return "unexpected character";
    case json::JsonErrorCode::TrailingContent:
        return "trailing content after the document";
    case json::JsonErrorCode::InvalidLiteral:
        return "invalid literal";
    case json::JsonErrorCode::InvalidNumber:
        return "invalid number";
    case json::JsonErrorCode::InvalidEscape:
        return "invalid escape sequence";
    case json::JsonErrorCode::DuplicateKey:
        return "duplicate object key";
    case json::JsonErrorCode::NestingTooDeep:
        return "nesting too deep";
    case json::JsonErrorCode::InvalidUtf8:
        return "invalid UTF-8";
    }
    return "invalid JSON";
}

} // namespace

std::string_view to_string(RuntimeConfigErrorCode code) noexcept {
    switch (code) {
    case RuntimeConfigErrorCode::MalformedJson:
        return "malformed_json";
    case RuntimeConfigErrorCode::InvalidType:
        return "invalid_type";
    case RuntimeConfigErrorCode::InvalidValue:
        return "invalid_value";
    case RuntimeConfigErrorCode::OutOfRange:
        return "out_of_range";
    case RuntimeConfigErrorCode::NonFinite:
        return "non_finite";
    case RuntimeConfigErrorCode::CrossField:
        return "cross_field";
    case RuntimeConfigErrorCode::UnknownField:
        return "unknown_field";
    }
    return "unknown";
}

std::expected<RuntimeConfig, RuntimeConfigError>
parse_runtime_config(std::string_view project_json) {
    const std::expected<json::JsonValue, json::JsonParseError> document =
        json::parse_json(project_json, kMaxDocumentDepth);
    if (!document) {
        return std::unexpected(RuntimeConfigError{RuntimeConfigErrorCode::MalformedJson, "",
                                                  "the document is not valid JSON: " +
                                                      std::string(describe(document.error().code)),
                                                  document.error().offset});
    }
    if (!document->is_object()) {
        return std::unexpected(RuntimeConfigError{RuntimeConfigErrorCode::InvalidType, "",
                                                  "the document must be a JSON object",
                                                  document->offset});
    }
    const json::JsonValue* runtime = document->find("runtime");
    if (runtime == nullptr || runtime->is_null()) {
        // The `runtime` object is optional in project-config-v1.md; absent means all defaults.
        return RuntimeConfig{};
    }
    if (!runtime->is_object()) {
        return std::unexpected(RuntimeConfigError{RuntimeConfigErrorCode::InvalidType, "runtime",
                                                  "`runtime` must be an object", runtime->offset});
    }
    return validate_runtime_object(*runtime);
}

} // namespace tessera
