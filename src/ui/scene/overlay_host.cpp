// SPDX-License-Identifier: MIT

#include "tessera/ui/overlay/overlay.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace tessera::ui {

namespace {

// Re-entrant opens and closes (a mount callback opening a second overlay) are applied in further
// rounds; a callback that keeps doing it cannot hold the frame.
constexpr int kMaxFlushRounds = 4;

constexpr std::size_t index_of(OverlayLayer layer) noexcept {
    return static_cast<std::size_t>(layer);
}

// Explicit stacking per layer; the CSS z-index of arbitrary siblings never decides it. Inline style
// values are borrowed by the scene, so every value here is a literal.
constexpr std::array<const char*, kOverlayLayerCount> kLayerZText = {"1000", "2000", "3000",
                                                                     "4000"};
constexpr std::array<int, kOverlayLayerCount> kLayerZ = {1000, 2000, 3000, 4000};

const char* role_of(OverlayLayer layer) noexcept {
    switch (layer) {
    case OverlayLayer::Modal:
        return "dialog";
    case OverlayLayer::Tooltip:
        return "tooltip";
    case OverlayLayer::Toast:
        return "alert";
    case OverlayLayer::Overlay:
        break;
    }
    return "";
}

void fill_window(Node& node) {
    node.set_inline_style("position", "absolute");
    node.set_inline_style("left", "0");
    node.set_inline_style("top", "0");
    node.set_inline_style("right", "0");
    node.set_inline_style("bottom", "0");
}

} // namespace

struct OverlayHost::Entry {
    enum class State : std::uint8_t { Pending, Open };

    std::uint64_t sequence = 0;
    OverlaySpec spec;
    State state = State::Pending;
    bool closing = false;
    Node* wrapper = nullptr; // owned by the layer node while Open
    Node* content = nullptr; // owned by the wrapper
};

OverlayHost::OverlayHost(Node& root)
    : root_(root) {}

OverlayHost::~OverlayHost() {
    // The scene outlives the host and must not keep layer nodes whose entries are gone.
    for (Node*& layer : layers_) {
        if (layer != nullptr) {
            static_cast<void>(root_.remove_child(*layer));
            layer = nullptr;
        }
    }
}

OverlayHandle OverlayHost::open(OverlaySpec spec) {
    auto entry = std::make_unique<Entry>();
    entry->sequence = next_sequence_++;
    entry->spec = std::move(spec);
    const OverlayHandle handle{entry->sequence};
    entries_.push_back(std::move(entry));
    return handle;
}

OverlayHost::Entry* OverlayHost::find(OverlayHandle handle) const {
    if (!handle) {
        return nullptr;
    }
    for (const auto& entry : entries_) {
        if (entry->sequence == handle.sequence) {
            return entry.get();
        }
    }
    return nullptr;
}

bool OverlayHost::close(OverlayHandle handle) {
    Entry* entry = find(handle);
    if (entry == nullptr || entry->closing) {
        return false;
    }
    entry->closing = true;
    return true;
}

std::size_t OverlayHost::close_if(const std::function<bool(const OverlayInfo&)>& predicate) {
    std::size_t closed = 0;
    for (const auto& entry : entries_) {
        if (entry->closing) {
            continue;
        }
        const OverlayInfo info{OverlayHandle{entry->sequence}, entry->spec.id, entry->spec.layer,
                               entry->spec.priority};
        if (predicate(info)) {
            entry->closing = true;
            ++closed;
        }
    }
    return closed;
}

std::size_t OverlayHost::close_all() {
    return close_if([](const OverlayInfo&) { return true; });
}

bool OverlayHost::is_open(OverlayHandle handle) const {
    const Entry* entry = find(handle);
    return entry != nullptr && entry->state == Entry::State::Open && !entry->closing;
}

std::size_t OverlayHost::size() const {
    return static_cast<std::size_t>(
        std::ranges::count_if(entries_, [](const auto& entry) { return !entry->closing; }));
}

std::size_t OverlayHost::size(OverlayLayer layer) const {
    return static_cast<std::size_t>(std::ranges::count_if(entries_, [layer](const auto& entry) {
        return !entry->closing && entry->spec.layer == layer;
    }));
}

Node* OverlayHost::content(OverlayHandle handle) const {
    const Entry* entry = find(handle);
    return entry != nullptr && entry->state == Entry::State::Open ? entry->content : nullptr;
}

Node* OverlayHost::layer_node(OverlayLayer layer) const {
    return layers_[index_of(layer)];
}

OverlayHandle OverlayHost::top_modal() const {
    const Entry* top = nullptr;
    for (const auto& entry : entries_) {
        if (entry->spec.layer != OverlayLayer::Modal || entry->closing ||
            entry->state != Entry::State::Open) {
            continue;
        }
        if (top == nullptr || entry->spec.priority > top->spec.priority ||
            (entry->spec.priority == top->spec.priority && entry->sequence > top->sequence)) {
            top = entry.get();
        }
    }
    return top != nullptr ? OverlayHandle{top->sequence} : OverlayHandle{};
}

Node& OverlayHost::ensure_layer(OverlayLayer layer) {
    Node*& slot = layers_[index_of(layer)];
    if (slot != nullptr) {
        return *slot;
    }
    auto node = std::make_unique<Node>(next_node_id());
    node->set_class_name("tessera-overlay-layer");
    fill_window(*node);
    // The layer itself never takes a hit: an empty layer must not shadow the base content, and
    // each entry opts its own content back in.
    node->set_inline_style("pointer-events", "none");
    node->set_inline_style("z-index", kLayerZText[index_of(layer)]);
    node->set_z_index(kLayerZ[index_of(layer)]);
    slot = &root_.add_child(std::move(node));
    return *slot;
}

void OverlayHost::mount_entry(Entry& entry) {
    const OverlayLayer layer = entry.spec.layer;
    const bool modal = layer == OverlayLayer::Modal;

    auto wrapper = std::make_unique<Node>(next_node_id());
    wrapper->set_class_name("tessera-overlay-entry");
    fill_window(*wrapper);
    wrapper->set_inline_style("pointer-events", "none");
    if (modal) {
        // Dialogs sit centred until placement (slice 2) positions them.
        wrapper->set_inline_style("display", "flex");
        wrapper->set_inline_style("align-items", "center");
        wrapper->set_inline_style("justify-content", "center");
        // The scrim takes every hit the content does not, so the base and lower layers are inert
        // for the pointer while the dialog is up.
        auto scrim = std::make_unique<Node>(next_node_id());
        scrim->set_class_name("tessera-overlay-scrim");
        fill_window(*scrim);
        scrim->set_inline_style("pointer-events", "auto");
        scrim->set_inline_style("background-color", "rgba(0, 0, 0, 0.32)");
        wrapper->add_child(std::move(scrim));
    }

    auto content = std::make_unique<Node>(next_node_id());
    content->set_class_name("tessera-overlay-content");
    content->set_inline_style("pointer-events", "auto");
    if (!entry.spec.id.empty()) {
        content->set_element_id(entry.spec.id);
        content->set_aria("aria-label", entry.spec.id);
    }
    if (const std::string_view role = role_of(layer); !role.empty()) {
        content->set_role(role);
    }
    if (modal) {
        content->set_aria("aria-modal", "true");
        content->set_focus_trap(true);
    }
    Node* const content_ptr = content.get();
    Node* const wrapper_ptr = wrapper.get();
    wrapper->add_child(std::move(content));

    // Mounted before anything is attached: a throwing callback leaves the scene untouched.
    try {
        if (entry.spec.mount) {
            entry.spec.mount(*content_ptr);
        }
    } catch (const std::exception&) {
        ++failed_;
        entry.closing = true;
        return;
    } catch (...) {
        ++failed_;
        entry.closing = true;
        return;
    }

    Node& parent = ensure_layer(layer);
    std::size_t index = 0;
    for (const auto& other : entries_) {
        if (other.get() == &entry || other->state != Entry::State::Open || other->closing ||
            other->spec.layer != layer) {
            continue;
        }
        if (other->spec.priority < entry.spec.priority ||
            (other->spec.priority == entry.spec.priority && other->sequence < entry.sequence)) {
            ++index;
        }
    }
    parent.insert_child(index, std::move(wrapper));
    entry.wrapper = wrapper_ptr;
    entry.content = content_ptr;
    entry.state = Entry::State::Open;
}

void OverlayHost::unmount_entry(Entry& entry) {
    if (entry.wrapper != nullptr) {
        if (Node* parent = entry.wrapper->parent()) {
            static_cast<void>(parent->remove_child(*entry.wrapper));
        }
    }
    entry.wrapper = nullptr;
    entry.content = nullptr;
}

bool OverlayHost::flush() {
    bool changed = false;
    for (int round = 0; round < kMaxFlushRounds; ++round) {
        bool progress = false;
        // Closes first, so a close and a reopen of the same slot in one frame never overlap.
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            Entry& entry = *entries_[i];
            if (entry.closing && entry.state == Entry::State::Open) {
                unmount_entry(entry);
                entry.state = Entry::State::Pending;
                progress = true;
            }
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            Entry& entry = *entries_[i];
            if (!entry.closing && entry.state == Entry::State::Pending) {
                mount_entry(entry);
                progress = true;
            }
        }
        const auto dead = std::ranges::remove_if(entries_, [](const auto& entry) {
            return entry->closing && entry->wrapper == nullptr;
        });
        changed = changed || progress || dead.begin() != entries_.end();
        entries_.erase(dead.begin(), dead.end());
        if (!progress) {
            break;
        }
    }
    return changed;
}

} // namespace tessera::ui
