// SPDX-License-Identifier: MIT

#include "tessera/ui/style/container_query.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace tessera::ui::style {
namespace {

bool is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

bool parse_length(std::string_view text, double& out) {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const std::string owned(text);
    char* end = nullptr;
    const double number = std::strtod(owned.c_str(), &end);
    if (end == owned.c_str()) {
        return false;
    }
    const std::string unit = lower(std::string_view(end));
    if (unit == "px") {
        out = number;
    } else if (unit == "em" || unit == "rem") {
        out = number * 16.0;
    } else if (unit.empty() && number == 0.0) {
        out = 0.0;
    } else {
        return false;
    }
    return true;
}

bool parse_axis(std::string_view feature, ContainerAxis& axis) {
    if (feature == "width" || feature == "inline-size") {
        axis = ContainerAxis::Width;
        return true;
    }
    if (feature == "height" || feature == "block-size") {
        axis = ContainerAxis::Height;
        return true;
    }
    return false;
}

// One parenthesised condition body: `min-width: 400px` or `width >= 400px`.
bool parse_condition(std::string_view body, ContainerCondition& condition) {
    const std::size_t colon = body.find(':');
    if (colon != std::string_view::npos) {
        std::string feature = lower(trim(body.substr(0, colon)));
        ContainerOp op = ContainerOp::Equal;
        if (feature.rfind("min-", 0) == 0) {
            op = ContainerOp::GreaterEqual;
            feature.erase(0, 4);
        } else if (feature.rfind("max-", 0) == 0) {
            op = ContainerOp::LessEqual;
            feature.erase(0, 4);
        }
        condition.op = op;
        return parse_axis(feature, condition.axis) &&
               parse_length(body.substr(colon + 1), condition.value);
    }

    const std::size_t op_at = body.find_first_of("<>=");
    if (op_at == std::string_view::npos) {
        return false;
    }
    std::size_t op_end = op_at + 1;
    if (op_end < body.size() && body[op_end] == '=' && body[op_at] != '=') {
        ++op_end;
    }
    const std::string_view op_text = body.substr(op_at, op_end - op_at);
    if (op_text == ">=") {
        condition.op = ContainerOp::GreaterEqual;
    } else if (op_text == "<=") {
        condition.op = ContainerOp::LessEqual;
    } else if (op_text == ">") {
        condition.op = ContainerOp::Greater;
    } else if (op_text == "<") {
        condition.op = ContainerOp::Less;
    } else if (op_text == "=") {
        condition.op = ContainerOp::Equal;
    } else {
        return false;
    }
    return parse_axis(lower(trim(body.substr(0, op_at))), condition.axis) &&
           parse_length(body.substr(op_end), condition.value);
}

bool holds(const ContainerCondition& condition, const ContainerSize& size) {
    if (condition.axis == ContainerAxis::Height && !size.height_queryable) {
        return false;
    }
    const double actual = condition.axis == ContainerAxis::Width ? size.width : size.height;
    switch (condition.op) {
    case ContainerOp::Equal:
        return actual == condition.value;
    case ContainerOp::Greater:
        return actual > condition.value;
    case ContainerOp::GreaterEqual:
        return actual >= condition.value;
    case ContainerOp::Less:
        return actual < condition.value;
    case ContainerOp::LessEqual:
        return actual <= condition.value;
    }
    return false;
}

} // namespace

ContainerQueryParseResult parse_container_query(std::string_view prelude) {
    ContainerQueryParseResult result;
    const auto fail = [&](std::size_t offset) {
        result.ok = false;
        result.error_offset = offset;
        return result;
    };

    std::size_t pos = 0;
    const auto skip_space = [&] {
        while (pos < prelude.size() && is_space(prelude[pos])) {
            ++pos;
        }
    };
    skip_space();
    if (pos < prelude.size() && prelude[pos] != '(') {
        const std::size_t start = pos;
        while (pos < prelude.size() && !is_space(prelude[pos]) && prelude[pos] != '(') {
            ++pos;
        }
        result.query.name = std::string(prelude.substr(start, pos - start));
        const std::string keyword = lower(result.query.name);
        if (keyword == "not" || keyword == "style" || keyword == "scroll-state") {
            return fail(start);
        }
    }

    bool expect_condition = true;
    while (true) {
        skip_space();
        if (pos >= prelude.size()) {
            break;
        }
        if (!expect_condition) {
            const std::size_t start = pos;
            while (pos < prelude.size() && !is_space(prelude[pos]) && prelude[pos] != '(') {
                ++pos;
            }
            if (lower(prelude.substr(start, pos - start)) != "and") {
                return fail(start);
            }
            expect_condition = true;
            continue;
        }
        if (prelude[pos] != '(') {
            return fail(pos);
        }
        const std::size_t close = prelude.find(')', pos);
        if (close == std::string_view::npos) {
            return fail(pos);
        }
        ContainerCondition condition;
        if (!parse_condition(prelude.substr(pos + 1, close - pos - 1), condition)) {
            return fail(pos);
        }
        result.query.conditions.push_back(condition);
        pos = close + 1;
        expect_condition = false;
    }
    if (result.query.conditions.empty() || expect_condition) {
        return fail(pos);
    }
    return result;
}

bool evaluate(const ContainerQuery& query, const ContainerSize& size) {
    return std::all_of(query.conditions.begin(), query.conditions.end(),
                       [&](const ContainerCondition& c) { return holds(c, size); });
}

} // namespace tessera::ui::style
