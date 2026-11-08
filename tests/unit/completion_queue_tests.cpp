// SPDX-License-Identifier: MIT

#include "tessera/runtime/async.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CompletionPriority;
using tessera::runtime::CompletionQueue;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "completion_queue: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    CompletionQueue queue(8);
    std::vector<int> order;
    if (!expect(queue.post(CompletionPriority::Background, [&] { order.push_back(4); }),
                "post background") ||
        !expect(queue.post(CompletionPriority::LayoutPaint, [&] { order.push_back(3); }),
                "post layout") ||
        !expect(queue.post(CompletionPriority::Visible, [&] { order.push_back(1); }),
                "post visible one") ||
        !expect(queue.post(CompletionPriority::Visible, [&] { order.push_back(2); }),
                "post visible two") ||
        !expect(queue.pending(CompletionPriority::Visible) == 2, "visible pending") ||
        !expect(queue.size() == 4, "total pending") ||
        !expect(queue.run(1) == 1 && order == std::vector<int>{1}, "visible first") ||
        !expect(queue.run(1) == 1 && order == std::vector<int>{1, 2}, "visible fifo") ||
        !expect(queue.run(1) == 1 && order == std::vector<int>{1, 2, 3}, "layout next") ||
        !expect(queue.run(1) == 1 && order == std::vector<int>{1, 2, 3, 4}, "background last") ||
        !expect(queue.empty(), "queue empty")) {
        return 1;
    }

    CompletionQueue nested(8);
    std::vector<int> nested_order;
    nested.post(CompletionPriority::Background, [&] { nested_order.push_back(3); });
    nested.post(CompletionPriority::Visible, [&] {
        nested_order.push_back(1);
        nested.post(CompletionPriority::LayoutPaint, [&] { nested_order.push_back(2); });
    });
    if (!expect(nested.run(3) == 3, "nested run count") ||
        !expect(nested_order == std::vector<int>{1, 2, 3}, "newly posted priority wins")) {
        return 2;
    }

    CancellationSource source;
    const auto token = source.token();
    CompletionQueue cancelled(8);
    if (!expect(cancelled.post(CompletionPriority::Visible, [] {}, token), "post before cancel")) {
        return 3;
    }
    source.cancel();
    if (!expect(cancelled.run(1) == 0, "cancelled not run") ||
        !expect(cancelled.stats().cancelled == 1, "cancelled stat") ||
        !expect(cancelled.empty(), "cancelled removed") ||
        !expect(!cancelled.post(
                    CompletionPriority::Visible, [] {}, token),
                "cancel rejects post") ||
        !expect(cancelled.stats().cancelled == 2, "post cancellation stat")) {
        return 4;
    }

    CompletionQueue bounded(2);
    if (!expect(bounded.post(CompletionPriority::Visible, [] {}), "bounded first") ||
        !expect(bounded.post(CompletionPriority::Background, [] {}), "bounded second") ||
        !expect(!bounded.post(CompletionPriority::LayoutPaint, [] {}), "bounded overflow") ||
        !expect(bounded.stats().dropped == 1, "overflow stat") ||
        !expect(bounded.stats().pending_visible == 1, "pending visible") ||
        !expect(bounded.stats().pending_background == 1, "pending background") ||
        !expect(bounded.clear() == 2, "clear count") ||
        !expect(bounded.stats().discarded == 2, "discard stat") ||
        !expect(bounded.empty(), "clear empty") ||
        !expect(bounded.stats().pending == 0, "pending clear")) {
        return 5;
    }

    CompletionQueue rejected(2);
    if (!expect(!rejected.post(CompletionPriority::Visible, {}), "null task rejected") ||
        !expect(rejected.stats().dropped == 1, "null task dropped") ||
        !expect(rejected.run(0) == 0, "zero budget")) {
        return 6;
    }

    bool threw = false;
    try {
        CompletionQueue invalid(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero capacity rejected")) {
        return 7;
    }

    std::cout << "completion_queue: ok\n";
    return 0;
}
