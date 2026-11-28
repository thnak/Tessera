// SPDX-License-Identifier: MIT
//
// The service interface of the `codegen_observable_tsx` fixture (ADR-050 open question 6): what
// `ui/services.json` points at for `Navigator`. The two observables are the C++ members the
// manifest's `observables` list names; the contract is an accessor returning
// `tessera::appfw::Observable<T>&` (subscribing registers an observer, so it is not const).

#pragma once

#include "tessera/appfw/observable.hpp"
#include "tessera/appfw/service_id.hpp"

#include <string>

namespace demo {

class Navigator {
public:
    static constexpr tessera::appfw::ServiceId kServiceId = tessera::appfw::service_id("demo.nav");
    virtual ~Navigator() = default;
    virtual tessera::appfw::Observable<std::string>& title() = 0;
    virtual tessera::appfw::Observable<int>& unread() = 0;
};

// What the composition root provides: the service owns its observables and writes them.
class NavigatorImpl final : public Navigator {
public:
    explicit NavigatorImpl(tessera::ui::UpdateScheduler& scheduler)
        : title_(scheduler, "Inbox"),
          unread_(scheduler, 0) {}
    tessera::appfw::Observable<std::string>& title() override {
        return title_;
    }
    tessera::appfw::Observable<int>& unread() override {
        return unread_;
    }

private:
    tessera::appfw::Observable<std::string> title_;
    tessera::appfw::Observable<int> unread_;
};

} // namespace demo
