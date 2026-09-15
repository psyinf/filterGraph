# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
