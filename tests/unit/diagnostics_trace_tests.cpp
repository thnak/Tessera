// SPDX-License-Identifier: MIT
//
// The recorder's own behaviour. Only meaningful where tracing is compiled in
// (TESSERA_ENABLE_TRACING=ON, the dev/CI presets); the production build replaces the recorder with
// a no-op and `tracing_strip_tests` covers that flavour.

#include "tessera/diagnostics/trace.hpp"

#include <iostream>
#include <string>

#if defined(TESSERA_ENABLE_TRACING)

namespace {

using tessera::diagnostics::to_chrome_trace_json;
using tessera::diagnostics::TraceCategory;
using tessera::diagnostics::TracePhase;
using tessera::diagnostics::TraceRecorder;
using tessera::diagnostics::TraceSpan;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "diagnostics_trace: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    TraceRecorder recorder(8);
    recorder.begin("layout", TraceCategory::Layout, 5, 7);
    recorder.instant("mark", TraceCategory::Paint, 5, 7);
    recorder.end("layout", TraceCategory::Layout, 5, 7);
    const auto& events = recorder.events();
    if (!expect(events.size() == 3, "three events") ||
        !expect(events[0].phase == TracePhase::Begin && events[0].depth == 0, "begin depth") ||
        !expect(events[0].name == "layout" && events[0].ts == 0, "begin name/ts") ||
        !expect(events[0].frame == 5 && events[0].tick == 7, "frame/tick") ||
        !expect(events[1].phase == TracePhase::Instant && events[1].depth == 1, "instant depth") ||
        !expect(events[2].phase == TracePhase::End && events[2].depth == 0, "end depth") ||
        !expect(recorder.next_timestamp() == 3, "sequence timestamp")) {
        return 1;
    }

    {
        TraceSpan span(recorder, "paint", TraceCategory::Paint, 6, 8);
        if (!expect(recorder.size() == 4, "span begin recorded") ||
            !expect(recorder.events().back().phase == TracePhase::Begin, "span begin phase")) {
            return 2;
        }
    }
    if (!expect(recorder.size() == 5, "span end recorded") ||
        !expect(recorder.events().back().phase == TracePhase::End, "span end phase") ||
        !expect(recorder.events().back().name == "paint", "span end name") ||
        !expect(recorder.events().back().frame == 6 && recorder.events().back().tick == 8,
                "span end ids")) {
        return 3;
    }

    {
        TraceSpan outer(recorder, "outer", TraceCategory::Frame);
        TraceSpan inner(std::move(outer));
        outer.end();
    }
    const auto& moved = recorder.events();
    if (!expect(moved[moved.size() - 2].name == "outer", "moved span begins once") ||
        !expect(moved.back().phase == TracePhase::End, "moved span ends once")) {
        return 4;
    }

    TraceRecorder bounded(3);
    for (int i = 0; i < 5; ++i) {
        bounded.instant("e", TraceCategory::Async, static_cast<std::uint64_t>(i));
    }
    if (!expect(bounded.size() == 3, "ring capacity") ||
        !expect(bounded.dropped() == 2, "ring drops oldest") ||
        !expect(bounded.events().front().ts == 2, "ring keeps newest timestamps") ||
        !expect(bounded.events().back().ts == 4, "ring last timestamp")) {
        return 5;
    }

    bounded.end("unmatched", TraceCategory::Async);
    if (!expect(bounded.events().back().depth == 0, "unmatched end clamps depth")) {
        return 6;
    }

    bounded.clear();
    if (!expect(bounded.size() == 0 && bounded.dropped() == 0 && bounded.next_timestamp() == 0,
                "clear resets")) {
        return 7;
    }

    if (!expect(to_chrome_trace_json(bounded) == "[]", "empty trace json")) {
        return 8;
    }

    TraceRecorder json(8);
    json.begin("a\"b\\c\n", TraceCategory::Rhi, 2, 3);
    json.end("a\"b\\c\n", TraceCategory::Rhi, 2, 3);
    const std::string text = to_chrome_trace_json(json);
    if (!expect(text.front() == '[' && text.back() == ']', "json array") ||
        !expect(text.find("\\\"b\\\\c\\n") != std::string::npos, "json escapes") ||
        !expect(text.find("\"ph\":\"B\"") != std::string::npos, "begin phase") ||
        !expect(text.find("\"ph\":\"E\"") != std::string::npos, "end phase") ||
        !expect(text.find("\"cat\":\"rhi\"") != std::string::npos, "category") ||
        !expect(text.find("\"args\":{\"frame\":2,\"tick\":3}") != std::string::npos,
                "frame/tick args")) {
        return 9;
    }

    std::cout << "diagnostics_trace: ok\n";
    return 0;
}

#else // TESSERA_ENABLE_TRACING

// The production flavour: the recorder is a no-op that still answers the documented empty Chrome
// document, so automation keeps working without a line of tracing code in the binary.
int main() {
    using tessera::diagnostics::to_chrome_trace_json;
    using tessera::diagnostics::TraceCategory;
    using tessera::diagnostics::TraceRecorder;
    using tessera::diagnostics::TraceSpan;

    TraceRecorder recorder(8);
    recorder.begin("layout", TraceCategory::Layout, 5, 7);
    recorder.instant("mark", TraceCategory::Paint, 5, 7);
    {
        TraceSpan span(recorder, "paint", TraceCategory::Paint, 6, 8);
    }
    recorder.end("layout", TraceCategory::Layout, 5, 7);

    if (recorder.size() != 0 || recorder.dropped() != 0 || recorder.next_timestamp() != 0) {
        std::cerr << "diagnostics_trace: a disabled recorder must stay empty\n";
        return 1;
    }
    if (to_chrome_trace_json(recorder) != "[]") {
        std::cerr << "diagnostics_trace: a disabled recorder must answer the empty document\n";
        return 2;
    }
    std::cout << "diagnostics_trace: ok (tracing compiled out)\n";
    return 0;
}

#endif // TESSERA_ENABLE_TRACING
