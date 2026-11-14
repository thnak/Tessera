// SPDX-License-Identifier: MIT
//
// Per-node memory budget (issue #13): sizeof(Node) stays within kNodeSizeBudget, a plain node
// allocates nothing beyond itself, and each rarely used payload (aria, inline style, event
// bindings, paint data) allocates only when first used. Counting global operator new keeps the
// report deterministic: no RSS, no wall clock.

#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>

namespace {

std::size_t g_allocations = 0;
std::size_t g_bytes = 0;

} // namespace

void* operator new(std::size_t size) {
    ++g_allocations;
    g_bytes += size;
    if (void* pointer = std::malloc(size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {

namespace ui = tessera::ui;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "node_size: " << message << "\n";
    }
    return condition;
}

struct Cost {
    std::size_t allocations = 0;
    std::size_t bytes = 0;
};

template<class Fn> Cost measure(Fn&& fn) {
    const std::size_t allocations = g_allocations;
    const std::size_t bytes = g_bytes;
    fn();
    return {g_allocations - allocations, g_bytes - bytes};
}

void handler(void*, const ui::Event&) {}

// The ten booleans share one flags word (issue #285): each must read back what was written and
// leave its neighbours alone, from the defaults and from the all-set state.
bool flags_are_independent() {
    struct Bit {
        const char* name;
        void (*set)(ui::Node&, bool);
        bool (*get)(const ui::Node&);
        bool initial;
    };
    const Bit bits[] = {
        {"visible", [](ui::Node& n, bool on) { n.set_visible(on); },
         [](const ui::Node& n) { return n.visible(); }, true},
        {"focusable", [](ui::Node& n, bool on) { n.set_focusable(on); },
         [](const ui::Node& n) { return n.focusable(); }, false},
        {"auto_focus", [](ui::Node& n, bool on) { n.set_auto_focus(on); },
         [](const ui::Node& n) { return n.auto_focus(); }, false},
        {"focus_trap", [](ui::Node& n, bool on) { n.set_focus_trap(on); },
         [](const ui::Node& n) { return n.focus_trap(); }, false},
        {"typeahead", [](ui::Node& n, bool on) { n.set_typeahead(on); },
         [](const ui::Node& n) { return n.typeahead(); }, false},
        {"glass_subtree", [](ui::Node& n, bool on) { n.set_glass_subtree(on); },
         [](const ui::Node& n) { return n.glass_subtree(); }, false},
        {"item_key",
         [](ui::Node& n, bool on) {
             if (on) {
                 n.set_item_key(7);
             } else {
                 n.clear_item_key();
             }
         },
         [](const ui::Node& n) { return n.item_key().has_value(); }, false},
        {"scroll_animation",
         [](ui::Node& n, bool on) {
             if (on) {
                 n.set_scroll_animation_target(1.0f, 2.0f);
             } else {
                 n.clear_scroll_animation();
             }
         },
         [](const ui::Node& n) { return n.scroll_animation_active(); }, false},
        {"pointer_events", [](ui::Node& n, bool on) { n.set_pointer_events(on); },
         [](const ui::Node& n) { return n.pointer_events(); }, true},
        {"style_hidden", [](ui::Node& n, bool on) { n.set_style_hidden(on); },
         [](const ui::Node& n) { return n.style_hidden(); }, false},
    };
    constexpr std::size_t kCount = sizeof(bits) / sizeof(bits[0]);

    ui::Node node;
    for (const Bit& bit : bits) {
        if (!expect(bit.get(node) == bit.initial, bit.name)) {
            return false;
        }
    }
    if (!expect(node.rendered(), "a new node is rendered")) {
        return false;
    }
    for (std::size_t i = 0; i < kCount; ++i) {
        bits[i].set(node, !bits[i].initial);
        for (std::size_t j = 0; j < kCount; ++j) {
            const bool want = j == i ? !bits[j].initial : bits[j].initial;
            if (!expect(bits[j].get(node) == want, bits[j].name)) {
                return false;
            }
        }
        bits[i].set(node, bits[i].initial);
    }
    for (const Bit& bit : bits) {
        bit.set(node, !bit.initial);
    }
    for (const Bit& bit : bits) {
        if (!expect(bit.get(node) == !bit.initial, bit.name)) {
            return false;
        }
    }
    return expect(!node.rendered(), "visible, but style-hidden, is not rendered");
}

} // namespace

int main() {
    if (!expect(sizeof(ui::Node) <= ui::kNodeSizeBudget, "sizeof(Node) is within the budget")) {
        return 1;
    }
    if (!flags_are_independent()) {
        return 5;
    }

    std::unique_ptr<ui::Node> plain;
    const Cost plain_cost = measure([&] { plain = std::make_unique<ui::Node>(1); });
    // The debug MSVC STL adds a container proxy allocation per vector; allow that slack.
    if (!expect(plain_cost.allocations <= 2 && plain_cost.bytes <= sizeof(ui::Node) + 64,
                "a plain node allocates itself and nothing else")) {
        return 2;
    }

    const Cost aria = measure([&] { plain->set_aria("aria-label", "x"); });
    const Cost aria_again = measure([&] { plain->set_aria("aria-hidden", "true"); });
    const Cost style = measure([&] { plain->set_inline_style("width", "10px"); });
    const Cost binding = measure([&] { plain->bind_event(ui::EventType::PointerUp, &handler); });
    const Cost paint = measure([&] {
        ui::PaintData data;
        data.text = "hello";
        plain->set_paint_data(data);
    });
    const Cost paint_again = measure([&] {
        ui::PaintData data;
        data.text = "hello";
        data.max_lines = 2;
        plain->set_paint_data(data);
    });
    if (!expect(aria.allocations == 1, "the first aria pair allocates its table once") ||
        !expect(aria_again.allocations == 0, "further aria pairs allocate nothing") ||
        !expect(style.allocations == 1, "the first inline style allocates its table once") ||
        !expect(binding.allocations == 1, "the first event binding allocates its table once") ||
        !expect(paint.allocations >= 1, "paint data allocates on first use") ||
        !expect(paint_again.allocations <= paint.allocations,
                "replacing paint data does not reallocate the slot")) {
        return 3;
    }

    plain->clear_aria();
    plain->clear_inline_style();
    plain->clear_event_bindings();
    const Cost reuse = measure([&] {
        plain->set_aria("aria-label", "y");
        plain->set_inline_style("width", "20px");
        plain->bind_event(ui::EventType::PointerDown, &handler);
    });
    if (!expect(reuse.allocations == 0,
                "clearing keeps the tables, so toggling never reallocates")) {
        return 4;
    }

    std::cout << "node_size: sizeof(Node)=" << sizeof(ui::Node) << " budget=" << ui::kNodeSizeBudget
              << " plain=" << plain_cost.bytes << "B aria_table=" << aria.bytes
              << "B inline_style_table=" << style.bytes << "B event_table=" << binding.bytes
              << "B paint_slot=" << paint.bytes << "B\n";
    std::cout << "node_size: ok\n";
    return 0;
}
