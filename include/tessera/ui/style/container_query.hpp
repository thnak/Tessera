// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

struct StyleNode;

// `@container` conditions (ADR-041 slice 8, docs/design/layout-observation.md section 5.4).
// Supported: size features `width`, `height`, `inline-size`, `block-size` (horizontal writing mode,
// so inline = width) in the legacy `(min-width: 400px)` form and the range form
// `(width >= 400px)`, joined with `and`. Lengths are `px`, `em`/`rem` (16px) or a bare 0.
// `or`, `not` and style/scroll-state queries are rejected as an invalid at-rule.
enum class ContainerAxis {
    Width,
    Height,
};

enum class ContainerOp {
    Equal,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
};

struct ContainerCondition {
    ContainerAxis axis = ContainerAxis::Width;
    ContainerOp op = ContainerOp::GreaterEqual;
    double value = 0.0;
};

struct ContainerQuery {
    std::string name; // empty: the nearest container of any name
    std::vector<ContainerCondition> conditions;
};

struct ContainerQueryParseResult {
    ContainerQuery query;
    bool ok = true;
    std::size_t error_offset = 0;
};

ContainerQueryParseResult parse_container_query(std::string_view prelude);

// Content-box size of a query container in logical pixels. A container with
// `container-type: inline-size` contains only the inline axis, so height conditions are false.
struct ContainerSize {
    double width = 0.0;
    double height = 0.0;
    bool height_queryable = true;
};

bool evaluate(const ContainerQuery& query, const ContainerSize& size);

// Supplies the size of the nearest ancestor query container of `node` (never `node` itself) whose
// `container-name` matches `name` (any container when `name` is empty). Returns false when there
// is none; the rule then does not apply.
class ContainerSizeSource {
public:
    virtual ~ContainerSizeSource() = default;
    virtual bool find(const StyleNode& node, std::string_view name, ContainerSize& out) const = 0;
};

} // namespace tessera::ui::style
