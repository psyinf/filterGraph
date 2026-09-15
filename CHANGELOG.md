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

### Changed
- README, EXAMPLE.md and `apps/textPipeline` now describe runtime graphs in the
  DSL first; the JSON format is documented as a supported alternative.
- `dsl::toMermaid` now uses generated node ids (instead of edge names such as
  `end` or the internal `$out0`), labels outputs as `out` / `out.<key>`, gives
  each dead end its own node, and lists stage arguments.
- DSL stages written without arguments now receive an empty config object
  instead of `null`, matching JSON stages without `"config"`.
- `dsl::parseGraphProgramLexy` now reports the same located, descriptive
  diagnostics as `parseGraphProgram` (previously every problem was a bare
  "syntax error" at column 1), gives stages their real columns, and supports
  decimal numbers, string escapes and `#` inside strings. Tests check both
  parsers against each other.
- Both DSL parsers: syntax errors say what was expected and what was found; a
  syntax error ends its statement instead of producing follow-on errors; an
  unterminated string no longer swallows the following lines; malformed or
  out-of-range numbers and empty fan-in groups `()` are reported; messages say
  "argument" instead of "config"; diagnostics are sorted by location.

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
