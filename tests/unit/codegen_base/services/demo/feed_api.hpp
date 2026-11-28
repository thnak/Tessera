// SPDX-License-Identifier: MIT
//
// A `ResourceProvider` service for the pending-UI fixtures (pending-ui-v1.md section 10.2): a
// component that declares `useService<demo::FeedApi>()` and `useResource(api.loadRow, ...)` asks
// this service for the view of (method, canonical key) and never binds a view by hand.

#pragma once

#include "tessera/appfw/service_id.hpp"
#include "tessera/ui/pending/resource_view.hpp"

#include <string_view>

namespace demo {

class FeedApi {
public:
    static constexpr tessera::appfw::ServiceId kServiceId =
        tessera::appfw::service_id("demo.feed-api");
    virtual ~FeedApi() = default;
    virtual tessera::ui::pending::ResourceView resource(std::string_view method,
                                                        std::string_view key) noexcept = 0;
};

} // namespace demo
