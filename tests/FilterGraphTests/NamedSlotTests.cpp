#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <any>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace filterGraph;

namespace {

class PlusOneFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override
    {
        return value + 1;
    }
};

class TimesTenFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override
    {
        return value * 10;
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

// Two slots of the same type, where the order matters: "<base>-<bonus>", with
// "?" for a hole.
class DifferenceMerge : public TypedMergeFilter<std::string, int, int>
{
public:
    std::optional<std::string> merge(std::optional<int>&& base, std::optional<int>&& bonus) override
    {
        return std::format("{}-{}", base ? std::to_string(*base) : "?", bonus ? std::to_string(*bonus) : "?");
    }

    std::vector<std::string> mergeInputNames() const override
    {
        return {"base", "bonus"};
    }
};

// Two slots of different types, named.
class LabelMerge : public TypedMergeFilter<std::string, std::string, int>
{
public:
    std::optional<std::string> merge(std::optional<std::string>&& text, std::optional<int>&& number) override
    {
        return std::format("{}/{}", text.value_or("-"), number.value_or(0));
    }

    std::vector<std::string> mergeInputNames() const override
    {
        return {"text", "number"};
    }
};

// Declares more names than it has slot types: a bug in the stage.
class BrokenNamesMerge : public TypedMergeFilter<int, int, int>
{
public:
    std::optional<int> merge(std::optional<int>&&, std::optional<int>&&) override
    {
        return 0;
    }

    std::vector<std::string> mergeInputNames() const override
    {
        return {"a", "b", "c"};
    }
};

// Untyped and unnamed: positional slots only.
class PositionalMerge : public UniformMergeFilter<int, int>
{
public:
    std::optional<int> merge(std::vector<std::optional<int>>&& inputs) override
    {
        return static_cast<int>(inputs.size());
    }
};

static FilterRegistrar<PlusOneFilter>    registerPlusOne("SlotPlusOne");
static FilterRegistrar<TimesTenFilter>   registerTimesTen("SlotTimesTen");
static FilterRegistrar<ToStringFilter>   registerToString("SlotToString");
static FilterRegistrar<DropOddFilter>    registerDropOdd("SlotDropOdd");
static FilterRegistrar<DifferenceMerge>  registerDifference("SlotDifference");
static FilterRegistrar<LabelMerge>       registerLabel("SlotLabel");
static FilterRegistrar<BrokenNamesMerge> registerBroken("SlotBroken");
static FilterRegistrar<PositionalMerge>  registerPositional("SlotPositional");

static const bool sRegisterCallables = [] {
    registerMergeFilter<std::string>("SlotUntyped", {"first", "second"}, [](MergeInputs&& inputs) {
        return std::optional<std::string>(std::format("{},{}", std::any_cast<int>(inputs[0]), std::any_cast<int>(inputs[1])));
    });
    registerTypedMergeFilter<std::string, int, std::string>(
        "SlotLambda",
        {"number", "text"},
        [](std::optional<int>&& number, std::optional<std::string>&& text) -> std::optional<std::string> {
            return std::format("{}:{}", number.value_or(0), text.value_or("-"));
        });
    return true;
}();

std::vector<dsl::TextDiagnostic> buildErrors(std::string_view text)
{
    return validateDslGraph<int, std::string>(text);
}

bool mentions(const std::vector<dsl::TextDiagnostic>& diagnostics, std::string_view fragment)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const dsl::TextDiagnostic& diagnostic) {
        return diagnostic.message.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("A named group matches slots by name, in any order", "[NamedSlots]")
{
    const char* inOrder = R"(
        in -> SlotPlusOne -> small
        in -> SlotTimesTen -> large
        (base: large, bonus: small) -> SlotDifference -> out
    )";
    const char* swapped = R"(
        in -> SlotPlusOne -> small
        in -> SlotTimesTen -> large
        (bonus: small, base: large) -> SlotDifference -> out
    )";

    REQUIRE(DslFilterGraph<int, std::string>(inOrder).filter(4).value() == "40-5");
    REQUIRE(DslFilterGraph<int, std::string>(swapped).filter(4).value() == "40-5");
}

TEST_CASE("A positional group still feeds a named merge by position", "[NamedSlots]")
{
    DslFilterGraph<int, std::string> graph(R"(
        in -> SlotPlusOne -> small
        in -> SlotTimesTen -> large
        (small, large) -> SlotDifference -> out
    )");

    REQUIRE(graph.filter(4).value() == "5-40");
}

TEST_CASE("Named slots keep their holes when reordered", "[NamedSlots]")
{
    DslFilterGraph<int, std::string> graph(R"(
        in -> SlotDropOdd -> even
        in -> SlotTimesTen -> large
        (bonus: even, base: large) -> SlotDifference -> out
    )");

    REQUIRE(graph.filter(2).value() == "20-2");
    REQUIRE(graph.filter(3).value() == "30-?");
}

TEST_CASE("Named slots work for untyped and lambda-registered merges", "[NamedSlots]")
{
    DslFilterGraph<int, std::string> untyped(R"(
        in -> SlotPlusOne -> small
        in -> SlotTimesTen -> large
        (second: small, first: large) -> SlotUntyped -> out
    )");
    REQUIRE(untyped.filter(1).value() == "10,2");

    DslFilterGraph<int, std::string> lambda(R"(
        in -> SlotToString -> text
        in -> SlotTimesTen -> large
        (text: text, number: large) -> SlotLambda -> out
    )");
    REQUIRE(lambda.filter(3).value() == "30:3");
}

TEST_CASE("A slot type mismatch names the slot", "[NamedSlots]")
{
    const auto diagnostics = buildErrors(R"(
        in -> SlotToString -> text
        in -> SlotTimesTen -> large
        (text: large, number: text) -> SlotLabel -> out
    )");

    REQUIRE(diagnostics.size() == 2);
    REQUIRE(diagnostics[0].message.starts_with("slot 'text' of 'SlotLabel' expects"));
    REQUIRE(diagnostics[0].message.find("edge 'large' carries") != std::string::npos);
    REQUIRE(diagnostics[1].message.starts_with("slot 'number' of 'SlotLabel' expects"));
    REQUIRE(diagnostics[0].loc.line == 4);
}

TEST_CASE("Unknown and missing slot names are reported", "[NamedSlots]")
{
    const auto unknown = buildErrors(R"(
        in -> SlotPlusOne -> small
        in -> SlotTimesTen -> large
        (base: large, bonsu: small) -> SlotDifference -> out
    )");
    REQUIRE(unknown.size() == 2);
    REQUIRE(unknown[0].message.starts_with("'SlotDifference' has no slot named 'bonsu'"));
    REQUIRE(unknown[0].message.ends_with("did you mean 'bonus'? (its slots: base, bonus)"));
    REQUIRE(unknown[1].message == "the fan-in group does not feed slot 'bonus' of 'SlotDifference'");

    const auto missing = buildErrors(R"(
        in -> SlotTimesTen -> large
        (base: large) -> SlotDifference -> out
    )");
    REQUIRE(missing.size() == 1);
    REQUIRE(missing[0].message == "the fan-in group does not feed slot 'bonus' of 'SlotDifference'");
}

TEST_CASE("A named group needs a merge that names its slots", "[NamedSlots]")
{
    const auto diagnostics = buildErrors(R"(
        in -> SlotPlusOne -> a
        in -> SlotTimesTen -> b
        (x: a, y: b) -> SlotPositional -> n -> SlotToString -> out
    )");

    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].message == "'SlotPositional' has no named slots; list the group's edges without names");
}

TEST_CASE("A merge that names its slots fixes their number", "[NamedSlots]")
{
    const auto diagnostics = buildErrors(R"(
        in -> SlotPlusOne -> a
        in -> SlotTimesTen -> b
        in -> SlotTimesTen -> c
        (a, b, c) -> SlotUntyped -> out
    )");

    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].message == "stage 'SlotUntyped' takes 2 inputs but the group has 3");
}

TEST_CASE("Inconsistent slot declarations of a stage are reported", "[NamedSlots]")
{
    const auto diagnostics = buildErrors(R"(
        in -> SlotPlusOne -> a
        in -> SlotTimesTen -> b
        (a, b) -> SlotBroken -> n -> SlotToString -> out
    )");

    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].message == "stage 'SlotBroken' declares 3 slot names for 2 slot types");
}

TEST_CASE("toDot labels a named group's links with the slot names", "[NamedSlots]")
{
    const auto program = dsl::parseGraphProgram("in -> SlotPlusOne -> small\n"
                                                "in -> SlotTimesTen -> large\n"
                                                "(bonus: small, base: large) -> SlotDifference -> out\n");
    REQUIRE(program.ok());

    const std::string dot = dsl::toDot(program);
    REQUIRE(dot.find("-> s2 [label=\"bonus\"];") != std::string::npos);
    REQUIRE(dot.find("-> s2 [label=\"base\"];") != std::string::npos);
    REQUIRE(dot.find("e0 -> s0;") != std::string::npos);
}

TEST_CASE("toAscii heads a named merge with its named group", "[NamedSlots]")
{
    const auto program = dsl::parseGraphProgram("in.x -> SlotPlusOne -> small\n"
                                                "in.x -> SlotTimesTen -> large\n"
                                                "(bonus: small, base: large) -> SlotDifference -> out\n");
    REQUIRE(program.ok());

    REQUIRE(dsl::toAscii(program) == "in.x\n"
                                     "+-> SlotPlusOne -> small\n"
                                     "|   `-> SlotDifference (merge, see below)\n"
                                     "`-> SlotTimesTen -> large\n"
                                     "    `-> SlotDifference (merge, see below)\n"
                                     "\n"
                                     "(bonus: small, base: large)\n"
                                     "`-> SlotDifference -> out\n");
}

TEST_CASE("The parsed program and toMermaid carry the slot names", "[NamedSlots]")
{
    const auto program = dsl::parseGraphProgram("in -> SlotPlusOne -> small\n"
                                                "in -> SlotTimesTen -> large\n"
                                                "(bonus: small, base: large) -> SlotDifference -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.stages[2].slotNames == std::vector<std::string>{"bonus", "base"});
    REQUIRE(program.stages[2].inputs == std::vector<std::string>{"small", "large"});
    REQUIRE(program.stages[0].slotNames.empty());

    const std::string mermaid = dsl::toMermaid(program);
    REQUIRE(mermaid.find("-->|\"bonus\"| s2") != std::string::npos);
    REQUIRE(mermaid.find("-->|\"base\"| s2") != std::string::npos);
}

TEST_CASE("Merge stages expose their declared slot names", "[NamedSlots]")
{
    REQUIRE(DifferenceMerge{}.mergeInputNames() == std::vector<std::string>{"base", "bonus"});
    REQUIRE(PositionalMerge{}.mergeInputNames().empty());

    auto adapted = FilterRegistry::instance().create("SlotUntyped", nlohmann::json::object());
    REQUIRE(adapted->mergeInputNames() == std::vector<std::string>{"first", "second"});
    REQUIRE(adapted->mergeInputTypes().empty());
}
