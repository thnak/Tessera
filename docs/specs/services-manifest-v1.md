# Services manifest v1 (`ui/services.json`)

Status: **draft**, the first slice of [#177](https://github.com/thnak/Tessera/issues/177) (the TSX half of
`useService<T>()`). It gives [ADR-050](../../decisions/ADR-050/service-provisioning.md) section 3.1 item 1
its schema; it decides nothing the ADR does not already say. Consumers: the front end (`useService<T>()`
type checking and lowering), codegen (the `ir::Component::services` entries,
[`service-injection.md`](../design/services/service-injection.md) section 11.5), the generated
`services.d.ts`, startup validation, and the Studio mock services
([ADR-055](../../decisions/ADR-055-studio-mock-services.md), [`studio-mock-services-v1.md`](studio-mock-services-v1.md)).

Terms: a **service interface** is a C++ abstract class with a `kServiceId`
([`service_id.hpp`](../../include/tessera/appfw/service_id.hpp)). The **manifest** lists the interfaces
TSX may name and, for each, the methods TSX may call. **IR type** is a type of
[`ir-v1.md`](ir-v1.md) section 3 (scalars, `optional<T>`, a declared record).

## 1. File

`ui/services.json` in the project, next to `ui/elements.json` (the pattern of
[ADR-028](../../decisions/ADR-028-custom-native-elements.md)). A project with no `useService` needs no
file. The file is a build input: the compiler reads it, never the C++ header.

```json
{
  "version": 1,
  "services": [
    {
      "id": "tessera.http",
      "name": "HttpService",
      "cpp": { "type": "tessera::appfw::HttpService", "header": "tessera/appfw/http_service.hpp" },
      "methods": [
        {
          "name": "cancel",
          "params": [{ "name": "ticket", "type": "int" }],
          "returns": "bool"
        }
      ]
    },
    {
      "id": "demo.nav",
      "name": "Navigator",
      "cpp": { "type": "demo::Navigator", "header": "demo/navigator.hpp" },
      "methods": [{ "name": "push", "params": [{ "name": "route", "type": "string" }], "returns": "void" }]
    }
  ]
}
```

## 2. Fields

| Field | Type | Rule |
|---|---|---|
| `version` | int | Must be `1`. A larger value is `TESSERA_SVC_MANIFEST_VERSION`; the file is not partly read |
| `services[].id` | string | Reverse-DNS name; the registry id is `service_id(id)` (FNV-1a, `service_id.hpp`). Unique in the file |
| `services[].name` | string | The TSX type name used as `useService<Name>()`. A TSX identifier, unique in the file |
| `services[].cpp.type` | string | The C++ spelling of the interface (`demo::Navigator`). Copied to `ir::Component::services[].type` |
| `services[].cpp.header` | string | Include path of the declaring header. Copied to `...services[].header` |
| `services[].methods[]` | array | The methods TSX may call. An omitted method is not callable from TSX |
| `methods[].name` | string | The C++ member function name; unique per service (no overloads in V1) |
| `methods[].params[]` | array | `{name, type}`, in call order. `type` is an IR type |
| `methods[].returns` | string | An IR type or `void`. A non-`void` result may be bound with `const` in a handler |

Not in V1, reserved so a later version is additive: `observables` (service-owned `Observable<T>` readable in
render, ADR-050 open question 6), `events`, and per-method `threading` notes. A reader ignores a key it
does not know only inside `cpp`; elsewhere an unknown key is `TESSERA_SVC_MANIFEST_UNKNOWN_KEY` so a typo
cannot silently drop a method.

## 3. What the compiler does with it

1. **`useService<Name>()`** (rules of hooks, top level of the component body) resolves `Name` against
   `services[].name`. A name not in the manifest, a concrete class, a string or a variable is
   `TESSERA_REACT_SERVICE_UNKNOWN` with the TSX location (ADR-050 section 3.1 item 2).
2. **A call `svc.method(args)`** is checked against `methods[]`: an unknown method, a wrong argument count
   or an argument that does not convert to the declared IR type is a diagnostic naming the method and
   the manifest line. The call is accepted only in a handler or an effect (ADR-050 section 3.1 item 4);
   in the render body it is `TESSERA_REACT_SERVICE_CALL_IN_RENDER`. An unknown method is `TESSERA_REACT_SERVICE_METHOD`; using the service value other than as a call receiver is `TESSERA_REACT_SERVICE_VALUE`.
3. **Lowering** adds one `ir::Component::services` entry per distinct service (`id`, `name`, `type`,
   `header`, `source`) and one call action per call. Codegen reads the IR only.
4. **`services.d.ts`** is generated from the manifest: one `interface Name` per service with the listed
   methods, and the overload of `useService` for each name, so an editor type-checks the same rules.
5. **The id check.** Codegen emits `static_assert(T::kServiceId == tessera::appfw::service_id("<id>"))` in
   the generated base, so a manifest id that disagrees with the C++ interface is a compile error, not a
   runtime miss.
6. **Startup validation.** The union of the requirements of every compiled component is emitted as a
   list of ids; `Application` checks it against the frozen scopes before P4 and reports every missing
   provider with the component and TSX location (`TESSERA_APPFW_SERVICE_MISSING`).

## 4. Diagnostics

| Code | When |
|---|---|
| `TESSERA_SVC_MANIFEST_SYNTAX` | not valid JSON, or a required field is missing or of the wrong JSON type |
| `TESSERA_SVC_MANIFEST_VERSION` | `version` is not `1` |
| `TESSERA_SVC_MANIFEST_DUPLICATE` | two services share an `id` or a `name`, or two methods of one service share a name |
| `TESSERA_SVC_MANIFEST_TYPE` | a param or return type is not an IR type, or names a record the module does not declare |
| `TESSERA_SVC_MANIFEST_UNKNOWN_KEY` | an unknown key outside `cpp` |

Every diagnostic carries the file, the JSON pointer of the offending value and the line and column.

## 5. Tests (target)

A reader table test (one case per diagnostic, plus a valid file and a round trip to the IR entry), a
front-end test for each rule in section 3, a `services.d.ts` golden, and the Studio mock test that
replaces `readServiceManifest() -> null` with this reader.
