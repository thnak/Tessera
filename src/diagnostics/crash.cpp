// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/crash.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::diagnostics {
namespace {

constexpr std::string_view kSchema = "tessera.crash.v1";
// Room kept free for the envelope (schema, signature, truncation counters) so the arrays never push
// the document past its budget.
constexpr std::size_t kEnvelopeReserve = 1024;

// Truncates to at most `max_bytes` without splitting a UTF-8 sequence: a truncated multi-byte
// character would make the JSON document invalid, and a crash report is uploaded, not trusted.
std::size_t utf8_cut(std::string_view value, std::size_t max_bytes) {
    std::size_t count = std::min(value.size(), max_bytes);
    while (count > 0 && count < value.size() &&
           (static_cast<unsigned char>(value[count]) & 0xC0U) == 0x80U) {
        --count;
    }
    return count;
}

// Appends a JSON string, escaping the characters JSON requires and control bytes; bytes >= 0x80 are
// kept verbatim so a valid UTF-8 name survives round-tripping.
void append_escaped(std::string& out, std::string_view value, std::size_t max_bytes) {
    const std::size_t count = utf8_cut(value, max_bytes);
    out.push_back('"');
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 0x0FU]);
            } else {
                out.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    out.push_back('"');
}

// `"key":<escaped>,` for one field; an empty value is omitted so reports stay small.
void append_field(std::string& out, std::string_view key, std::string_view value,
                  std::size_t max_bytes) {
    if (value.empty()) {
        return;
    }
    out.push_back('"');
    out += key;
    out += "\":";
    append_escaped(out, value, max_bytes);
    out.push_back(',');
}

std::string frame_json(const CrashFrame& frame, const CrashReportLimits& limits) {
    std::string out = "{\"module\":";
    append_escaped(out, redact_path(frame.module), limits.max_field_bytes);
    if (!frame.symbol.empty()) {
        out += ",\"symbol\":";
        append_escaped(out, frame.symbol, limits.max_field_bytes);
    }
    out += ",\"offset\":";
    out += std::to_string(frame.offset);
    if (frame.line != 0) {
        out += ",\"line\":";
        out += std::to_string(frame.line);
    }
    out.push_back('}');
    return out;
}

std::string breadcrumb_json(const CrashBreadcrumb& breadcrumb, const CrashReportLimits& limits) {
    std::string out = "{\"sequence\":";
    out += std::to_string(breadcrumb.sequence);
    if (!breadcrumb.category.empty()) {
        out += ",\"category\":";
        append_escaped(out, breadcrumb.category, limits.max_field_bytes);
    }
    out += ",\"message\":";
    append_escaped(out, breadcrumb.message, limits.max_breadcrumb_bytes);
    out.push_back('}');
    return out;
}

} // namespace

std::string redact_path(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos || slash + 1 >= path.size()) {
        return std::string(path);
    }
    return std::string(path.substr(slash + 1));
}

std::string crash_signature(const CrashMetadata& metadata, CrashReportLimits limits) {
    std::string out(
        metadata.build_id.substr(0, utf8_cut(metadata.build_id, limits.max_field_bytes)));
    const std::size_t depth =
        std::min(limits.max_signature_depth, static_cast<std::size_t>(metadata.frames.size()));
    for (std::size_t index = 0; index < depth; ++index) {
        const CrashFrame& frame = metadata.frames[index];
        out.push_back('|');
        out += redact_path(frame.module);
        out.push_back('!');
        if (frame.symbol.empty()) {
            out.push_back('?');
        } else {
            out.append(frame.symbol.substr(0, utf8_cut(frame.symbol, limits.max_field_bytes)));
        }
        out.push_back('+');
        out += std::to_string(frame.offset);
    }
    return out;
}

std::string crash_metadata_json(const CrashMetadata& metadata, CrashReportLimits limits) {
    std::string out;
    out.reserve(1024);
    out += "{\"schema\":";
    append_escaped(out, kSchema, kSchema.size());
    out.push_back(',');
    append_field(out, "appVersion", metadata.app_version, limits.max_field_bytes);
    append_field(out, "commit", metadata.commit, limits.max_field_bytes);
    append_field(out, "buildId", metadata.build_id, limits.max_field_bytes);
    append_field(out, "platform", metadata.platform, limits.max_field_bytes);
    append_field(out, "arch", metadata.arch, limits.max_field_bytes);
    append_field(out, "os", metadata.os, limits.max_field_bytes);
    append_field(out, "gpu", metadata.gpu, limits.max_field_bytes);
    append_field(out, "configHash", metadata.config_hash, limits.max_field_bytes);
    append_field(out, "reason", metadata.crash_reason, limits.max_field_bytes);
    append_field(out, "thread", metadata.thread, limits.max_field_bytes);
    append_field(out, "lastTraceId", metadata.last_trace_id, limits.max_field_bytes);

    // Frames are ordered innermost first; the cap keeps the innermost ones (they group the report)
    // and reports the rest as dropped.
    std::size_t dropped_frames = 0;
    out += "\"frames\":[";
    const std::size_t frame_limit = std::min(limits.max_frames, metadata.frames.size());
    bool first = true;
    bool text_full = false;
    for (std::size_t index = 0; index < frame_limit; ++index) {
        const std::string entry = frame_json(metadata.frames[index], limits);
        if (out.size() + entry.size() + kEnvelopeReserve > limits.max_text_bytes) {
            dropped_frames = metadata.frames.size() - index;
            text_full = true;
            break;
        }
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += entry;
    }
    if (!text_full) {
        dropped_frames = metadata.frames.size() - frame_limit;
    }
    out += "],";

    // Breadcrumbs keep the newest entries in their original order: the tail is what explains the
    // crash, and the ring is oldest-first.
    const std::size_t breadcrumb_limit =
        std::min(limits.max_breadcrumbs, static_cast<std::size_t>(metadata.breadcrumbs.size()));
    const std::size_t breadcrumb_begin = metadata.breadcrumbs.size() - breadcrumb_limit;
    std::size_t dropped_breadcrumbs = breadcrumb_begin;
    out += "\"breadcrumbs\":[";
    first = true;
    for (std::size_t index = breadcrumb_begin; index < metadata.breadcrumbs.size(); ++index) {
        const std::string entry = breadcrumb_json(metadata.breadcrumbs[index], limits);
        if (out.size() + entry.size() + kEnvelopeReserve > limits.max_text_bytes) {
            dropped_breadcrumbs += metadata.breadcrumbs.size() - index;
            text_full = true;
            break;
        }
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += entry;
    }
    out += "],";

    out += "\"signature\":";
    append_escaped(out, crash_signature(metadata, limits), limits.max_field_bytes * 4);
    out += ",\"truncated\":{\"frames\":";
    out += std::to_string(dropped_frames);
    out += ",\"breadcrumbs\":";
    out += std::to_string(dropped_breadcrumbs);
    out += ",\"text\":";
    out += text_full ? "true" : "false";
    out += "}}";
    return out;
}

} // namespace tessera::diagnostics
