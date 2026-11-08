// SPDX-License-Identifier: MIT

#include "tessera/runtime/async.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CancellationToken;
using tessera::runtime::ManualExecutor;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "async: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    const CancellationToken empty;
    if (!expect(!empty.cancelled(), "default token not cancelled")) {
        return 1;
    }

    CancellationSource source;
    const CancellationToken token = source.token();
    const CancellationToken copy = token;
    if (!expect(!token.cancelled(), "token initially live") ||
        !expect(!source.cancelled(), "source initially live")) {
        return 2;
    }
    source.cancel();
    if (!expect(token.cancelled(), "token cancelled") ||
        !expect(copy.cancelled(), "copied token cancelled") ||
        !expect(source.cancelled(), "source cancelled")) {
        return 3;
    }
    source.cancel();
    if (!expect(token.cancelled(), "cancel idempotent")) {
        return 4;
    }

    ManualExecutor executor(4);
    std::vector<int> order;
    if (!expect(executor.post([&] { order.push_back(1); }), "post one") ||
        !expect(executor.post([&] { order.push_back(2); }), "post two") ||
        !expect(executor.size() == 2, "queue size") ||
        !expect(executor.run_one(), "run one true") ||
        !expect(order.size() == 1 && order[0] == 1, "fifo first") ||
        !expect(executor.run(10) == 1, "run remaining") ||
        !expect(order.size() == 2 && order[1] == 2, "fifo second") ||
        !expect(executor.empty(), "queue drained") ||
        !expect(executor.stats().posted == 2 && executor.stats().executed == 2, "executor stats")) {
        return 5;
    }

    ManualExecutor nested(4);
    std::vector<int> nested_order;
    nested.post([&] {
        nested_order.push_back(1);
        nested.post([&] { nested_order.push_back(3); });
    });
    nested.post([&] { nested_order.push_back(2); });
    if (!expect(nested.run(3) == 3, "nested run count") ||
        !expect(nested_order == std::vector<int>{1, 2, 3}, "nested fifo")) {
        return 6;
    }

    ManualExecutor bounded(2);
    if (!expect(bounded.post([] {}), "bounded first") ||
        !expect(bounded.post([] {}), "bounded second") ||
        !expect(!bounded.post([] {}), "bounded overflow") ||
        !expect(bounded.stats().dropped == 1, "overflow stat") ||
        !expect(bounded.stats().pending == 2, "pending stat") ||
        !expect(bounded.clear() == 2, "clear count") ||
        !expect(bounded.stats().discarded == 2, "discard stat") ||
        !expect(bounded.empty(), "clear empty")) {
        return 7;
    }

    CancellationSource cancelled_after;
    const CancellationToken late = cancelled_after.token();
    ManualExecutor cancellation(4);
    if (!expect(cancellation.post([] {}, late), "post before cancel")) {
        return 8;
    }
    cancelled_after.cancel();
    if (!expect(!cancellation.run_one(), "cancelled task skipped") ||
        !expect(cancellation.stats().executed == 0, "cancelled not executed") ||
        !expect(cancellation.stats().cancelled == 1, "cancelled stat")) {
        return 9;
    }

    CancellationSource cancelled_before;
    cancelled_before.cancel();
    if (!expect(!cancellation.post([] {}, cancelled_before.token()), "post cancel rejected") ||
        !expect(cancellation.stats().cancelled == 2, "post cancel stat") ||
        !expect(!cancellation.post({}), "null task rejected") ||
        !expect(cancellation.stats().dropped == 1, "null task dropped")) {
        return 10;
    }

    ManualExecutor budgeted(2);
    int runs = 0;
    budgeted.post([&] { ++runs; });
    if (!expect(budgeted.run(0) == 0, "zero budget") ||
        !expect(runs == 0, "zero budget executes nothing") ||
        !expect(budgeted.run(1) == 1 && runs == 1, "budget executes one")) {
        return 11;
    }

    bool threw = false;
    try {
        ManualExecutor invalid(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero capacity rejected")) {
        return 12;
    }

    std::cout << "async: ok\n";
    return 0;
}
