// SPDX-License-Identifier: MIT

#include "benchmark_budget.hpp"
#include "benchmark_profile.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <utility>

namespace tessera::benchmark {
namespace {

constexpr CounterField kFields[] = {
    {"paint_nodes", &StageCounters::paint_nodes, false},
    {"culled_nodes", &StageCounters::culled_nodes, false},
    {"culled_subtrees", &StageCounters::culled_subtrees, false},
    {"primitives", &StageCounters::primitives, false},
    {"text_runs", &StageCounters::text_runs, false},
    {"text_glyphs", &StageCounters::text_glyphs, false},
    {"text_glyph_bytes_est", &StageCounters::text_glyph_bytes_est, false},
    {"images", &StageCounters::images, false},
    {"gradients", &StageCounters::gradients, false},
    {"clips", &StageCounters::clips, false},
    {"transforms", &StageCounters::transforms, false},
    {"quads", &StageCounters::quads, false},
    {"quads_culled", &StageCounters::quads_culled, false},
    {"vertex_bytes", &StageCounters::vertex_bytes, false},
    {"clip_upload_bytes", &StageCounters::clip_upload_bytes, false},
    {"clip_upload_calls", &StageCounters::clip_upload_calls, false},
    {"draw_calls", &StageCounters::draw_calls, false},
    {"split_texture", &StageCounters::split_texture, false},
    {"split_pipeline", &StageCounters::split_pipeline, false},
    {"split_scissor", &StageCounters::split_scissor, false},
    {"split_clip", &StageCounters::split_clip, false},
    {"split_flush", &StageCounters::split_flush, false},
    {"render_passes", &StageCounters::render_passes, true},
    {"pipeline_binds", &StageCounters::pipeline_binds, true},
    {"texture_binds", &StageCounters::texture_binds, true},
    {"vertex_buffer_binds", &StageCounters::vertex_buffer_binds, true},
    {"storage_buffer_binds", &StageCounters::storage_buffer_binds, true},
    {"scissor_changes", &StageCounters::scissor_changes, true},
    {"sampled_target_binds", &StageCounters::sampled_target_binds, true},
    {"region_copies", &StageCounters::region_copies, true},
    {"layers_isolated", &StageCounters::layers_isolated, false},
    {"layers_flat", &StageCounters::layers_flat, false},
    {"glass_draws", &StageCounters::glass_draws, false},
    {"offscreen_targets", &StageCounters::offscreen_targets, false},
    {"offscreen_peak_live", &StageCounters::offscreen_peak_live, false},
    {"offscreen_bytes", &StageCounters::offscreen_bytes, false},
    {"offscreen_refused", &StageCounters::offscreen_refused, false},
    {"damage_rects", &StageCounters::damage_rects, false},
    {"full_redraw", &StageCounters::full_redraw, false},
};

constexpr std::array<const char*, kStageCount> kStageNames{
    "tick", "style", "layout", "damage", "paint_pass", "sink_batch", "begin", "flush", "submit"};

[[nodiscard]] std::string fmt(const char* format, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}

[[nodiscard]] std::string fmt_u(std::uint64_t value) {
    return std::to_string(value);
}

[[nodiscard]] std::string pad_left(const std::string& text, std::size_t width) {
    return text.size() >= width ? text : std::string(width - text.size(), ' ') + text;
}

[[nodiscard]] std::string pad_right(const std::string& text, std::size_t width) {
    return text.size() >= width ? text : text + std::string(width - text.size(), ' ');
}

constexpr std::size_t kLabelWidth = 22;
constexpr std::size_t kColumnWidth = 21;

} // namespace

const char* stage_name(Stage stage) noexcept {
    return kStageNames[static_cast<std::size_t>(stage)];
}

std::span<const CounterField> counter_fields() noexcept {
    return {kFields, std::size(kFields)};
}

double SceneProfile::total_ms() const noexcept {
    double total = 0.0;
    for (const double value : ms) {
        total += value;
    }
    return total;
}

Stage SceneProfile::dominant_stage() const noexcept {
    std::size_t best = 0;
    for (std::size_t index = 1; index < kStageCount; ++index) {
        if (ms[index] > ms[best]) {
            best = index;
        }
    }
    return static_cast<Stage>(best);
}

bool aggregate_runs(const std::vector<std::vector<SceneProfile>>& runs, ProfileReport& out,
                    std::string& error) {
    if (runs.empty() || runs.front().empty()) {
        error = "no runs";
        return false;
    }
    out.scenes = runs.front();
    out.runs = runs.size();
    for (std::size_t scene = 0; scene < out.scenes.size(); ++scene) {
        for (const std::vector<SceneProfile>& run : runs) {
            if (run.size() != out.scenes.size() || run[scene].name != out.scenes[scene].name) {
                error = "the runs produced different scene lists";
                return false;
            }
            for (const CounterField& field : kFields) {
                if (run[scene].counters.*(field.member) !=
                    out.scenes[scene].counters.*(field.member)) {
                    error = "scene " + out.scenes[scene].name + ": counter " + field.name +
                            " differs between runs";
                    return false;
                }
            }
        }
        for (std::size_t stage = 0; stage < kStageCount; ++stage) {
            std::vector<double> samples;
            samples.reserve(runs.size());
            for (const std::vector<SceneProfile>& run : runs) {
                samples.push_back(run[scene].ms[stage]);
            }
            out.scenes[scene].ms[stage] = summarize(std::move(samples)).median;
        }
    }
    return true;
}

std::string format_profile_text(const ProfileReport& report, bool timings) {
    std::string out;
    out += "stage_profile: backend " + report.backend + ", scene " + fmt_u(report.nodes) +
           " nodes, " + (timings ? fmt_u(report.runs) + " runs (median)" : "counters only") + "\n";
    if (timings && report.stylesheet_compile_ms > 0.0) {
        out +=
            "stylesheet compile (ms, median): " + fmt("%.3f", report.stylesheet_compile_ms) + "\n";
    }
    const auto header = [&](const char* title) {
        out += pad_right(title, kLabelWidth);
        for (const SceneProfile& scene : report.scenes) {
            out += pad_left(scene.name, kColumnWidth);
        }
        out += "\n";
    };
    if (timings) {
        out += "\n";
        header("stage (ms)");
        for (std::size_t stage = 0; stage < kStageCount; ++stage) {
            out += pad_right(kStageNames[stage], kLabelWidth);
            for (const SceneProfile& scene : report.scenes) {
                const bool na =
                    stage == static_cast<std::size_t>(Stage::Submit) && !scene.submit_measured;
                out += pad_left(na ? "n/a" : fmt("%.3f", scene.ms[stage]), kColumnWidth);
            }
            out += "\n";
        }
        out += pad_right("total", kLabelWidth);
        for (const SceneProfile& scene : report.scenes) {
            out += pad_left(fmt("%.3f", scene.total_ms()), kColumnWidth);
        }
        out += "\n";
        out += pad_right("dominant", kLabelWidth);
        for (const SceneProfile& scene : report.scenes) {
            out += pad_left(stage_name(scene.dominant_stage()), kColumnWidth);
        }
        out += "\n";
    }
    out += "\n";
    header("counter");
    out += pad_right("upload_bytes", kLabelWidth);
    for (const SceneProfile& scene : report.scenes) {
        out += pad_left(fmt_u(scene.upload_bytes()), kColumnWidth);
    }
    out += "\n";
    for (const CounterField& field : kFields) {
        out += pad_right(field.name, kLabelWidth);
        for (const SceneProfile& scene : report.scenes) {
            const bool na = field.command_stat && !scene.command_stats;
            out += pad_left(na ? "n/a" : fmt_u(scene.counters.*(field.member)), kColumnWidth);
        }
        out += "\n";
    }
    return out;
}

std::string format_profile_json(const ProfileReport& report, bool timings) {
    std::string out = "{\n";
    out += "  \"backend\": \"" + report.backend + "\",\n";
    out += "  \"nodes\": " + fmt_u(report.nodes) + ",\n";
    if (timings) {
        out += "  \"runs\": " + fmt_u(report.runs) + ",\n";
        out += "  \"stylesheet_compile_ms\": " + fmt("%.4f", report.stylesheet_compile_ms) + ",\n";
    }
    out += "  \"scenes\": [\n";
    for (std::size_t index = 0; index < report.scenes.size(); ++index) {
        const SceneProfile& scene = report.scenes[index];
        out += "    {\n      \"name\": \"" + scene.name + "\",\n";
        if (timings) {
            out += "      \"stage_ms\": {";
            for (std::size_t stage = 0; stage < kStageCount; ++stage) {
                const bool na =
                    stage == static_cast<std::size_t>(Stage::Submit) && !scene.submit_measured;
                out += std::string(stage == 0 ? "" : ", ") + "\"" + kStageNames[stage] +
                       "\": " + (na ? "null" : fmt("%.4f", scene.ms[stage]));
            }
            out += "},\n";
            out += "      \"total_ms\": " + fmt("%.4f", scene.total_ms()) + ",\n";
            out += std::string("      \"dominant_stage\": \"") +
                   stage_name(scene.dominant_stage()) + "\",\n";
        }
        out += "      \"upload_bytes\": " + fmt_u(scene.upload_bytes()) + ",\n";
        out += "      \"counters\": {";
        for (std::size_t field = 0; field < std::size(kFields); ++field) {
            const CounterField& entry = kFields[field];
            const bool na = entry.command_stat && !scene.command_stats;
            out += std::string(field == 0 ? "" : ", ") + "\"" + entry.name +
                   "\": " + (na ? "null" : fmt_u(scene.counters.*(entry.member)));
        }
        out += "}\n    }";
        out += index + 1 == report.scenes.size() ? "\n" : ",\n";
    }
    out += "  ]\n}\n";
    return out;
}

} // namespace tessera::benchmark
