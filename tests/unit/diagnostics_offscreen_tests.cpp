// SPDX-License-Identifier: MIT
//
// The offscreen pool's observable fallback (docs/design/gpu/offscreen-layers.md section 5e): the
// reporter turns a refused request into a counter, two gauges and ONE warning per run.

#include "tessera/diagnostics/log.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/offscreen_pressure.hpp"

#include <cstdio>
#include <iostream>
#include <string>

namespace {

namespace diag = tessera::diagnostics;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "diagnostics_offscreen: " << message << "\n";
    }
    return condition;
}

diag::OffscreenPressure healthy() {
    diag::OffscreenPressure pressure;
    pressure.targets = 2;
    pressure.peak_live = 2;
    pressure.max_targets = 4;
    pressure.bytes = 16588800;
    pressure.max_bytes = 268435456;
    return pressure;
}

bool quiet_when_nothing_is_refused() {
    diag::Logger logger;
    diag::MemoryLogSink sink;
    logger.add_sink(sink);
    diag::MetricsRegistry metrics;
    diag::OffscreenPressureReporter reporter(&logger, &metrics);
    for (std::uint64_t frame = 1; frame <= 100; ++frame) {
        if (reporter.observe(healthy(), frame)) {
            return expect(false, "a healthy frame logged");
        }
    }
    const auto samples = metrics.snapshot();
    return expect(sink.records().empty() && !reporter.reported() && reporter.refused() == 0,
                  "nothing logged, nothing refused") &&
           expect(samples.size() == 3 && samples[0].name == "gpu.offscreen.refused" &&
                      samples[0].value == 0,
                  "the refusal counter exists and is zero") &&
           expect(samples[1].name == "gpu.offscreen.peak_live" && samples[1].value == 2 &&
                      samples[2].name == "gpu.offscreen.bytes" && samples[2].value == 16588800,
                  "the gauges follow the frame");
}

bool one_warning_per_run_and_a_counter() {
    diag::Logger logger;
    diag::MemoryLogSink sink;
    logger.add_sink(sink);
    diag::MetricsRegistry metrics;
    diag::OffscreenPressureReporter reporter(&logger, &metrics);

    diag::OffscreenPressure refused = healthy();
    refused.exhausted = 3;
    refused.exhausted_total = 3;
    refused.peak_live = 4;
    const bool first = reporter.observe(refused, 7);
    refused.exhausted = 2;
    const bool second = reporter.observe(refused, 8);
    const bool third = reporter.observe(refused, 9);

    bool ok = expect(first && !second && !third, "only the first refusal logs");
    ok = expect(sink.records().size() == 1, "exactly one record in the sink") && ok;
    if (sink.records().empty()) {
        return false;
    }
    const diag::LogRecord& record = sink.records().front();
    ok = expect(record.level == diag::LogLevel::Warn && record.category == diag::LogCategory::Gpu &&
                    record.frame == 7,
                "a warning in the gpu category, on the frame that hit it") &&
         expect(record.message.find(diag::OffscreenPressureReporter::kCode) == 0 &&
                    record.message.find("glassMaxBlurTargets") != std::string::npos &&
                    record.message.find("4 live at the peak") != std::string::npos,
                "it carries the code, the numbers and the knob") &&
         expect(reporter.reported() && reporter.refused() == 7, "the reporter counted 3 + 2 + 2") &&
         expect(metrics.value(0) == 7, "and so did the metric") && ok;
    return ok;
}

bool halves_are_optional() {
    diag::OffscreenPressure refused = healthy();
    refused.exhausted = 1;
    diag::OffscreenPressureReporter bare;
    diag::Logger logger;
    diag::MemoryLogSink sink;
    logger.add_sink(sink);
    diag::OffscreenPressureReporter log_only(&logger, nullptr);
    diag::MetricsRegistry metrics;
    diag::OffscreenPressureReporter metrics_only(nullptr, &metrics);
    return expect(!bare.observe(refused, 1) && bare.refused() == 1,
                  "no collaborators: counted only") &&
           expect(log_only.observe(refused, 1) && sink.records().size() == 1, "logger only") &&
           expect(!metrics_only.observe(refused, 1) && metrics.value(0) == 1, "metrics only");
}

std::FILE* open_temp_file() {
#if defined(_MSC_VER)
    std::FILE* file = nullptr;
    return tmpfile_s(&file) == 0 ? file : nullptr;
#else
    return std::tmpfile();
#endif
}

bool the_log_line_is_readable() {
    std::FILE* file = open_temp_file();
    if (file == nullptr) {
        return true; // no temp file on this host; the sink is trivial
    }
    diag::FileLogSink sink(file);
    diag::LogRecord record;
    record.level = diag::LogLevel::Warn;
    record.category = diag::LogCategory::Gpu;
    record.message = "hello";
    sink.write(record);
    std::rewind(file);
    char line[64] = {};
    const bool read = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    return expect(read && std::string(line) == "[warn] [gpu] hello\n", "FileLogSink line format");
}

} // namespace

int main() {
    const bool ok = quiet_when_nothing_is_refused() && one_warning_per_run_and_a_counter() &&
                    halves_are_optional() && the_log_line_is_readable();
    if (!ok) {
        return 1;
    }
    std::cout << "diagnostics_offscreen: ok\n";
    return 0;
}
