# Compiler: per-branch and per-item instance state

Status: implemented (issue #231). This is the part of the C++ codegen design
([`compiler.md`](compiler.md) section 11) that decides where a **component instance** lives when it
is not a fixed child of the generated struct: one instance per conditional branch, and one instance
per keyed list item. It also gives the keyed-list item record the per-item presence slots that the
future `AnimatePresence` state machine of [`element-transitions-v1.md`](../specs/element-transitions-v1.md)
section 3 needs, so that work does not have to redesign the list table again.

The form of a JSX component instance is an ordinary IR `Component` node (ir-v1 section 3.4); the IR
already carries `condition` and `loop` on every node, so this slice adds **no IR field** and no
validation change. What changes is only where codegen puts the generated instance object.

## 1. The model in one paragraph

Every generated component instance is one object of the callee's generated struct. A **fixed child**
is a member of the caller's struct (unchanged: `Card child_2_;`). A **conditional instance** keeps
that member, and the *host node* it built moves between a branch slot and its source position, so the
instance (state, effects, refs, bindings) survives every toggle. A **list item** owns its instances
inside the keyed item record (`struct Item_<n>`), one `std::unique_ptr<Callee>` per `Component` node
in the template subtree; the record also owns the item's nodes, its per-item conditional branch
slots, its dynamic-group parent pointers and its per-item prop guard hashes. Because the whole
record is what moves during keyed reconciliation, an item's instances, their state and their nodes
stay together and follow the key across reorder.

## 2. Conditional instance (fixed child, branch slot)

Lowering already emits a conditional child as a sibling node carrying `condition`, and codegen builds
every branch subtree once in `build()` into `branch_<n>_owned_` and attaches/detaches it in
`apply_children_<p>()` (compiler.md section 11). A `Component` node joins that path unchanged:

- `build()` creates the host node and the instance object, calls the callee's `build(host)`, and
  stores the host node in the branch slot instead of attaching it (the plan already records the child
  as `DynamicChildKind::Conditional`);
- `apply_children_<p>()` attaches/detaches the host node at its source position by hash-guarded
  condition, exactly like an element branch;
- the instance is built, mounted, updated and unmounted **once**, independent of presence. A hidden
  instance therefore keeps its state, and showing it again reattaches the same nodes with the same
  `NodeId`s. That continues the engine's existing branch-slot rule ("a detached branch stays alive in
  its slot and keeps its stable `node::N` id"); it is deliberately **not** React's destroy-on-false,
  which the presence state machine of element-transitions-v1 section 3.2 replaces with an explicit
  `Exiting` state.

## 3. Keyed list of instances

`Item_<n>` (emitted in the caller's header, inside `namespace gen`) gains, for every `Component` node
inside that template subtree:

```c++
std::unique_ptr<Row> child_<ordinal>;                   // one instance per item
std::uint64_t child_<ordinal>_prop_<name>_hash = 0;     // per-item prop change guard
bool child_<ordinal>_prop_<name>_bound = false;         // callback prop, bound once
```

`build_item_<n>(item, index, id_slot, out)` creates the host node (item id slot), makes the instance,
calls `out.child_<ordinal>->build(host)`, and mounts it when the caller is already mounted. Slot
content inside a template (`<Row key={i}><Text/></Row>`) and a size-observing callee are rejected
loudly (deferred), as are forms and refs inside a template, matching section 6.

`apply_item_<n>(slot, item, index)` is the per-item content pass. It now (1) applies the per-item
props hash-guarded (`force` on the first pass for a new item), and (2) runs the per-item conditional
child walks (section 4) before the text pass. `update()` calls each live item instance's `update()`
after the fixed-child updates; `mount()`/`unmount()` mount/unmount the live item instances, and an
item created or removed at run time mounts/unmounts its instance at that moment; the parent stores
the `UpdateScheduler*` from `attach_scheduler()` so a late-created instance attaches immediately.

Reconciliation is unchanged: the key list is hashed first (`order_hash_`), a changed list moves the
item records through the table, matching new keys against the previous key strings. A matched record
carries its `unique_ptr` instances, so **state survives reorder**; a new key builds a fresh record; a
removed key unmounts its instances and drops the record (and its nodes). Ids: node ids keep the
existing `template id + id_slot * kItemIdStride` scheme (a reused item keeps its slot, an inserted
one takes a free slot), and the automation identity stays the item's `(template uiId, item key)`:
item nodes carry the template's `automationId`/`uiId`, and a numeric item key is also published as
`Node::set_item_key()` on the item root so the automation layer can address one live instance
(automation-ids-v1 section 1; a string key is matched by the generated key table, which is what the
keyed reuse and the presence table key on).

## 4. Conditional inside a list item

The item record also owns one branch slot per `node::<ordinal>` conditional in its template subtree,
plus one `Node* group_parent_<ordinal>` per dynamic group inside it (the item's own copies of the
component-level `branch_<n>_*` / `children_parent_<p>_` members):

```c++
std::unique_ptr<tessera::ui::Node> branch_<ordinal>_owned;
tessera::ui::Node* branch_<ordinal>_node = nullptr;
std::uint64_t branch_<ordinal>_hash = 0;
bool branch_<ordinal>_set = false;
bool branch_<ordinal>_present = false;
tessera::ui::Node* group_parent_<ordinal> = nullptr;
```

`build_item_<n>` fills `group_parent_<ordinal>` for every element inside the item that owns dynamic
children, and builds each conditional branch subtree into its slot (the same `emit_node` emitter, in
"item mode": branch slots and group parents resolve to `out.` fields instead of `this->` members).
`apply_item_<n>` runs the same deterministic position walk as `apply_children_<p>()` — a static child
advances the position, a present branch advances it after `insert_child`, a nested list advances it
by its count — using the item's slots, so a branch lands at its source position inside the item and
an unchanged condition touches no node. The condition may contain a component instance (its
`unique_ptr` lives in the same record), which is the "conditional inside a keyed list of components"
case: state belongs to the item and survives both the branch toggle and a reorder.

## 5. Presence (future `AnimatePresence`)

The item record is where per-item presence state will live: an exiting item keeps its record (nodes,
instances, state) while its key is gone from the source list, and a `presence` field per record
(`Absent`/`Entering`/`Present`/`Exiting`) plus the exit deadline drives removal. The reconciliation
already has the three phases that needs — match by key, detach the old records, attach the new ones —
so the presence state machine only changes *when* a record is detached and adds the per-record
presence bit; it does not need a second table or a different identity. Nothing in this slice reads
that field yet; the design keeps the record the single owner of everything an item has so the future
step is additive.

## 6. Still rejected loudly (`TESSERA_CODEGEN_UNSUPPORTED`)

Slot content of an instance inside a template, a `useRef` target inside a template, dynamic
metadata/inline style inside a template, a nested list template, a list template inside a virtual
item template, a size-observing (`onSizeChange`) or form-bound (`Form`/`Field`) callee inside a
template, a conditional or list node as *direct* slot content of a `Component`, a conditional list
node directly in a `Component`'s tree position, and record element types.

## 7. Tests

- `codegen_composition_tests`: conditional instance emission and the `--write` goldens.
- `codegen_dynamic_tests`: keyed list of instances and conditional-inside-item emission; the item
  record fields, per-item props and the item-mode branch slots.
- `codegen_composition_runtime_tests` / `codegen_dynamic_runtime_tests`: a keyed list of component
  instances (state survives reorder, removed items unmount, ids follow the item slot), a conditional
  instance (toggle keeps state and node ids), a conditional inside a list item (per-item presence),
  and a list-typed prop passed through a component boundary.
