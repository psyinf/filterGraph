# filterGraph by example

This walkthrough follows the runnable sample in
[`apps/textPipeline/main.cpp`](apps/textPipeline/main.cpp), a small text
pipeline that exercises every core building block: a **compile-time**
`FilterGraph`, and a **runtime**, JSON-configured `JsonFilterGraph` with a
parallel `FanoutFilter` branch.

## Core concept: a chain of stages

Every stage is a `MessageFilter<InputType, OutputType>`. It consumes an
`InputType&&` and returns `std::optional<OutputType>`. Each stage's `OutType`
feeds the next stage's `InType`, forming a chain. Returning `std::nullopt`
**short-circuits** the chain: no later stage runs.

The short-circuit is handled by the container (`FilterGraph` /
`AnyFilterChain`), not by the stages. As soon as a stage returns
`std::nullopt`, the chain stops and propagates `std::nullopt` out as its own
result. A downstream stage only ever receives a real, unwrapped value, so
stages never have to handle an empty optional as input — their only
responsibility is deciding whether to emit `std::nullopt` themselves. Within a
`FanoutFilter` branch the same rule applies, but the termination is confined to
that branch and never reaches the main path.

A stage's last `OutputType` is normally the graph's own result: a path
typically ends in a stage that produces a real value (e.g. `JsonFilterGraph<In,
Out>` yields an `Out`). A stage may instead declare `MessageFilter<InputType,
Void>` to mark the path as a pure side-effect *sink* — it deliberately produces
no consumable output, so the path ends there. This is distinct from
`std::nullopt`, which means a single message was dropped or could not be
processed. A `Void` stage must be the last stage in a path.

```mermaid
flowchart LR
    In(["input: InType"]) --> A["Stage A<br/>MessageFilter&lt;In, Mid&gt;"]
    A -->|"optional&lt;Mid&gt;"| B["Stage B<br/>MessageFilter&lt;Mid, Out&gt;"]
    B -->|"optional&lt;Out&gt;"| Out(["output: Out"])
    A -. "nullopt" .-> Drop[["dropped<br/>(chain stops)"]]
    B -. "nullopt" .-> Drop
```

The whole chain is *itself* a `MessageFilter<FirstIn, LastOut>`, so a graph can
be nested inside another graph anywhere a single stage is expected.

## 1. Compile-time pipeline

The compile-time `FilterGraph<Filters...>` composes stages via variadic
templates. Types are checked by the compiler: each stage's `OutType` must equal
the next stage's `InType`, with zero runtime configuration overhead.

```cpp
using Pipeline = FilterGraph<UppercaseFilter, ReverseFilter, PrintFilter>;
Pipeline pipeline(
    std::make_shared<UppercaseFilter>(),
    std::make_shared<ReverseFilter>(),
    std::make_shared<PrintFilter>("[compile-time] "));

pipeline.filter(std::string{"Hello, filterGraph!"});
```

```mermaid
flowchart LR
    In(["std::string<br/>&quot;Hello, filterGraph!&quot;"]) --> U["UppercaseFilter<br/>MessageFilter&lt;string&gt;"]
    U -->|"HELLO, FILTERGRAPH!"| R["ReverseFilter<br/>MessageFilter&lt;string&gt;"]
    R -->|"!HPARGRETLIF ,OLLEH"| P["PrintFilter<br/>MessageFilter&lt;string, int&gt;"]
    P -->|"int status = 0"| Out(["stdout"])
```

Output:

```text
[compile-time] !HPARGRETLIF ,OLLEH
```

## 2. Runtime, JSON-configured pipeline with a fan-out branch

The same stages are registered by name (via `FilterRegistrar` /
`registerFanoutFilter`) and assembled at runtime from JSON. A `FanoutFilter`
duplicates the incoming message to one or more independent branches (each a
full multi-stage path), then passes the **original** message through unchanged
to the rest of the main path.

```json
[
    { "type": "Fanout", "config": { "branches": [
        [ { "type": "Uppercase" }, { "type": "Print", "config": { "prefix": "[branch] " } } ]
    ] } },
    { "type": "Reverse" },
    { "type": "MinLength", "config": { "minLength": 3 } },
    { "type": "Print", "config": { "prefix": "[main] " } }
]
```

```cpp
JsonFilterGraph<std::string, int> pipeline(pipelineConfig);
pipeline.filter(std::string{"Hello, filterGraph!"});
pipeline.filter(std::string{"ab"}); // dropped by MinLength on the main path
```

```mermaid
flowchart LR
    In(["std::string<br/>&quot;Hello, filterGraph!&quot;"]) --> F{{"FanoutFilter&lt;string&gt;"}}

    subgraph branch ["side branch (copy, output discarded)"]
        direction LR
        B1["Uppercase"] --> B2["Print<br/>prefix=[branch]"]
    end

    F -. "copy" .-> B1
    F ==>|"original, unchanged"| R["Reverse"]
    R -->|"!HPARGRETLIF ,OLLEH"| M["MinLength<br/>minLength=3"]
    M ==>|"len &ge; 3"| P["Print<br/>prefix=[main]"]
    M -. "len &lt; 3" .-> Drop[["dropped"]]
    P --> Out(["stdout"])
```

Output for the two inputs:

```text
[branch] HELLO, FILTERGRAPH!
[main] !HPARGRETLIF ,OLLEH
```

The second call, `"ab"`, is copied to the branch (which uppercases and prints
it), but on the main path `MinLength` returns `std::nullopt`, so it is dropped
before the final `[main]` print.

> Note: branch order vs. main-path order is a side-effect of how the fanout
> runs its branches before returning the original message; the branch print
> happens first.

### Which branch contributes to the output?

None of them. A `FanoutFilter` is a pure *tap*: it hands each branch its own
copy of the message, runs every branch to completion, and **discards each
branch's result**. Whatever the branches compute (or whether a branch
short-circuits with `std::nullopt`) never affects the main path — the value the
fanout forwards downstream is always the unchanged original input. This holds
regardless of how many branches there are or in what order they run; branches
matter only for their side effects (logging, forwarding, metrics). If you need
a branch's result to feed the output, that requires a separate merge/reduce
stage — `FanoutFilter` deliberately does not merge.

## How construction-time validation works

`AnyFilterChain` resolves each stage by name through the global
`FilterRegistry`, then checks that consecutive stages' erased types line up
(`outputType()` of one must equal `inputType()` of the next). `JsonFilterGraph`
additionally validates that the chain's first `inputType()` and last
`outputType()` match its own `InputType`/`OutputType`. Any mismatch throws at
construction time — the pipeline fails fast, before a single message flows.

```mermaid
flowchart TD
    Json["JSON stage array"] --> Chain["AnyFilterChain"]
    Chain -->|"lookup by type name"| Reg[("FilterRegistry")]
    Reg -->|"creator(config)"| Stage["AnyMessageFilter<br/>(type-erased stage)"]
    Chain -->|"outputType == next inputType?"| Check{"types line up?"}
    Check -->|"no"| Throw[["throw std::runtime_error"]]
    Check -->|"yes"| Ok(["validated chain"])
```

## Build & run this example

```powershell
# Configure + build (pick a preset for your toolchain from CMakePresets.json)
cmake --preset windows-msvc-release-user-mode
cmake --build --preset windows-msvc-release-user-mode

# Run the sample
./out/build/windows-msvc-release-user-mode/apps/textPipeline/textPipeline
```

On Linux/macOS, use a matching preset such as `unixlike-gcc-release` or
`unixlike-clang-release`.
