# filterGraph

A small, header-only C++20 library for building message/data processing
pipelines out of composable filter stages � both at **compile time** (fully
type-safe, zero-overhead composition) and at **runtime** (JSON-configured,
type-erased, with validation at construction time).

It grew out of a need to turn a fixed sequence of transformation steps into a
flexible, reconfigurable **filter graph**: chain stages, fan out to multiple
parallel branches, and drop/short-circuit messages � all without hard-coding
the pipeline shape in source code.

## Features

- **`MessageFilter<InputType, OutputType>`** � the base stage interface. A
  filter consumes `InputType&&` and returns `std::optional<OutputType>`;
  returning `std::nullopt` short-circuits (drops) the message, terminating
  the chain early.
- **`FilterGraph<Filters...>`** � compile-time, variadic-template composition
  of stages. Fully type-checked at compile time; each stage's `OutType` must
  match the next stage's `InType`. Zero runtime configuration overhead.
- **`AnyMessageFilter`** / **`AnyMessageFilterAdapter<Filter>`** � type-erased
  view of a `MessageFilter`, used to store/chain stages of different
  (otherwise incompatible) types at runtime.
- **`FilterRegistry`** / **`FilterRegistrar<FilterImpl>`** � a global registry
  mapping string names to filter factories, so a runtime configuration (e.g.
  JSON) can select and construct filters by name. Supports both
  parameterless filters and filters configured from a JSON object.
- **`AnyFilterChain`** � builds and validates a sequence of stages ("a path")
  from a JSON array, resolving each stage via `FilterRegistry`. Fails fast at
  construction time if two consecutive stages' types don't match.
- **`JsonFilterGraph<InputType, OutputType>`** � a typed wrapper around
  `AnyFilterChain`, exposing it as a regular `MessageFilter<InputType,
  OutputType>` so a JSON-configured pipeline can be used anywhere a
  compile-time one can.
- **`FanoutFilter<InputType>`** � duplicates an incoming message across
  multiple independent, multi-stage branches (side-effecting "taps": logging,
  forwarding, metrics, ...), then passes the *original* message through
  unchanged to the rest of the chain. Each branch is itself an
  `AnyFilterChain`, so branches can have several stages, not just one.
- **`SinkFilter<InputType>`** � a generic terminal stage that forwards data to
  a caller-supplied `std::function` callback and returns a simple status
  code, useful for terminating a compile-time `FilterGraph`.

## Installation (CPM)

```cmake
CPMAddPackage(
    NAME filterGraph
    GITHUB_REPOSITORY "psyinf/filterGraph"
    GIT_TAG v0.1.0 # or main
)

target_link_libraries(myTarget PRIVATE filterGraph::filterGraph)
```

`filterGraph` depends on [nlohmann/json](https://github.com/nlohmann/json),
which is pulled in transitively via CPM.

## Quick start

### Compile-time pipeline

```cpp
#include <filterGraph/core/filterGraph/FilterGraph.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

using filterGraph::FilterGraph;
using filterGraph::MessageFilter;

class Double : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override { return value * 2; }
};

class ToString : public MessageFilter<int, std::string>
{
public:
    std::optional<std::string> filter(int&& value) override { return std::to_string(value); }
};

FilterGraph<Double, ToString> pipeline(std::make_shared<Double>(), std::make_shared<ToString>());
auto result = pipeline.filter(21); // -> "42"
```

### Runtime, JSON-configured pipeline with a parallel branch

```cpp
#include <filterGraph/core/filterGraph/FanoutFilter.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>

using namespace filterGraph;

// Register stages once, e.g. near their definitions:
static FilterRegistrar<Double>   registerDouble("Double");
static FilterRegistrar<ToString> registerToString("ToString");
static const bool sRegisterFanout = [] { registerFanoutFilter<int>("Fanout"); return true; }();

auto config = nlohmann::json::parse(R"([
    { "type": "Fanout", "config": { "branches": [
        [ { "type": "Double" } ]
    ] } },
    { "type": "ToString" }
])");

JsonFilterGraph<int, std::string> pipeline(config);
auto result = pipeline.filter(21); // side branch computes 42 (discarded); main path -> "21"
```

See `apps/textPipeline` for a complete, runnable example (a small text
pipeline: uppercase/reverse/print, with a JSON-configured variant including a
fanout branch and a length-based filter).

## JSON pipeline schema

A pipeline (or a `FanoutFilter` branch) is described as an array of stages:

```json
[
  { "type": "StageName" },
  { "type": "OtherStage", "config": { "someParam": 123 } }
]
```

- `type` is the name a filter was registered under via `FilterRegistrar`.
- `config` is optional and is passed verbatim to the filter's registered
  creator function; its shape is entirely up to that filter.

A `FanoutFilter`'s config has one special key, `branches`: an array where
each entry is itself a full stage array (a path), not a single stage:

```json
{
  "type": "Fanout",
  "config": {
    "branches": [
      [ { "type": "StageA" } ],
      [ { "type": "StageB" }, { "type": "StageC" } ]
    ]
  }
}
```

## Building & testing

```powershell
cmake --preset <preset-from-CMakePresets.json>
cmake --build --preset <preset>
ctest --preset <preset>
```

Tests use Catch2 (fetched via CPM) and live under `tests/`.

## License

MIT, see [LICENSE](LICENSE).
