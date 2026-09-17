#pragma once

#include <filterGraph/core/filterGraph/AnyMessageFilter.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphLang.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>
#include <filterGraph/core/filterGraph/Void.hpp>

#include <algorithm>
#include <any>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// DslFilterGraph runs a filter graph described in the text DSL (syntax: see
// GraphLang.hpp), e.g.
//
//   in -> Uppercase -> shouted -> Print(prefix="[tap] ") -> end
//   in -> Reverse -> reversed -> MinLength(minLength=3) -> long -> Print -> out
//
// Execution semantics, per message:
//  - stages run in source order, except that a stage waits until every edge it
//    reads has been produced;
//  - fan-out: every reader of an edge gets its own copy of the value (the last
//    reader receives it by move);
//  - a stage returning std::nullopt leaves its edge empty; stages reading an
//    empty edge are skipped, so the drop propagates downstream;
//  - a merge `(a, b) -> Merge` receives one MergeInputs slot per edge, with
//    empty slots ("holes") for dropped edges; it is skipped only when every
//    edge is empty;
//  - values routed to `end` are discarded; a stage producing Void must route
//    to `end`.
namespace filterGraph {

// The results of a graph with several outputs: one slot per `-> out` /
// `-> out.<key>`, in DSL order. An empty slot means that output's path dropped
// the message.
class GraphOutputs
{
public:
    using Keys = std::vector<std::optional<std::string>>;

    GraphOutputs(std::vector<std::any> slots, std::shared_ptr<const Keys> keys)
        : mSlots(std::move(slots))
        , mKeys(std::move(keys))
    {
    }

    std::size_t size() const
    {
        return mSlots.size();
    }

    const Keys& keys() const
    {
        return *mKeys;
    }

    bool has(std::size_t index) const
    {
        return mSlots.at(index).has_value();
    }

    bool has(std::string_view key) const
    {
        return has(indexOf(key));
    }

    // Returns a copy of the output, or std::nullopt if its path dropped the
    // message. Throws std::bad_any_cast if T is not the output's type.
    template <typename T>
    std::optional<T> get(std::size_t index) const
    {
        const std::any& slot = mSlots.at(index);
        if (!slot.has_value())
        {
            return std::nullopt;
        }
        return std::any_cast<const T&>(slot);
    }

    template <typename T>
    std::optional<T> get(std::string_view key) const
    {
        return get<T>(indexOf(key));
    }

    // Like get(), but moves the value out and leaves the slot empty.
    template <typename T>
    std::optional<T> take(std::size_t index)
    {
        std::any& slot = mSlots.at(index);
        if (!slot.has_value())
        {
            return std::nullopt;
        }
        std::optional<T> value = std::any_cast<T>(std::move(slot));
        slot.reset();
        return value;
    }

    template <typename T>
    std::optional<T> take(std::string_view key)
    {
        return take<T>(indexOf(key));
    }

private:
    std::size_t indexOf(std::string_view key) const
    {
        for (std::size_t i = 0; i < mKeys->size(); ++i)
        {
            if ((*mKeys)[i] && *(*mKeys)[i] == key)
            {
                return i;
            }
        }
        throw std::out_of_range(std::format("GraphOutputs: the graph has no output named '{}'", key));
    }

    std::vector<std::any>       mSlots;
    std::shared_ptr<const Keys> mKeys;
};

// Thrown when a DSL graph cannot be built. what() lists every problem as
// "line:column: message"; diagnostics() gives them individually.
class GraphError : public std::runtime_error
{
public:
    explicit GraphError(std::vector<dsl::TextDiagnostic> diagnostics)
        : std::runtime_error("DslFilterGraph: invalid graph\n" + dsl::formatDiagnostics(diagnostics))
        , mDiagnostics(std::move(diagnostics))
    {
    }

    const std::vector<dsl::TextDiagnostic>& diagnostics() const noexcept
    {
        return mDiagnostics;
    }

private:
    std::vector<dsl::TextDiagnostic> mDiagnostics;
};

namespace dsl::detail {

// What a typed graph expects at its output boundary.
enum class OutputShape
{
    Single,   // exactly one `-> out` of the graph's OutputType
    Multiple, // one or more outputs, returned as GraphOutputs
    None      // no outputs at all: a pure sink graph (OutputType = Void)
};

struct ExpectedOutputs
{
    OutputShape     shape;
    std::type_index type;
};

template <typename OutputType>
ExpectedOutputs expectedOutputs()
{
    if (std::is_same_v<OutputType, GraphOutputs>)
    {
        return {OutputShape::Multiple, typeid(GraphOutputs)};
    }
    if (std::is_same_v<OutputType, Void>)
    {
        return {OutputShape::None, typeid(Void)};
    }
    return {OutputShape::Single, typeid(OutputType)};
}

// Stage indices in run order: source order, except that a stage waits until all
// of its inputs have been produced. Stages that can never run (an input is
// never produced, or a cycle) are left out.
inline std::vector<std::size_t> runOrder(const GraphProgram& program)
{
    std::unordered_set<std::string> produced{"in"};
    std::vector<bool>               scheduled(program.stages.size(), false);
    std::vector<std::size_t>        order;

    bool progress = true;
    while (progress)
    {
        progress = false;
        for (std::size_t i = 0; i < program.stages.size(); ++i)
        {
            const StageNode& node = program.stages[i];
            const bool       ready = std::all_of(node.inputs.begin(), node.inputs.end(), [&](const std::string& edge) {
                return produced.contains(edge);
            });
            if (scheduled[i] || !ready)
            {
                continue;
            }
            scheduled[i] = true;
            order.push_back(i);
            if (node.output)
            {
                produced.insert(*node.output);
            }
            progress = true;
            break; // rescan from the top so earlier statements keep priority
        }
    }
    return order;
}

inline std::string describeEdge(const std::string& edge)
{
    if (edge == "in")
    {
        return "the graph input 'in'";
    }
    return std::format("edge '{}'", edge);
}

// Checks the edges of a fan-in group against the slot types the merge stage
// declares (MergeStage); an untyped merge declares none and is not checked.
inline void checkMergeInputs(const StageNode&                                        node,
                             const MergeSlotTypes&                                   slots,
                             const std::unordered_map<std::string, std::type_index>& edgeTypes,
                             std::vector<TextDiagnostic>&                            diagnostics)
{
    if (slots.empty())
    {
        return;
    }

    if (!slots.uniform && slots.types.size() != node.inputs.size())
    {
        // Without a slot-to-edge correspondence, per-slot messages would only
        // repeat this one.
        diagnostics.push_back({node.loc,
                               std::format("stage '{}' takes {} inputs but the group has {}",
                                           node.type,
                                           slots.types.size(),
                                           node.inputs.size())});
        return;
    }

    for (std::size_t slot = 0; slot < node.inputs.size(); ++slot)
    {
        const std::type_index expected = slots.uniform ? slots.types.front() : slots.types[slot];
        const auto            type     = edgeTypes.find(node.inputs[slot]);
        if (type != edgeTypes.end() && type->second != expected)
        {
            diagnostics.push_back({node.loc,
                                   std::format("slot {} of '{}' expects '{}' but {} carries '{}'",
                                               slot + 1,
                                               node.type,
                                               expected.name(),
                                               describeEdge(node.inputs[slot]),
                                               type->second.name())});
        }
    }
}

struct CompiledStage
{
    std::shared_ptr<AnyMessageFilter> filter;
    std::vector<std::size_t>          inputs;         // edge slots read by the stage
    std::optional<std::size_t>        output;         // edge slot written; nullopt => `end`
    bool                              merge = false;  // inputs are gathered into MergeInputs
};

// The executable form of a GraphProgram: every stage instantiated through the
// FilterRegistry, type-checked, and wired to numbered edge slots in run order.
class GraphPlan
{
public:
    GraphPlan() = default;

    // Builds the plan, appending every problem found (bad config, type
    // mismatches along edges, misused merges/Void, output shape) to
    // `diagnostics`. The plan may only be run if no diagnostics were added.
    GraphPlan(const GraphProgram&          program,
              std::type_index              inputType,
              const ExpectedOutputs&       expected,
              std::vector<TextDiagnostic>& diagnostics)
    {
        std::unordered_map<std::string, std::size_t> slots;
        auto slotOf = [&slots](const std::string& edge) {
            return slots.try_emplace(edge, slots.size()).first->second;
        };
        slotOf("in"); // always slot 0 (kInputSlot)

        if (program.stages.empty())
        {
            diagnostics.push_back({SourceLoc{}, "the graph has no stages; add a statement such as 'in -> Stage -> out'"});
        }

        const std::vector<std::size_t> order = runOrder(program);
        if (order.size() != program.stages.size() && program.ok())
        {
            diagnostics.push_back({SourceLoc{}, "some stages can never run: an input is never produced or depends on itself"});
        }

        auto&                                            registry = FilterRegistry::instance();
        std::unordered_map<std::string, std::type_index> edgeTypes;
        edgeTypes.emplace("in", inputType);

        for (std::size_t index : order)
        {
            const StageNode& node = program.stages[index];
            if (!registry.contains(node.type))
            {
                continue; // already reported by parseGraphProgram; its output stays untyped
            }

            std::shared_ptr<AnyMessageFilter> filter;
            try
            {
                filter = registry.create(node.type, node.config);
            }
            catch (const std::exception& error)
            {
                diagnostics.push_back({node.loc, std::format("could not construct '{}': {}", node.type, error.what())});
                continue;
            }

            const bool merge = filter->inputType() == std::type_index(typeid(MergeInputs));
            if (node.fanIn && !merge)
            {
                diagnostics.push_back(
                    {node.loc,
                     std::format("stage '{}' takes a single input and cannot follow a fan-in group; combine the "
                                 "edges with a merge stage (see registerMergeFilter)",
                                 node.type)});
            }
            else if (!node.fanIn && merge)
            {
                diagnostics.push_back(
                    {node.loc,
                     std::format("'{}' is a merge stage; feed it a fan-in group, e.g. '(a, b) -> {} -> merged'",
                                 node.type,
                                 node.type)});
            }
            else if (merge)
            {
                checkMergeInputs(node, filter->mergeInputTypes(), edgeTypes, diagnostics);
            }
            else
            {
                const std::string& edge = node.inputs.front();
                auto               type = edgeTypes.find(edge);
                if (type != edgeTypes.end() && type->second != filter->inputType())
                {
                    diagnostics.push_back({node.loc,
                                           std::format("stage '{}' expects input type '{}' but {} carries '{}'",
                                                       node.type,
                                                       filter->inputType().name(),
                                                       describeEdge(edge),
                                                       type->second.name())});
                }
            }

            if (node.output)
            {
                if (filter->outputType() == std::type_index(typeid(Void)))
                {
                    diagnostics.push_back(
                        {node.loc,
                         std::format("stage '{}' produces Void (no value), so it must route to 'end'", node.type)});
                }
                else
                {
                    edgeTypes.emplace(*node.output, filter->outputType());
                }
            }

            CompiledStage stage;
            stage.filter = std::move(filter);
            stage.merge  = merge;
            for (const auto& edge : node.inputs)
            {
                stage.inputs.push_back(slotOf(edge));
            }
            if (node.output)
            {
                stage.output = slotOf(*node.output);
            }
            mStages.push_back(std::move(stage));
        }

        auto keys = std::make_shared<GraphOutputs::Keys>();
        for (const auto& binding : program.outputs)
        {
            mOutputEdges.push_back(slotOf(binding.edge));
            keys->push_back(binding.key);
        }
        mOutputKeys = std::move(keys);

        checkOutputs(program, expected, edgeTypes, diagnostics);

        mEdgeCount = slots.size();
        mReaders.assign(mEdgeCount, 0);
        for (const auto& stage : mStages)
        {
            for (std::size_t edge : stage.inputs)
            {
                ++mReaders[edge];
            }
        }
    }

    // Runs one message through the graph and returns the output slots, in DSL
    // order; an empty slot means that output's path dropped the message.
    std::vector<std::any> run(std::any&& input) const
    {
        std::vector<std::any>    edges(mEdgeCount);
        std::vector<std::size_t> remaining = mReaders;
        edges[kInputSlot]                  = std::move(input);

        for (const auto& stage : mStages)
        {
            std::optional<std::any> result;
            if (stage.merge)
            {
                MergeInputs gathered;
                gathered.reserve(stage.inputs.size());
                for (std::size_t edge : stage.inputs)
                {
                    gathered.push_back(read(edges, remaining, edge));
                }
                const bool anyPresent = std::any_of(gathered.begin(), gathered.end(), [](const std::any& slot) {
                    return slot.has_value();
                });
                if (!anyPresent)
                {
                    continue; // every path into the merge dropped the message
                }
                result = stage.filter->filter(std::any(std::move(gathered)));
            }
            else
            {
                std::any value = read(edges, remaining, stage.inputs.front());
                if (!value.has_value())
                {
                    continue; // dropped upstream
                }
                result = stage.filter->filter(std::move(value));
            }

            if (result && stage.output)
            {
                edges[*stage.output] = std::move(*result);
            }
        }

        std::vector<std::any> outputs;
        outputs.reserve(mOutputEdges.size());
        for (std::size_t edge : mOutputEdges)
        {
            outputs.push_back(std::move(edges[edge]));
        }
        return outputs;
    }

    const std::shared_ptr<const GraphOutputs::Keys>& outputKeys() const
    {
        return mOutputKeys;
    }

    void setContext(const std::shared_ptr<GraphContext>& context)
    {
        for (auto& stage : mStages)
        {
            stage.filter->setContext(context);
        }
    }

    // Finishes every stage in run order, so that a stage sees its upstream
    // stages finished before itself.
    void finish()
    {
        filterGraph::detail::FinishScope finished;
        for (auto& stage : mStages)
        {
            finished.run([&stage] { stage.filter->finish(); });
        }
        finished.rethrow();
    }

private:
    static constexpr std::size_t kInputSlot = 0;

    // The last reader of an edge takes the value by move; earlier readers
    // (fan-out) each get their own copy.
    static std::any read(std::vector<std::any>& edges, std::vector<std::size_t>& remaining, std::size_t edge)
    {
        if (--remaining[edge] == 0)
        {
            return std::move(edges[edge]);
        }
        return edges[edge];
    }

    static void checkOutputs(const GraphProgram&                                     program,
                             const ExpectedOutputs&                                  expected,
                             const std::unordered_map<std::string, std::type_index>& edgeTypes,
                             std::vector<TextDiagnostic>&                            diagnostics)
    {
        const auto& outputs = program.outputs;
        switch (expected.shape)
        {
        case OutputShape::Single:
            if (outputs.empty())
            {
                diagnostics.push_back(
                    {SourceLoc{},
                     std::format("the graph's output type '{}' needs one '-> out', but the graph has none "
                                 "(use Void as the output type for a graph without outputs)",
                                 expected.type.name())});
            }
            else if (outputs.size() > 1)
            {
                diagnostics.push_back(
                    {outputs[1].loc,
                     std::format("the graph's output type '{}' needs exactly one '-> out', but the graph has {} "
                                 "(use GraphOutputs as the output type to receive several)",
                                 expected.type.name(),
                                 outputs.size())});
            }
            else if (auto type = edgeTypes.find(outputs[0].edge);
                     type != edgeTypes.end() && type->second != expected.type)
            {
                diagnostics.push_back({outputs[0].loc,
                                       std::format("the graph's output type is '{}' but 'out' receives '{}'",
                                                   expected.type.name(),
                                                   type->second.name())});
            }
            break;
        case OutputShape::Multiple:
            if (outputs.empty())
            {
                diagnostics.push_back({SourceLoc{},
                                       "GraphOutputs needs at least one '-> out' (use Void as the output type for a "
                                       "graph without outputs)"});
            }
            break;
        case OutputShape::None:
            if (!outputs.empty())
            {
                diagnostics.push_back({outputs[0].loc,
                                       "the graph's output type is Void, but the graph routes a value to 'out'; "
                                       "route it to 'end' instead"});
            }
            break;
        }
    }

    std::vector<CompiledStage>                mStages;
    std::size_t                               mEdgeCount = 0;
    std::vector<std::size_t>                  mReaders; // per edge slot: number of stage inputs reading it
    std::vector<std::size_t>                  mOutputEdges;
    std::shared_ptr<const GraphOutputs::Keys> mOutputKeys;
};

} // namespace dsl::detail

// DslFilterGraph<InputType, OutputType> builds a graph from DSL text (or an
// already parsed GraphProgram) and exposes it as a MessageFilter, so it plugs
// in anywhere a compile-time FilterGraph or a JsonFilterGraph can.
//
// OutputType selects the output boundary the graph must have:
//  - any concrete type: exactly one `-> out` producing that type;
//    std::nullopt when its path drops the message;
//  - GraphOutputs: one or more (optionally keyed) outputs of any types;
//    std::nullopt only when every output was dropped;
//  - Void: no outputs, only `-> end` (a pure sink graph).
//
// Construction instantiates every stage and checks the whole graph up front:
// syntax, unknown stages, bad config, type mismatches along every edge, merges
// and Void used correctly, and the output shape. All problems are thrown
// together in one GraphError; use validateDslGraph to get them without an
// exception.
template <typename InputType, typename OutputType = GraphOutputs>
class DslFilterGraph : public MessageFilter<InputType, OutputType>
{
public:
    explicit DslFilterGraph(std::string_view text)
        : DslFilterGraph(dsl::parseGraphProgram(text))
    {
    }

    explicit DslFilterGraph(dsl::GraphProgram program)
        : mProgram(std::move(program))
    {
        std::vector<dsl::TextDiagnostic> diagnostics = mProgram.diagnostics;
        mPlan = dsl::detail::GraphPlan(
            mProgram, typeid(InputType), dsl::detail::expectedOutputs<OutputType>(), diagnostics);
        if (!diagnostics.empty())
        {
            dsl::detail::sortDiagnostics(diagnostics);
            throw GraphError(std::move(diagnostics));
        }

        DslFilterGraph::setContext(this->sharedContext());
    }

    std::optional<OutputType> filter(InputType&& data) override
    {
        std::vector<std::any> outputs = mPlan.run(std::any(std::move(data)));

        if constexpr (std::is_same_v<OutputType, Void>)
        {
            return Void{};
        }
        else if constexpr (std::is_same_v<OutputType, GraphOutputs>)
        {
            const bool anyPresent = std::any_of(outputs.begin(), outputs.end(), [](const std::any& slot) {
                return slot.has_value();
            });
            if (!anyPresent)
            {
                return std::nullopt;
            }
            return GraphOutputs(std::move(outputs), mPlan.outputKeys());
        }
        else
        {
            if (!outputs.front().has_value())
            {
                return std::nullopt;
            }
            return std::any_cast<OutputType>(std::move(outputs.front()));
        }
    }

    void setContext(std::shared_ptr<GraphContext> context) override
    {
        mPlan.setContext(context);
        MessageFilter<InputType, OutputType>::setContext(std::move(context));
    }

    // Finishes every stage of the graph, in run order; nested graphs are
    // stages, so they finish their own stages in turn. See
    // MessageFilter::finish.
    void finish() override
    {
        mPlan.finish();
    }

    // The parsed graph, e.g. for dsl::toMermaid.
    const dsl::GraphProgram& program() const
    {
        return mProgram;
    }

private:
    dsl::GraphProgram       mProgram;
    dsl::detail::GraphPlan  mPlan;
};

// Checks DSL text for use as DslFilterGraph<InputType, OutputType> without
// running any messages, and returns every problem found (sorted by location)
// instead of throwing. Stages are instantiated to check their config and types.
template <typename InputType, typename OutputType = GraphOutputs>
std::vector<dsl::TextDiagnostic> validateDslGraph(std::string_view text)
{
    const dsl::GraphProgram          program     = dsl::parseGraphProgram(text);
    std::vector<dsl::TextDiagnostic> diagnostics = program.diagnostics;
    [[maybe_unused]] const dsl::detail::GraphPlan plan(
        program, typeid(InputType), dsl::detail::expectedOutputs<OutputType>(), diagnostics);
    dsl::detail::sortDiagnostics(diagnostics);
    return diagnostics;
}

} // namespace filterGraph
