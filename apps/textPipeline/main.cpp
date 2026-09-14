// Generic, domain-unrelated example: a small text-processing pipeline that
// demonstrates the core building blocks of filterGraph:
//  - MessageFilter: the base stage interface
//  - FilterGraph: compile-time chaining of stages
//  - FanoutFilter + FilterRegistry + JsonFilterGraph: a runtime, JSON
//    configured pipeline with a parallel branch ("tap")
#include <filterGraph/core/filterGraph/FanoutFilter.hpp>
#include <filterGraph/core/filterGraph/FilterGraph.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

using filterGraph::FanoutFilter;
using filterGraph::FilterGraph;
using filterGraph::FilterRegistrar;
using filterGraph::JsonFilterGraph;
using filterGraph::MessageFilter;
using filterGraph::registerFanoutFilter;

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

// A configurable stage: drops (short-circuits) any message shorter than a
// configurable minimum length, demonstrating both JSON-driven configuration
// and early chain termination via std::nullopt.
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

// --- Registration for the JSON-driven part of the example --------------

static FilterRegistrar<UppercaseFilter> registerUppercase("Uppercase");
static FilterRegistrar<ReverseFilter>    registerReverse("Reverse");

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

    // 2) Runtime, JSON-configured pipeline with a parallel branch ("tap"):
    //    the original text is duplicated to a side branch (uppercase+print)
    //    while the main path reverses it, drops it if too short, and prints.
    {
        auto pipelineConfig = nlohmann::json::parse(R"([
            { "type": "Fanout", "config": { "branches": [
                [ { "type": "Uppercase" }, { "type": "Print", "config": { "prefix": "[branch] " } } ]
            ] } },
            { "type": "Reverse" },
            { "type": "MinLength", "config": { "minLength": 3 } },
            { "type": "Print", "config": { "prefix": "[main] " } }
        ])");

        JsonFilterGraph<std::string, int> pipeline(pipelineConfig);
        pipeline.filter(std::string{"Hello, filterGraph!"});
        pipeline.filter(std::string{"ab"}); // dropped by MinLength on the main path
    }

    return 0;
}