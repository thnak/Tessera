// SPDX-License-Identifier: MIT
//
// `RouterService`: the service interface the `react-router-dom` shim lowers to
// (docs/specs/router-shim-v1.md, ADR-058 section 5 step 3, issue #549).
//
// A ported `const navigate = useNavigate(); navigate("/orders/42");` becomes
// `useService<RouterService>()` plus a handler-only call of `push`, so the TSX side needs no new
// IR kind: the call is the ordinary service call of ADR-050 section 3.1. The implementation is
// `NavRouterService`, a thin adaptor over `NavController`; an app or a test may register another
// (a recording fake, a guard that asks before leaving), which is why the interface is not the
// controller.
//
// Every request is fire-and-forget, as `navigate` is in react-router: the outcome is visible in the
// controller's published route at the next frame boundary, and a refused request (unknown path,
// full stack) is recorded as `last_error()` instead of thrown, because a handler has no way to
// catch and the shim never invents one.
#pragma once

#include "tessera/appfw/navigation_route.hpp"
#include "tessera/appfw/navigation_stack.hpp"
#include "tessera/appfw/service_id.hpp"

#include <string_view>

namespace tessera::appfw {

class RouterService {
public:
    static constexpr ServiceId kServiceId = service_id("tessera.router");

    virtual ~RouterService() = default;

    // `navigate(to)`: push the route `path` resolves to.
    virtual void push(std::string_view path) = 0;
    // `navigate(to, { replace: true })` and `<Navigate replace />`: history does not grow.
    virtual void replace(std::string_view path) = 0;
    // `navigate(-1)`: the controller's back, in its section 10.1 priority.
    virtual void back() = 0;
    // The most recent refusal, or `None`. Cleared by the next accepted request.
    [[nodiscard]] virtual NavigationErrorCode last_error() const noexcept = 0;
};

// The default implementation over a window's controller, which must outlive it.
class NavRouterService final : public RouterService {
public:
    explicit NavRouterService(NavController& controller) noexcept
        : controller_(&controller) {}

    void push(std::string_view path) override;
    void replace(std::string_view path) override;
    void back() override;
    [[nodiscard]] NavigationErrorCode last_error() const noexcept override {
        return last_error_;
    }

private:
    NavController* controller_;
    NavigationErrorCode last_error_ = NavigationErrorCode::None;
};

} // namespace tessera::appfw
