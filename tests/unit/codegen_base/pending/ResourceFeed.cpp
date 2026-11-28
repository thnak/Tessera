// SPDX-License-Identifier: MIT

#include "ResourceFeed.hpp"

// No `bind::` members to define (ADR-040); the translation unit links the same shape a real
// component does.
namespace pending {} // namespace pending
