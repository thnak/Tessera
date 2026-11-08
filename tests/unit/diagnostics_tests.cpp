// SPDX-License-Identifier: MIT

#include "tessera/runtime/diagnostics.hpp"

#include <iostream>

namespace {

using tessera::runtime::FrameDiagnostics;
using tessera::runtime::FrameDiagnosticsSample;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "diagnostics: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    FrameDiagnostics diagnostics;
    if (!expect(diagnostics.frames() == 0, "initially empty") ||
        !expect(diagnostics.last().frame_index == 0, "default last frame") ||
        !expect(diagnostics.total_steps() == 0, "default steps")) {
        return 1;
    }

    FrameDiagnosticsSample first;
    first.steps = 2;
    first.completions = 3;
    first.pending_completions = 1;
    first.damage_rects = 4;
    first.redraw_scissors = 5;
    first.animation_samples = 6;
    diagnostics.record(first);
    if (!expect(diagnostics.frames() == 1, "first frame count") ||
        !expect(diagnostics.last().frame_index == 1, "first frame index") ||
        !expect(diagnostics.last().steps == 2, "first steps") ||
        !expect(diagnostics.last().pending_completions == 1, "first pending") ||
        !expect(diagnostics.total_steps() == 2, "first total steps") ||
        !expect(diagnostics.total_completions() == 3, "first total completions") ||
        !expect(diagnostics.total_damage_rects() == 4, "first total damage") ||
        !expect(diagnostics.total_scissors() == 5, "first total scissor") ||
        !expect(diagnostics.total_animation_samples() == 6, "first total animations") ||
        !expect(diagnostics.full_redraw_frames() == 0, "first not full")) {
        return 2;
    }

    FrameDiagnosticsSample second;
    second.steps = 1;
    second.completions = 2;
    second.damage_rects = 1;
    second.redraw_scissors = 2;
    second.animation_samples = 3;
    second.full_redraw = true;
    diagnostics.record(second);
    if (!expect(diagnostics.frames() == 2, "second frame count") ||
        !expect(diagnostics.last().frame_index == 2, "second frame index") ||
        !expect(diagnostics.total_steps() == 3, "second total steps") ||
        !expect(diagnostics.total_completions() == 5, "second total completions") ||
        !expect(diagnostics.total_damage_rects() == 5, "second total damage") ||
        !expect(diagnostics.total_scissors() == 7, "second total scissor") ||
        !expect(diagnostics.total_animation_samples() == 9, "second total animations") ||
        !expect(diagnostics.full_redraw_frames() == 1, "second full count")) {
        return 3;
    }

    diagnostics.reset();
    if (!expect(diagnostics.frames() == 0, "reset frames") ||
        !expect(diagnostics.last().frame_index == 0, "reset last") ||
        !expect(diagnostics.total_steps() == 0, "reset steps") ||
        !expect(diagnostics.total_completions() == 0, "reset completions") ||
        !expect(diagnostics.full_redraw_frames() == 0, "reset full")) {
        return 4;
    }

    diagnostics.record(FrameDiagnosticsSample{});
    if (!expect(diagnostics.frames() == 1, "empty sample frame") ||
        !expect(diagnostics.last().frame_index == 1, "empty sample index")) {
        return 5;
    }

    std::cout << "diagnostics: ok\n";
    return 0;
}
