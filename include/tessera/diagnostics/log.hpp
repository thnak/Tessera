// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::diagnostics {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Fatal };

enum class LogCategory : std::uint8_t {
    App,
    Window,
    Input,
    Style,
    Layout,
    Paint,
    Rhi,
    Vulkan,
    Gpu,
    Async,
    Io,
    Text,
    Audio,
    Security,
};

[[nodiscard]] std::string_view level_name(LogLevel level);
[[nodiscard]] std::string_view category_name(LogCategory category);

[[nodiscard]] constexpr std::uint32_t category_bit(LogCategory category) {
    return 1u << static_cast<std::uint32_t>(category);
}

[[nodiscard]] constexpr std::uint32_t all_categories() {
    return (1u << 14u) - 1u;
}

// Replaces absolute user home prefixes (Windows and POSIX) with "<home>" and leaves the rest of
// the path intact. Deterministic and allocation-light, so it can run on the logging path.
[[nodiscard]] std::string redact_paths(std::string_view text);

struct LogRecord {
    LogLevel level = LogLevel::Info;
    LogCategory category = LogCategory::App;
    std::string message;
    std::uint64_t frame = 0;
    std::uint64_t tick = 0;
    std::uint32_t window = 0;
};

class LogSink {
public:
    virtual ~LogSink() = default;

    virtual void write(const LogRecord& record) = 0;
};

// Bounded breadcrumb ring for tests and crash reports. Records are kept in chronological order;
// the oldest record is discarded when the capacity is reached.
class MemoryLogSink final : public LogSink {
public:
    explicit MemoryLogSink(std::size_t capacity = 256);

    void write(const LogRecord& record) override;
    void clear();

    [[nodiscard]] const std::deque<LogRecord>& records() const {
        return records_;
    }
    [[nodiscard]] std::size_t dropped() const {
        return dropped_;
    }
    [[nodiscard]] std::size_t capacity() const {
        return capacity_;
    }

private:
    std::deque<LogRecord> records_;
    std::size_t capacity_ = 0;
    std::size_t dropped_ = 0;
};

struct LogConfig {
    LogLevel level = LogLevel::Info;
    std::uint32_t categories = all_categories();
    bool redact_paths = true;
};

// Structured logger core. A disabled record costs one level/category comparison and never
// touches the message; sinks are non-owning and must outlive the logger. Not thread-safe yet;
// the future lock-free per-thread rings wrap this seam.
class Logger {
public:
    explicit Logger(LogConfig config = {});

    void add_sink(LogSink& sink);
    void clear_sinks();

    [[nodiscard]] bool enabled(LogLevel level, LogCategory category) const noexcept;

    // Redacts and forwards an enabled record. Returns false when it was filtered out.
    bool log(LogRecord record);
    bool log(LogLevel level, LogCategory category, std::string message);

    [[nodiscard]] const LogConfig& config() const {
        return config_;
    }
    void set_config(LogConfig config) {
        config_ = config;
    }

private:
    LogConfig config_;
    std::vector<LogSink*> sinks_;
};

} // namespace tessera::diagnostics
