// SPDX-License-Identifier: MIT

#include "tessera/automation/manifest_scene.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/event_payloads.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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

// The context is the field node itself, so the binding needs no owner object and cannot dangle.
void on_edit_text(void* context, const ui::Event& event) {
    if (context == nullptr || event.type != ui::EventType::EditText) {
        return;
    }
    static_cast<Node*>(context)->set_paint_data(text_paint(std::string(ui::event_text(event))));
}

std::string parent_path(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

void configure(Node& node, const std::string& kind) {
    node.set_element_kind(kind);
    if (kind == "Button") {
        node.set_focusable(true);
    } else if (kind == "TextInput" || kind == "TextArea") {
        node.set_focusable(true);
        node.set_paint_data(text_paint(""));
        node.bind_event(ui::EventType::EditText, &on_edit_text, &node);
    } else if (kind == "Text") {
        node.set_paint_data(text_paint(""));
    } else if (kind == "Scroll" || kind == "VirtualList" || kind == "VirtualGrid") {
        node.set_scrollable_y(true);
        node.set_content_size(200.0f, 600.0f);
    }
}

} // namespace

std::string_view ManifestScene::stylesheet() noexcept {
    return "View { display: flex; flex-direction: column; padding: 8px; }\n"
           "Text { font-size: 16px; height: 24px; color: #e2e8f0; }\n"
           "Button { width: 120px; height: 32px; background-color: #2563eb; }\n"
           "TextInput { width: 180px; height: 28px; background-color: #0f172a; }\n"
           "TextArea { width: 180px; height: 64px; background-color: #0f172a; }\n"
           "Image { width: 64px; height: 64px; }\n"
           "Spacer { height: 8px; }\n"
           "Scroll { width: 200px; height: 80px; background-color: #111827; }\n"
           "VirtualList { width: 200px; height: 80px; background-color: #111827; }\n"
           "VirtualGrid { width: 200px; height: 80px; background-color: #111827; }\n";
}

NodeAnnotations annotations_from_manifest(const AppManifest& manifest) {
    NodeAnnotations annotations;
    annotations.reserve(manifest.nodes.size());
    for (const ManifestNode& entry : manifest.nodes) {
        annotations.emplace(entry.ui_id, NodeAnnotation{entry.path, entry.component_id});
    }
    return annotations;
}

ManifestScene::ManifestScene(Node& root, const AppManifest& manifest) {
    // Manifest order is document order, so a parent precedes its children; a node whose parent
    // path is absent (a component root) attaches to the scene root.
    std::unordered_map<std::string, Node*> by_path;
    by_path.reserve(manifest.nodes.size());
    std::uint64_t node_id = 2;
    for (const ManifestNode& entry : manifest.nodes) {
        const auto parent = by_path.find(parent_path(entry.path));
        Node& host = parent != by_path.end() ? *parent->second : root;
        Node& node = host.add_child(std::make_unique<Node>(node_id++));
        node.set_automation_id(entry.ui_id);
        configure(node, entry.kind);
        if (!entry.name.empty()) {
            (void)node.set_aria("aria-label", entry.name);
        }
        by_path.emplace(entry.path, &node);
    }
}

} // namespace tessera::automation
