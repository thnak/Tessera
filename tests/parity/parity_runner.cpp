// SPDX-License-Identifier: MIT

#include "parity_runner.hpp"
#include "tessera/compiler/eval/evaluator.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>
#include <variant>

namespace tessera::parity {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// One argument token starting at `pos`; advances `pos` past it.
std::expected<eval::Value, std::string> parse_argument(std::string_view line, std::size_t& pos) {
    if (line[pos] == '"') {
        std::string text;
        ++pos;
        while (pos < line.size() && line[pos] != '"') {
            if (line[pos] == '\\' && pos + 1 < line.size()) {
                ++pos;
            }
            text.push_back(line[pos++]);
        }
        if (pos >= line.size()) {
            return std::unexpected("unterminated string");
        }
        ++pos;
        return eval::Value::of_string(std::move(text));
    }
    const std::size_t begin = pos;
    while (pos < line.size() && !is_space(line[pos])) {
        ++pos;
    }
    const std::string token(line.substr(begin, pos - begin));
    if (token == "true" || token == "false") {
        return eval::Value::of_bool(token == "true");
    }
    if (token.find_first_of(".eE") == std::string::npos) {
        std::int64_t whole = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), whole);
        if (error != std::errc{} || end != token.data() + token.size() ||
            whole < std::numeric_limits<std::int32_t>::min() ||
            whole > std::numeric_limits<std::int32_t>::max()) {
            return std::unexpected("`" + token + "` is not an int, float, bool or string");
        }
        return eval::Value::of_int(static_cast<std::int32_t>(whole));
    }
    char* end = nullptr;
    errno = 0;
    const double number = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || errno != 0) {
        return std::unexpected("`" + token + "` is not an int, float, bool or string");
    }
    return eval::Value::of_float(number);
}

eval::Value to_value(const ui::StateScalar& scalar) {
    if (const bool* flag = std::get_if<bool>(&scalar)) {
        return eval::Value::of_bool(*flag);
    }
    if (const int* whole = std::get_if<int>(&scalar)) {
        return eval::Value::of_int(*whole);
    }
    if (const double* number = std::get_if<double>(&scalar)) {
        return eval::Value::of_float(*number);
    }
    return eval::Value::of_string(std::get<std::string>(scalar));
}

eval::Value to_value(const ui::StateCell& cell) {
    if (!cell.is_list) {
        return to_value(cell.value);
    }
    eval::List items;
    items.reserve(cell.items.size());
    for (const ui::StateScalar& item : cell.items) {
        items.push_back(to_value(item));
    }
    return eval::Value::of_list(std::move(items));
}

bool is_float_type(const std::string& type) {
    return type == "float" || type == "length";
}

// A state of a float type holds a double even when its initial is written as an integer.
eval::Value coerce(const std::string& type, eval::Value value) {
    if (is_float_type(type) && value.as_int() != nullptr) {
        return eval::Value::of_float(static_cast<double>(*value.as_int()));
    }
    return value;
}

std::expected<void, std::string> bind_arguments(const std::vector<ir::Param>& params,
                                                const Step& step, eval::Scope& scope) {
    if (params.size() != step.args.size()) {
        return std::unexpected(step.target + " takes " + std::to_string(params.size()) +
                               " argument(s), the script gives " +
                               std::to_string(step.args.size()));
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const ir::Param& param = params[i];
        const eval::Value& arg = step.args[i];
        const bool fits =
            (param.type == "int" && arg.as_int() != nullptr) ||
            (is_float_type(param.type) && arg.is_number()) ||
            (param.type == "bool" && arg.as_bool() != nullptr) ||
            ((param.type == "string" || param.type == "color") && arg.as_string() != nullptr);
        if (!fits) {
            return std::unexpected("argument " + std::to_string(i + 1) + " of " + step.target +
                                   " is " + std::string(eval::type_name(arg)) +
                                   ", the parameter `" + param.name + "` is " + param.type);
        }
        scope[param.name] = coerce(param.type, arg);
    }
    return {};
}

struct PreviewBody {
    const std::vector<ir::Param>* params = nullptr;
    const std::vector<ir::Action>* actions = nullptr;
};

std::optional<PreviewBody> find_body(const ir::IrModule& module, const ir::Component& component,
                                     const std::string& target) {
    for (const ir::Handler& handler : component.handlers) {
        if (handler.id == target) {
            return PreviewBody{&handler.params, &handler.actions};
        }
    }
    static const std::vector<ir::Action> kNoActions;
    for (const ir::Binding& binding : module.bindings) {
        if (binding.id != target ||
            (binding.component != component.name && binding.component != component.id)) {
            continue;
        }
        // A native handler without a preview body does nothing in the designer.
        return PreviewBody{&binding.params,
                           binding.preview ? &binding.preview->actions : &kNoActions};
    }
    return std::nullopt;
}

// Every native cell must exist on the IR side and hold the identical value.
void compare(const ui::StateSnapshot& native, const eval::Scope& scope, int step,
             const std::string& target, std::vector<Mismatch>& out) {
    for (const ui::StateCell& cell : native) {
        const auto it = scope.find(cell.name);
        const eval::Value actual = to_value(cell);
        if (it == scope.end()) {
            out.push_back(
                {step, target, cell.name, "(no such cell in the IR)", eval::describe(actual)});
        } else if (!eval::identical(it->second, actual)) {
            out.push_back(
                {step, target, cell.name, eval::describe(it->second), eval::describe(actual)});
        }
    }
}

} // namespace

std::expected<std::vector<Step>, std::string> parse_script(std::string_view text) {
    std::vector<Step> steps;
    std::size_t line_number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++line_number;
        std::size_t pos = 0;
        while (pos < line.size() && is_space(line[pos])) {
            ++pos;
        }
        if (pos >= line.size() || line[pos] == '#') {
            continue;
        }
        const auto fail = [&](const std::string& why) {
            return std::unexpected("script line " + std::to_string(line_number) + ": " + why);
        };
        const auto word = [&]() {
            const std::size_t begin = pos;
            while (pos < line.size() && !is_space(line[pos])) {
                ++pos;
            }
            const std::string_view token = line.substr(begin, pos - begin);
            while (pos < line.size() && is_space(line[pos])) {
                ++pos;
            }
            return token;
        };
        if (word() != "call") {
            return fail("a step starts with `call`");
        }
        Step step;
        step.target = std::string(word());
        if (step.target.empty()) {
            return fail("`call` needs a handler or binding id");
        }
        while (pos < line.size()) {
            auto arg = parse_argument(line, pos);
            if (!arg) {
                return fail(arg.error());
            }
            step.args.push_back(std::move(*arg));
            while (pos < line.size() && is_space(line[pos])) {
                ++pos;
            }
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

std::expected<eval::Scope, std::string> initial_scope(const ir::Component& component) {
    eval::Evaluator evaluator;
    const eval::Scope empty;
    eval::Scope scope;
    const auto add = [&](const std::string& id, const std::string& type,
                         const ir::ExprPtr& initial) -> std::expected<void, std::string> {
        if (!initial) {
            scope[id] = eval::Value::null();
            return {};
        }
        auto value = evaluator.evaluate(*initial, empty);
        if (!value) {
            return std::unexpected(id + ": " + value.error().code + ": " + value.error().message);
        }
        scope[id] = coerce(type, std::move(*value));
        return {};
    };
    for (const ir::State& state : component.state) {
        if (auto added = add(state.id, state.type, state.initial); !added) {
            return std::unexpected(added.error());
        }
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (auto added = add(value.id, value.type, value.initial); !added) {
            return std::unexpected(added.error());
        }
    }
    return scope;
}

Report run_parity(const ir::IrModule& module, const ir::Component& component,
                  const std::vector<Step>& script, const NativeSide& native) {
    Report report;
    auto scope = initial_scope(component);
    if (!scope) {
        report.error = "initial state: " + scope.error();
        return report;
    }
    eval::Evaluator evaluator;

    // The native side starts from its own initial state; compare it before any step.
    auto start = native(nullptr);
    if (!start) {
        report.error = "native side: " + start.error();
        return report;
    }
    compare(*start, *scope, -1, "(initial state)", report.mismatches);
    if (!report.mismatches.empty()) {
        return report;
    }

    for (std::size_t index = 0; index < script.size(); ++index) {
        const Step& step = script[index];
        const std::string where = "step " + std::to_string(index) + " (" + step.target + "): ";
        const std::optional<PreviewBody> body = find_body(module, component, step.target);
        if (!body) {
            report.error =
                where + "no handler or native binding with that id in `" + component.name + "`";
            return report;
        }
        eval::Scope with_params = *scope;
        if (auto bound = bind_arguments(*body->params, step, with_params); !bound) {
            report.error = where + bound.error();
            return report;
        }
        if (auto ran = evaluator.run(*body->actions, with_params); !ran) {
            report.error = where + ran.error().code + ": " + ran.error().message;
            return report;
        }
        for (auto& [name, value] : *scope) {
            value = std::move(with_params[name]);
        }

        auto after = native(&step);
        if (!after) {
            report.error = where + "native side: " + after.error();
            return report;
        }
        ++report.steps_run;
        compare(*after, *scope, static_cast<int>(index), step.target, report.mismatches);
        if (!report.mismatches.empty()) {
            return report;
        }
    }
    return report;
}

std::string format_report(const Report& report) {
    std::ostringstream out;
    if (!report.error.empty()) {
        out << "parity run failed: " << report.error << "\n";
    }
    for (const Mismatch& mismatch : report.mismatches) {
        if (mismatch.step < 0) {
            out << "before any step";
        } else {
            out << "step " << mismatch.step << " (" << mismatch.target << ")";
        }
        out << ": " << mismatch.cell << " is " << mismatch.preview << " in the preview and "
            << mismatch.native << " in the native code\n";
    }
    return out.str();
}

} // namespace tessera::parity
