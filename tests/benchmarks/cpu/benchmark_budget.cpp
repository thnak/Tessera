// SPDX-License-Identifier: MIT

#include "benchmark_budget.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace tessera::benchmark {
namespace {

struct Key {
    const char* name;
    std::uint64_t* value;
};

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

bool load_budget(const char* path, Budget& out) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    const Key keys[] = {
        {"nodes", &out.nodes},
        {"runs", &out.runs},
        {"warmup_runs", &out.warmup_runs},
        {"max_first_style_layout_ms", &out.max_first_style_layout_ms},
        {"max_full_paint_ms", &out.max_full_paint_ms},
        {"max_layout_paint_text_ms", &out.max_layout_paint_text_ms},
        {"max_layout_paint_container_ms", &out.max_layout_paint_container_ms},
        {"max_damage_rects", &out.max_damage_rects},
        {"max_draw_calls", &out.max_draw_calls},
        {"max_quads", &out.max_quads},
        {"max_primitives", &out.max_primitives},
    };
    std::size_t found = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string_view key = trim(std::string_view(line).substr(0, equals));
        const std::string_view value_text = trim(std::string_view(line).substr(equals + 1));
        for (const Key& candidate : keys) {
            if (key != candidate.name) {
                continue;
            }
            const std::string text(value_text);
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0') {
                return false;
            }
            *candidate.value = parsed;
            ++found;
            break;
        }
    }
    // `warmup_runs` may legitimately be 0; every other value is a positive budget.
    return found == std::size(keys) && out.nodes != 0 && out.runs != 0 &&
           out.max_first_style_layout_ms != 0 && out.max_full_paint_ms != 0 &&
           out.max_layout_paint_text_ms != 0 && out.max_layout_paint_container_ms != 0 &&
           out.max_damage_rects != 0 && out.max_draw_calls != 0 && out.max_quads != 0 &&
           out.max_primitives != 0;
}

Percentiles summarize(std::vector<double> samples) {
    Percentiles result;
    if (samples.empty()) {
        return result;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t count = samples.size();
    if (count % 2 == 0) {
        result.median = (samples[count / 2 - 1] + samples[count / 2]) / 2.0;
    } else {
        result.median = samples[count / 2];
    }
    const double position = 0.95 * static_cast<double>(count - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    result.p95 = samples[lower] + (samples[upper] - samples[lower]) * fraction;
    return result;
}

} // namespace tessera::benchmark
