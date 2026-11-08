// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/trace.hpp"

#include <algorithm>
#include <utility>

namespace tessera::diagnostics {
namespace {

void append_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
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
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
}

const char* phase_name(TracePhase phase) {
    switch (phase) {
    case TracePhase::Begin:
        return "B";
    case TracePhase::End:
        return "E";
    case TracePhase::Instant:
        return "i";
    }
    return "i";
}

} // namespace

std::string_view trace_category_name(TraceCategory category) {
    switch (category) {
    case TraceCategory::Frame:
        return "frame";
    case TraceCategory::Input:
        return "input";
    case TraceCategory::Style:
        return "style";
    case TraceCategory::Layout:
        return "layout";
    case TraceCategory::Paint:
        return "paint";
    case TraceCategory::Rhi:
        return "rhi";
    case TraceCategory::Async:
        return "async";
    case TraceCategory::Text:
        return "text";
    }
    return "frame";
}

TraceRecorder::TraceRecorder(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

void TraceRecorder::begin(std::string_view name, TraceCategory category, std::uint64_t frame,
                          std::uint64_t tick) {
    TraceEvent event;
    event.phase = TracePhase::Begin;
    event.category = category;
    event.name = name;
    event.frame = frame;
    event.tick = tick;
    event.depth = depth_;
    ++depth_;
    push(event);
}

void TraceRecorder::end(std::string_view name, TraceCategory category, std::uint64_t frame,
                        std::uint64_t tick) {
    if (depth_ > 0) {
        --depth_;
    }
    TraceEvent event;
    event.phase = TracePhase::End;
    event.category = category;
    event.name = name;
    event.frame = frame;
    event.tick = tick;
    event.depth = depth_;
    push(event);
}

void TraceRecorder::instant(std::string_view name, TraceCategory category, std::uint64_t frame,
                            std::uint64_t tick) {
    TraceEvent event;
    event.phase = TracePhase::Instant;
    event.category = category;
    event.name = name;
    event.frame = frame;
    event.tick = tick;
    event.depth = depth_;
    push(event);
}

void TraceRecorder::clear() {
    events_.clear();
    dropped_ = 0;
    next_ts_ = 0;
    depth_ = 0;
}

void TraceRecorder::push(TraceEvent event) {
    if (events_.size() >= capacity_) {
        events_.pop_front();
        ++dropped_;
    }
    event.ts = next_ts_++;
    events_.push_back(event);
}

TraceSpan::TraceSpan(TraceRecorder& recorder, std::string_view name, TraceCategory category,
                     std::uint64_t frame, std::uint64_t tick)
    : recorder_(&recorder),
      name_(name),
      category_(category),
      frame_(frame),
      tick_(tick),
      active_(true) {
    recorder_->begin(name_, category_, frame_, tick_);
}

TraceSpan::~TraceSpan() {
    end();
}

TraceSpan::TraceSpan(TraceSpan&& other) noexcept
    : recorder_(other.recorder_),
      name_(other.name_),
      category_(other.category_),
      frame_(other.frame_),
      tick_(other.tick_),
      active_(other.active_) {
    other.active_ = false;
    other.recorder_ = nullptr;
}

TraceSpan& TraceSpan::operator=(TraceSpan&& other) noexcept {
    if (this != &other) {
        end();
        recorder_ = other.recorder_;
        name_ = other.name_;
        category_ = other.category_;
        frame_ = other.frame_;
        tick_ = other.tick_;
        active_ = other.active_;
        other.active_ = false;
        other.recorder_ = nullptr;
    }
    return *this;
}

void TraceSpan::end() {
    if (!active_ || recorder_ == nullptr) {
        return;
    }
    recorder_->end(name_, category_, frame_, tick_);
    active_ = false;
}

std::string to_chrome_trace_json(const TraceRecorder& recorder) {
    std::string out;
    out.reserve(recorder.size() * 96 + 2);
    out.push_back('[');
    bool first = true;
    for (const TraceEvent& event : recorder.events()) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += "{\"name\":";
        append_json_string(out, event.name);
        out += ",\"cat\":";
        append_json_string(out, trace_category_name(event.category));
        out += ",\"ph\":\"";
        out += phase_name(event.phase);
        out += "\",\"ts\":";
        out += std::to_string(event.ts);
        out += ",\"pid\":1,\"tid\":";
        out += std::to_string(event.depth + 1);
        if (event.phase == TracePhase::Instant) {
            out += ",\"s\":\"t\"";
        }
        out += ",\"args\":{\"frame\":";
        out += std::to_string(event.frame);
        out += ",\"tick\":";
        out += std::to_string(event.tick);
        out += "}}";
    }
    out.push_back(']');
    return out;
}

} // namespace tessera::diagnostics
