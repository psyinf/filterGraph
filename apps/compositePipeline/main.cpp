// Putting graphs together out of graphs — the composition features:
//  - JoinFilter: scatter one message through JSON-configured paths and combine
//    their results in C++, next to the equivalent DSL fan-out + merge
//  - registerTypedMergeFilter: a typed merge registered from a lambda
//  - a DslFilterGraph registered as a stage of an outer graph, with finish()
//    and the GraphContext propagating into it
//  - Void: a graph that only has side effects, with no consumable output
//  - the in-band tick message, the pattern that stands in for a tick() hook
//
// See EXAMPLE.md, "Composition: joins, nested graphs, sinks and ticks".
#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphContext.hpp>
#include <filterGraph/core/filterGraph/JoinFilter.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>
#include <filterGraph/core/filterGraph/Void.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <any>
#include <cctype>
#include <cstddef>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using filterGraph::DslFilterGraph;
using filterGraph::FilterRegistrar;
using filterGraph::GraphContext;
using filterGraph::JsonFilterGraph;
using filterGraph::MessageFilter;
using filterGraph::Void;
using filterGraph::registerJoinFilter;
using filterGraph::registerTypedMergeFilter;

namespace {

// --- Messages and context values ---------------------------------------

// A tick alternative in the input type is how a time-driven stage is fed:
// there is no tick() hook, because time is domain-specific (event time, wall
// clock, a sensor clock). Ticks travel through the graph like any other
// message, so the same graph shape handles both.
struct Tick
{
};

using Event = std::variant<std::string, Tick>;

// Published by the application, read by a stage inside the nested graph.
struct Tag
{
    std::string value;
};

// --- Stages ------------------------------------------------------------

class UpperFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return std::move(text);
    }
};

class ReverseFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        std::ranges::reverse(text);
        return std::move(text);
    }
};

// Prefixes the tag the application put into the context. This stage sits
// inside the nested graph, so seeing the tag proves that the outer graph's
// context reached it.
class TagFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        return std::format("{}{}", context().getOr(Tag{"<untagged> "}).value, text);
    }
};

// Counts what passes through and reports when the stream ends. It lives inside
// the nested graph, so its report shows that finish() reached the inner stages.
class CountFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        ++mSeen;
        return std::move(text);
    }

    void finish() override
    {
        std::cout << std::format("[nested] the inner stage saw {} message(s)\n", mSeen);
    }

private:
    std::size_t mSeen = 0;
};

// A sink: Void says "this path deliberately ends here", which is distinct from
// returning std::nullopt ("this message was dropped"). A Void stage must be the
// last one in its path.
class WriteFilter : public MessageFilter<std::string, Void>
{
public:
    std::optional<Void> filter(std::string&& text) override
    {
        std::cout << "[sink] " << text << '\n';
        return Void{};
    }
};

// A time-driven stage, fed by in-band ticks: it buffers the lines it receives
// and emits the batch when a Tick arrives. Between ticks it returns
// std::nullopt, so nothing downstream runs.
class BatchFilter : public MessageFilter<Event, std::string>
{
public:
    std::optional<std::string> filter(Event&& event) override
    {
        if (const auto* line = std::get_if<std::string>(&event))
        {
            mBatch.push_back(*line);
            return std::nullopt; // buffered; nothing to emit yet
        }

        if (mBatch.empty())
        {
            return std::nullopt; // a tick with nothing buffered
        }

        std::string batch;
        for (const auto& line : mBatch)
        {
            batch += (batch.empty() ? "" : ", ") + line;
        }
        mBatch.clear();
        return batch;
    }

    void finish() override
    {
        if (!mBatch.empty())
        {
            std::cout << std::format("[batch] {} line(s) left unflushed at the end of the stream\n", mBatch.size());
        }
    }

private:
    std::vector<std::string> mBatch;
};

// --- Registration ------------------------------------------------------

static FilterRegistrar<UpperFilter>   registerUpper("Upper");
static FilterRegistrar<ReverseFilter> registerReverse("Reverse");
static FilterRegistrar<TagFilter>     registerTag("Tag");
static FilterRegistrar<CountFilter>   registerCount("Count");
static FilterRegistrar<WriteFilter>   registerWrite("Write");
static FilterRegistrar<BatchFilter>   registerBatch("Batch");

// A DslFilterGraph is itself a MessageFilter, so a whole graph can be
// registered as a stage and used inside another graph.
static FilterRegistrar<DslFilterGraph<std::string, std::string>> registerInner("Inner", [](const nlohmann::json&) {
    return std::make_shared<DslFilterGraph<std::string, std::string>>("in -> Tag -> tagged -> Count -> out");
});

// A typed merge registered from a lambda: it declares one slot type per
// argument, so the DSL checks the group's edges when the graph is built.
static const bool sRegisterConcat = [] {
    registerTypedMergeFilter<std::string, std::string, std::string>(
        "Concat",
        [](std::optional<std::string>&& upper, std::optional<std::string>&& reversed) -> std::optional<std::string> {
            return std::format("{} | {}", upper.value_or("-"), reversed.value_or("-"));
        });
    return true;
}();

// The JSON counterpart of that fan-out + merge: a Join scatters a copy of the
// message through each configured path and hands the gathered results to a
// combiner written in C++ (paths come from configuration, the combiner cannot).
static const bool sRegisterJoin = [] {
    registerJoinFilter<std::string, std::string>("Join", [](std::vector<std::any>&& slots) -> std::optional<std::string> {
        std::string joined;
        for (auto& slot : slots)
        {
            joined += (joined.empty() ? "" : " | ");
            joined += slot.has_value() ? std::any_cast<std::string>(std::move(slot)) : std::string{"-"};
        }
        return joined;
    });
    return true;
}();

} // namespace

int main()
{
    // 1) Fan-out and merge in the DSL: both statements read `in`, so each gets
    //    its own copy, and the typed merge combines the two results.
    {
        DslFilterGraph<std::string, std::string> graph(R"dsl(
            in -> Upper   -> upper
            in -> Reverse -> reversed
            (upper, reversed) -> Concat -> out
        )dsl");

        std::cout << "[dsl]  " << *graph.filter(std::string{"compose me"}) << '\n';
    }

    // 2) The same shape in JSON, where the branching lives in a Join stage's
    //    config. The DSL expresses it as graph structure; JSON needs a
    //    composite stage, and the combiner is the same kind of C++ callable.
    {
        JsonFilterGraph<std::string, std::string> graph(nlohmann::json::parse(R"json([
            { "type": "Join", "config": { "paths": [
                [ { "type": "Upper" } ],
                [ { "type": "Reverse" } ]
            ] } }
        ])json"));

        std::cout << "[json] " << *graph.filter(std::string{"compose me"}) << "\n\n";
    }

    // 3) A nested graph as a stage. The outer graph hands its context to the
    //    stage — including into the inner graph's own stages — and finishing
    //    the outer graph finishes the inner ones too.
    {
        DslFilterGraph<std::string, std::string> graph("in -> Inner -> inner -> Upper -> out");
        graph.context().set(Tag{"[tagged] "});

        std::cout << "[outer] " << *graph.filter(std::string{"nested graphs compose"}) << '\n';
        std::cout << "[outer] " << *graph.filter(std::string{"and share a context"}) << '\n';

        graph.finish(); // reaches the Count stage inside the nested graph
        std::cout << '\n';
    }

    // 4) A graph with no consumable output: every path ends in a sink, so the
    //    graph's output type is Void. filter() returns a Void value (the graph
    //    ran), not a message.
    {
        DslFilterGraph<std::string, Void> graph(R"dsl(
            in -> Upper   -> upper    -> Write -> end
            in -> Reverse -> reversed -> Write -> end
        )dsl");

        const auto ran = graph.filter(std::string{"side effects only"});
        std::cout << std::format("[sink] the graph ran: {}\n\n", ran.has_value());
    }

    // 5) In-band ticks: the input type is a variant of the payload and a Tick,
    //    so a stage that has to act while no data arrives is driven by messages
    //    like every other stage. Between ticks the graph produces nothing.
    {
        DslFilterGraph<Event, std::string> graph("in -> Batch -> out");

        const std::vector<Event> stream{
            std::string{"first"},
            std::string{"second"},
            Tick{},
            std::string{"third"},
            Tick{},
            std::string{"unflushed"},
        };

        for (auto event : stream)
        {
            if (auto batch = graph.filter(std::move(event)))
            {
                std::cout << "[batch] " << *batch << '\n';
            }
        }

        graph.finish(); // reports what is still buffered
    }

    return 0;
}
