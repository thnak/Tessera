// SPDX-License-Identifier: MIT
//
// Layout-input signature: a single allocation-free hash over the inputs `layout_engine.cpp` turns
// into Yoga styles and measured boxes. Kept separate from the scheduler so tests can assert the
// inclusion list directly.

#include "tessera/ui/layout/layout_signature.hpp"
#include "tessera/ui/style/layout_digest.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace tessera::ui::layout {
namespace {

// Word-at-a-time multiply-xorshift mixing. The first version folded one byte per multiply (FNV-1a),
// which made the hash walk about a fifth of an incremental frame at 12k nodes: eight multiplies for
// each 64-bit id and one for every character of text. Each word below is one xor, two multiplies
// and a shift, and text goes in eight bytes at a time. The signature is only ever compared with
// another one computed by the same function in the same process, so the exact values are free to
// change.
constexpr std::uint64_t kSeed = 0x243F6A8885A308D3ULL; // pi, so an empty tree is not zero
constexpr std::uint64_t kMulA = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kMulB = 0xBF58476D1CE4E5B9ULL;

// Framing words keep the hash unambiguous: ids, property values and text are all length-prefixed
// and every node is delimited, so shifting bytes between fields changes the signature.
constexpr std::uint64_t kNodeBegin = 0x4E;
constexpr std::uint64_t kText = 0x54;
constexpr std::uint64_t kTextWrap = 0x57;
constexpr std::uint64_t kNodeEnd = 0x45;

class Hasher {
public:
    void word(std::uint64_t value) noexcept {
        state_ = (state_ ^ value) * kMulA;
        state_ ^= state_ >> 29;
        state_ *= kMulB;
        state_ ^= state_ >> 32;
    }

    void f32(float value) noexcept {
        word(std::bit_cast<std::uint32_t>(value));
    }

    void bytes(std::string_view value) noexcept {
        word(value.size());
        const char* data = value.data();
        std::size_t left = value.size();
        while (left >= sizeof(std::uint64_t)) {
            std::uint64_t chunk = 0;
            std::memcpy(&chunk, data, sizeof(chunk));
            word(chunk);
            data += sizeof(chunk);
            left -= sizeof(chunk);
        }
        if (left > 0) {
            std::uint64_t tail = 0; // zero padding is unambiguous: the length went in first
            std::memcpy(&tail, data, left);
            word(tail);
        }
    }

    explicit Hasher(std::uint64_t state = kSeed) noexcept
        : state_(state) {}

    [[nodiscard]] std::uint64_t value() const noexcept {
        return state_;
    }

private:
    std::uint64_t state_;
};

} // namespace

LayoutSignatureBuilder::LayoutSignatureBuilder(const LayoutViewport& viewport) noexcept {
    Hasher hasher;
    hasher.f32(viewport.width);
    hasher.f32(viewport.height);
    hasher.f32(viewport.dpi_scale);
    viewport_state_ = hasher.value();
    value_ = viewport_state_;
}

std::uint64_t& LayoutSignatureBuilder::top() noexcept {
    return depth_ <= kInlineDepth ? inline_[depth_ - 1] : spill_[depth_ - kInlineDepth - 1];
}

// A finished subtree joins its parent's running state, or becomes the whole tree's value when no
// node is open.
void LayoutSignatureBuilder::fold_child(std::uint64_t subtree_signature) noexcept {
    if (depth_ == 0) {
        Hasher hasher(viewport_state_);
        hasher.word(subtree_signature);
        value_ = hasher.value();
        return;
    }
    Hasher hasher(top());
    hasher.word(subtree_signature);
    top() = hasher.value();
}

void LayoutSignatureBuilder::reuse(std::uint64_t subtree_signature) noexcept {
    fold_child(subtree_signature);
}

void LayoutSignatureBuilder::enter(const LayoutInput& input) noexcept {
    Hasher hasher(kSeed);
    hasher.word(kNodeBegin);
    hasher.word(input.id);

    // `ComputedStyle` owns a sorted map, so this walk is deterministic regardless of insertion
    // order. Only the layout set is folded in; a paint-only change leaves the signature intact.
    // The layout part of the style is hashed on its own and cached with the style's shared
    // entries, so a node whose style did not change costs one load instead of a pass over ~100
    // names (issue #711). The property set and the hash live in the style module
    // (`style::layout_digest`), because `tessera_layout` depends on `tessera_style` and not the
    // other way round; the layout-unchanged gate reads the same digest (issue #735).
    hasher.word(style::layout_digest(input.style));

    // Text drives Yoga's measure callback, so it is a layout input even when the surrounding box
    // has fixed dimensions. Including it can only cost a redundant reflow, never a wrong reuse.
    hasher.word(kText);
    hasher.bytes(input.text);

    // Paint's wrap opt-in changes no box, but it decides `LayoutBox::text_fit`, which is part of
    // the result the scheduler reuses (issue #490). Folding it in means a wrap flip re-measures
    // instead of reusing a stale fit fact.
    hasher.word(kTextWrap | (input.text_wrap ? 0x100U : 0U));
    hasher.word(0x5A000000U | input.text_runs.value);
    // A table cell's spans place it in the grid: a change reflows the table.
    hasher.word((std::uint64_t{0x5B} << 32U) | (std::uint64_t{input.col_span} << 16U) |
                input.row_span);
    // Open the node: its running state starts from its own fields and takes each child's signature
    // as the children finish.
    ++depth_;
    if (depth_ > kInlineDepth && spill_.size() < depth_ - kInlineDepth) {
        spill_.resize(depth_ - kInlineDepth);
    }
    top() = hasher.value();
}

std::uint64_t LayoutSignatureBuilder::leave(std::size_t child_count) noexcept {
    Hasher hasher(top());
    hasher.word(kNodeEnd | (static_cast<std::uint64_t>(child_count) << 8));
    --depth_;
    // Zero is reserved for "never computed" in `LayoutInput::subtree_signature`.
    const std::uint64_t signature = hasher.value() == 0 ? 1 : hasher.value();
    fold_child(signature);
    return signature;
}

std::uint64_t layout_signature(const LayoutInput& root, const LayoutViewport& viewport) noexcept {
    LayoutSignatureBuilder builder(viewport);
    const auto fold = [&builder](auto&& self, const LayoutInput& node) -> void {
        builder.enter(node);
        for (const LayoutInput& child : node.children) {
            self(self, child);
        }
        builder.leave(node.children.size());
    };
    fold(fold, root);
    return builder.value();
}

} // namespace tessera::ui::layout
