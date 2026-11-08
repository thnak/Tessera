// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/log.hpp"

#include <algorithm>
#include <utility>

namespace tessera::diagnostics {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

} // namespace

std::string_view level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Fatal:
        return "fatal";
    }
    return "info";
}

std::string_view category_name(LogCategory category) {
    switch (category) {
    case LogCategory::App:
        return "app";
    case LogCategory::Window:
        return "window";
    case LogCategory::Input:
        return "input";
    case LogCategory::Style:
        return "style";
    case LogCategory::Layout:
        return "layout";
    case LogCategory::Paint:
        return "paint";
    case LogCategory::Rhi:
        return "rhi";
    case LogCategory::Vulkan:
        return "vulkan";
    case LogCategory::Gpu:
        return "gpu";
    case LogCategory::Async:
        return "async";
    case LogCategory::Io:
        return "io";
    case LogCategory::Text:
        return "text";
    case LogCategory::Audio:
        return "audio";
    case LogCategory::Security:
        return "security";
    }
    return "app";
}

std::string redact_paths(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    constexpr std::string_view kWindowsBackslash = "C:\\Users\\";
    constexpr std::string_view kWindowsSlash = "C:/Users/";
    constexpr std::string_view kPosixHome = "/home/";
    constexpr std::string_view kMacHome = "/Users/";

    std::size_t index = 0;
    while (index < text.size()) {
        std::string_view prefix;
        if (starts_with(text.substr(index), kWindowsBackslash)) {
            prefix = kWindowsBackslash;
        } else if (starts_with(text.substr(index), kWindowsSlash)) {
            prefix = kWindowsSlash;
        } else if (starts_with(text.substr(index), kPosixHome)) {
            prefix = kPosixHome;
        } else if (starts_with(text.substr(index), kMacHome)) {
            prefix = kMacHome;
        }

        if (prefix.empty()) {
            result.push_back(text[index]);
            ++index;
            continue;
        }

        const std::size_t user_start = index + prefix.size();
        std::size_t user_end = user_start;
        while (user_end < text.size() && text[user_end] != '\\' && text[user_end] != '/' &&
               text[user_end] != '"' && text[user_end] != '\'' && text[user_end] != ' ' &&
               text[user_end] != '\n' && text[user_end] != '\t') {
            ++user_end;
        }
        if (user_end == user_start) {
            result.push_back(text[index]);
            ++index;
            continue;
        }
        result += "<home>";
        index = user_end;
    }
    return result;
}

MemoryLogSink::MemoryLogSink(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

void MemoryLogSink::write(const LogRecord& record) {
    if (records_.size() >= capacity_) {
        records_.pop_front();
        ++dropped_;
    }
    records_.push_back(record);
}

void MemoryLogSink::clear() {
    records_.clear();
    dropped_ = 0;
}

Logger::Logger(LogConfig config)
    : config_(config) {}

void Logger::add_sink(LogSink& sink) {
    sinks_.push_back(&sink);
}

void Logger::clear_sinks() {
    sinks_.clear();
}

bool Logger::enabled(LogLevel level, LogCategory category) const noexcept {
    return static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(config_.level) &&
           (config_.categories & category_bit(category)) != 0;
}

bool Logger::log(LogRecord record) {
    if (!enabled(record.level, record.category)) {
        return false;
    }
    if (config_.redact_paths) {
        record.message = redact_paths(record.message);
    }
    for (LogSink* sink : sinks_) {
        sink->write(record);
    }
    return true;
}

bool Logger::log(LogLevel level, LogCategory category, std::string message) {
    LogRecord record;
    record.level = level;
    record.category = category;
    record.message = std::move(message);
    return log(std::move(record));
}

} // namespace tessera::diagnostics
