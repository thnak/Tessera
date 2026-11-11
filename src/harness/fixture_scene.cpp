// SPDX-License-Identifier: MIT

#include "tessera/automation/fixture_scene.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/event_payloads.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <string>
#include <utility>

namespace tessera::automation {
namespace {

using ui::Node;
using ui::PaintData;
using ui::PaintKind;

PaintData text_paint(std::string text) {
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::move(text);
    return data;
}

} // namespace

std::string_view FixtureScene::stylesheet() noexcept {
    return "View { display: flex; flex-direction: column; padding: 8px; }\n"
           ".card { width: 200px; height: 72px; background-color: #1e293b; }\n"
           "Text { font-size: 16px; height: 24px; color: #e2e8f0; }\n"
           "Button { width: 120px; height: 32px; background-color: #2563eb; }\n"
           "TextInput { width: 180px; height: 28px; background-color: #0f172a; }\n"
           ".list { width: 200px; height: 80px; background-color: #111827; }\n";
}

FixtureScene::FixtureScene(Node& root)
    : root_(root) {
    auto view_ptr = std::make_unique<Node>(2);
    Node& view = root_.add_child(std::move(view_ptr));
    view.set_element_kind("View");
    view.set_class_name("card");
    view.set_automation_id(kFixtureViewId);

    auto label_ptr = std::make_unique<Node>(3);
    Node& label = view.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    label.set_automation_id(kFixtureCounterId);
    label.set_paint_data(text_paint("Count: 0"));
    label_ = &label;

    auto button_ptr = std::make_unique<Node>(4);
    Node& button = view.add_child(std::move(button_ptr));
    button.set_element_kind("Button");
    button.set_automation_id(kFixtureButtonId);
    button.set_focusable(true);
    button.bind_event(ui::EventType::Click, &FixtureScene::on_click, this);
    button_ = &button;

    auto field_ptr = std::make_unique<Node>(5);
    Node& field = root_.add_child(std::move(field_ptr));
    field.set_element_kind("TextInput");
    field.set_automation_id(kFixtureFieldId);
    field.set_focusable(true);
    field.set_paint_data(text_paint(""));
    field.bind_event(ui::EventType::EditText, &FixtureScene::on_edit_text, this);
    field_ = &field;

    auto list_ptr = std::make_unique<Node>(6);
    Node& list = root_.add_child(std::move(list_ptr));
    list.set_element_kind("View");
    list.set_class_name("list");
    list.set_automation_id(kFixtureListId);
    list.set_scrollable_y(true);
    list.set_content_size(200.0f, 600.0f);

    auto item_ptr = std::make_unique<Node>(7);
    Node& item = list.add_child(std::move(item_ptr));
    item.set_element_kind("Text");
    item.set_automation_id(kFixtureListItemId);
    item.set_paint_data(text_paint("Item"));
}

FixtureScene::~FixtureScene() {
    // The bindings hold `this` as their context; drop them so a surviving scene cannot call into a
    // destroyed fixture. The nodes must still be alive (the root outlives the fixture).
    if (button_ != nullptr) {
        button_->unbind_event(ui::EventType::Click);
    }
    if (field_ != nullptr) {
        field_->unbind_event(ui::EventType::EditText);
    }
}

void FixtureScene::on_click(void* context, const ui::Event& event) {
    if (context == nullptr || event.type != ui::EventType::Click) {
        return;
    }
    auto* self = static_cast<FixtureScene*>(context);
    ++self->count_;
    self->refresh_label();
}

void FixtureScene::on_edit_text(void* context, const ui::Event& event) {
    if (context == nullptr || event.type != ui::EventType::EditText) {
        return;
    }
    auto* self = static_cast<FixtureScene*>(context);
    self->typed_ = std::string(ui::event_text(event));
    if (self->field_ != nullptr) {
        self->field_->set_paint_data(text_paint(self->typed_));
    }
}

void FixtureScene::refresh_label() {
    if (label_ == nullptr) {
        return;
    }
    label_->set_paint_data(text_paint("Count: " + std::to_string(count_)));
}

} // namespace tessera::automation
