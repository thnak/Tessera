// SPDX-License-Identifier: MIT
//
// Per-stage frame profile of the reference scenes (`tessera_benchmark --stage-profile`). It answers
// "where does a frame's cost go" before anyone decides on GPU-driven rendering or instancing: CPU
// stages (style, layout, damage, paint-pass op build, sink/batch build, flush), what reaches the
// GPU (vertex and clip bytes, draws, binds, passes, offscreen layers) and, on a real device, the
// synchronous submit time.
//
// Counters are the engine's own (PaintStats, the batched renderer's cull/split/layer/offscreen
// stats, the null command list's recorded stats, the clip buffer's upload bytes); nothing here
// invents a second accounting. Counters are deterministic, so the report with timings off is
// byte-identical run to run and is what the `cpu_stage_profile` CTest checks. Timings exist only
// in the benchmark executable and are never compared by a test.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tessera::benchmark {

// The stages of one frame, in the order the host runs them. `SinkBatch` is time inside the
// Batched2DSink/renderer calls the paint pass makes; `PaintPass` is the rest of paint_frame (style
// lookups, culling, op building). `Submit` is only measured on a real device.
enum class Stage : std::uint8_t {
    Tick,
    Style,
    Layout,
    Damage,
    PaintPass,
    SinkBatch,
    Begin,
    Flush,
    Submit,
};
inline constexpr std::size_t kStageCount = 9;

[[nodiscard]] const char* stage_name(Stage stage) noexcept;

struct StageCounters {
    // Paint pass (PaintStats and the sink's draw-list counts).
    std::uint64_t paint_nodes = 0;
    std::uint64_t culled_nodes = 0;
    std::uint64_t culled_subtrees = 0;
    std::uint64_t primitives = 0;
    std::uint64_t text_runs = 0;
    // Non-space bytes of the text runs, and what a glyph quad each would add to the vertex upload
    // (6 vertices x 64 bytes). The benchmark has no font, so these are the geometry the text stub
    // leaves out of `quads`/`vertex_bytes`; an estimate, not a measurement.
    std::uint64_t text_glyphs = 0;
    std::uint64_t text_glyph_bytes_est = 0;
    std::uint64_t images = 0;
    std::uint64_t gradients = 0;
    std::uint64_t clips = 0;
    std::uint64_t transforms = 0;
    // Batch build and upload (renderer counters; a vertex is 64 bytes).
    std::uint64_t quads = 0;
    std::uint64_t quads_culled = 0;
    std::uint64_t vertex_bytes = 0;
    std::uint64_t clip_upload_bytes = 0;
    std::uint64_t clip_upload_calls = 0;
    std::uint64_t draw_calls = 0;
    std::uint64_t split_texture = 0;
    std::uint64_t split_pipeline = 0;
    std::uint64_t split_scissor = 0;
    std::uint64_t split_clip = 0;
    std::uint64_t split_flush = 0;
    // Recorded commands (the null command list; unavailable when the backend is a real device).
    std::uint64_t render_passes = 0;
    std::uint64_t pipeline_binds = 0;
    std::uint64_t texture_binds = 0;
    std::uint64_t vertex_buffer_binds = 0;
    std::uint64_t storage_buffer_binds = 0;
    std::uint64_t scissor_changes = 0;
    std::uint64_t sampled_target_binds = 0;
    std::uint64_t region_copies = 0;
    // Offscreen layers and glass (renderer/pool stats).
    std::uint64_t layers_isolated = 0;
    std::uint64_t layers_flat = 0;
    std::uint64_t glass_draws = 0;
    std::uint64_t offscreen_targets = 0;
    std::uint64_t offscreen_peak_live = 0;
    std::uint64_t offscreen_bytes = 0;
    std::uint64_t offscreen_refused = 0;
    // Damage plan of the frame.
    std::uint64_t damage_rects = 0;
    std::uint64_t full_redraw = 0;
};

struct CounterField {
    const char* name;
    std::uint64_t StageCounters::* member;
    // True when only the null backend's command list can fill it.
    bool command_stat;
};

[[nodiscard]] std::span<const CounterField> counter_fields() noexcept;

struct SceneProfile {
    std::string name;
    std::array<double, kStageCount> ms{};
    StageCounters counters;
    // False on a real device: the recorded-command counters are not available there.
    bool command_stats = true;
    // True when Submit was measured (a real device).
    bool submit_measured = false;

    [[nodiscard]] double total_ms() const noexcept;
    [[nodiscard]] Stage dominant_stage() const noexcept;
    // vertex_bytes + clip_upload_bytes: what the frame wrote into GPU-visible buffers.
    [[nodiscard]] std::uint64_t upload_bytes() const noexcept {
        return counters.vertex_bytes + counters.clip_upload_bytes;
    }
};

struct ProfileReport {
    std::string backend; // "null" or "vulkan"
    std::size_t nodes = 0;
    std::size_t runs = 0;
    // Median time to compile the reference scene's stylesheet (the startup style cost; the
    // per-frame style stage starts from an already compiled sheet). Zero when not measured.
    double stylesheet_compile_ms = 0.0;
    std::vector<SceneProfile> scenes;
};

// Folds the per-run scene lists into one report: counters from the first run, each stage's median
// time. Returns false (and says why in `error`) when a scene's counters differ between runs, which
// would mean the scene is not deterministic and the report would be misleading.
[[nodiscard]] bool aggregate_runs(const std::vector<std::vector<SceneProfile>>& runs,
                                  ProfileReport& out, std::string& error);

// `timings` false omits every wall-clock field, which makes the output a pure function of the
// counters (the deterministic form).
[[nodiscard]] std::string format_profile_text(const ProfileReport& report, bool timings);
[[nodiscard]] std::string format_profile_json(const ProfileReport& report, bool timings);

// The `--stage-profile` and `--profile-check` entry points of tessera_benchmark (argv[1] is the
// mode). Return the process exit code.
[[nodiscard]] int run_stage_profile(int argc, char** argv);
[[nodiscard]] int run_profile_check();

} // namespace tessera::benchmark
