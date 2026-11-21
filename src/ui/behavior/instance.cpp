// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/instance.hpp"

#include <cstdint>
#include <cstring>

namespace tessera::ui::behavior {
namespace {

// FNV-1a over the evaluated values. Chosen over `std::hash` because the result must be identical on
// every platform and in every build: a preview session and a native build hash the same options,
// and a test that pins a hash would otherwise be a test of the standard library.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t& hash, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFULL;
        hash *= kFnvPrime;
    }
}

} // namespace

std::uint64_t hash_args(std::span<const ArgValue> values) {
    std::uint64_t hash = kFnvOffset;
    // The kind is part of the hash: a bool `true` and a number `1.0` are different options even
    // though a careless cast would make them equal.
    for (const ArgValue& value : values) {
        mix(hash, static_cast<std::uint64_t>(value.kind));
        switch (value.kind) {
        case ArgValue::Kind::Bool:
            mix(hash, value.boolean ? 1ULL : 0ULL);
            break;
        case ArgValue::Kind::Number: {
            // Hash the bit pattern, not the value: `-0.0` and `0.0` compare equal but are different
            // options, and a NaN must not collapse two distinct updates into one.
            std::uint64_t bits = 0;
            static_assert(sizeof(bits) == sizeof(value.number));
            std::memcpy(&bits, &value.number, sizeof(bits));
            mix(hash, bits);
            break;
        }
        case ArgValue::Kind::String:
            mix(hash, value.text.size());
            for (const char character : value.text) {
                mix(hash, static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
            }
            break;
        case ArgValue::Kind::Handler:
            mix(hash, value.handler);
            break;
        }
    }
    return hash;
}

Instance::~Instance() {
    detach();
}

bool Instance::attach(std::string_view behavior, const Targets& targets, const Services& services,
                      const ArgsView& args) {
    // A behavior attaches only when every target holds a node (spec section 8). Attaching to a
    // partial target set would let a two-target behavior read a null node.
    if (!targets.all_attached() || targets.nodes.empty()) {
        return false;
    }
    // Re-attaching replaces the previous instance rather than stacking a second one: a rebuild that
    // reuses the slot must not leave the old behavior registered.
    detach();
    behavior_ = create_behavior(behavior);
    if (!behavior_) {
        return false;
    }
    behavior_->attach(targets, services, args);
    args_hash_ = hash_args(args.values);
    has_hash_ = true;
    return true;
}

void Instance::update(const ArgsView& args) {
    if (!behavior_) {
        return;
    }
    const std::uint64_t hash = hash_args(args.values);
    if (has_hash_ && hash == args_hash_) {
        // The options did not change, so the behavior has nothing to recompute. This is the rule
        // the spec states and the reason an update pass over an unchanged component is free.
        return;
    }
    args_hash_ = hash;
    has_hash_ = true;
    behavior_->update(args);
}

void Instance::refresh(const ArgsView& args) {
    if (!behavior_) {
        return;
    }
    // Deliberately does NOT touch `args_hash_`: the options did not change, so a later `update`
    // with the same options must still be skipped. Only the outputs are recomputed.
    behavior_->update(args);
}

void Instance::detach() {
    if (!behavior_) {
        return;
    }
    behavior_->detach();
    behavior_.reset();
    has_hash_ = false;
    args_hash_ = 0;
}

std::span<const OutputValue> Instance::outputs() const {
    if (!behavior_) {
        return {};
    }
    return behavior_->outputs();
}

} // namespace tessera::ui::behavior
