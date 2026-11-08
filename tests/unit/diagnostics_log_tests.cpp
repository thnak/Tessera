// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/log.hpp"

#include <iostream>
#include <string>

namespace {

using tessera::diagnostics::category_bit;
using tessera::diagnostics::category_name;
using tessera::diagnostics::level_name;
using tessera::diagnostics::LogCategory;
using tessera::diagnostics::LogConfig;
using tessera::diagnostics::Logger;
using tessera::diagnostics::LogLevel;
using tessera::diagnostics::MemoryLogSink;
using tessera::diagnostics::redact_paths;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "diagnostics_log: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    if (!expect(level_name(LogLevel::Warn) == "warn", "level name") ||
        !expect(category_name(LogCategory::Layout) == "layout", "category name") ||
        !expect((category_bit(LogCategory::Security) & tessera::diagnostics::all_categories()) != 0,
                "security category is in all_categories")) {
        return 1;
    }

    if (!expect(redact_paths("open C:\\Users\\alice\\file.txt") == "open <home>\\file.txt",
                "windows home redaction") ||
        !expect(redact_paths("/home/bob/project/main.cpp") == "<home>/project/main.cpp",
                "posix home redaction") ||
        !expect(redact_paths("/Users/carol/Library") == "<home>/Library", "mac home redaction") ||
        !expect(redact_paths("relative/path/file.txt") == "relative/path/file.txt",
                "relative paths stay unchanged")) {
        return 2;
    }

    MemoryLogSink sink;
    Logger logger;
    logger.add_sink(sink);

    if (!expect(logger.enabled(LogLevel::Info, LogCategory::App), "info enabled by default") ||
        !expect(!logger.log(LogLevel::Debug, LogCategory::App, "hidden"), "debug filtered") ||
        !expect(sink.records().empty(), "filtered record is not written") ||
        !expect(logger.log(LogLevel::Info, LogCategory::App, "hello"), "info written") ||
        !expect(sink.records().size() == 1, "one record") ||
        !expect(sink.records().front().message == "hello", "message preserved")) {
        return 3;
    }

    tessera::diagnostics::LogRecord record;
    record.level = LogLevel::Warn;
    record.category = LogCategory::Gpu;
    record.message = "device C:\\Users\\alice\\gpu";
    record.frame = 7;
    record.tick = 9;
    record.window = 2;
    if (!expect(logger.log(record), "record written") ||
        !expect(sink.records().back().message == "device <home>\\gpu", "record message redacted") ||
        !expect(sink.records().back().frame == 7, "frame preserved") ||
        !expect(sink.records().back().tick == 9, "tick preserved") ||
        !expect(sink.records().back().window == 2, "window preserved")) {
        return 4;
    }

    LogConfig categories;
    categories.level = LogLevel::Debug;
    categories.categories = category_bit(LogCategory::Window);
    Logger filtered(categories);
    MemoryLogSink filtered_sink;
    filtered.add_sink(filtered_sink);
    if (!expect(!filtered.log(LogLevel::Info, LogCategory::App, "app"), "app filtered") ||
        !expect(filtered.log(LogLevel::Debug, LogCategory::Window, "window"), "window enabled") ||
        !expect(filtered_sink.records().size() == 1, "only window record")) {
        return 5;
    }

    LogConfig no_redaction;
    no_redaction.redact_paths = false;
    Logger unredacted(no_redaction);
    MemoryLogSink unredacted_sink;
    unredacted.add_sink(unredacted_sink);
    if (!expect(unredacted.log(LogLevel::Info, LogCategory::Io, "C:\\Users\\alice\\x"),
                "unredacted written") ||
        !expect(unredacted_sink.records().front().message == "C:\\Users\\alice\\x",
                "redaction can be disabled")) {
        return 6;
    }

    MemoryLogSink ring(2);
    Logger bounded;
    bounded.add_sink(ring);
    (void)bounded.log(LogLevel::Info, LogCategory::App, "a");
    (void)bounded.log(LogLevel::Info, LogCategory::App, "b");
    (void)bounded.log(LogLevel::Info, LogCategory::App, "c");
    if (!expect(ring.records().size() == 2, "ring keeps capacity") ||
        !expect(ring.records().front().message == "b", "ring keeps newest") ||
        !expect(ring.records().back().message == "c", "ring order") ||
        !expect(ring.dropped() == 1, "ring counts drops")) {
        return 7;
    }

    MemoryLogSink first;
    MemoryLogSink second;
    Logger multi;
    multi.add_sink(first);
    multi.add_sink(second);
    if (!expect(multi.log(LogLevel::Info, LogCategory::Async, "fanout"), "fanout written") ||
        !expect(first.records().size() == 1 && second.records().size() == 1, "both sinks")) {
        return 8;
    }

    std::cout << "diagnostics_log: ok\n";
    return 0;
}
