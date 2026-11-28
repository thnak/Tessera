// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/front_end_options.hpp"
#include "tessera/compiler/jsx/lexer.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace tessera::compiler::jsx {

// True for a custom hook's name: `use` plus an uppercase letter (docs/specs/custom-hooks-v1.md
// section 2). `useState` and friends are core hooks, not declarations, so they are excluded by the
// uppercase rule only in the sense that a core hook is never declared in a project file.
[[nodiscard]] bool is_hook_declaration_name(std::string_view name);

// Recursive-descent parser over the parser-driven Lexer. It holds one token of lookahead (`cur_`)
// that was lexed in the mode the previous advance() asked for, so every consumption site states
// the mode of the token that follows (see LexMode).
//
// Error model: fail() reports an error and sets `fatal_`; every loop and every parse function
// checks it and unwinds, so the parser stops at the first structural error. report() records a
// subset violation and parsing continues.
class Parser {
public:
    Parser(std::string_view source, const Limits& limits, const FrontEndOptions& options = {});

    JsxPtr parse_jsx_root();
    ExprPtr parse_expression_root();
    std::unique_ptr<Module> parse_module_root();
    TypePtr parse_type_root();

    template<typename Node> ParseResult<Node> finish(std::unique_ptr<Node> root) {
        ParseResult<Node> result;
        result.root = std::move(root);
        result.comments = lexer_.take_comments();
        result.diagnostics_truncated = sink_.truncated();
        result.diagnostics = sink_.take();
        return result;
    }

private:
    struct Checkpoint {
        Lexer::Mark lexer;
        DiagnosticSink::Mark sink;
        Token cur;
        std::uint32_t prev_end = 0;
        std::size_t brace_depth = 0;
        // The component-body context. A trial parse (an arrow component, a parenthesized arrow)
        // enters a component body and must leave the flags as it found them when it rewinds.
        bool in_component = false;
        std::string props_name;
    };

    // Counts one level of parser recursion; entered() is false (and the parse failed with
    // TESSERA_JSX_LIMIT_DEPTH) when the limit was hit.
    class DepthScope {
    public:
        explicit DepthScope(Parser& parser)
            : parser_(parser),
              entered_(parser.enter()) {}
        ~DepthScope() {
            if (entered_) {
                parser_.leave();
            }
        }
        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;
        [[nodiscard]] bool entered() const {
            return entered_;
        }

    private:
        Parser& parser_;
        bool entered_;
    };

    // parser.cpp
    void advance(LexMode mode);
    void advance_template();
    [[nodiscard]] Checkpoint checkpoint() const;
    void restore(Checkpoint saved);
    [[nodiscard]] bool at_punct(std::string_view punctuator) const {
        return cur_.is_punct(punctuator);
    }
    [[nodiscard]] Span span_from(std::uint32_t begin) const {
        return Span{begin, prev_end_ - begin};
    }
    void fail(std::string_view code, std::string message, Span span, std::string suggestion = {});
    void fail_unexpected(std::string_view expected);
    void report(std::string_view code, std::string message, Span span, std::string suggestion = {});
    bool expect_punct(std::string_view punctuator, LexMode after);
    [[nodiscard]] bool enter();
    void leave() {
        --depth_;
    }
    [[nodiscard]] bool finish_node(Expr& expr);
    [[nodiscard]] bool finish_node(JsxNode& node);
    [[nodiscard]] bool finish_node(TypeNode& type);
    bool expect_semicolon();
    [[nodiscard]] std::string describe_current() const;

    // parser_expr.cpp
    ExprPtr parse_expr();
    ExprPtr parse_conditional();
    ExprPtr parse_binary(int min_precedence);
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr make_expr(ExprKind kind, std::uint32_t begin);
    ExprPtr make_operator(ExprKind kind, std::uint32_t begin, std::string op,
                          std::vector<ExprPtr> operands);

    // parser_primary.cpp
    ExprPtr parse_primary();
    ExprPtr parse_parenthesized_or_arrow();
    struct ArrowSignature {
        std::vector<std::string> params;
        std::vector<TypePtr> types;
        TypePtr return_type;
    };
    ExprPtr parse_arrow(std::uint32_t begin, ArrowSignature signature);
    // ADR-066 item 2: the statements a handler block body may hold.
    bool parse_statement_list(std::vector<ExprPtr>& statements);
    ExprPtr parse_block_statement();
    ExprPtr parse_if_statement();
    ExprPtr parse_branch_block();
    ExprPtr parse_const_in_block();
    // ADR-066 item 4: the reducer form.
    ExprPtr parse_switch_statement();
    ExprPtr parse_case_body();
    // Whether the helper body about to be parsed declares a `switch` (a reducer).
    bool helper_has_switch();
    // ADR-066 item 4: the body of a reducer helper, recorded on the item.
    bool parse_reducer_body(ModuleItem& item);
    ExprPtr parse_template();
    ExprPtr parse_array();
    ExprPtr parse_object();
    ExprPtr parse_rejected_keyword();
    ExprPtr parse_return_statement();
    void check_call_rules(const Expr& call);
    void check_attribute_rules(const JsxAttribute& attribute);
    bool parse_call_type_args(std::vector<TypePtr>& type_args);

    // parser_type.cpp
    TypePtr parse_type_expr();
    TypePtr parse_type_intersection();
    TypePtr parse_type_primary();
    TypePtr parse_typeof_values(std::uint32_t begin);
    TypePtr parse_type_object(std::uint32_t begin);
    TypePtr parse_type_function(std::uint32_t begin);
    bool expect_type_close();

    // parser_module.cpp
    bool parse_item(ModuleItem& item);
    bool parse_import(ModuleItem& item);
    bool parse_reexport(ModuleItem& item);
    bool parse_type_alias(ModuleItem& item);
    bool parse_interface(ModuleItem& item);
    bool parse_enum(ModuleItem& item);
    // After `const X = { ... }`: a trailing `as const`, and the literal members of the object.
    bool finish_const_object(ModuleItem& item);
    // `Color.Red` over a known namespace: the literal, or null (and a diagnostic) when the name has
    // no such member. Returns the input untouched when `object` names no namespace.
    ExprPtr resolve_namespace_member(ExprPtr object, std::string name, std::uint32_t begin);
    // parser_type_expand.cpp: Partial/Required/Pick/Omit and intersections of records, expanded
    // once the whole module is parsed so an alias may be declared after its use.
    void expand_type_forms(Module& module);
    bool parse_const_item(ModuleItem& item);
    // ADR-066 item 1: `const Name = (props: Props) => { ... }`, tried from parse_const_item and
    // rewound when the text is an ordinary constant instead.
    bool parse_arrow_component(ModuleItem& item);
    // ADR-067 item 6: `const Name = lazy(() => import("./Name"));`, which is identity. It becomes
    // the import `import { Name } from "./Name";`, so the component is used directly.
    bool parse_lazy_import(ModuleItem& item);
    bool parse_import_names(ModuleItem& item);
    void check_module_source(ModuleItem& item, Span span);
    void recover_to_next_item(std::uint32_t item_start);
    [[nodiscard]] bool at_item_start() const;

    // parser_component.cpp
    bool parse_component(ModuleItem& item);
    bool parse_component_param(ModuleItem& item);
    bool parse_component_body(ModuleItem& item);
    bool parse_statement(ModuleItem& item);
    bool parse_const_statement(Statement& statement);
    // ADR-066 item 1: the safe forms of `useMemo`/`useCallback` become the value they wrap.
    void desugar_memo_or_callback(Statement& statement);
    bool parse_returned_element(ModuleItem& item);

    // parser_hook.cpp (spike #659, ADR-065): a project-declared `use...` function.
    bool parse_hook(ModuleItem& item);
    bool parse_hook_params(ModuleItem& item);
    bool parse_hook_body(ModuleItem& item);
    bool parse_hook_return(ModuleItem& item);

    // bind_pairing.cpp
    void check_item_comments(ModuleItem& item, bool pair_inside);
    void check_remaining_comments();
    void skip_comments_before_current();

    // parser_jsx.cpp
    ExprPtr parse_jsx_expression();
    JsxPtr parse_jsx_element(LexMode after);
    JsxPtr parse_jsx_after_lt(std::uint32_t begin, LexMode after);
    JsxPtr parse_fragment(std::uint32_t begin, LexMode after);
    JsxPtr parse_named_element(std::uint32_t begin, LexMode after);
    bool parse_tag_name(std::string& name, Span& span);
    bool parse_attributes(JsxNode& element);
    bool parse_attribute(JsxNode& element);
    bool parse_attribute_value(JsxAttribute& attribute);
    bool parse_children(JsxNode& parent, Span opening);
    bool parse_closing_tag(const JsxNode& element, Span opening, LexMode after);
    void add_text_child(JsxNode& parent);
    void add_container_child(JsxNode& parent);
    void check_tag_name(const std::string& name, Span span);

    Limits limits_;
    SourceMap map_;
    DiagnosticSink sink_;
    Lexer lexer_;
    Token cur_;
    std::uint32_t prev_end_ = 0;
    std::size_t brace_depth_ = 0; // `{` minus `}` consumed since the item start; for recovery
    Span closing_lt_;             // the `<` that started the closing tag found by parse_children
    std::size_t depth_ = 0;
    // ADR-058 typing rule 1 (#547): the members of every string `enum` and `as const` object
    // declared so far in the file. `Color.Red` is replaced by the literal at parse time, so the
    // rules, lowering and IR only ever see a literal. A name declared after its use is not known
    // yet, which is the same order TypeScript needs for an `enum` at module scope.
    struct NamespaceMember {
        std::string name;
        ExprKind kind = ExprKind::String;
        std::string text;
        double number = 0.0;
    };
    std::map<std::string, std::vector<NamespaceMember>> value_namespaces_;
    std::size_t conditional_then_depth_ = 0; // > 0 while parsing the then-branch of `?:`
    bool in_component_ = false;      // inside a component body: enables the props-spread rule
    std::string props_name_;         // name of the props parameter, empty when destructured
    std::set<std::string> bind_ids_; // every bind id (and reserved cleanup id) seen in the file
    // ADR-050 section 3.1 (issue #177): the bindings of `const nav = useService<T>();` seen so far
    // in the current component. A method call on one is a service call the rules pass checks
    // against the project manifest, not an array method, so `nav.push(route)` is not rejected here.
    std::set<std::string> service_bindings_;
    std::size_t comment_cursor_ = 0; // comments before this index were already checked
    // Spike #659: the experimental custom-hook switch. Off means a `use...` function keeps the
    // `TESSERA_REACT_CUSTOM_HOOK` rejection and is parsed as a component, as before.
    bool custom_hooks_ = false;
    bool fatal_ = false;
};

} // namespace tessera::compiler::jsx
