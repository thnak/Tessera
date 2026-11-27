// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui::paint {

// A retainable layer (issue #617, docs/design/gpu/offscreen-layers-retention.md): `owner` is the
// scene node, `key` folds every input of its pixels the paint pass can see (its signature, opacity,
// filter and the retain epoch), `clean` says the node and its subtree are unchanged since the last
// paint. `PaintSink::retain_layer` takes one right before the node's `push_layer`.
struct LayerRetention {
    std::uint64_t owner = 0;
    std::uint64_t key = 0;
    bool clean = false;
};

} // namespace tessera::ui::paint
