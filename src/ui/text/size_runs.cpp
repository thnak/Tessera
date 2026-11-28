// SPDX-License-Identifier: MIT

#include "tessera/ui/text/size_runs.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace tessera::ui::text {
namespace {

struct Table {
    std::mutex mutex;
    // A deque never moves an element, so a span handed out by resolve stays valid while later
    // tables are interned.
    std::deque<std::vector<TextSizeRun>> tables;
};

Table& table() {
    static Table instance;
    return instance;
}

} // namespace

TextSizeRunsId intern_text_size_runs(std::span<const TextSizeRun> runs) {
    std::vector<TextSizeRun> kept;
    kept.reserve(runs.size());
    std::uint32_t previous_end = 0;
    for (const TextSizeRun& run : runs) {
        const bool shrinks = std::isfinite(run.scale) && run.scale < 1.0f;
        if (!shrinks || run.end <= run.begin || run.begin < previous_end) {
            continue;
        }
        kept.push_back({run.begin, run.end, std::max(run.scale, kMinTextSizeRunScale)});
        previous_end = run.end;
    }
    if (kept.empty()) {
        return {};
    }
    Table& shared = table();
    const std::scoped_lock lock(shared.mutex);
    for (std::size_t index = 0; index < shared.tables.size(); ++index) {
        if (shared.tables[index] == kept) {
            return {static_cast<std::uint32_t>(index + 1)};
        }
    }
    if (shared.tables.size() >= kMaxInternedTextSizeRuns) {
        return {};
    }
    shared.tables.push_back(std::move(kept));
    return {static_cast<std::uint32_t>(shared.tables.size())};
}

std::span<const TextSizeRun> resolve_text_size_runs(TextSizeRunsId id) {
    if (id.value == 0) {
        return {};
    }
    Table& shared = table();
    const std::scoped_lock lock(shared.mutex);
    if (id.value > shared.tables.size()) {
        return {};
    }
    return shared.tables[id.value - 1];
}

TextSizeRunsId size_runs_from_spans(std::span<const SpanFontSize> spans, float parent_font_size) {
    if (!(parent_font_size > 0.0f)) {
        return {};
    }
    std::vector<TextSizeRun> runs;
    runs.reserve(spans.size());
    for (const SpanFontSize& span : spans) {
        if (span.font_size > 0.0f) {
            runs.push_back({static_cast<std::uint32_t>(span.begin),
                            static_cast<std::uint32_t>(span.end),
                            span.font_size / parent_font_size});
        }
    }
    return intern_text_size_runs(runs);
}

float text_size_run_scale(std::span<const TextSizeRun> runs, std::size_t offset) {
    for (const TextSizeRun& run : runs) {
        if (offset >= run.begin && offset < run.end) {
            return run.scale;
        }
    }
    return 1.0f;
}

} // namespace tessera::ui::text
