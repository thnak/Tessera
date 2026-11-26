// SPDX-License-Identifier: MIT
//
// Issue #197 (`net` module) entry point. One executable per area, matching the repository's
// unit-test convention; the groups live in the sibling TUs:
//
//   * appfw_net_state_tests.cpp    - the per-request state machine, and a 4xx/5xx being `Done`
//   * appfw_net_error_tests.cpp    - timeout, cancellation, offline, transport failure, the caps
//   * appfw_net_redirect_tests.cpp - the section 3 item 2 rules, headers, cleartext, redaction
//   * appfw_net_thread_tests.cpp   - "never blocks the UI thread", the deadline as a timer
//   * appfw_net_request_tests.cpp  - the interface table's defaults and the `send` coroutine
//   * appfw_net_http_service_tests.cpp - `HttpService` through the registry and a wrapper (ADR-075)
//
// Every group is deterministic: `ManualClock`, `ManualIoExecutor`, `FakeNet`, a hand-fired
// `TimerQueue` and a hand-drained `CompletionQueue`. No test in the suite opens a socket, reads a
// wall clock, sleeps, starts a thread or touches the file system.

#include "appfw_net_test_support.hpp"

#include <iostream>

namespace net_test {

int run_state_tests();
int run_error_tests();
int run_redirect_tests();
int run_thread_tests();
int run_request_tests();
int run_http_service_tests();

} // namespace net_test

int main() {
    const int state = net_test::run_state_tests();
    const int errors = net_test::run_error_tests();
    const int redirects = net_test::run_redirect_tests();
    const int threads = net_test::run_thread_tests();
    const int requests = net_test::run_request_tests();
    const int service = net_test::run_http_service_tests();
    (void)service;
    (void)state;
    (void)errors;
    (void)redirects;
    (void)threads;
    (void)requests;

    if (net_test::g_failures != 0) {
        std::cerr << "appfw_net: " << net_test::g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_net: ok\n";
    return 0;
}
