// Generic, domain-unrelated example: a small text-processing pipeline that
// demonstrates the core building blocks of filterGraph:
//  - MessageFilter: the base stage interface
//  - FilterGraph: compile-time chaining of stages
//  - FilterRegistry + DslFilterGraph: runtime graphs described in the text DSL,
//    with fan-out (a "tap"), fan-in (a merge) and several named outputs
//  - validateDslGraph: every problem in a broken graph, located by line:column
//  - JsonFilterGraph: the JSON format, which remains supported
#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FanoutFilter.hpp>
#include <filterGraph/core/filterGraph/FilterGraph.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

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

using filterGraph::DslFilterGraph;
using filterGraph::FilterGraph;
using filterGraph::FilterRegistrar;
using filterGraph::JsonFilterGraph;
using filterGraph::MergeInputs;
using filterGraph::MessageFilter;
using filterGraph::registerFanoutFilter;
using filterGraph::registerMergeFilter;
using filterGraph::validateDslGraph;

namespace dsl = filterGraph::dsl;

// --- Stages -----------------------------------------------------------

class UppercaseFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return text;
    }
};

class ReverseFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        std::ranges::reverse(text);
        return text;
    }
};

// Changes the message type: string in, length out.
class LengthFilter : public MessageFilter<std::string, std::size_t>
{
public:
    std::optional<std::size_t> filter(std::string&& text) override
    {
        return text.size();
    }
};

// A configurable stage: drops (short-circuits) any message shorter than a
// configurable minimum length, demonstrating both configuration and early
// termination via std::nullopt.
class MinLengthFilter : public MessageFilter<std::string>
{
public:
    explicit MinLengthFilter(std::size_t minLength)
        : mMinLength(minLength)
    {
    }

    std::optional<std::string> filter(std::string&& text) override
    {
        if (text.size() < mMinLength)
        {
            return std::nullopt;
        }
        return text;
    }

private:
    std::size_t mMinLength;
};

class PrintFilter : public MessageFilter<std::string, int>
{
public:
    explicit PrintFilter(std::string prefix = {})
        : mPrefix(std::move(prefix))
    {
    }

    std::optional<int> filter(std::string&& text) override
    {
        std::cout << mPrefix << text << std::endl;
        return 0;
    }

private:
    std::string mPrefix;
};

// --- Registration: the names the DSL (and JSON) refer to ---------------

static FilterRegistrar<UppercaseFilter> registerUppercase("Uppercase");
static FilterRegistrar<ReverseFilter>   registerReverse("Reverse");
static FilterRegistrar<LengthFilter>    registerLength("Length");

// Stage arguments, e.g. `MinLength(minLength=3)`, arrive as a JSON object.
static FilterRegistrar<MinLengthFilter> registerMinLength(
    "MinLength",
    [](const nlohmann::json& config) {
        return std::make_shared<MinLengthFilter>(config.at("minLength").get<std::size_t>());
    });

static FilterRegistrar<PrintFilter> registerPrint(
    "Print",
    [](const nlohmann::json& config) {
        return std::make_shared<PrintFilter>(config.value("prefix", std::string{}));
    });

// A merge stage for fan-in `(a, b, c) -> Concat`. The combiner receives one
// slot per edge; empty slots are "holes" left by paths that dropped the
// message. It concatenates the present values with " | " and counts the holes.
static const bool sRegisterConcat = [] {
    registerMergeFilter<std::string>(
        "Concat",
        [](MergeInputs&& inputs) -> std::optional<std::string> {
            std::string joined;
            std::size_t holes = 0;
            for (auto& input : inputs)
            {
                if (!input.has_value())
                {
                    ++holes;
                    continue;
                }
                if (!joined.empty())
                {
                    joined += " | ";
                }
                joined += std::any_cast<std::string>(input);
            }
            return joined + std::format(" ({} hole{})", holes, holes == 1 ? "" : "s");
        });
    return true;
}();

// Only needed by the JSON example: in the DSL, fan-out is built in.
static const bool sRegisterFanout = [] {
    registerFanoutFilter<std::string>("Fanout");
    return true;
}();

int main()
{
    // 1) Compile-time pipeline: FilterGraph<Uppercase, Reverse, Print>
    {
        using Pipeline = FilterGraph<UppercaseFilter, ReverseFilter, PrintFilter>;
        Pipeline pipeline(
            std::make_shared<UppercaseFilter>(),
            std::make_shared<ReverseFilter>(),
            std::make_shared<PrintFilter>("[compile-time] "));

        pipeline.filter(std::string{"Hello, filterGraph!"});
    }

    // 2) Runtime graph in the text DSL, with a tap. Both statements read `in`,
    //    so each gets its own copy: the first uppercases and prints its copy
    //    (then discards it at `end`); the second reverses the message, drops it
    //    if it is too short, and prints it as the graph's output.
    {
        DslFilterGraph<std::string, int> pipeline(R"dsl(
            in -> Uppercase -> shouted -> Print(prefix="[tap] ") -> end
            in -> Reverse -> reversed -> MinLength(minLength=3) -> long -> Print(prefix="[main] ") -> out
        )dsl");

        pipeline.filter(std::string{"Hello, filterGraph!"});
        pipeline.filter(std::string{"ab"}); // tapped, then dropped by MinLength on the main path
    }

    // 3) Fan-in: three paths read the same input and a merge combines them.
    //    MinLength=100 drops its copy, which leaves a hole in the merge.
    {
        DslFilterGraph<std::string, int> pipeline(R"dsl(
            in -> Uppercase -> upper
            in -> Reverse -> reversed
            in -> MinLength(minLength=100) -> long
            (upper, reversed, long) -> Concat -> joined -> Print(prefix="[merge] ") -> out
        )dsl");

        pipeline.filter(std::string{"Hello, filterGraph!"});
    }

    // 4) Several named outputs of different types, returned as GraphOutputs.
    {
        DslFilterGraph<std::string> analysis(R"dsl(
            in -> Uppercase -> out.upper
            in -> Length -> out.length
            in -> MinLength(minLength=100) -> out.long
        )dsl");

        auto outputs = analysis.filter(std::string{"Hello, filterGraph!"});
        std::cout << std::format("[outputs] upper={} length={} long={}\n",
                                 *outputs->get<std::string>("upper"),
                                 *outputs->get<std::size_t>("length"),
                                 outputs->has("long") ? "present" : "dropped");
    }

    // 5) Problems are reported before anything runs: all of them at once, each
    //    located by line:column. Constructing a DslFilterGraph from this text
    //    would throw a GraphError carrying the same diagnostics.
    {
        const auto diagnostics = validateDslGraph<std::string, int>(
            "in -> Uppercas -> shouted -> Print -> out\n"
            "in -> MinLength -> long -> Print -> end\n");

        for (const auto& diagnostic : diagnostics)
        {
            std::cout << "[check] " << dsl::formatDiagnostic(diagnostic) << '\n';
        }
    }

    // 6) The JSON format is still supported and uses the same registry. This is
    //    the tap pipeline from (2), written with a Fanout stage.
    {
        auto pipelineConfig = nlohmann::json::parse(R"json([
            { "type": "Fanout", "config": { "branches": [
                [ { "type": "Uppercase" }, { "type": "Print", "config": { "prefix": "[json tap] " } } ]
            ] } },
            { "type": "Reverse" },
            { "type": "MinLength", "config": { "minLength": 3 } },
            { "type": "Print", "config": { "prefix": "[json main] " } }
        ])json");

        JsonFilterGraph<std::string, int> pipeline(pipelineConfig);
        pipeline.filter(std::string{"Hello, filterGraph!"});
    }

    return 0;
}
