// SPDX-License-Identifier: MIT
//
// Path matching, query parsing and typed param validation (docs/specs/navigation-v1.md sections 3
// and 10; issue #196). Split from `navigation_route.cpp` so the table compiler and the matcher each
// stay under the soft file limit and can be read independently.

#include "navigation_internal.hpp"
#include "tessera/appfw/navigation_route.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {
namespace {

constexpr std::size_t kMaxPathBytes = 4096;

// One route's attempt at one decoded path. `params` and `rest` are only meaningful when `matched`,
// so a losing attempt never has to be rebuilt after the winner is known.
struct Attempt {
    bool matched = false;
    ParamMap params;
    std::string rest;
    std::size_t literals = 0;
    std::size_t wildcards = 0;
};

// Section 1 makes duplicate *paths* an error, but `/orders/new` and `/orders/:id` still both match
// the text `/orders/new`. The tie-break lives in one place so a test asserts the rule rather than
// the table order it happens to produce: most literal segments first (a literal is a stronger claim
// than a param), then fewest wildcards, then fewest params, then earliest row.
bool better(const Attempt& left, std::size_t left_route, const Attempt& right,
            std::size_t right_route) noexcept {
    if (left.literals != right.literals) {
        return left.literals > right.literals;
    }
    if (left.wildcards != right.wildcards) {
        return left.wildcards < right.wildcards;
    }
    if (left.params.size() != right.params.size()) {
        return left.params.size() < right.params.size();
    }
    return left_route < right_route;
}

// Splits a raw path on `/` without decoding, then decodes each segment exactly once (section 3:
// "decode/normalize once").
std::expected<std::vector<std::string>, NavigationError> decode_segments(std::string_view path,
                                                                         std::size_t max_segments) {
    if (path.empty() || path.front() != '/') {
        return std::unexpected(
            NavigationError{NavigationErrorCode::InvalidPath, std::string(path), 0});
    }
    std::vector<std::string> segments;
    std::size_t start = 1;
    while (start <= path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string_view raw = path.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!raw.empty()) {
            if (segments.size() >= max_segments) {
                return std::unexpected(NavigationError{NavigationErrorCode::CapacityExceeded,
                                                       std::string(path), start});
            }
            auto decoded = percent_decode(raw);
            if (!decoded.has_value()) {
                NavigationError error = decoded.error();
                error.offset += start; // relative to the whole path, not the segment
                return std::unexpected(error);
            }
            // Section 10: "reject ... traversal". `.` and `..` as a whole segment are the two forms
            // that can address something other than a child; a dot inside a longer name is ordinary
            // text and is preserved.
            if (*decoded == "." || *decoded == "..") {
                return std::unexpected(
                    NavigationError{NavigationErrorCode::InvalidPath, *decoded, start});
            }
            segments.push_back(std::move(*decoded));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return segments;
}

// The `?a=1&b=2` half of a path. A query value is decoded with `reject_separators = false` because
// it is not a path segment: a `/` in `?next=/orders` is data, not a re-segmentation risk.
ParamMap parse_query(std::string_view query, std::size_t max_params) {
    ParamMap params;
    std::size_t start = 0;
    while (start <= query.size() && params.size() < max_params) {
        const std::size_t end = query.find('&', start);
        const std::string_view pair = query.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!pair.empty()) {
            const std::size_t equals = pair.find('=');
            const std::string_view raw_key = pair.substr(0, equals);
            const std::string_view raw_value =
                equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
            // A malformed query value is dropped rather than failing the whole navigation: section
            // 3 makes typed *params* the validated input, and says a query key is "not a substitute
            // for typed params". Dropping keeps the screen reachable; the typed param is still
            // validated.
            const auto key = percent_decode(raw_key, false);
            const auto value = percent_decode(raw_value, false);
            if (key.has_value() && value.has_value() && !key->empty()) {
                params.emplace_back(*key, *value);
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return params;
}

} // namespace

std::expected<RouteMatch, NavigationError> RouteTable::match(std::string_view path) const {
    // 4096 is `deep-linking-v1.md` section 3's `maxUrlLength` default: a path longer than that is
    // refused before any walk, so the bound is a property of the module rather than of each caller.
    if (path.size() > kMaxPathBytes) {
        return std::unexpected(
            NavigationError{NavigationErrorCode::PathTooLong, {}, kMaxPathBytes});
    }
    const std::size_t query_at = path.find('?');
    const std::string_view path_part =
        query_at == std::string_view::npos ? path : path.substr(0, query_at);
    const std::string_view query_part =
        query_at == std::string_view::npos ? std::string_view{} : path.substr(query_at + 1);

    auto decoded = decode_segments(path_part, max_path_segments_);
    if (!decoded.has_value()) {
        return std::unexpected(decoded.error());
    }
    const std::vector<std::string>& segments = *decoded;

    Attempt best;
    std::size_t best_route = 0;
    bool found = false;

    for (std::size_t route_index = 0; route_index < compiled_.size(); ++route_index) {
        const CompiledRoute& compiled = compiled_[route_index];
        // A pattern without a wildcard matches when the decoded segment count is the pattern's
        // count or, when the pattern ends in an optional param, that count minus one. `:name?` is
        // only legal as the final segment (enforced by pattern compilation), so the shape is one
        // number.
        const std::size_t optional =
            !compiled.segments.empty() &&
                    compiled.segments.back().kind == PatternSegment::Kind::Param &&
                    compiled.segments.back().optional
                ? 1U
                : 0U;
        const std::size_t full = compiled.segments.size();
        // A wildcard lowers the minimum by one because it captures *nothing*: `/files` matches
        // `/files/*`, which is what makes the wildcard form usable for a collection index. It has
        // no maximum.
        const std::size_t shortest = compiled.wildcard ? full - 1 : full - optional;
        if (segments.size() < shortest || (!compiled.wildcard && segments.size() > full)) {
            continue;
        }

        Attempt attempt;
        bool matched = true;
        for (std::size_t index = 0; index < compiled.segments.size(); ++index) {
            const PatternSegment& segment = compiled.segments[index];
            if (segment.kind == PatternSegment::Kind::Wildcard) {
                // The wildcard captures the remaining raw path joined with `/`. Its consumer is the
                // route's own loader, which owns the deeper parse; the segments it covers were
                // already decoded and validated above.
                std::string joined;
                for (std::size_t rest_index = index; rest_index < segments.size(); ++rest_index) {
                    if (!joined.empty()) {
                        joined.push_back('/');
                    }
                    joined += segments[rest_index];
                }
                attempt.rest = std::move(joined);
                attempt.wildcards = 1;
                break;
            }
            if (index >= segments.size()) {
                // Only an optional trailing param may be absent, and it is last by grammar.
                matched = index + 1 == compiled.segments.size() &&
                          segment.kind == PatternSegment::Kind::Param && segment.optional;
                break;
            }
            if (segment.kind == PatternSegment::Kind::Literal) {
                if (segment.text != segments[index]) {
                    matched = false;
                    break;
                }
                ++attempt.literals;
                continue;
            }
            attempt.params.emplace_back(segment.text, segments[index]);
        }
        if (!matched) {
            continue;
        }
        if (!found || better(attempt, route_index, best, best_route)) {
            best = std::move(attempt);
            best_route = route_index;
        }
        found = true;
    }

    if (!found) {
        // Section 3: "Unknown route -> not-found screen with back/home actions." The controller
        // maps this code to the table's `notFound` route; the matcher reports the failure rather
        // than inventing a route, because which route is "not found" is table data.
        return std::unexpected(
            NavigationError{NavigationErrorCode::NotFound, std::string(path), 0});
    }

    const RouteDef& route = routes_[best_route];
    const CompiledRoute& compiled = compiled_[best_route];
    if (best.params.size() > max_params_) {
        return std::unexpected(NavigationError{NavigationErrorCode::CapacityExceeded, route.id, 0});
    }

    // Section 3: "typed params validated before screen mount". An undeclared param is a `String`
    // and always passes; a declared one must parse, and a failure names the param so a guard or a
    // diagnostic can say which one.
    for (const ParamSpec& spec : route.params) {
        const std::string* value = find_param(best.params, spec.name);
        if (value == nullptr) {
            const bool optional = !compiled.segments.empty() &&
                                  compiled.segments.back().kind == PatternSegment::Kind::Param &&
                                  compiled.segments.back().optional &&
                                  compiled.segments.back().text == spec.name;
            if (!optional) {
                return std::unexpected(
                    NavigationError{NavigationErrorCode::ParamMissing, spec.name, 0});
            }
            continue;
        }
        if (!validate_param_value(spec, *value)) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::ParamInvalid, spec.name, 0});
        }
    }

    RouteMatch result;
    result.route = best_route;
    result.route_id = route.id;
    result.params = std::move(best.params);
    result.query = parse_query(query_part, max_params_);
    result.matched_path = route.path;
    result.rest = std::move(best.rest);
    return result;
}

} // namespace tessera::appfw
