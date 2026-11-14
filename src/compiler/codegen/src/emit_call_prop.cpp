// SPDX-License-Identifier: MIT

#include "codegen_internal.hpp"
#include "emit_props.hpp"

#include <span>
#include <string>
#include <string_view>

namespace tessera::compiler::codegen::internal {

// A call of the component's own callback prop (issue #286). The prop is a `std::function` member;
// an unset one is skipped, so a component used without the callback never calls through an empty
// function. A payload callback receives the handler's whole payload parameter, which the lowering
// checked has the callback's record type.
bool emit_call_prop(const ir::Action& action, const ir::Component& component,
                    std::span<const ir::Param> params, std::string& body, std::string& error) {
    constexpr std::string_view kPropPrefix = "prop::";
    const std::string name = action.target.rfind(kPropPrefix, 0) == 0
                                 ? action.target.substr(kPropPrefix.size())
                                 : std::string();
    const ir::Prop* prop = nullptr;
    for (const ir::Prop& candidate : component.props) {
        if (candidate.name == name) {
            prop = &candidate;
        }
    }
    if (prop == nullptr || callback_prop_type(prop->type).empty()) {
        error = "`" + action.target + "` is not a callback prop of `" + component.name + "`";
        return false;
    }
    const std::string member = member_name(action.target);
    std::string argument;
    if (prop->type != "callback") {
        const ir::Expr* value = action.value.get();
        bool is_param = value != nullptr && value->kind == ir::ExprKind::Ref;
        if (is_param) {
            is_param = false;
            for (const ir::Param& param : params) {
                is_param = is_param || (param.name == value->id && param.type == prop->type);
            }
        }
        if (!is_param) {
            error = "callback prop `" + name + "` takes the `" + prop->type +
                    "` payload of the handler";
            return false;
        }
        argument = value->id;
    } else if (action.value) {
        error = "callback prop `" + name + "` takes no argument";
        return false;
    }
    body += "if (" + member + ") {\n" + member + "(" + argument + ");\n}\n";
    return true;
}

} // namespace tessera::compiler::codegen::internal
