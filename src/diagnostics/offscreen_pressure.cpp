// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/offscreen_pressure.hpp"

#include <string>

namespace tessera::diagnostics {

OffscreenPressureReporter::OffscreenPressureReporter(Logger* logger, MetricsRegistry* metrics)
    : logger_(logger),
      metrics_(metrics) {
    if (metrics_ != nullptr) {
        refused_metric_ = metrics_->add_counter("gpu.offscreen.refused");
        peak_metric_ = metrics_->add_gauge("gpu.offscreen.peak_live");
        bytes_metric_ = metrics_->add_gauge("gpu.offscreen.bytes");
    }
}

bool OffscreenPressureReporter::observe(const OffscreenPressure& pressure, std::uint64_t frame) {
    if (metrics_ != nullptr) {
        metrics_->set(peak_metric_, pressure.peak_live);
        metrics_->set(bytes_metric_, pressure.bytes);
        if (pressure.exhausted > 0) {
            metrics_->increment(refused_metric_, pressure.exhausted);
        }
    }
    if (pressure.exhausted == 0) {
        return false;
    }
    refused_ += pressure.exhausted;
    if (reported_ || logger_ == nullptr) {
        return false;
    }
    // Once per run: the first refusal says what to change, the counter says how often it recurs.
    reported_ = true;
    std::string message = kCode;
    message += ": ";
    message += std::to_string(pressure.exhausted);
    message += " offscreen target request(s) were refused this frame (";
    message += std::to_string(pressure.peak_live);
    message += " live at the peak, ";
    message += std::to_string(pressure.targets);
    message += " of at most ";
    message += std::to_string(pressure.max_targets);
    message += " targets, ";
    message += std::to_string(pressure.bytes);
    if (pressure.max_bytes != 0) {
        message += " of ";
        message += std::to_string(pressure.max_bytes);
    }
    message += " bytes); the layers, masks, filters or glass nodes that asked paint without their "
               "effect. Reduce effect nesting or raise glassMaxBlurTargets. Reported once per run; "
               "the gpu.offscreen.refused counter keeps counting.";
    LogRecord record;
    record.level = LogLevel::Warn;
    record.category = LogCategory::Gpu;
    record.message = std::move(message);
    record.frame = frame;
    return logger_->log(std::move(record));
}

} // namespace tessera::diagnostics
