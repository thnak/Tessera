// SPDX-License-Identifier: MIT
//
// ADR-058 typing rule 1 (#547): the type and declaration forms a port writes that the subset
// absorbs at compile time - `interface`, `Partial`/`Required`/`Pick`/`Omit`, record intersections,
// string `enum` and `as const` objects. Each is checked as "parses to the same module as the form
// it stands for", and each rejection carries its stable code.

#include "jsx_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

void test_forms() {
    // ADR-058 typing rule 1 (#547): an interface is the same module item as the alias it replaces.
    check_eq(dump_module("export interface Props { initial: int; label?: string }"),
             dump_module("export type Props = { initial: int; label?: string };"),
             "an interface parses to the type alias of the same members");
    check(parse_module("export interface P { a: int }").root->items[0].exported,
          "an exported interface keeps the flag");
    // #547: the utility types and record intersections expand to the one object type.
    check_eq(dump_module("type P = { a: int; b?: string };\ntype X = Partial<P>;"),
             dump_module("type P = { a: int; b?: string };\ntype X = { a?: int; b?: string };"),
             "Partial makes every member optional");
    check_eq(dump_module("type P = { a?: int; b: string };\ntype X = Required<P>;"),
             dump_module("type P = { a?: int; b: string };\ntype X = { a: int; b: string };"),
             "Required drops the optional marks");
    check_eq(
        dump_module("type P = { a: int; b: string; c: bool };\ntype X = Pick<P, \"a\" | \"c\">;"),
        dump_module("type P = { a: int; b: string; c: bool };\ntype X = { a: int; c: bool };"),
        "Pick keeps the named members");
    check_eq(dump_module("type P = { a: int; b: string };\ntype X = Omit<P, \"b\">;"),
             dump_module("type P = { a: int; b: string };\ntype X = { a: int };"),
             "Omit drops the named members");
    check_eq(dump_module("type X = A & B;\ntype A = { a: int };\ntype B = { b: string };"),
             dump_module(
                 "type X = { a: int; b: string };\ntype A = { a: int };\ntype B = { b: string };"),
             "an intersection of records expands, whatever the declaration order");
    check_eq(
        dump_module("interface A { a: int }\ninterface B { b: int }\ntype X = Partial<A & B>;"),
        dump_module(
            "interface A { a: int }\ninterface B { b: int }\ntype X = { a?: int; b?: int };"),
        "utility types compose with intersections");
    check_eq(codes(parse_module("type X = Partial<Imported>;")), "TESSERA_JSX_TYPE_UTILITY;",
             "a utility type over a name not declared here is rejected with the fix");
    check_eq(codes(parse_module("type A = { a: int };\ntype X = Omit<A, \"zz\">;")),
             "TESSERA_JSX_TYPE_UTILITY;", "an unknown key is rejected");
    check_eq(codes(parse_module("type A = { a: int };\ntype X = A & int;")),
             "TESSERA_JSX_TYPE_INTERSECTION;", "only records intersect");
    check_eq(codes(parse_module("interface P extends Q { a: int }")), "TESSERA_JSX_SUBSET_TYPES;",
             "interface extends is rejected with the fix");
    check_eq(codes(parse_module("interface P<T> { a: T }")), "TESSERA_JSX_SUBSET_TYPES;",
             "a generic interface is rejected");
    // A string enum is the union of its values, and `Color.Red` is the literal.
    check_eq(dump_module("enum Color { Red = \"red\", Blue = \"blue\" }\nconst k = Color.Blue;"),
             dump_module("type Color = \"red\" | \"blue\";\nconst k = \"blue\";"),
             "a string enum lowers to a literal union and its members to literals");
    check_eq(dump_module("export enum Size { S = \"s\", }"),
             dump_module("export type Size = \"s\";"),
             "an exported one-member enum with a trailing comma");
    check_eq(dump_module("const T = { dark: \"d\", light: \"l\" } as const;\nconst x = T.light;"),
             dump_module("const T = { dark: \"d\", light: \"l\" };\nconst x = \"l\";"),
             "an as const object is a namespace of literals");
    check_eq(codes(parse_module("const COLORS = { dark: \"d\" };")), "",
             "an uppercase constant with an object initializer is not an arrow component");
    check_eq(codes(parse_module("enum E { A = 1 }")), "TESSERA_JSX_TYPE_ENUM;",
             "a numeric enum member is rejected with the fix");
    check_eq(codes(parse_module("enum E { A }")), "TESSERA_JSX_TYPE_ENUM;",
             "an auto-numbered member is rejected");
    check_eq(codes(parse_module("enum E { A = \"a\" }\nconst k = E.B;")), "TESSERA_JSX_TYPE_ENUM;",
             "an unknown member names the known ones");
    check_eq(dump_module("const T = { dark: \"d\", light: \"l\" } as const;\n"
                         "type Theme = typeof T[keyof typeof T];"),
             dump_module("const T = { dark: \"d\", light: \"l\" };\ntype Theme = \"d\" | \"l\";"),
             "typeof NS[keyof typeof NS] is the union of the namespace values");
    check_eq(codes(parse_module("type Theme = typeof T[keyof typeof T];")),
             "TESSERA_JSX_SUBSET_TYPES;",
             "typeof over an undeclared namespace is rejected with the fix");
    check_eq(codes(parse_module("const T = { a: \"a\" } as const;\ntype X = typeof T;")),
             "TESSERA_JSX_SUBSET_TYPES;", "a bare typeof type stays rejected");
    check_eq(codes(parse_module("const a = 1 as int;")), "TESSERA_JSX_SUBSET_TYPES;",
             "only as const is accepted as an assertion");
}

} // namespace

int main() {
    test_forms();
    return finish("jsx type forms tests");
}
