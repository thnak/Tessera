// SPDX-License-Identifier: MIT
//
// Route patterns and the route table (docs/specs/navigation-v1.md sections 1 and 3; issue #196).
// The matcher lives in `navigation_match.cpp` and the token codec in `navigation_codec.cpp`, so
// each of the three reads on its own.

#include "navigation_internal.hpp"
#include "tessera/appfw/navigation_route.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {
namespace {

constexpr char kFingerprintSeparator = '\x1f';

using detail::split_path;

std::expected<void, NavigationError> compile_pattern(std::string_view pattern) {
    const NavigationError invalid{NavigationErrorCode::InvalidPattern, std::string(pattern), 0};
    if (pattern.empty() || pattern.front() != '/') {
        return std::unexpected(invalid);
    }
    const std::vector<std::string> segments = split_path(pattern);
    bool saw_wildcard = false;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const std::string_view segment = segments[index];
        // `*` is a whole trailing segment (section 1's "`*` wildcard"), never a substring: a route
        // author who writes `/files/*.txt` gets a pattern error instead of a matcher that silently
        // treats `*` as a literal.
        if (segment == "*") {
            if (index + 1 != segments.size()) {
                return std::unexpected(invalid);
            }
            saw_wildcard = true;
            continue;
        }
        if (saw_wildcard) {
            return std::unexpected(invalid);
        }
        if (segment.front() == ':') {
            std::string_view name = segment.substr(1);
            bool optional = false;
            if (!name.empty() && name.back() == '?') {
                optional = true;
                name.remove_suffix(1);
            }
            if (name.empty()) {
                return std::unexpected(invalid);
            }
            for (const char character : name) {
                const bool legal = (character >= 'a' && character <= 'z') ||
                                   (character >= 'A' && character <= 'Z') ||
                                   (character >= '0' && character <= '9') || character == '_';
                if (!legal) {
                    return std::unexpected(invalid);
                }
            }
            // `?` is the optional-segment marker and section 1 lists it as a pattern feature, not
            // as a mid-path hole: an optional segment in the middle makes two different segment
            // counts match the same route, which the matcher deliberately does not model. Only the
            // final segment may be optional.
            if (optional && index + 1 != segments.size()) {
                return std::unexpected(invalid);
            }
            continue;
        }
        if (segment.find('*') != std::string_view::npos ||
            segment.find(':') != std::string_view::npos ||
            segment.find('?') != std::string_view::npos) {
            return std::unexpected(invalid);
        }
    }
    return {};
}

// The `RouteDef::params` declaration must name a param its own pattern captures. A `ParamSpec` for
// a param that is not in the pattern can never be satisfied, so it is a table error rather than a
// validation that silently never runs.
std::expected<void, NavigationError> validate_param_declarations(const RouteDef& route) {
    for (const ParamSpec& spec : route.params) {
        if (spec.name.empty()) {
            return std::unexpected(NavigationError{NavigationErrorCode::UnknownParam, route.id, 0});
        }
        if (spec.type == ParamType::Enum && spec.allowed.empty()) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::EmptyEnumSpec, spec.name, 0});
        }
        const std::string token = ":" + spec.name;
        bool declared_in_pattern = false;
        const std::vector<std::string> segments = split_path(route.path);
        for (const std::string& segment : segments) {
            if (segment == token || segment == token + "?") {
                declared_in_pattern = true;
                break;
            }
        }
        if (!declared_in_pattern) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::UnknownParam, spec.name, 0});
        }
    }
    return {};
}

} // namespace

std::expected<void, NavigationError>
RouteTable::validate_route_config(const RouteTableConfig& config) {
    if (config.routes.empty()) {
        return std::unexpected(NavigationError{NavigationErrorCode::EmptyRouteTable, {}, 0});
    }
    if (config.max_path_segments == 0 || config.max_params == 0) {
        return std::unexpected(NavigationError{NavigationErrorCode::CapacityExceeded, {}, 0});
    }
    for (std::size_t index = 0; index < config.routes.size(); ++index) {
        const RouteDef& route = config.routes[index];
        if (route.id.empty()) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::InvalidPattern, route.path, 0});
        }
        if (auto compiled = compile_pattern(route.path); !compiled.has_value()) {
            return std::unexpected(compiled.error());
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const RouteDef& other = config.routes[earlier];
            if (other.id == route.id) {
                return std::unexpected(
                    NavigationError{NavigationErrorCode::DuplicateRouteId, route.id, 0});
            }
            if (other.path == route.path) {
                return std::unexpected(
                    NavigationError{NavigationErrorCode::DuplicateRoutePath, route.path, 0});
            }
        }
        if (auto declared = validate_param_declarations(route); !declared.has_value()) {
            return std::unexpected(declared.error());
        }
    }
    if (!config.not_found_route.empty()) {
        const bool exists =
            std::any_of(config.routes.begin(), config.routes.end(),
                        [&](const RouteDef& route) { return route.id == config.not_found_route; });
        if (!exists) {
            return std::unexpected(NavigationError{NavigationErrorCode::UnknownNotFoundRoute,
                                                   config.not_found_route, 0});
        }
    }
    if (!config.initial_route.empty()) {
        const bool exists =
            std::any_of(config.routes.begin(), config.routes.end(),
                        [&](const RouteDef& route) { return route.id == config.initial_route; });
        if (!exists) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::UnknownInitialRoute, config.initial_route, 0});
        }
    }
    return {};
}

std::expected<RouteTable, NavigationError> RouteTable::create(RouteTableConfig config) {
    if (auto valid = validate_route_config(config); !valid.has_value()) {
        return std::unexpected(valid.error());
    }
    RouteTable table;
    table.routes_ = std::move(config.routes);
    table.not_found_route_ = std::move(config.not_found_route);
    table.initial_route_ = std::move(config.initial_route);
    table.max_path_segments_ = config.max_path_segments;
    table.max_params_ = config.max_params;
    table.compiled_.reserve(table.routes_.size());

    for (const RouteDef& route : table.routes_) {
        CompiledRoute compiled;
        for (const std::string& segment : split_path(route.path)) {
            PatternSegment part;
            if (segment == "*") {
                part.kind = PatternSegment::Kind::Wildcard;
                compiled.wildcard = true;
            } else if (segment.front() == ':') {
                part.kind = PatternSegment::Kind::Param;
                std::string_view name = std::string_view(segment).substr(1);
                if (!name.empty() && name.back() == '?') {
                    part.optional = true;
                    name.remove_suffix(1);
                }
                part.text = std::string(name);
                ++compiled.params;
            } else {
                part.kind = PatternSegment::Kind::Literal;
                part.text = segment;
            }
            compiled.segments.push_back(std::move(part));
        }
        table.compiled_.push_back(std::move(compiled));
    }

    // Section 5's table fingerprint: the canonical `id`, `path` rows in order. FNV-1a over the same
    // byte stream on every platform, so a snapshot written on one build is comparable on another.
    std::uint64_t fingerprint = 14695981039346656037ULL;
    for (const RouteDef& route : table.routes_) {
        for (const std::string* text : {&route.id, &route.path}) {
            for (const char character : *text) {
                fingerprint ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
                fingerprint *= 1099511628211ULL;
            }
            fingerprint ^=
                static_cast<std::uint64_t>(static_cast<unsigned char>(kFingerprintSeparator));
            fingerprint *= 1099511628211ULL;
        }
    }
    table.fingerprint_ = fingerprint;
    return table;
}

std::string RouteTable::fingerprint() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(16);
    for (int shift = 60; shift >= 0; shift -= 4) {
        text.push_back(kHex[(fingerprint_ >> static_cast<unsigned>(shift)) & 0xFU]);
    }
    return text;
}

const RouteDef* RouteTable::at(std::size_t index) const noexcept {
    return index < routes_.size() ? &routes_[index] : nullptr;
}

const RouteDef* RouteTable::find(std::string_view route_id) const noexcept {
    for (const RouteDef& route : routes_) {
        if (route.id == route_id) {
            return &route;
        }
    }
    return nullptr;
}

} // namespace tessera::appfw