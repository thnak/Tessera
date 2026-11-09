// SPDX-License-Identifier: MIT

#include "jsx_test_util.hpp"

#include <string>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

void test_accepted_types() {
    check_eq(dump_type("int"), "int", "name");
    check_eq(dump_type("string"), "string", "string");
    check_eq(dump_type("int | null"), "int|null", "nullable");
    check_eq(dump_type("\"a\" | \"b\""), "\"a\"|\"b\"", "string literal union");
    check_eq(dump_type("int[]"), "int[]", "array");
    check_eq(dump_type("Row[][]"), "Row[][]", "nested array");
    check_eq(dump_type("Ref<Text>"), "Ref<Text>", "generic name");
    check_eq(dump_type("Map<string, Array<int>>"), "Map<string,Array<int>>", "nested generic `>>`");
    check_eq(dump_type("{ a: int; b?: string, c: Row[] }"), "{a:int,b?:string,c:Row[]}",
             "object type with both separators and an optional member");
    check_eq(dump_type("{}"), "{}", "empty object type");
    check_eq(dump_type("(id: int, label: string) => void"), "(id:int,label:string)=>void",
             "function type");
    check_eq(dump_type("() => int | null"), "()=>int|null", "return type is a full union");
    check_eq(dump_type("{ onPick: (id: int) => void }"), "{onPick:(id:int)=>void}",
             "function type as a member");
    check_eq(codes(parse_type("int | null")), "", "no diagnostics");
}

void test_rejected_types() {
    struct Case {
        const char* source;
        const char* code;
        bool tree;
    };
    const Case cases[] = {
        {"number", "TESSERA_JSX_TYPE_NUMBER;", true},
        {"number[]", "TESSERA_JSX_TYPE_NUMBER;", true},
        {"{ a: number }", "TESSERA_JSX_TYPE_NUMBER;", true},
        {"any", "TESSERA_JSX_SUBSET_TYPES;", true},
        {"unknown", "TESSERA_JSX_SUBSET_TYPES;", true},
        {"never", "TESSERA_JSX_SUBSET_TYPES;", true},
        {"object", "TESSERA_JSX_SUBSET_TYPES;", true},
        {"Function", "TESSERA_JSX_SUBSET_TYPES;", true},
        {"A & B", "TESSERA_JSX_SUBSET_TYPES;", false},
        {"[int, string]", "TESSERA_JSX_SUBSET_TYPES;", false},
        {"typeof x", "TESSERA_JSX_SUBSET_TYPES;", false},
        {"", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", false},
        {"int |", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", false},
        {"Ref<", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", false},
        {"Ref<int", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", false},
        {"{ a int }", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", false},
        {"(a) => int", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", false},
        {"int int", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", false},
    };
    for (const auto& test : cases) {
        const auto result = parse_type(test.source);
        check_eq(codes(result), test.code, std::string("type `") + test.source + "`");
        check((result.root != nullptr) == test.tree,
              std::string("tree presence for ") + test.source);
    }
    check_eq(first_diag(parse_type("number")).suggestion, "write `int` or `float`",
             "suggestion for number");
}

void test_type_spans() {
    const std::string source = "{ a: Row[] } | null";
    const auto result = parse_type(source);
    if (!result.root) {
        check(false, "parses");
        return;
    }
    const SourceMap map(source);
    check_eq(std::string(map.slice(result.root->span)), source, "union span");
    check_eq(std::string(map.slice(result.root->children[0]->span)), "{ a: Row[] }", "object span");
    check_eq(std::string(map.slice(result.root->children[0]->members[0].span)), "a: Row[]",
             "member span");
    check_eq(std::string(map.slice(result.root->children[0]->members[0].type->span)), "Row[]",
             "array span covers the element and brackets");
}

void test_type_depth_is_limited() {
    std::string deep;
    for (int i = 0; i < 5000; ++i) {
        deep += "A<";
    }
    check_eq(codes(parse_type(deep)), "TESSERA_JSX_LIMIT_DEPTH;", "deeply nested generics");
    std::string arrays = "A";
    for (int i = 0; i < 5000; ++i) {
        arrays += "[]";
    }
    check_eq(codes(parse_type(arrays)), "TESSERA_JSX_LIMIT_DEPTH;", "long array suffix chain");
    Limits small;
    small.max_nesting_depth = 3;
    check(parse_type("A<B<C>>", small).ok(), "3 levels fit a limit of 3");
    check(parse_type("A[][]", small).ok(), "an array chain of height 3 fits");
    check_eq(codes(parse_type("A[][][]", small)), "TESSERA_JSX_LIMIT_DEPTH;",
             "an array chain of height 4 does not (tree height, not recursion)");
    check_eq(codes(parse_type("A<B<C<D>>>", small)), "TESSERA_JSX_LIMIT_DEPTH;", "4 levels do not");
}

} // namespace

int main() {
    test_accepted_types();
    test_rejected_types();
    test_type_spans();
    test_type_depth_is_limited();
    return finish("jsx type tests");
}
