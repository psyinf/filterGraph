# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **`DslFilterGraph<InputType, OutputType>`** (`DslFilterGraph.hpp`) — runs
  graphs described in the text DSL, making it a first-class way to describe
  runtime graphs. Executes named-edge graphs with fan-out (every reader gets a
  copy), fan-in merges (holes for dropped paths), drop propagation and dead
  ends, and exposes the graph as a `MessageFilter`, so it can be nested or
  registered as a stage. Construction instantiates and type-checks the whole
  graph and throws a `GraphError` listing every located problem.
- **`GraphOutputs`** — the ordered, optionally keyed results of a graph with
  several outputs (`get` / `take` / `has`).
- **`MergeFilter<OutputType>` / `registerMergeFilter`** (`MergeFilter.hpp`) —
  fan-in stages for `(a, b) -> Merge`, using the same combiner signature as
  `JoinFilter`.
- **`validateDslGraph<In, Out>(text)`** — every build-time diagnostic without
  throwing; `dsl::formatDiagnostic(s)` formats them as `line:column: message`.
- `dsl::StageNode::fanIn` records whether a stage's inputs came from a group.
- **`GraphContext`** (`GraphContext.hpp`) — a graph-scoped, type-keyed,
  thread-safe blackboard for side-channel data between stages
  (`set` / `get` / `getOr` / `contains` / `erase` / `update` / `clear`). It is a
  polymorphic base; derived contexts are recovered with `as<Derived>()`.
- **`TypedMergeFilter<OutputType, InputTypes...>`** /
  **`UniformMergeFilter<InputType, OutputType>`** /
  **`registerTypedMergeFilter`** (`MergeFilter.hpp`) — merge stages that declare
  their slot types. The DSL checks the edges of their fan-in group when the
  graph is built (`slot 2 of 'Summarize' expects ... but edge 'msg' carries ...`,
  `stage 'Summarize' takes 2 inputs but the group has 3`), so a mis-wired merge
  no longer fails with a `std::bad_any_cast` on the first message. The stages
  receive their slots as typed `std::optional`s (`std::nullopt` for a hole),
  with no `any_cast` in user code. `MergeFilter` / `registerMergeFilter` declare
  no slot types and keep their current, unchecked behaviour.
- **`MessageFilter::finish()`** — end-of-stream hook, called once after the last
  message, so that a stage holding state can flush it (write a report, close a
  file, publish a result to the `GraphContext`). It defaults to a no-op and
  produces no message. `DslFilterGraph::finish()` finishes every stage in run
  order; `FilterGraph`, `JsonFilterGraph`, `AnyFilterChain`, `FanoutFilter`,
  `JoinFilter` and nested graphs forward it. Every stage is finished even if one
  throws; the first exception is rethrown afterwards. There is no `tick()`: use
  an in-band tick message (see README).
- **`MergeStage` / `MergeSlotTypes`** (`MergeStage.hpp`, new header) — how a
  merge stage declares its slot types; `AnyMessageFilter::mergeInputTypes()`
  exposes them to the DSL. `MergeInputs` moved here from `MergeFilter.hpp`
  (which still provides it).
- **`apps/statefulPipeline` and `apps/compositePipeline`** — two runnable
  examples for the features `apps/textPipeline` does not cover.
  `statefulPipeline` shows a stage that accumulates across messages and flushes
  in `finish()`, hands its result to the application through the
  `GraphContext`, a merge with per-instance state (subclass +
  `FilterRegistrar`) next to the shared-combiner semantics of
  `registerMergeFilter`, and an application's own derived context recovered
  with `as<AppContext>()`. `compositePipeline` shows `registerTypedMergeFilter`
  and the JSON `JoinFilter` side by side, a `DslFilterGraph` registered as a
  stage of an outer graph (with `finish()` and the context reaching into it), a
  `Void`-terminated sink graph, and the in-band tick message. EXAMPLE.md walks
  through both in sections 8 and 9.
- **Named merge slots** — a fan-in group can name the slots of a merge,
  `(raw: msg, checked: valid) -> Merge`, and is then matched by name, in any
  order: the merge receives its slots in the order it declares them. A merge
  declares names with `MergeStage::mergeInputNames()` (override it in a
  `TypedMergeFilter` / `UniformMergeFilter` subclass), or with the new
  overloads `registerTypedMergeFilter<Out, Ins...>(name, slotNames, merger)`
  and `registerMergeFilter<Out>(name, slotNames, combiner)`. Unknown or missing
  names, a group that names only some slots, and names on a merge that declares
  none are build-time diagnostics; type mismatches name the slot
  (`slot 'raw' of 'Merge' expects ...`). Positional groups keep working and are
  matched by position. `dsl::StageNode::slotNames` records the names, and
  `dsl::toMermaid` labels the links with them.
- **`TODO.md`** — the planned work, with what the library does today, the gap,
  an API sketch and the workaround for each item; the README roadmap summarizes
  it.
- **`MessageFilter::setContext` / `context()` / `sharedContext()`** — stages
  receive the graph's `GraphContext`. `FilterGraph`, `DslFilterGraph`,
  `JsonFilterGraph`, `AnyFilterChain`, `FanoutFilter` and `JoinFilter` create an
  empty context when they are built and forward it to their stages, so stages of
  a graph share one context without any setup; `setContext` replaces it (with
  nullptr meaning "a fresh empty one"). `context()` returns a `GraphContext&`
  and is never null, so stages need no null check; `sharedContext()` hands out
  the `std::shared_ptr` for composites that forward it.

### Changed
- `MergeStage::mergeInputTypes()` is no longer pure; it defaults to no slot
  types, so a merge can declare only names. `MergeFilter` now implements
  `MergeStage`, and `AnyMessageFilter` has a new `mergeInputNames()`, which
  defaults to none.
- **Breaking:** `AnyMessageFilter` has new pure virtuals
  `setContext(std::shared_ptr<GraphContext>)` and `finish()`; custom
  implementations must forward both to the stages they wrap. They are pure
  rather than no-ops on purpose: a composite that forgot to forward them would
  otherwise fail silently.
- README, EXAMPLE.md and `apps/textPipeline` now describe runtime graphs in the
  DSL first; the JSON format is documented as a supported alternative.
- `dsl::toMermaid` now uses generated node ids (instead of edge names such as
  `end` or the internal `$out0`), labels outputs as `out` / `out.<key>`, gives
  each dead end its own node, and lists stage arguments.
- DSL stages written without arguments now receive an empty config object
  instead of `null`, matching JSON stages without `"config"`.
- `dsl::parseGraphProgram` now uses the lexy-based parser, which moved into
  `GraphLang.hpp`. It reports located, descriptive diagnostics (the lexy parser
  used to report every problem as a bare "syntax error" at column 1), gives
  stages their real columns, and supports decimal numbers, string escapes and
  `#` inside strings. `GraphLangLexy.hpp` / `parseGraphProgramLexy` remain as
  an alias.
- DSL syntax errors say what was expected and what was found; a syntax error
  ends its statement instead of producing follow-on errors; an unterminated
  string no longer swallows the following lines; malformed or out-of-range
  numbers and empty fan-in groups `()` are reported; messages say "argument"
  instead of "config"; diagnostics are sorted by location.

### Deprecated
- The hand-written DSL parser. It moved to `GraphLangHandwritten.hpp` as
  `dsl::parseGraphProgramHandwritten`, marked `[[deprecated]]`, and will be
  removed in a future release. It produces the same results as
  `dsl::parseGraphProgram`; the tests check the two against each other.

## [0.2.0] - 2026-09-15

### Added
- **`JoinFilter<InputType, OutputType>`** — scatter-gather stage, the mirror of
  `FanoutFilter` (N paths → 1 output). Scatters one message through N paths and
  merges their gathered outputs via a C++ combiner; dropped paths leave holes
  the combiner can see, and `Void`-terminated paths are rejected at
  construction. `registerJoinFilter` builds one from JSON-configured paths.
- **`validateGraph(json)` / `Diagnostic`** — pre-flight config validation that
  walks a JSON graph without running messages and returns all problems at once
  (unknown/typo'd stage types with suggestions, structural mistakes, adjacent
  leaf type mismatches, bad/missing config), each located by a JSON pointer.
  Backed by new `FilterRegistry::contains` / `registeredNames`.
- **Text DSL front-end (`filterGraph::dsl`, `GraphLang.hpp`)** — parses a
  named-edge graph description (`edge -> Stage -> edge`, reserved `in`/`out`/
  `out.<key>`/`end`, fan-out by edge reuse, fan-in via `(a, b) -> Merge`,
  ordered/keyed outputs, dead-ends) into a node/edge IR with line:col
  diagnostics; `toMermaid` visualizes it. Text front-end only (execution is a
  planned follow-up).

## [0.1.0]

### Added
- Initial release: `MessageFilter`, compile-time `FilterGraph`, type-erased
  `AnyMessageFilter` / `AnyFilterChain`, `FilterRegistry`, `JsonFilterGraph`,
  `FanoutFilter`, `SinkFilter`, and the `Void` terminal marker.

[Unreleased]: https://github.com/psyinf/filterGraph/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/psyinf/filterGraph/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/psyinf/filterGraph/releases/tag/v0.1.0
