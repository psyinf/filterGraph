#include <filterGraph/core/filterGraph/AnyFilterChain.hpp>
#include <filterGraph/core/filterGraph/FanoutFilter.hpp>
#include <filterGraph/core/filterGraph/FilterGraph.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>
#include <filterGraph/core/filterGraph/Void.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>

using namespace filterGraph;

namespace {

class DoubleFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override
    {
        return value * 2;
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

class DropOddFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override
    {
        if (value % 2 != 0)
        {
            return std::nullopt;
        }
        return value;
    }
};

// Side-effect-only terminal stage: consumes an int and produces Void,
// explicitly declaring that the path ends here.
class VoidSinkFilter : public MessageFilter<int, Void>
{
public:
    std::optional<Void> filter(int&& /*value*/) override
    {
        return Void{};
    }
};

} // namespace

TEST_CASE("FilterGraph chains stages and converts types", "[FilterGraph]")
{
    FilterGraph<DoubleFilter, ToStringFilter> graph(
        std::make_shared<DoubleFilter>(),
        std::make_shared<ToStringFilter>());

    auto result = graph.filter(21);
    REQUIRE(result.has_value());
    REQUIRE(*result == "42");
}

TEST_CASE("MessageFilter short-circuits the chain via std::nullopt", "[FilterGraph]")
{
    FilterGraph<DropOddFilter, DoubleFilter> graph(
        std::make_shared<DropOddFilter>(),
        std::make_shared<DoubleFilter>());

    REQUIRE(graph.filter(3) == std::nullopt);
    REQUIRE(graph.filter(4) == 8);
}

TEST_CASE("JsonFilterGraph builds and validates a chain from JSON", "[JsonFilterGraph]")
{
    static FilterRegistrar<DoubleFilter>   registerDouble("Double");
    static FilterRegistrar<ToStringFilter> registerToString("ToString");

    auto config = nlohmann::json::parse(R"([
        { "type": "Double" },
        { "type": "ToString" }
    ])");

    JsonFilterGraph<int, std::string> graph(config);
    auto                              result = graph.filter(10);
    REQUIRE(result.has_value());
    REQUIRE(*result == "20");
}

TEST_CASE("JsonFilterGraph throws on type mismatch", "[JsonFilterGraph]")
{
    static FilterRegistrar<ToStringFilter> registerToString2("ToString2");

    auto config = nlohmann::json::parse(R"([
        { "type": "ToString2" }
    ])");

    // Declared output type (int) doesn't match the actual chain output (string).
    REQUIRE_THROWS_AS((JsonFilterGraph<int, int>(config)), std::runtime_error);
}

TEST_CASE("FanoutFilter duplicates to branches and passes original through", "[FanoutFilter]")
{
    static FilterRegistrar<DoubleFilter> registerDouble3("Double3");

    auto config = nlohmann::json::parse(R"({
        "branches": [
            [ { "type": "Double3" } ]
        ]
    })");

    auto fanout = std::make_shared<FanoutFilter<int>>();
    fanout->addReceiver(std::make_shared<AnyFilterChain>(config.at("branches")[0]));

    auto result = fanout->filter(5);
    REQUIRE(result.has_value());
    REQUIRE(*result == 5); // original value passed through unchanged
}

TEST_CASE("JsonFilterGraph builds a Void-terminated path", "[Void]")
{
    static FilterRegistrar<DoubleFilter>   registerDoubleVoid("DoubleVoid");
    static FilterRegistrar<VoidSinkFilter> registerVoidSink("VoidSink");

    auto config = nlohmann::json::parse(R"([
        { "type": "DoubleVoid" },
        { "type": "VoidSink" }
    ])");

    JsonFilterGraph<int, Void> graph(config);
    REQUIRE(graph.filter(7).has_value());
}

TEST_CASE("AnyFilterChain rejects a stage after a terminal Void stage", "[Void]")
{
    static FilterRegistrar<VoidSinkFilter> registerVoidSink2("VoidSink2");
    static FilterRegistrar<DoubleFilter>   registerDoubleAfterVoid("DoubleAfterVoid");

    auto config = nlohmann::json::parse(R"([
        { "type": "VoidSink2" },
        { "type": "DoubleAfterVoid" }
    ])");

    REQUIRE_THROWS_AS(AnyFilterChain(config), std::runtime_error);
}
