# filterGraph — TODO

Planned work, in one place. Each item states what the library does **today**,
the **gap**, a **proposal** with an API sketch, its **compatibility** impact,
and the **workaround** that exists in the meantime.

The list started as a set of extension ideas that came up while designing
stateful fan-out/fan-in graphs (several parallel stages whose results a
stateful merge combines, plus a result-checking stage), so most items are about
merges, stage state and the end of a run. They are written for the library in
general, and the API sketches are sketches: nothing here is implemented.

Examples use the generic stages of the [README](README.md) (`Parse`,
`Validate`, `Summarize`, …). Shipped features are documented in the
[README](README.md) and [CHANGELOG.md](CHANGELOG.md), not here.

## Overview

| # | Item | Impact | Compatibility | Workaround today |
|---|------|--------|---------------|------------------|
| 1 | [Named merge slots](#1-named-merge-slots) | medium | additive if done carefully | rely on group order |
| 2 | [Several named graph inputs](#2-several-named-graph-inputs) | medium | additive (new graph shape) | `std::variant` input + select stages |
| 3 | [Stage labels and typed access](#3-stage-labels-and-typed-access) | medium | additive (DSL syntax) | side registry filled by creator lambdas |
| 4 | [Track which stage short-circuited](#4-track-which-stage-short-circuited) | medium | additive | tap the edge, or log in the stage |
| 5 | [Config-aware `registerMergeFilter`](#5-config-aware-registermergefilter) | low | additive | subclass + `FilterRegistrar` creator |
| 6 | [Injectable registry, duplicate detection](#6-injectable-registry-duplicate-detection) | low | mostly additive | unique names |
| 7 | [Documentation: 0..n outputs, large messages](#7-documentation-0n-outputs-large-messages) | doc only | — | — |
| 8 | [Known limitations to lift](#8-known-limitations-to-lift) | low–medium | additive | JSON config; read `GraphOutputs` carefully |

Items of the same list that are done, and therefore not repeated here:
**type-checked merge inputs** (`TypedMergeFilter` / `UniformMergeFilter` /
`registerTypedMergeFilter`), the **end-of-stream hook**
(`MessageFilter::finish()`), both described in the README, and **examples for
the newer features** — `apps/statefulPipeline` and `apps/compositePipeline`,
walked through in [EXAMPLE.md](EXAMPLE.md) sections 8 and 9.

---

## 1. Named merge slots

**Today.** Slots are positional, in the order the DSL group lists them. A merge
stage can only know which upstream a slot came from by convention. Since
type-checked merges shipped, a mis-*typed* slot is caught when the graph is
built — but two slots of the *same* type in the wrong order still pass
validation and then run silently wrong.

**Proposal.** Optional slot names in the group, checked against the names the
stage declares:

```text
(raw: msg, checked: valid) -> Summarize -> out.stats
```

```cpp
// empty = positional, today's behaviour
virtual std::vector<std::string> mergeInputNames() const { return {}; }
```

Behaviour:

- **Stage declares names, group uses names:** slots are matched by name, so the
  order in the group no longer matters. Unknown or missing names produce
  diagnostics.
- **Stage declares names, group is positional:** allowed, matched by position
  (keeps today's graphs valid).
- **Stage declares nothing:** names in the group are an error (*'Summarize' has
  no named slots*).

Together with the slot types a typed merge already declares, this gives every
slot a name *and* a type, which also makes `dsl::toMermaid` output
self-explanatory.

**Compatibility.** Keep `MergeInputs` a `std::vector<std::any>` and reorder the
values into declared order before calling the stage, so stage code does not
change. Making `MergeInputs` a struct with names would be a breaking change and
is not needed.

**Workaround today.** Rely on group order, and give same-typed slots distinct
wrapper types so that the existing slot-type check can tell them apart.

## 2. Several named graph inputs

**Today.** A graph has exactly one input edge `in`, with one type. A graph that
consumes several kinds of message needs a single `std::variant` input, and then
every stage has to unpack it, or one "select" stage per alternative drops the
others.

**Proposal.** A graph with named inputs, mirroring `GraphOutputs`:

```text
in.orders -> ParseOrder -> order
in.quotes -> ParseQuote -> quote
(order, quote) -> Match -> out
```

```cpp
DslFilterGraph<GraphInputs, OutputType> graph(text);
graph.push("quotes", Quote{...});   // runs only the stages reachable from in.quotes
```

Semantics:

- One `push` is one run.
- Input edges not fed in this run are **holes**, exactly like dropped paths
  today, so merges and outputs need no new rules.
- Validation checks each named input's type at construction, which needs a way
  to declare them, e.g. `GraphInputs::of<Order, Quote>("orders", "quotes")`.
- `filter(GraphInputs&&)` stays available, for "push several inputs in one run".

**Compatibility.** Additive: `DslFilterGraph<In, Out>` with a plain input type
keeps the single `in` edge.

**Workaround today.** A `std::variant` input plus one select stage per
alternative, each dropping the alternatives it does not handle.

## 3. Stage labels and typed access

**Today.** The owner of a `DslFilterGraph` cannot reach a stage instance. The
DSL has no labels (`#` starts a comment), and the compiled plan is private.
Tests, diagnostics and statistics therefore have to go through edges or global
state.

**Proposal.** An optional label per stage, and typed lookup:

```text
(msg, valid) -> Summarize@stats -> out.stats
```

```cpp
auto& stats = graph.stage<SummarizeFilter>("stats"); // throws if unknown or the type differs
```

- Labels are unique per graph, which is checked at construction.
- `dsl::toMermaid` can show them, and diagnostics can say `stats` instead of
  "stage 'Summarize' at 3:17".
- Lookup needs `AnyMessageFilterAdapter` to expose the wrapped filter
  (`std::shared_ptr<void>` + `std::type_index`, or a virtual
  `target(std::type_index)` in the style of `std::function::target`).

**Compatibility.** Additive. The `@` character is unused in the DSL today.

**Workaround today.** Register the stage with a creator lambda that records
every instance it builds in a side registry, or have the stage publish what the
owner needs through the `GraphContext`.

## 4. Track which stage short-circuited

**Today.** When a graph drops a message, neither `DslFilterGraph` nor
`AnyFilterChain` tells the caller *which* stage returned `std::nullopt`.

**Gap.** A graph that silently produces nothing is hard to diagnose: the caller
sees an empty `std::optional` and has to bisect the graph to find the stage that
dropped the message. (`Void` already distinguishes an *intentional* dead end
from a dropped message; this item is about observing *unintentional* drops.)

**Proposal.** Record the stage that short-circuited — its name, and its location
in the DSL text — and expose it, e.g. through an accessor valid after a run, or
a richer result type. Whatever the shape, it should stay allocation-free on the
happy path and say something useful for a drop inside a nested graph.

**Compatibility.** Additive as an accessor; a new result type would be breaking,
so that variant needs an overload or an opt-in.

**Workaround today.** Tap the suspect edge with a logging stage ending in `end`,
or log inside the stage that decides to drop.

## 5. Config-aware `registerMergeFilter`

**Today.** `registerMergeFilter<Out>(name, combiner)` ignores the DSL config and
**copies one combiner into every instance**. A combiner lambda that captures a
`shared_ptr` therefore **shares state across all instances and graphs**. The
header says so; the README and EXAMPLE.md do not.

A merge with config and per-instance state is already possible: derive from
`MessageFilter<MergeInputs, Out>` (or from `TypedMergeFilter`) and register it
with `FilterRegistrar<T>(name, creator)`. The gap is ergonomics and
documentation, not capability.

**Proposal.**

```cpp
// A fresh combiner per instance, built from the stage's DSL config.
template <typename Out>
void registerMergeFilter(const std::string& name,
                         std::function<typename MergeFilter<Out>::Combiner(const nlohmann::json& config)> factory);
```

[EXAMPLE.md](EXAMPLE.md) section 8 now shows the subclass + `FilterRegistrar`
pattern and the shared-combiner semantics; what is still missing is the
overload above, and a note in the README on the copy semantics of the existing
one.

**Compatibility.** Additive overload.

**Workaround today.** Subclass and register with a creator lambda.

## 6. Injectable registry, duplicate detection

**Today.** `FilterRegistry::instance()` is a process-wide singleton, and
`registerFilter` **silently overwrites** an existing name.

**Gap.**

- Two libraries registering the same name shadow each other without notice.
- A test that wants to swap a stage for a fake has to mutate global state and
  restore it.
- Graphs cannot be built against different stage sets in one process.

**Proposal.**

- `registerFilter` reports a duplicate name (throw, or return `false`), with an
  explicit `replaceFilter(name, creator)` for intentional overrides.
- `DslFilterGraph(text, const FilterRegistry& registry = FilterRegistry::instance())`,
  and the same parameter on `validateDslGraph`, `validateGraph` and
  `JsonFilterGraph`.
- A copyable `FilterRegistry` (or `FilterRegistry::derive()`), so a test can
  start from the global set and replace single stages.

**Compatibility.** Turning an overwrite into an error is behaviour-breaking.
Ship it behind a transition: warn first, or add
`registerFilter(..., OnDuplicate)`.

**Workaround today.** Keep names unique, and register test fakes under their own
names.

## 7. Documentation: 0..n outputs, large messages

No API change. These points belong in the README / EXAMPLE.md, because they come
up as soon as stages carry state:

- **0..n outputs per input.** A stage emits at most one value per run. A general
  multi-output stage would conflict with merge semantics (which output pairs
  with which slot?), so the recommendation is **not** to add one. Document the
  convention instead: a stage that can produce several results has a collection
  as its output type, and downstream stages iterate over it.
- **Fan-out copies.** Every reader of an edge except the last gets a copy
  (`GraphPlan::read`). Cheap values are fine; large or shared payloads should
  travel as `std::shared_ptr<const T>`.
- **Stateful stages.** A stage instance lives as long as its graph, and state in
  its members persists across messages. Say explicitly that this is supported
  and intended, and that each graph construction creates fresh instances.

## 8. Known limitations to lift

The [current limitations](README.md#current-limitations) the README lists, as
work items:

- **Nested stage arguments in the DSL.** Arguments are flat `key=value` pairs;
  nested objects and lists are not expressible, so a stage that needs them has
  to be configured in JSON. Lifting this means a value grammar for objects and
  arrays, plus diagnostics for it.
- **Type checking inside `GraphOutputs`.** Only the graph's input type and the
  single `out` type are checked against the C++ template parameters. The types
  behind `out.<key>` are checked when they are read (`get<T>` throws
  `std::bad_any_cast`), not when the graph is built. Checking them up front
  needs a way to declare the expected type per key.
