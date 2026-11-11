// SPDX-License-Identifier: MIT
//
// LayoutObserverSet (docs/design/layout-observation.md sections 5.2, 5.6, 5.7, 8): initial
// delivery, one delivery per collect carrying the latest size, change detection on the border and
// content box, position-only moves, a node with no layout box, detach and attach during delivery,
// attach-order determinism, the depth floor, RAII lifetime, and zero allocation after reserve.

#include "tessera/ui/layout/layout_observer.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

std::size_t g_allocations = 0;

} // namespace

void* operator new(std::size_t size) {
    ++g_allocations;
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

using tessera::ui::Node;
using tessera::ui::layout::kNoDepth;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutInsets;
using tessera::ui::layout::LayoutObservation;
using tessera::ui::layout::LayoutObserverSet;
using tessera::ui::layout::LayoutReadModel;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::SizeChange;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_observer: " << message << "\n";
    }
    return condition;
}

LayoutBox box(std::uint64_t id, float x, float y, float width, float height,
              LayoutInsets insets = {}) {
    return LayoutBox{id, {x, y, width, height}, insets};
}

struct Log {
    std::vector<std::string> entries;
    std::vector<SizeChange> sizes;
};

struct Probe {
    Probe(Log* sink, std::string label)
        : log(sink),
          name(std::move(label)) {}

    Log* log = nullptr;
    std::string name;
    int deliveries = 0;
    SizeChange last;
    LayoutObserverSet* set = nullptr;
    LayoutObservation* detach_target = nullptr;
    LayoutObservation* attach_target = nullptr;
};

void deliver_cb(void* context, const SizeChange& size) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->deliveries;
    probe->last = size;
    if (probe->log !=
        nullptr) { // the allocation test passes no log: std::string copies allocate in debug STLs
        probe->log->entries.push_back(probe->name);
        probe->log->sizes.push_back(size);
    }
    if (probe->detach_target != nullptr) {
        probe->set->detach(*probe->detach_target);
        probe->detach_target = nullptr;
    }
    if (probe->attach_target != nullptr) {
        probe->set->attach(*probe->attach_target);
        probe->attach_target = nullptr;
    }
}

// root(1) -> a(2) -> b(3)
struct Scene {
    Node root{1};
    Node* a = nullptr;
    Node* b = nullptr;
    Scene() {
        a = &root.add_child(std::make_unique<Node>(2));
        b = &a->add_child(std::make_unique<Node>(3));
    }
};

bool initial_and_change_detection() {
    Scene scene;
    Log log;
    Probe probe{&log, "a"};
    LayoutObserverSet set;
    LayoutObservation observation(*scene.a, &deliver_cb, &probe);
    set.attach(observation);
    LayoutReadModel model;

    if (!expect(set.collect(model) == 0, "nothing is due before the first layout is published")) {
        return false;
    }
    // First layout: a 0x0 box still delivers (unlike browsers).
    LayoutResult layout{{box(1, 0, 0, 100, 100), box(2, 0, 0, 0, 0)}};
    model.publish(layout, 1.0f);
    if (!expect(set.collect(model) == 1, "the first layout makes the observation due")) {
        return false;
    }
    set.deliver();
    if (!expect(probe.deliveries == 1 && probe.last == SizeChange{}, "initial 0x0 is delivered")) {
        return false;
    }
    model.publish(layout, 1.0f);
    if (!expect(set.collect(model) == 0, "an unchanged size delivers nothing")) {
        return false;
    }

    // Several layouts between collects coalesce into one delivery carrying the latest size.
    model.publish(LayoutResult{{box(2, 0, 0, 10, 10)}}, 1.0f);
    model.publish(LayoutResult{{box(2, 0, 0, 20, 30)}}, 1.0f);
    model.publish(LayoutResult{{box(2, 0, 0, 40, 50)}}, 1.0f);
    set.collect(model);
    set.deliver();
    if (!expect(probe.deliveries == 2, "three layouts before a collect deliver once") ||
        !expect(probe.last.width == 40.0f && probe.last.height == 50.0f,
                "the delivery carries the latest size")) {
        return false;
    }

    // A size that returns to the last delivered value is not delivered.
    model.publish(LayoutResult{{box(2, 0, 0, 7, 7)}}, 1.0f);
    model.publish(LayoutResult{{box(2, 0, 0, 40, 50)}}, 1.0f);
    if (!expect(set.collect(model) == 0, "a size back at its delivered value is not delivered")) {
        return false;
    }

    // Position-only moves do not deliver.
    model.publish(LayoutResult{{box(2, 300, 200, 40, 50)}}, 1.0f);
    if (!expect(set.collect(model) == 0, "a position-only move does not deliver")) {
        return false;
    }

    // The content box alone changing (a padding change at the same border box) delivers.
    model.publish(LayoutResult{{box(2, 0, 0, 40, 50, LayoutInsets{4, 4, 4, 4})}}, 1.0f);
    if (!expect(set.collect(model) == 1, "a content-box change delivers")) {
        return false;
    }
    set.deliver();
    return expect(probe.last.content_width == 32.0f && probe.last.content_height == 42.0f &&
                      probe.last.width == 40.0f,
                  "the payload carries both boxes");
}

bool no_layout_box() {
    Scene scene;
    Log log;
    Probe probe{&log, "b"};
    LayoutObserverSet set;
    LayoutObservation observation(*scene.b, &deliver_cb, &probe);
    set.attach(observation);
    LayoutReadModel model;

    model.publish(LayoutResult{{box(3, 0, 0, 10, 10)}}, 1.0f);
    set.collect(model);
    set.deliver();
    // display: none -> the node leaves the result: one 0x0 delivery, then nothing.
    model.publish(LayoutResult{{box(1, 0, 0, 5, 5)}}, 1.0f);
    if (!expect(set.collect(model) == 1, "a node that lost its box delivers once")) {
        return false;
    }
    set.deliver();
    if (!expect(probe.last == SizeChange{}, "the lost box reads as 0x0")) {
        return false;
    }
    model.publish(LayoutResult{{box(1, 0, 0, 6, 6)}}, 1.0f);
    if (!expect(set.collect(model) == 0, "still no box: no further delivery")) {
        return false;
    }
    model.publish(LayoutResult{{box(3, 0, 0, 10, 10)}}, 1.0f);
    return expect(set.collect(model) == 1, "the box returning delivers again");
}

bool order_and_lifetime_during_delivery() {
    Scene scene;
    Log log;
    Probe pa{&log, "a"};
    Probe pb{&log, "b"};
    Probe pr{&log, "root"};
    LayoutObserverSet set;
    // Attach order is root, a, b regardless of construction order.
    LayoutObservation obs_b(*scene.b, &deliver_cb, &pb);
    LayoutObservation obs_root(scene.root, &deliver_cb, &pr);
    LayoutObservation obs_a(*scene.a, &deliver_cb, &pa);
    set.attach(obs_root);
    set.attach(obs_a);
    set.attach(obs_b);
    LayoutReadModel model;
    model.publish(LayoutResult{{box(1, 0, 0, 9, 9), box(2, 0, 0, 5, 5), box(3, 0, 0, 2, 2)}}, 1.0f);
    set.collect(model);
    set.deliver();
    if (!expect(log.entries.size() == 3 && log.entries[0] == "root" && log.entries[1] == "a" &&
                    log.entries[2] == "b",
                "deliveries follow attach order")) {
        return false;
    }

    // Detach a due observation from an earlier callback: it must not be called.
    model.publish(LayoutResult{{box(1, 0, 0, 19, 19), box(2, 0, 0, 15, 15), box(3, 0, 0, 12, 12)}},
                  1.0f);
    log.entries.clear();
    pr.set = &set;
    pr.detach_target = &obs_b;
    set.collect(model);
    set.deliver();
    if (!expect(log.entries.size() == 2 && log.entries[0] == "root" && log.entries[1] == "a",
                "an observation detached by a callback is not delivered") ||
        !expect(!obs_b.attached() && set.size() == 2, "it is detached and compacted")) {
        return false;
    }

    // Attach from a callback: first collected by the next collect, not this delivery.
    pa.set = &set;
    pa.attach_target = &obs_b;
    model.publish(LayoutResult{{box(1, 0, 0, 29, 29), box(2, 0, 0, 25, 25), box(3, 0, 0, 22, 22)}},
                  1.0f);
    log.entries.clear();
    set.collect(model);
    set.deliver();
    if (!expect(log.entries.size() == 2, "an observation attached by a callback waits a round") ||
        !expect(obs_b.attached(), "it is attached")) {
        return false;
    }
    log.entries.clear();
    set.collect(model);
    set.deliver();
    return expect(log.entries.size() == 1 && log.entries[0] == "b",
                  "the next collect delivers the new observation its initial size");
}

bool depth_floor() {
    Scene scene;
    Log log;
    Probe pr{&log, "root"};
    Probe pa{&log, "a"};
    Probe pb{&log, "b"};
    LayoutObserverSet set;
    LayoutObservation obs_root(scene.root, &deliver_cb, &pr);
    LayoutObservation obs_a(*scene.a, &deliver_cb, &pa);
    LayoutObservation obs_b(*scene.b, &deliver_cb, &pb);
    set.attach(obs_root);
    set.attach(obs_a);
    set.attach(obs_b);
    LayoutReadModel model;
    model.publish(LayoutResult{{box(1, 0, 0, 9, 9), box(2, 0, 0, 5, 5), box(3, 0, 0, 2, 2)}}, 1.0f);

    if (!expect(set.collect(model, 2) == 1 && set.deferred() == 2,
                "a floor of 2 selects only the deepest and defers the others")) {
        return false;
    }
    const auto first = set.deliver();
    if (!expect(first.delivered == 1 && first.shallowest_depth == 2,
                "the delivery reports the shallowest depth it served")) {
        return false;
    }
    // Deferred observations stay due: they are collected once the floor drops.
    if (!expect(set.collect(model, 0) == 2 && set.deferred() == 0, "deferred ones stay due")) {
        return false;
    }
    const auto second = set.deliver();
    return expect(second.delivered == 2 && second.shallowest_depth == 0,
                  "and are then delivered") &&
           expect(set.collect(model) == 0, "nothing is left due");
}

bool raii_and_moves() {
    Scene scene;
    Log log;
    Probe probe{&log, "a"};
    LayoutReadModel model;
    model.publish(LayoutResult{{box(2, 0, 0, 5, 5)}}, 1.0f);
    {
        LayoutObserverSet set;
        {
            LayoutObservation temporary(*scene.a, &deliver_cb, &probe);
            set.attach(temporary);
            if (!expect(set.size() == 1, "attached")) {
                return false;
            }
        }
        if (!expect(set.size() == 0, "the destructor detaches")) {
            return false;
        }

        LayoutObservation first(*scene.a, &deliver_cb, &probe);
        set.attach(first);
        LayoutObservation moved(std::move(first));
        if (!expect(!first.attached() && moved.attached() && set.size() == 1,
                    "a move transfers the registration")) {
            return false;
        }
        set.collect(model);
        set.deliver();
        if (!expect(probe.deliveries == 1, "the moved observation is the one delivered")) {
            return false;
        }
        LayoutObservation assigned(*scene.b, &deliver_cb, &probe);
        assigned = std::move(moved);
        if (!expect(assigned.attached() && set.size() == 1, "move assignment keeps one entry")) {
            return false;
        }
        // The set dies first: the observation must end up detached, not dangling.
        LayoutObserverSet* leaked = new LayoutObserverSet();
        leaked->attach(assigned);
        delete leaked;
        if (!expect(!assigned.attached(), "destroying the set detaches its observations")) {
            return false;
        }
    }
    return true;
}

bool no_work_when_empty_and_no_allocation() {
    Scene scene;
    Probe pa{nullptr, "a"};
    Probe pb{nullptr, "b"};
    LayoutObserverSet set;
    LayoutReadModel model;
    model.publish(LayoutResult{{box(2, 0, 0, 5, 5), box(3, 0, 0, 3, 3)}}, 1.0f);

    std::size_t before = g_allocations;
    if (!expect(set.collect(model) == 0 && set.deliver().delivered == 0,
                "an empty set is a no-op") ||
        !expect(g_allocations == before, "an empty set does not allocate")) {
        return false;
    }

    LayoutObservation oa(*scene.a, &deliver_cb, &pa);
    LayoutObservation ob(*scene.b, &deliver_cb, &pb);
    set.reserve(8);
    set.attach(oa);
    set.attach(ob);
    // Build the next layout first: constructing a LayoutResult allocates and is not under test.
    const LayoutResult next{{box(2, 0, 0, 6, 6), box(3, 0, 0, 4, 4)}};
    before = g_allocations;
    set.collect(model);
    set.deliver();
    model.publish(next, 1.0f);
    set.collect(model);
    set.deliver();
    return expect(g_allocations == before, "collect and deliver allocate nothing after reserve") &&
           expect(pa.deliveries == 2 && pb.deliveries == 2, "and both observations were delivered");
}

} // namespace

int main() {
    if (!initial_and_change_detection()) {
        return 1;
    }
    if (!no_layout_box()) {
        return 2;
    }
    if (!order_and_lifetime_during_delivery()) {
        return 3;
    }
    if (!depth_floor()) {
        return 4;
    }
    if (!raii_and_moves()) {
        return 5;
    }
    if (!no_work_when_empty_and_no_allocation()) {
        return 6;
    }
    (void)kNoDepth;
    std::cout << "layout_observer: ok\n";
    return 0;
}
