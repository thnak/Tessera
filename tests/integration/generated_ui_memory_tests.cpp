// SPDX-License-Identifier: MIT
//
// Memory benchmark on the headless generated-UI sample (issue #13). The compiler-generated `Demo`
// component is built, laid out, painted through the CPU canvas and driven by synthetic clicks; the
// process's resident memory is sampled at checkpoints and compared with the committed baseline
// (tests/golden/memory/generated_ui.txt, argv[2]):
//
//   build_bytes        resident growth from instantiating the generated tree and laying it out
//   first_frame_bytes  growth of the first paint (glyph rasterization, shaping, canvas)
//   interaction_bytes  growth across `cycles` click + update + repaint cycles after warm-up
//                      (a leak guard: a steady app adds nothing)
//   peak_bytes         the process's resident high-water mark at the end
//
// Real RSS is machine dependent, so the bounds are generous: they catch leaks and
// order-of-magnitude regressions, not noise. Raise one only with a recorded decision
// (docs/design/performance.md, section 3.1). Usage: generated_ui_memory_tests <font.ttf> <baseline>

#include "generated_ui_demo.hpp"
#include "tessera/diagnostics/memory.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

namespace {

using namespace generated_ui_demo; // NOLINT: test-local helpers live in one namespace.
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::paint::FontPaintTextMetrics;
using tessera::ui::paint::PaintContext;

namespace diag = tessera::diagnostics;

struct Baseline {
    std::uint64_t warmup_cycles = 0;
    std::uint64_t cycles = 0;
    std::uint64_t max_build_bytes = 0;
    std::uint64_t max_first_frame_bytes = 0;
    std::uint64_t max_interaction_bytes = 0;
    std::uint64_t max_peak_bytes = 0;
};

bool load(const char* path, Baseline& out) {
    std::ifstream file(path);
    std::string line;
    int found = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::uint64_t value = std::strtoull(line.c_str() + eq + 1, nullptr, 10);
        if (key == "warmup_cycles") {
            out.warmup_cycles = value;
        } else if (key == "cycles") {
            out.cycles = value;
        } else if (key == "max_build_bytes") {
            out.max_build_bytes = value;
        } else if (key == "max_first_frame_bytes") {
            out.max_first_frame_bytes = value;
        } else if (key == "max_interaction_bytes") {
            out.max_interaction_bytes = value;
        } else if (key == "max_peak_bytes") {
            out.max_peak_bytes = value;
        } else {
            continue;
        }
        ++found;
    }
    return found == 6 && out.cycles != 0;
}

std::uint64_t grown(const diag::ProcessMemory& before, const diag::ProcessMemory& after) {
    return after.resident_bytes > before.resident_bytes
               ? after.resident_bytes - before.resident_bytes
               : 0;
}

bool within(std::uint64_t value, std::uint64_t bound, const char* what) {
    if (value > bound) {
        std::fprintf(stderr, "generated_ui_memory: %s %llu exceeds the baseline %llu\n", what,
                     static_cast<unsigned long long>(value),
                     static_cast<unsigned long long>(bound));
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: generated_ui_memory_tests <font.ttf> <baseline>\n");
        return 2;
    }
    Baseline baseline;
    if (!load(argv[2], baseline)) {
        std::fprintf(stderr, "generated_ui_memory: cannot read the baseline %s\n", argv[2]);
        return 2;
    }
    auto loaded = tessera::ui::text::FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::fprintf(stderr, "generated_ui_memory: failed to load font: %s\n",
                     loaded.error().message.c_str());
        return 2;
    }
    const diag::ProcessMemory start = diag::sample_process_memory();
    if (!start.valid) {
        std::fprintf(stderr, "generated_ui_memory: the platform reports no resident memory\n");
        return 2;
    }

    const tessera::ui::text::FontFace face = *loaded;
    GlyphRasterizer rasterizer(face);
    HarfBuzzShaper shaper(face);
    FontPaintTextMetrics metrics(face, 1);
    PaintContext paint_context;
    paint_context.text_metrics_source = &metrics;

    // Checkpoint 1: the generated tree and its layout.
    const diag::ProcessMemory before_build = diag::sample_process_memory();
    auto fixture = std::make_unique<Fixture>();
    const diag::ProcessMemory after_build = diag::sample_process_memory();
    Node* const button = fixture->find(*fixture->root, "Button");
    if (button == nullptr) {
        std::fprintf(stderr, "generated_ui_memory: the generated Button is missing\n");
        return 3;
    }

    // Checkpoint 2: the first paint.
    CpuCanvas canvas(kWidth, kHeight);
    (void)render(*fixture->root, fixture->styles, canvas, rasterizer, shaper, paint_context);
    const diag::ProcessMemory after_frame = diag::sample_process_memory();

    // Checkpoint 3: scripted interaction. One cycle is a click on the Button, the generated
    // update and a repaint of the changed scene.
    InputRouter router;
    const auto cycle = [&] {
        const auto bounds = button->world_bounds();
        const float x = bounds.x + bounds.width * 0.5f;
        const float y = bounds.y + bounds.height * 0.5f;
        (void)router.dispatch(*fixture->root, pointer_event(InputEventType::PointerDown, x, y));
        (void)router.dispatch(*fixture->root, pointer_event(InputEventType::PointerUp, x, y));
        fixture->demo.update();
        (void)render(*fixture->root, fixture->styles, canvas, rasterizer, shaper, paint_context);
    };
    for (std::uint64_t i = 0; i < baseline.warmup_cycles; ++i) {
        cycle();
    }
    const diag::ProcessMemory before_cycles = diag::sample_process_memory();
    for (std::uint64_t i = 0; i < baseline.cycles; ++i) {
        cycle();
    }
    const diag::ProcessMemory after_cycles = diag::sample_process_memory();

    const std::uint64_t build_bytes = grown(before_build, after_build);
    const std::uint64_t first_frame_bytes = grown(after_build, after_frame);
    const std::uint64_t interaction_bytes = grown(before_cycles, after_cycles);
    const std::uint64_t peak_bytes = after_cycles.peak_resident_bytes;
    std::printf("generated_ui_memory: start %llu build %llu first_frame %llu interaction %llu "
                "(%llu cycles) steady %llu peak %llu\n",
                static_cast<unsigned long long>(start.resident_bytes),
                static_cast<unsigned long long>(build_bytes),
                static_cast<unsigned long long>(first_frame_bytes),
                static_cast<unsigned long long>(interaction_bytes),
                static_cast<unsigned long long>(baseline.cycles),
                static_cast<unsigned long long>(after_cycles.resident_bytes),
                static_cast<unsigned long long>(peak_bytes));

    bool ok = static_cast<std::uint64_t>(fixture->demo.state_count_) ==
              baseline.warmup_cycles + baseline.cycles;
    if (!ok) {
        std::fprintf(stderr, "generated_ui_memory: the scripted clicks did not all land\n");
    }
    ok = within(build_bytes, baseline.max_build_bytes, "build bytes") && ok;
    ok = within(first_frame_bytes, baseline.max_first_frame_bytes, "first-frame bytes") && ok;
    ok = within(interaction_bytes, baseline.max_interaction_bytes, "interaction bytes") && ok;
    ok = within(peak_bytes, baseline.max_peak_bytes, "peak resident bytes") && ok;
    return ok ? 0 : 1;
}
