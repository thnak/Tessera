// SPDX-License-Identifier: MIT
//
// Encoding of the navigation restore snapshot as canonical JSON (docs/specs/state-restoration-v1.md
// sections 3 and 7.1; issue #196).
//
// The wire form is the spec's, reduced to the members this module owns:
//
//   { "restoreVersion": 1,
//     "appVersion": "1.4.0",
//     "routeTable": "<16 hex digits>",
//     "activeStack": "main",
//     "stacks": [ { "id": "main",
//                   "activeEntry": 1,
//                   "entries": [ { "entryId": 1, "route": "home", "params": {}, "state": {} },
//                                { "entryId": 2, "route": "orders.detail",
//                                  "params": {"id":"42"}, "state": {"tab":"items"},
//                                  "presentation": "page" } ] } ] }
//
// `routeTable` is the addition: navigation-v1.md section 5 makes "the route table still matches" a
// condition for restoring, and section 7.1 has no member for it, so it is recorded here rather than
// inferred from the route ids (a table can be compatible with every recorded id and still have
// changed what a path means). A reader that does not know the member ignores it, which is section
// 3's rule.

#include "tessera/appfw/navigation_restore.hpp"
#include "tessera/harness/json_value.hpp"
#include "tessera/harness/json_write.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::appfw {
namespace {

using harness::JsonValue;

JsonValue encode_params(const ParamMap& params) {
    JsonValue object = JsonValue::object();
    for (const auto& [name, value] : params) {
        (void)object.set(name, JsonValue::string(value));
    }
    return object;
}

JsonValue encode_state(const StateMap& state) {
    JsonValue object = JsonValue::object();
    for (const auto& [key, value] : state) {
        (void)object.set(key, JsonValue::string(value));
    }
    return object;
}

JsonValue encode_entry(const NavEntrySnapshot& entry) {
    JsonValue object = JsonValue::object();
    // The entry id travels as a decimal string: section 1 of docs/specs/automation-ids-v1.md keeps
    // ids above 2^53 out of JSON numbers, and the same rule must hold for an id a snapshot
    // restores.
    (void)object.set("entryId", JsonValue::string(std::to_string(entry.entry_id)));
    (void)object.set("route", JsonValue::string(entry.route_id));
    (void)object.set("params", encode_params(entry.params));
    (void)object.set("state", encode_state(entry.state));
    // Only a non-page presentation is written: the default is the common case, and omitting it
    // keeps a plain stack's bytes minimal while a reader defaults a missing member to `page`.
    if (entry.presentation != Presentation::Page) {
        (void)object.set("presentation",
                         JsonValue::string(std::string(presentation_name(entry.presentation))));
    }
    return object;
}

} // namespace

std::expected<std::string, RestoreError> encode_nav_snapshot(const NavSnapshot& snapshot) {
    if (snapshot.restore_version != kNavRestoreVersion) {
        // Encoding a version this module cannot read back would write a snapshot no reader accepts.
        return std::unexpected(
            RestoreError{RestoreErrorCode::UnsupportedVersion, "restoreVersion", 0});
    }
    JsonValue entries = JsonValue::array();
    for (const NavEntrySnapshot& entry : snapshot.stack.entries) {
        (void)entries.push_back(encode_entry(entry));
    }
    JsonValue active_entry = JsonValue::integer(static_cast<std::int64_t>(snapshot.active_index));
    JsonValue stack = JsonValue::object();
    (void)stack.set("id", JsonValue::string(snapshot.stack.id));
    (void)stack.set("activeEntry", std::move(active_entry));
    (void)stack.set("entries", std::move(entries));

    JsonValue stacks = JsonValue::array();
    (void)stacks.push_back(std::move(stack));

    JsonValue root = JsonValue::object();
    (void)root.set("restoreVersion",
                   JsonValue::integer(static_cast<std::int64_t>(snapshot.restore_version)));
    (void)root.set("appVersion", JsonValue::string(snapshot.app_version));
    (void)root.set("routeTable", JsonValue::string(snapshot.route_fingerprint));
    (void)root.set("activeStack", JsonValue::string(std::string(snapshot.stack.id)));
    (void)root.set("stacks", std::move(stacks));

    auto text = harness::to_json(root);
    if (!text.has_value()) {
        // The only two write failures are a non-finite number (there are none: every value here is
        // an integer or a string) and invalid UTF-8 in a string. A route id, a param value or a
        // state value is app data, so an ill-formed one is reported rather than written as invalid
        // JSON.
        return std::unexpected(RestoreError{RestoreErrorCode::MalformedSnapshot, "utf8", 0});
    }
    if (text->size() > kNavSnapshotMaxBytes) {
        // G-14/Q-13's 256 KiB cap, enforced on the encoded form, so the number the caller is
        // bounded by is the number that lands on disk.
        return std::unexpected(RestoreError{RestoreErrorCode::TooLarge, {}, kNavSnapshotMaxBytes});
    }
    return std::move(*text);
}

} // namespace tessera::appfw
