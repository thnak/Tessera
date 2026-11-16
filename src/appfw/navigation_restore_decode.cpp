// SPDX-License-Identifier: MIT
//
// Decoding and validation of the navigation restore snapshot (docs/specs/state-restoration-v1.md
// sections 4, 6 and 7.1; docs/specs/navigation-v1.md sections 5 and 10.1; issue #196).
//
// The gate order is section 4's "load/validate snapshot (schema/version/limits)": size before
// parsing, then the schema and `restoreVersion` (section 3: "unknown fields are ignored safely"),
// then per-entry pruning (section 7.1: "drop incompatible entries, keep a safe root"), then the
// identity check (navigation-v1.md section 5: "only if the route table/app version still matches").
// Nothing here executes, resolves a path or touches the file system: the blob is data, and the
// worst it produces is a shorter stack.

#include "tessera/appfw/navigation_restore.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_value.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::appfw {
namespace {

using harness::JsonValue;

std::expected<void, RestoreError> read_string(const JsonValue& object, std::string_view key,
                                              std::string& out) {
    const JsonValue* value = object.find(key);
    if (value == nullptr) {
        return {}; // an absent optional member keeps its default
    }
    const auto text = value->as_string();
    if (!text.has_value()) {
        return std::unexpected(
            RestoreError{RestoreErrorCode::MalformedSnapshot, std::string(key), 0});
    }
    out = std::string(*text);
    return {};
}

// A (name, value) map: every member must be a string, and order is the document's, so re-encoding a
// decoded snapshot is byte-identical.
std::expected<void, RestoreError> read_map(const JsonValue& parent, std::string_view key,
                                           std::vector<std::pair<std::string, std::string>>& out,
                                           std::string_view member) {
    const JsonValue* value = parent.find(key);
    if (value == nullptr) {
        return {};
    }
    const JsonValue::Object* object = value->as_object();
    if (object == nullptr) {
        return std::unexpected(
            RestoreError{RestoreErrorCode::MalformedSnapshot, std::string(member), 0});
    }
    for (const auto& [name, item] : *object) {
        const auto text = item.as_string();
        if (!text.has_value()) {
            return std::unexpected(
                RestoreError{RestoreErrorCode::MalformedSnapshot, std::string(member), 0});
        }
        out.emplace_back(name, std::string(*text));
    }
    return {};
}
std::expected<NavEntrySnapshot, RestoreError> decode_entry(const JsonValue& value,
                                                           std::size_t index) {
    const std::string prefix = "entries[" + std::to_string(index) + "]";
    const JsonValue::Object* object = value.as_object();
    if (object == nullptr) {
        return std::unexpected(RestoreError{RestoreErrorCode::MalformedSnapshot, prefix, 0});
    }
    // The stack half of a `WindowSnapshot` is one `(route, params)` pair per entry plus the
    // optional per-route `state`; `presentation` is written only when it is not `page`, so a plain
    // stack's bytes stay minimal and a reader that finds no member defaults it. `entryId` is a
    // decimal string, not a JSON number: docs/specs/automation-ids-v1.md section 1 keeps ids above
    // 2^53 out of JSON numbers, and a restored id must be the exact same id.
    NavEntrySnapshot entry;
    const JsonValue* id = value.find("entryId");
    if (id != nullptr) {
        const auto text = id->as_string();
        if (!text.has_value() || text->empty()) {
            return std::unexpected(
                RestoreError{RestoreErrorCode::MalformedSnapshot, prefix + ".entryId", 0});
        }
        std::uint64_t parsed = 0;
        for (const char character : *text) {
            if (character < '0' || character > '9') {
                return std::unexpected(
                    RestoreError{RestoreErrorCode::MalformedSnapshot, prefix + ".entryId", 0});
            }
            const auto digit = static_cast<std::uint64_t>(character - '0');
            if (parsed > (UINT64_MAX - digit) / 10) {
                return std::unexpected(
                    RestoreError{RestoreErrorCode::MalformedSnapshot, prefix + ".entryId", 0});
            }
            parsed = parsed * 10 + digit;
        }
        entry.entry_id = parsed;
    }
    if (auto route = read_string(value, "route", entry.route_id); !route.has_value()) {
        return std::unexpected(route.error());
    }
    if (entry.route_id.empty()) {
        return std::unexpected(
            RestoreError{RestoreErrorCode::MalformedSnapshot, prefix + ".route", 0});
    }
    if (auto params = read_map(value, "params", entry.params, prefix + ".params");
        !params.has_value()) {
        return std::unexpected(params.error());
    }
    if (auto state = read_map(value, "state", entry.state, prefix + ".state"); !state.has_value()) {
        return std::unexpected(state.error());
    }
    std::string presentation;
    if (auto read = read_string(value, "presentation", presentation); !read.has_value()) {
        return std::unexpected(read.error());
    }
    if (!presentation.empty()) {
        if (presentation == "modal") {
            entry.presentation = Presentation::Modal;
        } else if (presentation == "sheet") {
            entry.presentation = Presentation::Sheet;
        } else if (presentation == "fullscreen") {
            entry.presentation = Presentation::FullScreen;
        } else if (presentation != "page") {
            return std::unexpected(
                RestoreError{RestoreErrorCode::MalformedSnapshot, prefix + ".presentation", 0});
        }
    }
    return entry;
}

} // namespace
std::expected<DecodedSnapshot, RestoreError>
decode_nav_snapshot(std::string_view text, const RouteTable& table, const RestorePolicy& policy) {
    // Gate 1: size, before the decoder sees the bytes.
    if (text.size() > policy.max_bytes) {
        return std::unexpected(RestoreError{RestoreErrorCode::TooLarge, {}, policy.max_bytes});
    }
    auto parsed = harness::parse_json(text);
    if (!parsed.has_value()) {
        return std::unexpected(
            RestoreError{RestoreErrorCode::MalformedJson, {}, parsed.error().offset});
    }
    const JsonValue::Object* root = parsed->as_object();
    if (root == nullptr) {
        return std::unexpected(RestoreError{RestoreErrorCode::MalformedSnapshot, {}, 0});
    }

    DecodedSnapshot decoded;
    NavSnapshot& snapshot = decoded.snapshot;

    // Gate 2: the version, then the identity fields.
    const JsonValue* version = parsed->find("restoreVersion");
    if (version == nullptr || !version->as_integer().has_value()) {
        return std::unexpected(
            RestoreError{RestoreErrorCode::MalformedSnapshot, "restoreVersion", 0});
    }
    const std::int64_t version_value = *version->as_integer();
    if (version_value < 0 || static_cast<std::uint64_t>(version_value) > UINT32_MAX ||
        static_cast<std::uint32_t>(version_value) != kNavRestoreVersion) {
        // state-restoration-v1.md section 3: "`restoreVersion` migrates forward". There is exactly
        // one version so far, so anything else is refused rather than guessed at; a migration is a
        // new branch here when a second shape exists.
        return std::unexpected(
            RestoreError{RestoreErrorCode::UnsupportedVersion, "restoreVersion", 0});
    }
    snapshot.restore_version = static_cast<std::uint32_t>(version_value);

    if (auto read = read_string(*parsed, "appVersion", snapshot.app_version); !read.has_value()) {
        return std::unexpected(read.error());
    }
    if (!policy.expected_app_version.empty() &&
        snapshot.app_version != policy.expected_app_version) {
        // An app that asked for a strict match (a store that cannot migrate a route table) gets
        // one; the default accepts another version, because section 3 says `restoreVersion`
        // migrates forward and a patch release must not lose the user's stack.
        return std::unexpected(RestoreError{RestoreErrorCode::AppVersionMismatch, "appVersion", 0});
    }
    if (auto read = read_string(*parsed, "routeTable", snapshot.route_fingerprint);
        !read.has_value()) {
        return std::unexpected(read.error());
    }

    // Gate 3: the stack. V1 writes one (`activeStack` == the one stack's id); a reader that finds
    // more reads the active one and ignores the rest, which is what makes per-tab stacks additive
    // later.
    const JsonValue* stacks = parsed->find("stacks");
    if (stacks == nullptr || stacks->as_array() == nullptr) {
        return std::unexpected(RestoreError{RestoreErrorCode::MalformedSnapshot, "stacks", 0});
    }
    std::string active_stack = std::string(kNavMainStackId);
    if (auto read = read_string(*parsed, "activeStack", active_stack); !read.has_value()) {
        return std::unexpected(read.error());
    }
    const JsonValue* selected = nullptr;
    for (const JsonValue& candidate : *stacks->as_array()) {
        if (candidate.as_object() == nullptr) {
            continue;
        }
        std::string id;
        if (auto read = read_string(candidate, "id", id); !read.has_value()) {
            return std::unexpected(read.error());
        }
        if (id == active_stack) {
            selected = &candidate;
            break;
        }
        if (selected == nullptr) {
            selected = &candidate; // the first stack, when `activeStack` names none of them
        }
    }
    if (selected == nullptr) {
        return std::unexpected(RestoreError{RestoreErrorCode::MalformedSnapshot, "stacks", 0});
    }
    if (auto id = read_string(*selected, "id", snapshot.stack.id); !id.has_value()) {
        return std::unexpected(id.error());
    }
    if (snapshot.stack.id.empty()) {
        snapshot.stack.id = std::string(kNavMainStackId);
    }

    const JsonValue* entries = selected->find("entries");
    if (entries == nullptr || entries->as_array() == nullptr) {
        return std::unexpected(
            RestoreError{RestoreErrorCode::MalformedSnapshot, "stacks[0].entries", 0});
    }
    for (std::size_t index = 0; index < entries->as_array()->size(); ++index) {
        auto entry = decode_entry((*entries->as_array())[index], index);
        if (!entry.has_value()) {
            // Section 7.1: "drop incompatible entries, keep a safe root". A single unreadable entry
            // is dropped and counted; the rest of the stack survives, which is the difference
            // between a partial restore and a lost session.
            ++decoded.dropped;
            continue;
        }
        // navigation-v1.md section 10.1: "invalid entries are dropped or replaced by the route
        // root", and state-restoration-v1.md section 7.1: "drop incompatible entries". A route the
        // table no longer has is exactly "a flow that no longer exists": the entry is dropped
        // rather than installed, so a restore cannot install a screen name the table cannot
        // resolve.
        if (table.find(entry->route_id) == nullptr) {
            ++decoded.dropped;
            continue;
        }
        snapshot.stack.entries.push_back(std::move(*entry));
    }

    const JsonValue* active_entry = selected->find("activeEntry");
    if (active_entry != nullptr) {
        const auto value = active_entry->as_integer();
        if (!value.has_value() || *value < 0) {
            return std::unexpected(
                RestoreError{RestoreErrorCode::MalformedSnapshot, "stacks[0].activeEntry", 0});
        }
        snapshot.active_index = static_cast<std::size_t>(*value);
    }
    // Clamped rather than trusted: an index past the end names the last entry, because a restored
    // stack with no active entry would mount nothing (section 4: "not an empty/white app").
    if (snapshot.stack.entries.empty()) {
        snapshot.active_index = 0;
    } else if (snapshot.active_index >= snapshot.stack.entries.size()) {
        snapshot.active_index = snapshot.stack.entries.size() - 1;
    }

    // Gate 4: the identity check, last because it is the cheapest refusal to report once the
    // snapshot proved readable. navigation-v1.md section 5 makes it the condition for restoring at
    // all: a different table means the ids may name different screens, which is what section 4 says
    // must fall back.
    if (snapshot.route_fingerprint != table.fingerprint()) {
        return std::unexpected(RestoreError{RestoreErrorCode::RouteTableMismatch, "routeTable", 0});
    }
    return decoded;
}

std::vector<NavEntry> restore_entries(const DecodedSnapshot& decoded) {
    std::vector<NavEntry> entries;
    // The active entry is last: the stack's invariant is "back pops the end", and section 7.1's
    // snapshot records the same information as a whole stack plus one active index.
    entries.reserve(decoded.snapshot.active_index + 1);
    for (std::size_t index = 0;
         index <= decoded.snapshot.active_index && index < decoded.snapshot.stack.entries.size();
         ++index) {
        const NavEntrySnapshot& source = decoded.snapshot.stack.entries[index];
        NavEntry entry;
        entry.entry_id = source.entry_id;
        entry.route_id = source.route_id;
        entry.params = source.params;
        entry.state = source.state;
        entry.presentation = source.presentation;
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace tessera::appfw
