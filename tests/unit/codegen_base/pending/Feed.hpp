// SPDX-License-Identifier: MIT

#pragma once

#include "Feed.g.hpp"

// The developer class of the pending-UI fixture (ADR-067 spike #661). It derives from the generated
// base and owns nothing: the boundary, the fake resource and the transition mark are all generated,
// which is the point of the spike - a screen gets the pending UI without writing it.
namespace pending {

class Feed final : public gen::FeedBase<Feed> {};

} // namespace pending
