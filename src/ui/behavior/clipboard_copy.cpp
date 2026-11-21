// SPDX-License-Identifier: MIT

#include "tessera/hal/clipboard.hpp"
#include "tessera/ui/behavior/clipboard_copy.hpp"

#include <string_view>

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `text`.
constexpr std::string_view kOptionNames[] = {"text"};

} // namespace

ClipboardCopy::~ClipboardCopy() {
    detach();
}

void ClipboardCopy::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    (void)targets; // a copy button has no target: the seam is the window's
    clipboard_ = services.clipboard;
    // The `text` option is what a copy button copies. A hook with no text copies nothing, which is
    // the honest answer rather than copying an empty string over the user's clipboard.
    const ArgValue* text = args.find("text", kOptionNames);
    if (text != nullptr && text->kind == ArgValue::Kind::String && !text->text.empty()) {
        copy(text->text, services.now_s);
    }
}

void ClipboardCopy::update(const ArgsView& args) {
    // An update re-evaluates the text: a component whose text changed copies the new value on the
    // next update. The feedback window is not restarted by an update that copies the same text,
    // because `copy` is only called when the text is non-empty and the caller asked for it.
    const ArgValue* text = args.find("text", kOptionNames);
    if (text != nullptr && text->kind == ArgValue::Kind::String && !text->text.empty()) {
        copy(text->text, 0.0);
    }
}

void ClipboardCopy::detach() {
    clipboard_ = nullptr;
    feedback_active_ = false;
    deadline_s_ = 0.0;
    outputs_ = {};
}

std::span<const OutputValue> ClipboardCopy::outputs() const {
    return outputs_;
}

bool ClipboardCopy::copy(std::string_view text, double now_s) {
    outputs_[0].kind = OutputValue::Kind::Bool;
    outputs_[1].kind = OutputValue::Kind::Bool;
    if (clipboard_ == nullptr) {
        // No seam: the copy failed, and the component can say so rather than showing "Copied".
        outputs_[0].boolean = false;
        outputs_[1].boolean = true;
        feedback_active_ = false;
        return false;
    }
    const std::expected<void, hal::ClipboardError> written = clipboard_->write_text(text);
    const bool ok = written.has_value();
    outputs_[0].boolean = ok;
    outputs_[1].boolean = !ok;
    feedback_active_ = true;
    deadline_s_ = now_s + kFeedbackSeconds;
    return ok;
}

void ClipboardCopy::tick(double now_s) {
    if (!feedback_active_ || now_s < deadline_s_) {
        return;
    }
    feedback_active_ = false;
    outputs_[0].boolean = false;
    outputs_[1].boolean = false;
}

} // namespace tessera::ui::behavior
