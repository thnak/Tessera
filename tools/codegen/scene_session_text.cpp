// SPDX-License-Identifier: MIT
//
// Typed text in the preview session (issue #601, docs/design/canvas-text-input.md). A `TextInput`
// or `TextArea` gets the engine's own `EditController` and `TextInputRuntime`, bound to the node,
// so the preview edits a field exactly as a generated component does: the router delivers
// Text/EditText/ Composition*/KeyDown to the focused node, the runtime edits the buffer and emits
// one `TextChange` per committed edit, and that event runs the `onChange` IR body with a
// `TextChangeEvent` record. Split out of `scene_session.cpp`, which is at its size budget.

#include "scene_build.hpp"
#include "scene_session.hpp"
#include "scene_session_slots.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

// The longest prefix of `text` that fits `limit` bytes and ends on a code point boundary.
std::size_t utf8_prefix(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return text.size();
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return end;
}

} // namespace

void fill_text_event(const PreviewInput& input, ui::InputEvent& event) {
    switch (input.kind) {
    case PreviewInput::Kind::CompositionStart:
        event.type = ui::InputEventType::CompositionStart;
        break;
    case PreviewInput::Kind::CompositionUpdate:
        event.type = ui::InputEventType::CompositionUpdate;
        break;
    case PreviewInput::Kind::CompositionEnd:
        event.type = ui::InputEventType::CompositionEnd;
        break;
    default:
        // Every commit is an `EditText`: one insert, one undo step, one `TextChange`.
        event.type = ui::InputEventType::EditText;
        break;
    }
    const std::size_t size = utf8_prefix(input.text, event.text.size());
    if (size != 0) {
        std::memcpy(event.text.data(), input.text.data(), size);
    }
    event.text_size = static_cast<std::uint32_t>(size);
    event.text_truncated = size < input.text.size();
    event.selection_start = event.text_size;
    event.selection_end = event.text_size;
}

void PreviewSession::detach_text_fields() {
    for (const auto& field : text_fields_) {
        field->runtime.detach();
    }
}

void PreviewSession::attach_text_fields(const SceneBuildResult& built) {
    std::vector<std::unique_ptr<TextField>> next;
    for (const SceneBuildResult::SceneTextField& spec : built.text_fields) {
        ui::Node* const node = node_by_automation_id(spec.automation_id);
        if (node == nullptr) {
            continue;
        }
        std::unique_ptr<TextField> field;
        for (auto& old : text_fields_) {
            if (old != nullptr && old->automation_id == spec.automation_id) {
                field = std::move(old);
                break;
            }
        }
        // The painted value the build evaluated is the controlled value (or the default).
        const std::optional<ui::PaintData> painted = node->paint_data();
        const std::string value = painted.has_value() ? painted->text : std::string();
        if (field == nullptr) {
            field = std::make_unique<TextField>(this, spec.automation_id);
            field->editor.set_text(value);
        } else if (spec.controlled && field->editor.buffer().text() != value) {
            // The scope disagrees with the buffer: an `onChange` that did not store the text (a
            // controlled field that refuses an edit) or a state change from elsewhere.
            field->editor.set_text(value);
        }
        field->editor.set_multiline(spec.multiline);

        field->handler_id.clear();
        field->params = nullptr;
        field->actions = nullptr;
        field->can_run = false;
        for (const SceneBuildResult::SceneEventHandler& handler : built.handlers) {
            if (handler.automation_id != spec.automation_id || handler.event != "onChange") {
                continue;
            }
            field->handler_id = handler.handler;
            field->frame = handler.frame;
            if (resolve_body(module_, *frames_[handler.frame].component, handler.handler,
                             field->params, field->actions)) {
                field->can_run = field->params == nullptr || field->params->empty() ||
                                 field->params->front().type == "TextChangeEvent";
            }
            break;
        }

        field->runtime.attach(node);
        constexpr ui::EventType kEditing[] = {
            ui::EventType::Text,
            ui::EventType::EditText,
            ui::EventType::CompositionStart,
            ui::EventType::CompositionUpdate,
            ui::EventType::CompositionEnd,
            ui::EventType::KeyDown,
        };
        for (const ui::EventType type : kEditing) {
            (void)node->bind_event(type, &PreviewSession::on_text_event, field.get());
        }
        (void)node->bind_event(ui::EventType::TextChange, &PreviewSession::on_text_change,
                               field.get());
        next.push_back(std::move(field));
    }
    text_fields_ = std::move(next);
}

void PreviewSession::on_text_event(void* context, const ui::Event& event) {
    auto* const field = static_cast<TextField*>(context);
    if (field != nullptr && field->runtime.handle_event(event)) {
        field->session->text_consumed_ = true;
    }
}

void PreviewSession::on_text_change(void* context, const ui::Event& event) {
    auto* const field = static_cast<TextField*>(context);
    if (field == nullptr || field->handler_id.empty()) {
        return; // an uncontrolled field with no `onChange`: the buffer is the value
    }
    PreviewSession& session = *field->session;
    if (!field->can_run) {
        session.unrun_.push_back(field->handler_id);
        return;
    }
    const std::string value(event.text_view != nullptr
                                ? std::string_view(event.text_view, event.text_view_size)
                                : event.text_bytes());
    // `TextChangeEvent{value, target}`: `e.value` and `e.target.value` are the same string (the
    // spec's rule), so an inline `(e) => setName(e.target.value)` runs here as it does natively.
    eval::Record target;
    target.type = "TextTarget";
    target.fields.push_back({"value", eval::Value::of_string(value)});
    eval::Record record;
    record.type = "TextChangeEvent";
    record.fields.push_back({"value", eval::Value::of_string(value)});
    record.fields.push_back({"target", eval::Value::of_record(std::move(target))});

    eval::Scope* const frame_scope = session.frames_[field->frame].scope;
    eval::Scope with_params = *frame_scope;
    if (field->params != nullptr && !field->params->empty()) {
        with_params[field->params->front().name] = eval::Value::of_record(std::move(record));
    }
    if (!session.run_actions(field->frame, *field->actions, &with_params)) {
        session.unrun_.push_back(field->handler_id);
        return;
    }
    session.ran_.push_back(field->handler_id);
    for (auto& [name, cell] : *frame_scope) {
        const auto found = with_params.find(name);
        if (found != with_params.end()) {
            cell = std::move(found->second);
        }
    }
    // The next rebuild re-syncs a controlled field to the scope, so an edit the handler did not
    // store is taken back, as React does.
    session.dirty_ = true;
}

PreviewTextField PreviewSession::focused_text_field() const {
    PreviewTextField info;
    const ui::Node* const focused = router_.focus_manager().focused();
    if (focused == nullptr || focused->automation_id() == 0) {
        return info;
    }
    for (const auto& field : text_fields_) {
        if (field->automation_id != focused->automation_id()) {
            continue;
        }
        const ui::text::EditBuffer& buffer = field->editor.buffer();
        info.focused = true;
        info.composing = field->editor.composing();
        info.multiline = field->editor.multiline();
        info.caret = static_cast<std::uint32_t>(buffer.caret());
        info.selection_start = static_cast<std::uint32_t>(buffer.selection_start());
        info.selection_end = static_cast<std::uint32_t>(buffer.selection_end());
        info.box = focused->world_bounds();
        break;
    }
    return info;
}

std::string PreviewSession::focused_text_selection() const {
    const ui::Node* const focused = router_.focus_manager().focused();
    if (focused == nullptr) {
        return {};
    }
    for (const auto& field : text_fields_) {
        if (field->automation_id == focused->automation_id()) {
            return std::string(field->editor.buffer().selected_text());
        }
    }
    return {};
}

void PreviewSession::sync_text_fields() {
    for (const auto& field : text_fields_) {
        field->runtime.publish_decoration();
    }
}

} // namespace tessera::compiler::codegen::cli
