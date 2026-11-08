// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace tessera::diagnostics {

enum class TraceCategory : std::uint8_t { Frame, Input, Style, Layout, Paint, Rhi, Async, Text };

enum class TracePhase : std::uint8_t { Begin, End, Instant };

[[nodiscard]] std::string_view trace_category_name(TraceCategory category);

struct TraceEvent {
    TracePhase phase = TracePhase::Instant;
    TraceCategory category = TraceCategory::Frame;
    std::string_view name;
    std::uint64_t ts = 0;
    std::uint64_t frame = 0;
    std::uint64_t tick = 0;
    std::uint32_t depth = 0;
};

// Bounded ring of span events. Names are string_views and must outlive the recorder (interned
// literals in practice); timestamps are a deterministic sequence, not wall clock, so recordings
// are reproducible in tests and golden traces.
class TraceRecorder {
public:
    explicit TraceRecorder(std::size_t capacity = 1024);

    void begin(std::string_view name, TraceCategory category, std::uint64_t frame = 0,
               std::uint64_t tick = 0);
    void end(std::string_view name, TraceCategory category, std::uint64_t frame = 0,
             std::uint64_t tick = 0);
    void instant(std::string_view name, TraceCategory category, std::uint64_t frame = 0,
                 std::uint64_t tick = 0);

    void clear();

    [[nodiscard]] const std::deque<TraceEvent>& events() const {
        return events_;
    }
    [[nodiscard]] std::size_t size() const {
        return events_.size();
    }
    [[nodiscard]] std::size_t capacity() const {
        return capacity_;
    }
    [[nodiscard]] std::size_t dropped() const {
        return dropped_;
    }
    [[nodiscard]] std::uint64_t next_timestamp() const {
        return next_ts_;
    }

private:
    void push(TraceEvent event);

    std::deque<TraceEvent> events_;
    std::size_t capacity_ = 0;
    std::size_t dropped_ = 0;
    std::uint64_t next_ts_ = 0;
    std::uint32_t depth_ = 0;
};

// RAII span: records Begin on construction and End on destruction (or end()). Movable, and the
// moved-from span no longer emits End.
class TraceSpan {
public:
    TraceSpan(TraceRecorder& recorder, std::string_view name, TraceCategory category,
              std::uint64_t frame = 0, std::uint64_t tick = 0);
    ~TraceSpan();

    TraceSpan(const TraceSpan&) = delete;
    TraceSpan& operator=(const TraceSpan&) = delete;

    TraceSpan(TraceSpan&& other) noexcept;
    TraceSpan& operator=(TraceSpan&& other) noexcept;

    void end();

private:
    TraceRecorder* recorder_ = nullptr;
    std::string_view name_;
    TraceCategory category_ = TraceCategory::Frame;
    std::uint64_t frame_ = 0;
    std::uint64_t tick_ = 0;
    bool active_ = false;
};

// Chrome Trace Event JSON (array of B/E/i events). The output is stable for a given recorder and
// escapes JSON control characters in names and categories.
[[nodiscard]] std::string to_chrome_trace_json(const TraceRecorder& recorder);

} // namespace tessera::diagnostics
