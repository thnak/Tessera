// SPDX-License-Identifier: MIT

// Diagnostics exporters: canonical, bounded JSON for the recorded trace events and the metrics
// snapshot. Pure string builders - no file I/O and no wall clock - so a trace or a metric snapshot
// is reproducible byte-for-byte in tests and golden files (docs/design/diagnostics.md).
//
// Both documents are objects (not bare arrays) so a bounded export can still be valid JSON while it
// reports what it dropped.

#pragma once

#include "tessera/diagnostics/metrics.hpp"

#if defined(TESSERA_ENABLE_TRACING)
#include "tessera/diagnostics/trace.hpp"
#endif

#include <cstddef>
#include <span>
#include <string>

namespace tessera::diagnostics {

struct ExportLimits {
    // Most recent trace events to emit; older events are dropped and counted.
    std::size_t max_events = 4096;
    // Hard cap on the emitted document; the builder stops adding entries before it is crossed.
    std::size_t max_text_bytes = 1U << 20;
};

#if defined(TESSERA_ENABLE_TRACING)
// Chrome Trace Event Format for `events` (chronological ring order). Phases map to B/E/i, `tid` is
// the span depth so nested spans read as lanes, and `frame`/`tick` travel in `args` when non-zero.
// `truncated`/`droppedEvents` report a bounded export; the document is always valid JSON.
//
// Declared for the bounded exporter that replaces `to_chrome_trace_json` once it lands; nothing
// defines it yet, so no caller may use it. Only available where tracing is compiled in.
[[nodiscard]] std::string chrome_trace_json(std::span<const TraceEvent> events,
                                            ExportLimits limits = {});
#endif

// Canonical metrics snapshot in the given order (the registry's registration order).
[[nodiscard]] std::string metrics_json(std::span<const MetricSample> samples,
                                       ExportLimits limits = {});

} // namespace tessera::diagnostics
