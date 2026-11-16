// SPDX-License-Identifier: MIT
//
// The route table and path matching for the navigation module (docs/specs/navigation-v1.md sections
// 1 and 3; ADR-050 section 8, row `navigation`; issue #196).
//
// The spec fixes the shape and this header makes the four decisions it leaves to the
// implementation, each stated where it applies:
//
//   1. **The pattern grammar is the spec's four forms and nothing else** (section 1): literal
//      segments, `:name`, `:name?` and a trailing `*` wildcard. No regex, so a route table cannot
//      become a matcher an author can accidentally make exponential.
//   2. **Matching is total and ordered** (section 1's "id is stable, path is for matching"): a path
//      that matches several patterns resolves to the one with the fewest wildcards, then the fewest
//      params, then the earliest table position. Section 1 only says duplicate *paths* are errors;
//      this rule is what makes `/orders/new` win over `/orders/:id` deterministically.
//   3. **An unknown route is a named failure**, not a crash and not an empty match (section 3:
//      "Unknown route -> not-found screen"). `match()` returns `NavigationErrorCode::NotFound`, and
//      the controller maps that to the table's `notFound` route, which is where sections 3 and 5
//      put the decision.
//   4. **Params are typed here, before a screen mounts** (section 3): `ParamSpec` declares
//      `string`/`int`/`uuid`/`enum` per name, an undeclared name defaults to `string`, and a value
//      that does not parse is `ParamInvalid`. Section 11 records "type generation for params from
//      the route table" as an open question, so a declaration is optional data rather than a
//      required column: the runtime half validates whatever the table declares.
//
// Everything a deep link or a snapshot supplies is untrusted input (section 10), so decoding is one
// pass, bounded, and rejects malformed percent-encoding, malformed UTF-8, control characters and
// traversal before a value reaches a match.
//
// Deliberately not in this slice, and named so a caller does not assume it: guards/auth (§4), route
// loaders (§10.1 `Route data`), layout routes and nested outlets (§1), and the build-time route
// generator (§1 "validated at build time" is `validate_route_config`, which a generator calls).

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

// Why a route/param/snapshot operation failed. Errors are values (ADR-025) and every one of them is
// reachable with data alone, so a test can name the failure instead of asserting "it did not
// crash".
enum class NavigationErrorCode : std::uint8_t {
    None = 0,
    // --- the route table (section 1: "duplicate ids/paths are errors") ---
    EmptyRouteTable,
    DuplicateRouteId,
    DuplicateRoutePath,
    InvalidPattern, // not the section 1 grammar (`*` not last, `:` in a literal, empty segment)
    UnknownNotFoundRoute,
    UnknownInitialRoute,
    UnknownParam,  // a `ParamSpec` that names no param in its own pattern
    EmptyEnumSpec, // an `enum` param with no allowed values can never validate
    // --- matching and parameters (sections 3 and 10) ---
    NotFound,    // the path matched no route: the not-found screen, not a crash
    InvalidPath, // no leading `/`, or a decoded segment is `.`/`..`
    MalformedPercentEncoding,
    InvalidUtf8,
    ControlCharacter,
    PathTooLong,
    CapacityExceeded, // too many segments or params for the configured bound
    ParamMissing,
    ParamInvalid, // typed validation failed
    // --- the stack (section 2) ---
    UnknownRouteId,   // the controller was asked for an id the table does not declare
    UnknownEntry,     // `pop_to` targeted an entry that is not in the stack
    EmptyStack,       // `reset`/`replace_all` with nothing to commit
    DuplicateIgnored, // the per-route duplicate policy (section 2) made the request a no-op
    // --- deep links and restore (sections 5 and 8) ---
    UnsupportedScheme,
    UnsupportedHost,
    EncodedSeparator, // `%2f`/`%5c` inside a segment: a decoded separator would re-segment the path
    MalformedSnapshot,
    SnapshotTooLarge,
    UnsupportedRestoreVersion,
    AppVersionMismatch,
    RouteTableMismatch,
};

[[nodiscard]] constexpr std::string_view
navigation_error_code_name(NavigationErrorCode code) noexcept {
    switch (code) {
    case NavigationErrorCode::None:
        return "none";
    case NavigationErrorCode::EmptyRouteTable:
        return "empty route table";
    case NavigationErrorCode::DuplicateRouteId:
        return "duplicate route id";
    case NavigationErrorCode::DuplicateRoutePath:
        return "duplicate route path";
    case NavigationErrorCode::InvalidPattern:
        return "invalid pattern";
    case NavigationErrorCode::UnknownNotFoundRoute:
        return "unknown not-found route";
    case NavigationErrorCode::UnknownInitialRoute:
        return "unknown initial route";
    case NavigationErrorCode::UnknownParam:
        return "unknown param declaration";
    case NavigationErrorCode::EmptyEnumSpec:
        return "empty enum spec";
    case NavigationErrorCode::NotFound:
        return "not found";
    case NavigationErrorCode::InvalidPath:
        return "invalid path";
    case NavigationErrorCode::MalformedPercentEncoding:
        return "malformed percent encoding";
    case NavigationErrorCode::InvalidUtf8:
        return "invalid utf-8";
    case NavigationErrorCode::ControlCharacter:
        return "control character";
    case NavigationErrorCode::PathTooLong:
        return "path too long";
    case NavigationErrorCode::CapacityExceeded:
        return "capacity exceeded";
    case NavigationErrorCode::ParamMissing:
        return "param missing";
    case NavigationErrorCode::ParamInvalid:
        return "param invalid";
    case NavigationErrorCode::UnknownRouteId:
        return "unknown route id";
    case NavigationErrorCode::UnknownEntry:
        return "unknown entry";
    case NavigationErrorCode::EmptyStack:
        return "empty stack";
    case NavigationErrorCode::DuplicateIgnored:
        return "duplicate ignored";
    case NavigationErrorCode::UnsupportedScheme:
        return "unsupported scheme";
    case NavigationErrorCode::UnsupportedHost:
        return "unsupported host";
    case NavigationErrorCode::EncodedSeparator:
        return "encoded separator";
    case NavigationErrorCode::MalformedSnapshot:
        return "malformed snapshot";
    case NavigationErrorCode::SnapshotTooLarge:
        return "snapshot too large";
    case NavigationErrorCode::UnsupportedRestoreVersion:
        return "unsupported restore version";
    case NavigationErrorCode::AppVersionMismatch:
        return "app version mismatch";
    case NavigationErrorCode::RouteTableMismatch:
        return "route table mismatch";
    }
    return "unknown";
}

// A failure with the value that caused it. `subject` is a route id, a param name or the offending
// text; `offset` is a byte offset into the input when the failure is positional. It owns its text
// so an error can outlive the buffer it came from, which is what a diagnostic or a test asserts on.
struct NavigationError {
    NavigationErrorCode code = NavigationErrorCode::None;
    std::string subject;
    std::size_t offset = 0;

    [[nodiscard]] bool operator==(const NavigationError&) const = default;
};

// The typed param vocabulary of section 3 (`int`, `uuid`, `enum`, `string`).
enum class ParamType : std::uint8_t { String = 0, Int, Uuid, Enum };

[[nodiscard]] constexpr std::string_view param_type_name(ParamType type) noexcept {
    switch (type) {
    case ParamType::String:
        return "string";
    case ParamType::Int:
        return "int";
    case ParamType::Uuid:
        return "uuid";
    case ParamType::Enum:
        return "enum";
    }
    return "string";
}

// One declared param. Declaring a param is optional; an undeclared one is validated as `String`.
struct ParamSpec {
    std::string name;
    ParamType type = ParamType::String;
    // `Enum` only, in declaration order so a diagnostic and a test name the same value.
    std::vector<std::string> allowed;

    [[nodiscard]] bool operator==(const ParamSpec&) const = default;
};

// Section 6: a modal route is a stack entry with a presentation, not a second stack (Q-11's one
// global stack). Back priority (section 10.1 item 2) is "topmost modal/sheet entry", so the stack
// needs to know which entries are dismissible as an overlay.
enum class Presentation : std::uint8_t { Page = 0, Modal, Sheet, FullScreen };

[[nodiscard]] constexpr std::string_view presentation_name(Presentation presentation) noexcept {
    switch (presentation) {
    case Presentation::Page:
        return "page";
    case Presentation::Modal:
        return "modal";
    case Presentation::Sheet:
        return "sheet";
    case Presentation::FullScreen:
        return "fullscreen";
    }
    return "page";
}

// Section 10.1 back priority item 2 covers modal *and* sheet entries; a fullscreen presentation is
// a page that fills the window, so back pops it like any other entry (section 6 lists it as a
// presentation, and section 10.1 names only modal/sheet).
[[nodiscard]] constexpr bool presentation_is_dismissible(Presentation presentation) noexcept {
    return presentation == Presentation::Modal || presentation == Presentation::Sheet;
}

// Section 2: "Duplicate policy: push same route with same params -> focus/no-op or replace,
// explicit per route." Two values, both explicit; the default is the no-op, which cannot lose
// state.
enum class DuplicatePolicy : std::uint8_t { Ignore = 0, Replace };

[[nodiscard]] constexpr std::string_view duplicate_policy_name(DuplicatePolicy policy) noexcept {
    switch (policy) {
    case DuplicatePolicy::Ignore:
        return "ignore";
    case DuplicatePolicy::Replace:
        return "replace";
    }
    return "ignore";
}

// One declared route (section 1's JSON, plus the two runtime fields section 2/6 need).
struct RouteDef {
    std::string id;   // stable, canonical (section 1: an entry stores the id, never a path)
    std::string path; // pattern: `/orders/:id`
    std::vector<ParamSpec> params;
    Presentation presentation = Presentation::Page;
    bool auth = false; // section 4: a guard reads it; this slice only carries it
    DuplicatePolicy duplicates = DuplicatePolicy::Ignore;

    [[nodiscard]] bool operator==(const RouteDef&) const = default;
};

// An ordered (name, value) list, the same shape `appfw::Settings` uses: iteration order is the
// declaration order of the pattern, so a serialized param map is byte-stable and a test compares it
// directly instead of through a lookup that has no order.
using ParamMap = std::vector<std::pair<std::string, std::string>>;

// Per-route UI state (section 10.1 `NavEntry.state`, "serializable, opt-in restore"). The value
// format is the screen's; the navigation module only carries and round-trips it, bounded.
using StateMap = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] const std::string* find_param(const ParamMap& params, std::string_view name) noexcept;

// Section 3's `RouteMatch`. `params` are the typed path values, `query` is the raw untrusted string
// map (never merged into `params`: section 3's "document precedence" is that a typed path param and
// a query key are different things), `rest` is the `*` capture.
struct RouteMatch {
    std::size_t route = 0; // index into `RouteTable::routes()`
    std::string route_id;
    ParamMap params;
    ParamMap query;
    std::string matched_path; // the pattern that matched, for a diagnostic
    std::string rest;
};

struct RouteTableConfig {
    std::vector<RouteDef> routes;
    // Section 1's `notFound`: a route id that must exist. Section 3 sends an unknown path here.
    std::string not_found_route;
    // The root of the stack (section 2's back-at-root, section 5's cold deep link). Optional; when
    // empty, the route whose pattern is exactly `/` is used.
    std::string initial_route;
    // Bounds on untrusted input (section 10). A path with more segments or params than this is
    // `CapacityExceeded` rather than an unbounded walk.
    std::size_t max_path_segments = 64;
    std::size_t max_params = 32;
};

// Section 1's table: static routes, compiled patterns, params, guard metadata. It owns no screen,
// no state and no data (section 10.1's ownership table: "static routes, patterns, params, guards
// metadata" / "does not own screen state/data"). Immutable after `create()`.
class RouteTable {
public:
    // Section 1: "Route table is validated at build time; duplicate ids/paths are errors." Returns
    // the first violation in declaration order (ids, then paths, then param declarations), so a
    // generator and a test report the same row.
    [[nodiscard]] static std::expected<RouteTable, NavigationError> create(RouteTableConfig config);

    // The same validation without building the compiled patterns: what a build step calls.
    [[nodiscard]] static std::expected<void, NavigationError>
    validate_route_config(const RouteTableConfig& config);

    [[nodiscard]] const std::vector<RouteDef>& routes() const noexcept {
        return routes_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return routes_.size();
    }
    [[nodiscard]] const RouteDef* at(std::size_t index) const noexcept;
    // Linear in the table size, which is what a static route table is: a few dozen rows, and the
    // caller that wants a fast path holds the index from a `RouteMatch`.
    [[nodiscard]] const RouteDef* find(std::string_view route_id) const noexcept;
    [[nodiscard]] std::string_view not_found_route() const noexcept {
        return not_found_route_;
    }
    [[nodiscard]] std::string_view initial_route() const noexcept {
        return initial_route_;
    }

    // Sections 3 and 10. Decodes each raw segment once (percent-decoding, UTF-8, control
    // characters, traversal), matches, then validates the typed params. `NotFound` when nothing
    // matches; every other failure is a malformed request and is also reported as a named error,
    // never a throw.
    [[nodiscard]] std::expected<RouteMatch, NavigationError> match(std::string_view path) const;

    // Section 5: "Startup restore uses the saved stack only if the route table/app version still
    // matches." The fingerprint is FNV-1a over the canonical `id \x1f path` rows in table order, so
    // it changes when a route is renamed, added, removed or re-pathed, and not when a param type or
    // a presentation changes. Its hex form is what the snapshot stores.
    [[nodiscard]] std::uint64_t fingerprint_value() const noexcept {
        return fingerprint_;
    }
    [[nodiscard]] std::string fingerprint() const;

    [[nodiscard]] std::size_t max_params() const noexcept {
        return max_params_;
    }

private:
    struct PatternSegment {
        enum class Kind : std::uint8_t { Literal = 0, Param, Wildcard };
        Kind kind = Kind::Literal;
        std::string text; // the literal, or the param name (empty for an unnamed wildcard)
        bool optional = false;
    };
    struct CompiledRoute {
        std::vector<PatternSegment> segments;
        std::size_t params = 0;
        bool wildcard = false;
    };

    RouteTable() = default;

    std::vector<RouteDef> routes_;
    std::vector<CompiledRoute> compiled_;
    std::string not_found_route_;
    std::string initial_route_;
    std::size_t max_path_segments_ = 64;
    std::size_t max_params_ = 32;
    std::uint64_t fingerprint_ = 0;
};

// Section 3's "decode/normalize once; reject malformed UTF-8/percent-encoding", applied to one
// token. Exposed because it is the rule a deeplink and a query value both go through, and a test is
// clearer asserting it directly than through a match. `offset` in the error is relative to `text`.
[[nodiscard]] std::expected<std::string, NavigationError>
percent_decode(std::string_view text, bool reject_separators = true);

// Section 3's "typed params ... validated before screen mount", per value.
[[nodiscard]] bool validate_param_value(const ParamSpec& spec, std::string_view value) noexcept;

} // namespace tessera::appfw
