// SPDX-License-Identifier: MIT
//
// The lifecycle script decoder (see lifecycle_script.hpp for why the format exists and why it is
// not a harness action).
//
// The decoder is a strict, bounded, all-or-nothing reader for the subset the format uses: one top
// level object with a closed member set, one array of objects, strings without escapes, integers
// and nothing else. It does not try to be a JSON library - layer 1 links none, and the harness's
// decoder is a tool dependency. Being strict is the point: a lifecycle script is untrusted input,
// and a silently ignored member would let a typo (`expects`) turn a check into a no-op.

#include "tessera/appfw/lifecycle_script.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {
namespace {

// The largest nesting the format needs (object -> array -> object). A deeper document is rejected
// instead of recursing, so a malformed script can never cost stack.
constexpr std::size_t kMaxDepth = 4;

class Reader {
public:
    explicit Reader(std::string_view text)
        : text_(text) {}

    [[nodiscard]] bool at_end() {
        skip_space();
        return index_ >= text_.size();
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        skip_space();
        if (index_ >= text_.size() || text_[index_] != expected) {
            return false;
        }
        ++index_;
        return true;
    }

    [[nodiscard]] bool peek(char expected) noexcept {
        skip_space();
        return index_ < text_.size() && text_[index_] == expected;
    }

    // A quoted string without escape sequences, which is all the format uses. An escape is rejected
    // rather than decoded: the values are names from a closed set, not user text.
    [[nodiscard]] std::optional<std::string> read_string() {
        skip_space();
        if (index_ >= text_.size() || text_[index_] != '"') {
            return std::nullopt;
        }
        ++index_;
        const std::size_t start = index_;
        while (index_ < text_.size() && text_[index_] != '"') {
            const char c = text_[index_];
            if (c == '\\' || static_cast<unsigned char>(c) < 0x20) {
                return std::nullopt;
            }
            ++index_;
        }
        if (index_ >= text_.size()) {
            return std::nullopt;
        }
        std::string value(text_.substr(start, index_ - start));
        ++index_;
        return value;
    }

    // A non-negative integer that fits the cap the caller passes.
    [[nodiscard]] std::optional<std::size_t> read_integer(std::size_t max_value) {
        skip_space();
        const std::size_t start = index_;
        std::size_t value = 0;
        while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
            value = value * 10 + static_cast<std::size_t>(text_[index_] - '0');
            if (value > max_value) {
                return std::nullopt;
            }
            ++index_;
        }
        if (index_ == start) {
            return std::nullopt;
        }
        return value;
    }

    // Skips one value the caller does not read, so an unknown-position value can be rejected by the
    // caller instead of desynchronizing the reader. Only the shapes the format can contain.
    [[nodiscard]] bool skip_value(std::size_t depth = 0) noexcept {
        if (depth > kMaxDepth) {
            return false;
        }
        skip_space();
        if (index_ >= text_.size()) {
            return false;
        }
        const char c = text_[index_];
        if (c == '"') {
            return read_string().has_value();
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++index_;
            if (peek(close)) {
                ++index_;
                return true;
            }
            while (true) {
                if (c == '{') {
                    if (!read_string().has_value() || !consume(':')) {
                        return false;
                    }
                }
                if (!skip_value(depth + 1)) {
                    return false;
                }
                if (consume(',')) {
                    continue;
                }
                return consume(close);
            }
        }
        // Number, true, false or null: consume the token and let the caller's closed member set
        // reject it if it is not a shape the format allows there.
        const std::size_t start = index_;
        while (index_ < text_.size() && text_[index_] != ',' && text_[index_] != '}' &&
               text_[index_] != ']' && text_[index_] != ' ' && text_[index_] != '\n' &&
               text_[index_] != '\r' && text_[index_] != '\t') {
            ++index_;
        }
        return index_ > start;
    }

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }

private:
    void skip_space() noexcept {
        while (index_ < text_.size()) {
            const char c = text_[index_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++index_;
        }
    }

    std::string_view text_;
    std::size_t index_ = 0;
};

std::unexpected<LifecycleScriptError> fail(LifecycleScriptErrorCode code, std::string detail,
                                           std::size_t step = kNoLifecycleStep) {
    return std::unexpected(LifecycleScriptError{code, std::move(detail), step});
}

// Parses one step object: exactly `event` and `expect`, both strings from their closed sets.
std::expected<LifecycleScriptStep, LifecycleScriptError> parse_step(Reader& reader,
                                                                    std::size_t index) {
    if (!reader.consume('{')) {
        return fail(LifecycleScriptErrorCode::InvalidStep, "step must be an object", index);
    }
    LifecycleScriptStep step;
    bool has_event = false;
    bool has_expect = false;
    if (reader.consume('}')) {
        return fail(LifecycleScriptErrorCode::InvalidStep, "step must not be empty", index);
    }
    while (true) {
        auto key = reader.read_string();
        if (!key.has_value() || !reader.consume(':')) {
            return fail(LifecycleScriptErrorCode::InvalidStep, "step member must be a string key",
                        index);
        }
        if (*key == "event") {
            if (has_event) {
                return fail(LifecycleScriptErrorCode::InvalidStep, "duplicate step.event", index);
            }
            auto name = reader.read_string();
            LifecycleSignal signal{};
            if (!name.has_value() || !lifecycle_signal_from_name(*name, signal)) {
                return fail(LifecycleScriptErrorCode::UnknownEvent,
                            "step.event is not a lifecycle signal", index);
            }
            step.event.signal = signal;
            has_event = true;
        } else if (*key == "expect") {
            if (has_expect) {
                return fail(LifecycleScriptErrorCode::InvalidStep, "duplicate step.expect", index);
            }
            auto name = reader.read_string();
            AppState state{};
            if (!name.has_value() || !app_state_from_name(*name, state)) {
                return fail(LifecycleScriptErrorCode::UnknownState,
                            "step.expect is not an app state", index);
            }
            step.expected = state;
            has_expect = true;
        } else {
            return fail(LifecycleScriptErrorCode::InvalidStep, "step has an unknown member", index);
        }
        if (reader.consume(',')) {
            continue;
        }
        if (!reader.consume('}')) {
            return fail(LifecycleScriptErrorCode::InvalidStep, "step is not closed", index);
        }
        break;
    }
    if (!has_event || !has_expect) {
        return fail(LifecycleScriptErrorCode::InvalidStep, "step needs event and expect", index);
    }
    return step;
}

} // namespace

std::expected<LifecycleScript, LifecycleScriptError> parse_lifecycle_script(std::string_view text) {
    if (text.size() > kMaxLifecycleScriptBytes) {
        return fail(LifecycleScriptErrorCode::ScriptTooLarge, "script exceeds the 64 KiB cap");
    }
    Reader reader(text);
    if (!reader.consume('{')) {
        return fail(LifecycleScriptErrorCode::MalformedScript, "script must be a JSON object");
    }
    LifecycleScript script;
    bool has_version = false;
    bool has_steps = false;
    if (reader.consume('}')) {
        return fail(LifecycleScriptErrorCode::MalformedScript, "script must not be empty");
    }
    while (true) {
        auto key = reader.read_string();
        if (!key.has_value() || !reader.consume(':')) {
            return fail(LifecycleScriptErrorCode::MalformedScript,
                        "script member must be a string key");
        }
        if (*key == "scriptVersion") {
            const auto version = reader.read_integer(1000);
            if (!version.has_value()) {
                return fail(LifecycleScriptErrorCode::MalformedScript,
                            "scriptVersion must be an integer");
            }
            if (*version != 1) {
                return fail(LifecycleScriptErrorCode::UnsupportedVersion,
                            "unsupported script version");
            }
            has_version = true;
        } else if (*key == "script") {
            auto name = reader.read_string();
            if (!name.has_value()) {
                return fail(LifecycleScriptErrorCode::MalformedScript, "script must be a string");
            }
            script.name = std::move(*name);
        } else if (*key == "description") {
            if (!reader.read_string().has_value()) {
                return fail(LifecycleScriptErrorCode::MalformedScript,
                            "description must be a string");
            }
        } else if (*key == "steps") {
            if (has_steps) {
                return fail(LifecycleScriptErrorCode::MalformedScript, "duplicate steps");
            }
            has_steps = true;
            if (!reader.consume('[')) {
                return fail(LifecycleScriptErrorCode::MalformedScript, "steps must be an array");
            }
            if (reader.consume(']')) {
                return fail(LifecycleScriptErrorCode::MalformedScript, "steps must not be empty");
            }
            while (true) {
                if (script.steps.size() >= kMaxLifecycleScriptSteps) {
                    return fail(LifecycleScriptErrorCode::ScriptTooLarge,
                                "script has more steps than the cap", script.steps.size());
                }
                auto step = parse_step(reader, script.steps.size());
                if (!step.has_value()) {
                    return std::unexpected(std::move(step.error()));
                }
                script.steps.push_back(std::move(*step));
                if (reader.consume(',')) {
                    continue;
                }
                if (!reader.consume(']')) {
                    return fail(LifecycleScriptErrorCode::MalformedScript, "steps is not closed");
                }
                break;
            }
        } else {
            return fail(LifecycleScriptErrorCode::MalformedScript, "script has an unknown member");
        }
        if (reader.consume(',')) {
            continue;
        }
        if (!reader.consume('}')) {
            return fail(LifecycleScriptErrorCode::MalformedScript, "script is not closed");
        }
        break;
    }
    if (!has_version) {
        return fail(LifecycleScriptErrorCode::MalformedScript, "scriptVersion is required");
    }
    if (!has_steps) {
        return fail(LifecycleScriptErrorCode::MalformedScript, "steps is required");
    }
    if (!reader.at_end()) {
        return fail(LifecycleScriptErrorCode::MalformedScript, "trailing content after the script",
                    reader.index());
    }
    return script;
}

} // namespace tessera::appfw
