#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FanoutFilter.hpp>
#include <filterGraph/core/filterGraph/FilterGraph.hpp>
#include <filterGraph/core/filterGraph/JoinFilter.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <any>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace filterGraph;

namespace {

// Every stage appends to this log, so a test can check what ran and in which
// order. Each test clears it first.
std::vector<std::string>& lifecycleLog()
{
    static std::vector<std::string> log;
    return log;
}

// Sums the values it sees and writes the total to the log when it is finished:
// the pattern finish() exists for.
class SumFilter : public MessageFilter<int>
{
public:
    explicit SumFilter(std::string label)
        : mLabel(std::move(label))
    {
    }

    std::optional<int> filter(int&& value) override
    {
        mSum += value;
        return value;
    }

    void finish() override
    {
        lifecycleLog().push_back(std::format("{}={}", mLabel, mSum));
    }

private:
    std::string mLabel;
    int         mSum = 0;
};

// A stage that does not override finish(): the default is a no-op.
class PassThroughFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override
    {
        return value;
    }
};

class ThrowOnFinishFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override
    {
        return value;
    }

    void finish() override
    {
        lifecycleLog().push_back("throwing");
        throw std::runtime_error("finish failed");
    }
};

class ToStringFilter : public MessageFilter<int, std::string>
{
public:
    std::optional<std::string> filter(int&& value) override
    {
        return std::to_string(value);
    }
};

static FilterRegistrar<PassThroughFilter>   registerPass("LifePass");
static FilterRegistrar<ToStringFilter>      registerToString("LifeToString");
static FilterRegistrar<ThrowOnFinishFilter> registerThrow("LifeThrow");

static FilterRegistrar<SumFilter> registerSum("LifeSum", [](const nlohmann::json& config) {
    return std::make_shared<SumFilter>(config.value("label", std::string{"sum"}));
});

// A nested DSL graph, registered as a stage.
static FilterRegistrar<DslFilterGraph<int, int>> registerNested("LifeNested", [](const nlohmann::json&) {
    return std::make_shared<DslFilterGraph<int, int>>("in -> LifeSum(label=inner) -> out");
});

} // namespace

TEST_CASE("DslFilterGraph finishes every stage in run order", "[Lifecycle]")
{
    lifecycleLog().clear();
    DslFilterGraph<int, int> graph(R"(
        in -> LifeSum(label=first) -> a
        a -> LifePass -> b
        b -> LifeSum(label=second) -> out
    )");

    graph.filter(1);
    graph.filter(2);
    REQUIRE(lifecycleLog().empty()); // nothing happens until the graph is finished

    graph.finish();
    REQUIRE(lifecycleLog() == std::vector<std::string>{"first=3", "second=3"});
}

TEST_CASE("A nested graph forwards finish to its own stages", "[Lifecycle]")
{
    lifecycleLog().clear();
    DslFilterGraph<int, int> graph("in -> LifeNested -> nested -> LifeSum(label=outer) -> out");

    graph.filter(4);
    graph.finish();

    REQUIRE(lifecycleLog() == std::vector<std::string>{"inner=4", "outer=4"});
}

TEST_CASE("The compile-time FilterGraph forwards finish", "[Lifecycle]")
{
    lifecycleLog().clear();
    FilterGraph<SumFilter, PassThroughFilter, SumFilter> graph(std::make_shared<SumFilter>("head"),
                                                               std::make_shared<PassThroughFilter>(),
                                                               std::make_shared<SumFilter>("tail"));

    graph.filter(5);
    graph.finish();

    REQUIRE(lifecycleLog() == std::vector<std::string>{"head=5", "tail=5"});
}

TEST_CASE("JsonFilterGraph forwards finish through fanout branches and join paths", "[Lifecycle]")
{
    lifecycleLog().clear();
    registerFanoutFilter<int>("LifeFanout");
    registerJoinFilter<int, std::string>("LifeJoin", [](std::vector<std::any>&& gathered) {
        return std::to_string(gathered.size());
    });

    JsonFilterGraph<int, std::string> graph(nlohmann::json::parse(R"([
        { "type": "LifeFanout", "config": { "branches": [
            [ { "type": "LifeSum", "config": { "label": "branch" } } ]
        ] } },
        { "type": "LifeSum", "config": { "label": "main" } },
        { "type": "LifeJoin", "config": { "paths": [
            [ { "type": "LifeSum", "config": { "label": "path" } }, { "type": "LifeToString" } ]
        ] } }
    ])"));

    graph.filter(6);
    graph.finish();

    REQUIRE(lifecycleLog() == std::vector<std::string>{"branch=6", "main=6", "path=6"});
}

TEST_CASE("Every stage is finished even when one throws", "[Lifecycle]")
{
    lifecycleLog().clear();
    DslFilterGraph<int, int> graph(R"(
        in -> LifeThrow -> a
        a -> LifeSum(label=after) -> out
    )");

    graph.filter(7);
    REQUIRE_THROWS_AS(graph.finish(), std::runtime_error);
    REQUIRE(lifecycleLog() == std::vector<std::string>{"throwing", "after=7"});
}

TEST_CASE("Finishing a graph twice finishes its stages twice", "[Lifecycle]")
{
    lifecycleLog().clear();
    DslFilterGraph<int, int> graph("in -> LifeSum(label=once) -> out");

    graph.filter(8);
    graph.finish();
    graph.finish();

    REQUIRE(lifecycleLog() == std::vector<std::string>{"once=8", "once=8"});
}
