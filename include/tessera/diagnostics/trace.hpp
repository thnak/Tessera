// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Span tracing is a development and automation feature, never a production one: ADR-022 decision 11
// requires disabled diagnostics to add zero measurable frame time, and
// docs/specs/diagnostics-config-v1.md section 5 keeps `tracing` off in the production profile.
//
// TESSERA_ENABLE_TRACING=OFF (the default) removes the recorder from the build entirely: the type
// below stores nothing, every method inlines to nothing, and no event ring or JSON encoder is
// compiled. Automation call sites keep compiling and `collect_trace()` still answers with the
// documented empty Chrome document (docs/specs/headless-automation-v1.md section 7.1), so nothing
// downstream needs to know which flavour it was built against. Only the recorder's own query
// surface (`events()`), which exists for tests, is unavailable when tracing is off.
#if defined(TESSERA_ENABLE_TRACING)

#include <deque>

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

#else // TESSERA_ENABLE_TRACING

namespace tessera::diagnostics {

// Categories stay available because automation call sites name them; they carry no storage.
enum class TraceCategory : std::uint8_t { Frame, Input, Style, Layout, Paint, Rhi, Async, Text };

// No-op recorder for builds without tracing: an empty object whose every method is an empty inline
// body, so the compiler emits no code for any of it and the object occupies no stack.
class TraceRecorder {
public:
    explicit TraceRecorder(std::size_t capacity = 1024) noexcept {
        (void)capacity;
    }

    void begin(std::string_view, TraceCategory, std::uint64_t = 0, std::uint64_t = 0) noexcept {}
    void end(std::string_view, TraceCategory, std::uint64_t = 0, std::uint64_t = 0) noexcept {}
    void instant(std::string_view, TraceCategory, std::uint64_t = 0, std::uint64_t = 0) noexcept {}

    void clear() noexcept {}

    [[nodiscard]] std::size_t size() const noexcept {
        return 0;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return 0;
    }
    [[nodiscard]] std::size_t dropped() const noexcept {
        return 0;
    }
    [[nodiscard]] std::uint64_t next_timestamp() const noexcept {
        return 0;
    }
};

// Kept so an RAII span declaration never needs its own #if; it records nothing.
class TraceSpan {
public:
    TraceSpan(TraceRecorder&, std::string_view, TraceCategory, std::uint64_t = 0,
              std::uint64_t = 0) noexcept {}
    ~TraceSpan() = default;

    TraceSpan(const TraceSpan&) = delete;
    TraceSpan& operator=(const TraceSpan&) = delete;

    void end() noexcept {}
};

// The documented empty Chrome document (docs/specs/headless-automation-v1.md section 7.1): a
// `trace` request still succeeds, it just reports no spans.
inline std::string to_chrome_trace_json(const TraceRecorder&) {
    return "[]";
}

} // namespace tessera::diagnostics

#endif // TESSERA_ENABLE_TRACING
