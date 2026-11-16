// SPDX-License-Identifier: MIT
//
// Navigation restoration: the text snapshot of a stack, and its persistence through AF7's `Storage`
// (docs/specs/navigation-v1.md section 8; docs/specs/state-restoration-v1.md sections 2, 3, 4, 5, 7
// and 7.1; ADR-050 section 8, row `restore`; issue #196).
//
// What the two specs fix, and what this header therefore does:
//
//   * **The route stack and its params are persisted by default** (state-restoration-v1.md section
//   2,
//     row "Route stack + params: yes"). Everything else in that table is opt-in, so the snapshot
//     this module writes carries the stack, the active stack id and per-entry `state`, and nothing
//     else. No window geometry, no scroll, no focus, no drafts: those are other slices with their
//     own columns.
//   * **The format is canonical JSON, `restoreVersion` 1, <=256 KiB per window** (section 3,
//   G-14/Q-13),
//     **versioned and forward-migrating** (section 3: "`restoreVersion` migrates forward; unknown
//     fields are ignored safely"). The encoder is deterministic - sorted or declaration-order
//     members, no timestamps, no floats - so two runs of one stack produce identical bytes, which
//     is what makes the round-trip test an equality instead of a tolerance.
//   * **One stack, id `main`** (Q-11 in navigation-v1.md section 10.1: "V1 writes one stack with id
//     `main`"). The wire shape already has `stacks: [{id, entries}]` so per-tab stacks stay
//     additive.
//   * **Persistence goes through `appfw::Storage`, not a file** (state-restoration-v1.md
//   section 7.1:
//     "`services/storage` owns atomic file/path/encryption primitives"). So a restore write is an
//     atomic replace on an I/O executor, the corruption case is `StorageErrorCode::Corrupted`, and
//     this module never opens a path or blocks the UI thread.
//   * **A snapshot is validated before it is trusted** (section 4 step 1, section 6 "never trust
//   the
//     blob", navigation-v1.md section 5 "only if the route table/app version still matches"). Three
//     independent gates: the restore version, the app version string, and the route-table
//     fingerprint. Each failure is a named value, and the caller falls back to the route root
//     exactly as section 4 says ("A failed step falls back to the route root, not an empty/white
//     app").
//   * **Invalid entries are dropped, not the whole stack** (navigation-v1.md section 10.1: "invalid
//     entries are dropped or replaced by the route root"; state-restoration-v1.md section 7.1:
//     "App/route table version mismatch: drop incompatible entries, keep a safe root"). `decode`
//     reports how many it dropped so a diagnostic can say so.
//
// The encoding is JSON text and the module parses it with the in-tree `tessera_harness` codec
// rather than a second parser (the same choice `src/ui/style` makes for the theme asset).
// `tessera_harness` is a leaf library that links nothing from the project, so layer 1 depending on
// it does not reach into the UI layer (ADR-050 section 1).

#pragma once

#include "tessera/appfw/navigation_route.hpp"
#include "tessera/appfw/navigation_stack.hpp"
#include "tessera/appfw/storage.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// state-restoration-v1.md section 3's `restoreVersion`. Bumped only by a migration that changes the
// shape; the reader accepts exactly this one and reports `UnsupportedRestoreVersion` otherwise,
// because a forward migration needs a real older shape to migrate from and there is only one so
// far.
inline constexpr std::uint32_t kNavRestoreVersion = 1;

// G-14/Q-13: "<=256 KiB/window". The stack is the only thing this module persists, so the cap
// applies to the whole encoded snapshot.
inline constexpr std::size_t kNavSnapshotMaxBytes = 256 * 1024;

// The stack id V1 writes (Q-11: "V1 writes one stack with id `main`").
inline constexpr std::string_view kNavMainStackId = "main";

// One entry in the encoded form. `entry_id` is persisted because section 10.1 calls it "stable for
// restore/automation", and `state` because it is the "serializable, opt-in restore" value of the
// same struct.
struct NavEntrySnapshot {
    std::uint64_t entry_id = 0;
    std::string route_id;
    ParamMap params;
    StateMap state;
    Presentation presentation = Presentation::Page;
};

// The stack half of a window snapshot (state-restoration-v1.md section 7.1's `StackSnapshot`).
struct NavStackSnapshot {
    std::string id = std::string(kNavMainStackId);
    std::vector<NavEntrySnapshot> entries;
};

// The navigation half of a `RestoreSnapshot`: version, the app version that wrote it, the
// route-table fingerprint that wrote it, and the stack. `saved_at` is deliberately absent from the
// *encoded* form this module owns: it is state-restoration-v1.md section 3's field, but a timestamp
// is a wall clock and a walk over the stack must be a function of the arguments alone for the round
// trip to be an equality. The host's own snapshot envelope carries it if it wants one.
struct NavSnapshot {
    std::uint32_t restore_version = kNavRestoreVersion;
    std::string app_version;
    std::string route_fingerprint;
    NavStackSnapshot stack;
    // The entry index the snapshot's `activeStack` points at. Section 3's wire form names the
    // active *stack*, and V1 has one; the active *entry* is what a restore needs, so it is an index
    // here and is clamped on read rather than trusted.
    std::size_t active_index = 0;
};

// Why a snapshot was refused. Every one is reachable from bytes alone, so a test can name it.
enum class RestoreErrorCode : std::uint8_t {
    None = 0,
    MalformedJson,     // not canonical JSON, or a member has the wrong type
    MalformedSnapshot, // JSON, but not this module's object shape
    TooLarge,          // over `max_bytes` (G-14/Q-13's 256 KiB)
    UnsupportedVersion,
    AppVersionMismatch,
    RouteTableMismatch,
};

[[nodiscard]] constexpr std::string_view restore_error_code_name(RestoreErrorCode code) noexcept {
    switch (code) {
    case RestoreErrorCode::None:
        return "none";
    case RestoreErrorCode::MalformedJson:
        return "malformed json";
    case RestoreErrorCode::MalformedSnapshot:
        return "malformed snapshot";
    case RestoreErrorCode::TooLarge:
        return "too large";
    case RestoreErrorCode::UnsupportedVersion:
        return "unsupported restore version";
    case RestoreErrorCode::AppVersionMismatch:
        return "app version mismatch";
    case RestoreErrorCode::RouteTableMismatch:
        return "route table mismatch";
    }
    return "unknown";
}

struct RestoreError {
    RestoreErrorCode code = RestoreErrorCode::MalformedSnapshot;
    // The member that failed ("entries[2].route", "restoreVersion"), so a diagnostic names what it
    // read.
    std::string member;
    // The JSON byte offset, when the failure is positional.
    std::size_t offset = 0;

    [[nodiscard]] bool operator==(const RestoreError&) const = default;
};

// What `decode` accepted. Section 4's "dropping invalid routes/params" is what `dropped` counts:
// the returned stack is already pruned, and the count lets a caller record one diagnostic instead
// of one per entry.
struct DecodedSnapshot {
    NavSnapshot snapshot;
    std::size_t dropped = 0;
};

// How strictly a decode compares identity fields. The default accepts a snapshot from a different
// app version, because state-restoration-v1.md section 3 says `restoreVersion` migrates forward and
// a patch release must not lose the user's stack; an app that needs a strict build match sets
// `expected_app_version` and the check becomes an equality. The route-table fingerprint is always
// compared: navigation-v1.md section 5 makes that the condition for restoring at all.
struct RestorePolicy {
    // When non-empty, a snapshot whose `appVersion` differs is refused with `AppVersionMismatch`.
    // Empty means "any version", which is the default and the migration-friendly reading.
    std::string expected_app_version;
    std::size_t max_bytes = kNavSnapshotMaxBytes;
};

// Encodes the stack as canonical JSON (state-restoration-v1.md sections 3 and 7.1). No allocation
// is avoided here on purpose: a save happens on state change, not on a frame, and the encoding is
// what the atomic write hands to the executor.
[[nodiscard]] std::expected<std::string, RestoreError>
encode_nav_snapshot(const NavSnapshot& snapshot);

// Decodes and validates against `table`. The table is needed for two things the spec requires and a
// fingerprint cannot do: the identity check (navigation-v1.md section 5's "only if the route table
// ... still matches") and the per-entry pruning of a route id the table no longer has
// (section 10.1's "invalid entries are dropped or replaced by the route root";
// state-restoration-v1.md section 7.1's "drop incompatible entries"). `dropped` reports the second.
[[nodiscard]] std::expected<DecodedSnapshot, RestoreError>
decode_nav_snapshot(std::string_view text, const RouteTable& table,
                    const RestorePolicy& policy = {});

// The stack a controller should install for `decoded`: the pruned entries, with the active entry
// last (the stack invariant back relies on). Exposed so a test can assert the mapping without a
// controller.
[[nodiscard]] std::vector<NavEntry> restore_entries(const DecodedSnapshot& decoded);

// Bounded persistence through AF7. It owns no file, no path and no thread: the caller hands it the
// store it already built, and every byte goes through the same atomic replace and the same I/O
// executor the rest of the app's data does (state-restoration-v1.md section 7.1's ownership row for
// `services/storage`).
//
// Two calls, not a queue: `save` encodes and submits one atomic write; `load` submits one read and
// hands back the decoded snapshot. A second save before the first completes is the caller's policy
// (state-restoration-v1.md section 7.1: "never write on every keystroke/scroll tick" - debouncing
// is the caller's, because the interval is a config field, not a navigation rule).
class NavRestoreStore {
public:
    using SaveCallback = std::function<void(std::expected<void, RestoreError>)>;
    using LoadCallback = std::function<void(std::expected<DecodedSnapshot, RestoreError>)>;

    // `store` is borrowed and must outlive this object. `entry_name` is one `Storage` entry, so the
    // snapshot lives beside the app's other documents and benefits from the same atomicity and
    // corruption reporting.
    NavRestoreStore(Storage& store, std::string entry_name, RestorePolicy policy = {});

    NavRestoreStore(const NavRestoreStore&) = delete;
    NavRestoreStore& operator=(const NavRestoreStore&) = delete;

    // Encodes, then submits the atomic write. Returns false when the encoding or the submission
    // failed and the callback will not run (the same submit contract `Storage::save` follows); true
    // means the callback runs exactly once.
    bool save(NavSnapshot snapshot, runtime::CancellationToken token, SaveCallback completion);

    // Submits the read. An absent entry is the first-run condition, not a corruption: it arrives as
    // `RestoreErrorCode::MalformedSnapshot` with `member == "not-found"`, which is how a caller
    // tells "no snapshot yet" (fall back to the route root silently, per state-restoration-v1.md
    // section 4) from "a snapshot I cannot trust" (record a diagnostic,
    // `TESSERA_STARTUP_RESTORE_INVALID`).
    bool load(const RouteTable& table, runtime::CancellationToken token, LoadCallback completion);

    [[nodiscard]] std::string_view entry_name() const noexcept {
        return entry_name_;
    }
    [[nodiscard]] const RestorePolicy& policy() const noexcept {
        return policy_;
    }

private:
    Storage* store_ = nullptr;
    std::string entry_name_;
    RestorePolicy policy_;
};

} // namespace tessera::appfw
