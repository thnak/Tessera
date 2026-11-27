// SPDX-License-Identifier: MIT

#pragma once

#include "ResourceFeed.g.hpp"

// The developer class of the resource-driven pending fixture (#656). Like `Feed` it owns nothing:
// the views, the boundary and the transition mark are generated; the test binds a view per
// resource.
namespace pending {

class ResourceFeed final : public gen::ResourceFeedBase<ResourceFeed> {};

} // namespace pending
