#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <any>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

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
        return value % 2 == 0 ? std::optional<int>(value) : std::nullopt;
    }
};

// Two slots of different types: "<text>/<number>", with "-" for a hole.
class LabelMerge : public TypedMergeFilter<std::string, std::string, int>
{
public:
    std::optional<std::string> merge(std::optional<std::string>&& text, std::optional<int>&& number) override
    {
        return std::format("{}/{}", text.value_or("-"), number ? std::to_string(*number) : "-");
    }
};

// Any number of int slots, summed.
class SumMerge : public UniformMergeFilter<int, int>
{
public:
    std::optional<int> merge(std::vector<std::optional<int>>&& inputs) override
    {
        int sum = 0;
        for (const auto& input : inputs)
        {
            sum += input.value_or(0);
        }
        return sum;
    }
};

// Stateful merge: keeps a running total across messages, so a fresh instance
// starts over.
class TotalMerge : public UniformMergeFilter<int, int>
{
public:
    std::optional<int> merge(std::vector<std::optional<int>>&& inputs) override
    {
        for (const auto& input : inputs)
        {
            mTotal += input.value_or(0);
        }
        return mTotal;
    }

private:
    int mTotal = 0;
};

static FilterRegistrar<DoubleFilter>   registerDouble("MergeDouble");
static FilterRegistrar<ToStringFilter> registerToString("MergeToString");
static FilterRegistrar<DropOddFilter>  registerDropOdd("MergeDropOdd");
static FilterRegistrar<LabelMerge>     registerLabel("MergeLabel");
static FilterRegistrar<SumMerge>       registerSum("MergeSum");
static FilterRegistrar<TotalMerge>     registerTotal("MergeTotal");

static const bool sRegisterUntyped = [] {
    registerMergeFilter<int>("MergeUntyped", [](MergeInputs&& inputs) -> std::optional<int> {
        return static_cast<int>(inputs.size());
    });
    registerTypedMergeFilter<std::string, int, std::string>(
        "MergeLambda",
        [](std::optional<int>&& number, std::optional<std::string>&& text) -> std::optional<std::string> {
            return std::format("{}:{}", number.value_or(0), text.value_or("-"));
        });
    return true;
}();

std::vector<dsl::TextDiagnostic> buildErrors(std::string_view text)
{
    try
    {
        DslFilterGraph<int, std::string> graph(text);
    }
    catch (const GraphError& error)
    {
        return error.diagnostics();
    }
    return {};
}

bool mentions(const std::vector<dsl::TextDiagnostic>& diagnostics, std::string_view fragment)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const dsl::TextDiagnostic& diagnostic) {
        return diagnostic.message.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("TypedMergeFilter receives its slots as typed optionals", "[TypedMerge]")
{
    DslFilterGraph<int, std::string> graph(R"(
        in -> MergeToString -> text
        in -> MergeDouble -> doubled
        (text, doubled) -> MergeLabel -> out
    )");

    REQUIRE(graph.filter(21).value() == "21/42");
}

TEST_CASE("TypedMergeFilter sees a hole for a dropped path", "[TypedMerge]")
{
    DslFilterGraph<int, std::string> graph(R"(
        in -> MergeToString -> text
        in -> MergeDropOdd -> even
        (text, even) -> MergeLabel -> out
    )");

    REQUIRE(graph.filter(4).value() == "4/4");
    REQUIRE(graph.filter(3).value() == "3/-");
}

TEST_CASE("registerTypedMergeFilter registers a typed merge from a lambda", "[TypedMerge]")
{
    DslFilterGraph<int, std::string> graph(R"(
        in -> MergeDouble -> doubled
        in -> MergeToString -> text
        (doubled, text) -> MergeLambda -> out
    )");

    REQUIRE(graph.filter(5).value() == "10:5");
}

TEST_CASE("UniformMergeFilter takes any number of slots of one type", "[TypedMerge]")
{
    DslFilterGraph<int, int> two(R"(
        in -> MergeDouble -> a
        in -> MergeDouble -> b
        (a, b) -> MergeSum -> out
    )");
    REQUIRE(two.filter(3).value() == 12);

    DslFilterGraph<int, int> three(R"(
        in -> MergeDouble -> a
        in -> MergeDouble -> b
        in -> MergeDropOdd -> c
        (a, b, c) -> MergeSum -> out
    )");
    REQUIRE(three.filter(3).value() == 12); // the odd input drops slot c
    REQUIRE(three.filter(4).value() == 20);
}

TEST_CASE("A typed merge reports a mis-wired slot when the graph is built", "[TypedMerge]")
{
    const auto swapped = buildErrors(R"(
        in -> MergeToString -> text
        in -> MergeDouble -> doubled
        (doubled, text) -> MergeLabel -> out
    )");

    REQUIRE(swapped.size() == 2);
    REQUIRE(swapped[0].message.starts_with("slot 1 of 'MergeLabel' expects"));
    REQUIRE(swapped[0].message.find("edge 'doubled' carries") != std::string::npos);
    REQUIRE(swapped[1].message.starts_with("slot 2 of 'MergeLabel' expects"));
    REQUIRE(swapped[0].loc.line == 4);

    REQUIRE(mentions(buildErrors(R"(
        in -> MergeDouble -> a
        in -> MergeToString -> b
        (a, b) -> MergeSum -> sum -> MergeToString -> out
    )"),
                     "slot 2 of 'MergeSum' expects"));
}

TEST_CASE("A typed merge reports the wrong number of slots", "[TypedMerge]")
{
    const auto diagnostics = buildErrors(R"(
        in -> MergeToString -> text
        in -> MergeDouble -> doubled
        in -> MergeDouble -> more
        (text, doubled, more) -> MergeLabel -> out
    )");

    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].message == "stage 'MergeLabel' takes 2 inputs but the group has 3");
    REQUIRE(diagnostics[0].loc.line == 5);
}

TEST_CASE("An untyped merge is still accepted and unchecked", "[TypedMerge]")
{
    DslFilterGraph<int, std::string> graph(R"(
        in -> MergeDouble -> a
        in -> MergeToString -> b
        (a, b) -> MergeUntyped -> count -> MergeToString -> out
    )");

    REQUIRE(graph.filter(1).value() == "2");
    REQUIRE(validateDslGraph<int, std::string>("in -> MergeDouble -> a\nin -> MergeToString -> b\n"
                                               "(a, b) -> MergeUntyped -> c -> MergeToString -> out\n")
                .empty());
}

TEST_CASE("A stateful merge keeps its state per graph instance", "[TypedMerge]")
{
    const char* text = R"(
        in -> MergeDouble -> a
        in -> MergeDouble -> b
        (a, b) -> MergeTotal -> out
    )";

    DslFilterGraph<int, int> first(text);
    DslFilterGraph<int, int> second(text);

    REQUIRE(first.filter(1).value() == 4);  // 2 + 2
    REQUIRE(first.filter(2).value() == 12); // 4 + (4 + 4)
    REQUIRE(second.filter(1).value() == 4); // its own TotalMerge, starting at 0
}

TEST_CASE("A typed merge used outside a graph checks its slot count", "[TypedMerge]")
{
    LabelMerge merge;
    REQUIRE(merge.filter(MergeInputs{std::any(std::string{"a"}), std::any(7)}).value() == "a/7");
    REQUIRE_THROWS_AS(merge.filter(MergeInputs{std::any(std::string{"a"})}), std::invalid_argument);

    SumMerge sum;
    REQUIRE(sum.filter(MergeInputs{std::any(1), std::any(2), std::any(3)}).value() == 6);

    const MergeSlotTypes slots = sum.mergeInputTypes();
    REQUIRE(slots.uniform);
    REQUIRE(slots.types.size() == 1);
    REQUIRE(slots.types.front() == std::type_index(typeid(int)));
    REQUIRE_FALSE(LabelMerge{}.mergeInputTypes().uniform);
    REQUIRE(LabelMerge{}.mergeInputTypes().types.size() == 2);
}
