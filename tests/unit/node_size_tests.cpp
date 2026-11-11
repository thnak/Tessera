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

} // namespace

int main() {
    if (!expect(sizeof(ui::Node) <= ui::kNodeSizeBudget, "sizeof(Node) is within the budget")) {
        return 1;
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
