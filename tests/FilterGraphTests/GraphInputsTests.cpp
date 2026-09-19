#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace filterGraph;

namespace {

struct Order
{
    int quantity = 0;
};

struct Quote
{
    double price = 0.0;
};

class ParseOrderFilter : public MessageFilter<Order, int>
{
public:
    std::optional<int> filter(Order&& order) override
    {
        return order.quantity;
    }
};

class ParseQuoteFilter : public MessageFilter<Quote, double>
{
public:
    std::optional<double> filter(Quote&& quote) override
    {
        return quote.price;
    }
};

class IntToStringFilter : public MessageFilter<int, std::string>
{
public:
    std::optional<std::string> filter(int&& value) override
    {
        return std::to_string(value);
    }
};

// "<quantity>@<price>", with "-" for a hole.
class MatchMerge : public TypedMergeFilter<std::string, int, double>
{
public:
    std::optional<std::string> merge(std::optional<int>&& quantity, std::optional<double>&& price) override
    {
        return std::format("{}@{}", quantity ? std::to_string(*quantity) : "-", price ? std::format("{}", *price) : "-");
    }
};

// Reads the raw inputs: "<quantity>@<price>", with "-" for a hole.
class OrderQuoteMerge : public TypedMergeFilter<std::string, Order, Quote>
{
public:
    std::optional<std::string> merge(std::optional<Order>&& order, std::optional<Quote>&& quote) override
    {
        return std::format("{}@{}", order ? std::to_string(order->quantity) : "-",
                           quote ? std::format("{}", quote->price) : "-");
    }
};

// OrderQuoteMerge with named slots.
class NamedOrderQuoteMerge : public OrderQuoteMerge
{
public:
    std::vector<std::string> mergeInputNames() const override
    {
        return {"order", "quote"};
    }
};

// "<raw quantity>=<parsed quantity>", for a single-input graph.
class OrderQuantityMerge : public TypedMergeFilter<std::string, Order, int>
{
public:
    std::optional<std::string> merge(std::optional<Order>&& order, std::optional<int>&& quantity) override
    {
        return std::format("{}={}", order ? std::to_string(order->quantity) : "-",
                           quantity ? std::to_string(*quantity) : "-");
    }
};

static FilterRegistrar<OrderQuantityMerge>   registerOrderQuantity("InOrderQuantity");
static FilterRegistrar<ParseOrderFilter>     registerParseOrder("InParseOrder");
static FilterRegistrar<ParseQuoteFilter>     registerParseQuote("InParseQuote");
static FilterRegistrar<IntToStringFilter>    registerIntToString("InIntToString");
static FilterRegistrar<MatchMerge>           registerMatch("InMatch");
static FilterRegistrar<OrderQuoteMerge>      registerOrderQuote("InOrderQuote");
static FilterRegistrar<NamedOrderQuoteMerge> registerNamedOrderQuote("InNamedOrderQuote");

const char* kMatchGraph = R"(
    in.orders -> InParseOrder -> quantity
    in.quotes -> InParseQuote -> price
    (quantity, price) -> InMatch -> out
)";

bool mentions(const std::vector<dsl::TextDiagnostic>& diagnostics, std::string_view fragment)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const dsl::TextDiagnostic& diagnostic) {
        return diagnostic.message.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("push runs only the stages reachable from one named input", "[GraphInputs]")
{
    DslFilterGraph<GraphInputs, std::string> graph(kMatchGraph);

    REQUIRE(graph.push("orders", Order{3}).value() == "3@-");
    REQUIRE(graph.push("quotes", Quote{1.5}).value() == "-@1.5");
}

TEST_CASE("filter feeds several named inputs in one run", "[GraphInputs]")
{
    DslFilterGraph<GraphInputs, std::string> graph(kMatchGraph);

    REQUIRE(graph.filter(GraphInputs{}.set("orders", Order{2}).set("quotes", Quote{0.5})).value() == "2@0.5");
    REQUIRE_FALSE(graph.filter(GraphInputs{}).has_value()); // nothing fed: every path is empty
}

TEST_CASE("Named inputs feed separate outputs", "[GraphInputs]")
{
    DslFilterGraph<GraphInputs> graph(R"(
        in.orders -> InParseOrder -> out.quantity
        in.quotes -> InParseQuote -> out.price
    )");

    auto outputs = graph.push("quotes", Quote{2.0});
    REQUIRE(outputs.has_value());
    REQUIRE_FALSE(outputs->has("quantity"));
    REQUIRE(outputs->get<double>("price").value() == 2.0);
}

TEST_CASE("Declared input types are checked when the graph is built", "[GraphInputs]")
{
    const auto declared = GraphInputs::of<Order, Quote>("orders", "quotes");
    DslFilterGraph<GraphInputs, std::string> graph(kMatchGraph, declared);
    REQUIRE(graph.push("orders", Order{1}).value() == "1@-");

    const auto swapped = validateDslGraph<GraphInputs, std::string>(kMatchGraph, GraphInputs::of<Quote, Order>("orders", "quotes"));
    REQUIRE(swapped.size() == 2);
    REQUIRE(swapped[0].message.starts_with("stage 'InParseOrder' expects input type"));
    REQUIRE(swapped[0].message.find("the graph input 'in.orders' carries") != std::string::npos);
    REQUIRE(swapped[0].loc.line == 2);
}

TEST_CASE("Declared and read inputs must agree", "[GraphInputs]")
{
    const auto undeclared =
        validateDslGraph<GraphInputs, std::string>(kMatchGraph, GraphInputs::of<Order>("orders"));
    REQUIRE(undeclared.size() == 1);
    REQUIRE(undeclared[0].message == "the graph reads 'in.quotes', but no input 'quotes' is declared");
    REQUIRE(undeclared[0].loc.line == 3);

    const auto unread = validateDslGraph<GraphInputs, std::string>(
        kMatchGraph, GraphInputs::of<Order, Quote, int>("orders", "quotes", "extra"));
    REQUIRE(unread.size() == 1);
    REQUIRE(unread[0].message == "input 'extra' is declared but the graph never reads 'in.extra'");
}

TEST_CASE("An undeclared input is typed by its first reader", "[GraphInputs]")
{
    const auto diagnostics = validateDslGraph<GraphInputs, GraphOutputs>(R"(
        in.orders -> InParseOrder -> out.a
        in.orders -> InIntToString -> out.b
    )");

    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].message.starts_with("stage 'InIntToString' expects input type"));
    REQUIRE(diagnostics[0].message.find("the graph input 'in.orders' (typed by its reader 'InParseOrder')")
            != std::string::npos);
}

TEST_CASE("Feeding an unknown or mistyped input throws", "[GraphInputs]")
{
    DslFilterGraph<GraphInputs, std::string> graph(kMatchGraph);

    REQUIRE_THROWS_AS(graph.push("trades", Order{1}), std::out_of_range);
    REQUIRE_THROWS_AS(graph.push("orders", Quote{1.0}), std::invalid_argument);
    REQUIRE_THROWS_AS(graph.filter(GraphInputs{}.set("orders", 7)), std::invalid_argument);
}

TEST_CASE("Input shape must match the graph's input type", "[GraphInputs]")
{
    const auto plainInNamedGraph = validateDslGraph<GraphInputs, std::string>("in -> InParseOrder -> q -> InIntToString -> out");
    REQUIRE(plainInNamedGraph.size() == 1);
    REQUIRE(plainInNamedGraph[0].message
            == "the graph's input type is GraphInputs, so its inputs are named: write 'in.<key>' instead of 'in'");

    const auto namedInPlainGraph = validateDslGraph<Order, std::string>("in.orders -> InParseOrder -> q -> InIntToString -> out");
    REQUIRE(namedInPlainGraph.size() == 1);
    REQUIRE(namedInPlainGraph[0].message == "the named input 'in.orders' needs GraphInputs as the graph's input type");
    REQUIRE(namedInPlainGraph[0].loc.column == 1);
}

TEST_CASE("Only in and out take a key", "[GraphInputs]")
{
    const auto program = dsl::parseGraphProgram("in -> InParseOrder -> q.x -> InIntToString -> out");
    REQUIRE(program.diagnostics.size() == 1);
    REQUIRE(program.diagnostics[0].message == "only 'in' and 'out' take a '.<key>', not 'q.x'");
}

TEST_CASE("The parsed program lists its inputs", "[GraphInputs]")
{
    const auto program = dsl::parseGraphProgram("in.orders -> InParseOrder -> a\n"
                                                "in.quotes -> InParseQuote -> b\n"
                                                "in.orders -> InParseOrder -> c\n"
                                                "(a, b, c) -> InMatch -> out\n");
    REQUIRE(program.inputs.size() == 2);
    REQUIRE(program.inputs[0].edge == "in.orders");
    REQUIRE(program.inputs[0].key == "orders");
    REQUIRE(program.inputs[1].loc.line == 2);
    REQUIRE(program.stages[0].inputs.front() == "in.orders");

    const auto plain = dsl::parseGraphProgram("in -> InParseOrder -> out");
    REQUIRE(plain.inputs.size() == 1);
    REQUIRE_FALSE(plain.inputs[0].key.has_value());
}

TEST_CASE("A fan-in group reads named inputs directly", "[GraphInputs]")
{
    DslFilterGraph<GraphInputs, std::string> graph("(in.orders, in.quotes) -> InOrderQuote -> out");

    REQUIRE(graph.push("orders", Order{3}).value() == "3@-");
    REQUIRE(graph.push("quotes", Quote{1.5}).value() == "-@1.5");
    REQUIRE(graph.filter(GraphInputs{}.set("orders", Order{2}).set("quotes", Quote{0.5})).value() == "2@0.5");
    REQUIRE_FALSE(graph.filter(GraphInputs{}).has_value());
}

TEST_CASE("A named slot binds a named input by name", "[GraphInputs]")
{
    DslFilterGraph<GraphInputs, std::string> graph("(quote: in.quotes, order: in.orders) -> InNamedOrderQuote -> out");

    REQUIRE(graph.push("orders", Order{4}).value() == "4@-");
    REQUIRE(graph.filter(GraphInputs{}.set("orders", Order{1}).set("quotes", Quote{2.5})).value() == "1@2.5");
}

TEST_CASE("A group mixes named inputs and ordinary edges", "[GraphInputs]")
{
    DslFilterGraph<GraphInputs, std::string> graph(R"(
        in.orders -> InParseOrder -> quantity
        (quantity, in.quotes) -> InMatch -> out
    )");

    REQUIRE(graph.push("orders", Order{3}).value() == "3@-");
    REQUIRE(graph.filter(GraphInputs{}.set("orders", Order{2}).set("quotes", 0.5)).value() == "2@0.5");
}

TEST_CASE("A plain in feeds a fan-in group of a single-input graph", "[GraphInputs]")
{
    DslFilterGraph<Order, std::string> graph(R"(
        in -> InParseOrder -> quantity
        (in, quantity) -> InOrderQuantity -> out
    )");

    REQUIRE(graph.filter(Order{5}).value() == "5=5");
    REQUIRE(dsl::parseGraphProgram("(in, in) -> InOrderQuote -> out").inputs.size() == 1);
}

TEST_CASE("Declared input types are checked for group readers", "[GraphInputs]")
{
    const char* source = "(in.orders, in.quotes) -> InOrderQuote -> out";

    DslFilterGraph<GraphInputs, std::string> graph(source, GraphInputs::of<Order, Quote>("orders", "quotes"));
    REQUIRE(graph.push("quotes", Quote{0.5}).value() == "-@0.5");

    const auto swapped =
        validateDslGraph<GraphInputs, std::string>(source, GraphInputs::of<Quote, Order>("orders", "quotes"));
    REQUIRE(swapped.size() == 2);
    REQUIRE(swapped[1].message.starts_with("slot 2 of 'InOrderQuote' expects"));
    REQUIRE(swapped[1].message.find("the graph input 'in.quotes' carries") != std::string::npos);
}

TEST_CASE("An undeclared input read only by a group takes the slot's type", "[GraphInputs]")
{
    DslFilterGraph<GraphInputs, std::string> graph(R"(
        in.orders -> InParseOrder -> quantity
        (quantity, in.quotes) -> InMatch -> out
    )");

    REQUIRE(graph.push("quotes", 1.5).value() == "-@1.5");
    REQUIRE_THROWS_AS(graph.push("quotes", Quote{1.5}), std::invalid_argument);

    const auto diagnostics = validateDslGraph<GraphInputs, GraphOutputs>(R"(
        (in.orders, in.quotes) -> InOrderQuote -> out.pair
        in.quotes -> InIntToString -> out.text
    )");
    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].message.find("the graph input 'in.quotes' (typed by its reader 'InOrderQuote')")
            != std::string::npos);
}

TEST_CASE("Only in takes a key inside a group, and sinks cannot feed one", "[GraphInputs]")
{
    const auto keyed = dsl::parseGraphProgram("(msg.x, y) -> InMatch -> out");
    REQUIRE(dsl::formatDiagnostics(keyed.diagnostics) == "1:2: only 'in' and 'out' take a '.<key>', not 'msg.x'");

    const auto out = dsl::parseGraphProgram("(out, y) -> InMatch -> out");
    REQUIRE(dsl::formatDiagnostics(out.diagnostics) == "1:2: 'out' may only appear as the last term of a statement");

    const auto outKey = dsl::parseGraphProgram("(y, out.x) -> InMatch -> out");
    REQUIRE(dsl::formatDiagnostics(outKey.diagnostics) == "1:5: 'out' may only appear as the last term of a statement");
}

TEST_CASE("The parsed program lists inputs read only by a group", "[GraphInputs]")
{
    const auto program = dsl::parseGraphProgram("in.orders -> InParseOrder -> a\n"
                                                "(a, in.quotes) -> InMatch -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.inputs.size() == 2);
    REQUIRE(program.inputs[1].edge == "in.quotes");
    REQUIRE(program.inputs[1].key == "quotes");
    REQUIRE(program.inputs[1].loc.line == 2);
    REQUIRE(program.inputs[1].loc.column == 5);
    REQUIRE(program.stages[1].inputs == std::vector<std::string>{"a", "in.quotes"});

    const auto twice = dsl::parseGraphProgram("(in.orders, in.orders) -> InOrderQuote -> out");
    REQUIRE(twice.inputs.size() == 1);
}

TEST_CASE("A graph with named inputs nests as a stage", "[GraphInputs]")
{
    FilterRegistrar<DslFilterGraph<GraphInputs, std::string>> registrar(
        "InNestedMatch",
        [](const nlohmann::json&) { return std::make_shared<DslFilterGraph<GraphInputs, std::string>>(kMatchGraph); });

    DslFilterGraph<GraphInputs, std::string> outer("in.both -> InNestedMatch -> out",
                                                   GraphInputs::of<GraphInputs>("both"));
    REQUIRE(outer.push("both", GraphInputs{}.set("orders", Order{4}).set("quotes", Quote{0.25})).value() == "4@0.25");
}
