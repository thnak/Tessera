// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

// The `runtime` object of `tessera.json`: the C++ model and its loader
// (docs/specs/runtime-config-v1.md sections 3, 4 and 6).
//
// Section 6 declares the mirror in namespace `tessera` rather than `tessera::runtime`, so a project
// config is named `tessera::RuntimeConfig` exactly as the spec shows.
namespace tessera {

enum class VsyncMode { Off, On, Adaptive, Mailbox };
enum class RefreshMode { Off, Auto, Preferred };
enum class FramePacing { Auto, Smooth, LowLatency, PowerSave };
enum class DirtyMode { Auto, On, Off };
enum class PowerPolicy { Adaptive, PowerSave, Balanced, Performance, Benchmark };
enum class ResizeMode { Live, Deferred, Adaptive };

struct RuntimeConfig {
    double fixed_timestep_hz = 60.0;
    std::uint32_t max_fps = 0;
    VsyncMode vsync = VsyncMode::On;
    RefreshMode dynamic_refresh_rate = RefreshMode::Auto;
    std::uint32_t preferred_refresh_rate = 0;
    FramePacing frame_pacing = FramePacing::Auto;
    std::uint32_t latency_frames = 2;
    bool allow_tearing = false;
    double max_frame_delta_ms = 250.0;
    std::uint32_t max_updates_per_frame = 5;
    float render_scale = 1.0f;
    bool render_on_demand = true;
    PowerPolicy power_policy = PowerPolicy::Adaptive;
    bool adaptive_quality = true;
    float min_render_scale = 0.6f;
    double frame_budget_ms = 0.0;
    bool pause_when_occluded = true;
    bool pause_when_unfocused = false;
    std::uint32_t background_max_fps = 0;
    ResizeMode resize_mode = ResizeMode::Adaptive;
    double resize_debounce_ms = 16.0;
    double swapchain_recreate_delay_ms = 16.0;
    std::uint32_t max_pending_resizes = 1;
    std::uint32_t layout_observation_max_rounds = 4;
    std::uint32_t async_worker_threads = 0;
    std::uint32_t async_io_threads = 1;
    std::uint32_t async_queue_capacity = 4096;
    double async_main_thread_budget_ms = 2.0;
    bool async_trace_tasks = false;
    DirtyMode dirty_rendering = DirtyMode::Auto;
    bool damage_rects = true;
    std::uint32_t max_damage_rects = 16;
    float full_redraw_threshold = 0.6f;
    std::uint32_t layer_cache_budget_mb = 64;
    std::uint32_t draw_list_cache_budget_mb = 32;
    std::uint32_t view_transition_max_mb = 64;
    std::uint32_t gpu_frames_in_flight = 2;
    bool animation_interpolation = true;
    float interpolation_epsilon_px = 0.0625f;
    bool deterministic = false;

    [[nodiscard]] bool operator==(const RuntimeConfig&) const = default;
};

// One failure class per kind of rejection. `MalformedJson` is a document-level syntax error (its
// `field` is empty); every other code names the runtime object's JSON key it rejected.
enum class RuntimeConfigErrorCode {
    MalformedJson, // not JSON, duplicate key, invalid UTF-8, bad escape, nesting too deep
    InvalidType,   // member has the wrong JSON type (integer vs number is part of the field table)
    InvalidValue,  // an enum string is not one of the documented values
    OutOfRange,    // outside the documented range of a field the table does not clamp
    NonFinite,     // NaN/+-inf, including a literal that overflows double
    CrossField,    // minRenderScale > renderScale
    UnknownField,  // a key that is not a runtime-config field (section 4 rule 11: strict schema)
};

[[nodiscard]] std::string_view to_string(RuntimeConfigErrorCode code) noexcept;

struct RuntimeConfigError {
    RuntimeConfigErrorCode code = RuntimeConfigErrorCode::MalformedJson;
    // JSON key of the rejected member; empty when the document itself is the problem.
    std::string field;
    // Human-readable explanation. Names `field` and, for `InvalidValue`, the allowed set; for
    // `CrossField`, both fields.
    std::string message;
    // Byte offset of the offending value in the input, or of the syntax error; 0 when not
    // applicable (including a missing member).
    std::size_t offset = 0;

    [[nodiscard]] bool operator==(const RuntimeConfigError&) const = default;
};

// Parses a whole `tessera.json` document and validates its optional `runtime` object. A document
// without `runtime` (or with `"runtime": null`) yields the documented defaults; `{}` does too.
//
// Validation follows the section 3 field table: `renderScale` and `layoutObservationMaxRounds`
// clamp to their ranges (the only two the table says clamp); every other range is an error, as is
// a wrong JSON type (an integer field needs an integer literal, not `60.0` or `1e2`), a non-finite
// number, and `minRenderScale > renderScale` after clamping. A key that is not a runtime-config
// field is an `UnknownField` error (section 4 rule 11), naming the first unknown key in document
// order.
//
// Deterministic: the same input always produces an identical struct, the same input and the same
// first error, no clock/RNG/locale is consulted, and the returned `RuntimeConfig` owns no
// allocation.
[[nodiscard]] std::expected<RuntimeConfig, RuntimeConfigError>
parse_runtime_config(std::string_view project_json);

} // namespace tessera
