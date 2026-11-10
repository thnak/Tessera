# IR v1: Counter example

Companion to [`ir-v1.md`](ir-v1.md) section 13; the normative rules stay there.

```json
{
  "irVersion": 1,
  "module": { "name": "demo", "imports": ["tessera"] },
  "components": [
    {
      "id": "comp::Counter",
      "componentId": "1234567890123456789",
      "name": "Counter",
      "source": { "file": "ui/Counter.tsx", "offset": 0, "length": 310, "line": 1, "column": 1, "endLine": 14, "endColumn": 2 },
      "props": [],
      "state": [
        { "id": "state::count", "name": "count", "type": "int", "initial": 0 }
      ],
      "effects": [],
      "refs": [],
      "animatedValues": [],
      "root": {
        "id": "node::1",
        "uiId": "111111111111111111",
        "source": { "file": "ui/Counter.tsx", "offset": 52, "length": 250, "line": 3, "column": 3, "endLine": 13, "endColumn": 9 },
        "kind": "View",
        "props": { "className": "counter" },
        "style": { "display": "flex", "gap": "8px" },
        "children": [
          {
            "id": "node::2",
            "uiId": "222222222222222222",
            "source": { "file": "ui/Counter.tsx", "offset": 80, "length": 52, "line": 4, "column": 5, "endLine": 4, "endColumn": 57 },
            "kind": "Text",
            "text": {
              "kind": "template",
              "parts": ["Count: ", { "kind": "ref", "id": "state::count" }]
            }
          },
          {
            "id": "node::3",
            "uiId": "333333333333333333",
            "source": { "file": "ui/Counter.tsx", "offset": 140, "length": 60, "line": 5, "column": 5, "endLine": 5, "endColumn": 65 },
            "kind": "Button",
            "props": { "label": "Increment" },
            "events": { "onClick": "bind::on_click" },
            "children": []
          }
        ]
      }
    }
  ],
  "bindings": [
    {
      "id": "bind::on_click",
      "component": "comp::Counter",
      "kind": "event",
      "source": { "file": "ui/Counter.tsx", "offset": 240, "length": 15, "line": 12, "column": 5, "endLine": 12, "endColumn": 20 },
      "region": "on_click"
    }
  ],
  "stylesheets": [
    { "id": "css::app", "path": "ui/app.css", "hash": "sha256:placeholder" }
  ]
}
```
