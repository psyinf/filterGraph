#include <filterGraph/core/filterGraph/AnyFilterChain.hpp>
#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FanoutFilter.hpp>
#include <filterGraph/core/filterGraph/FilterGraph.hpp>
#include <filterGraph/core/filterGraph/GraphLang.hpp>
#include <filterGraph/core/filterGraph/GraphLangHandwritten.hpp>
#include <filterGraph/core/filterGraph/GraphLangLexy.hpp>
#include <filterGraph/core/filterGraph/GraphValidator.hpp>
#include <filterGraph/core/filterGraph/JoinFilter.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>
#include <filterGraph/core/filterGraph/Void.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <any>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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

TEST_CASE("JoinFilter scatters one message through N paths and combines outputs", "[JoinFilter]")
{
    static FilterRegistrar<DoubleFilter> registerDoubleJoin("DoubleJoin");

    // Sum the two path outputs: input x -> path1 (x*2) + path2 (x*2*2) = 6x.
    auto join = std::make_shared<JoinFilter<int, int>>(
        [](std::vector<std::any>&& outputs) -> std::optional<int> {
            int sum = 0;
            for (auto& out : outputs)
            {
                if (out.has_value())
                {
                    sum += std::any_cast<int>(out);
                }
            }
            return sum;
        });

    auto path1 = nlohmann::json::parse(R"([ { "type": "DoubleJoin" } ])");
    auto path2 = nlohmann::json::parse(R"([ { "type": "DoubleJoin" }, { "type": "DoubleJoin" } ])");
    join->addPath(std::make_shared<AnyFilterChain>(path1));
    join->addPath(std::make_shared<AnyFilterChain>(path2));

    auto result = join->filter(5);
    REQUIRE(result.has_value());
    REQUIRE(*result == 30); // 5*2 + 5*2*2
}

TEST_CASE("JoinFilter leaves a hole when a path drops the message", "[JoinFilter]")
{
    static FilterRegistrar<DoubleFilter>  registerDoubleHole("DoubleHole");
    static FilterRegistrar<DropOddFilter> registerDropOddHole("DropOddHole");

    // The combiner records which slots arrived and which are holes.
    bool secondPathIsHole = false;
    auto join             = std::make_shared<JoinFilter<int, int>>(
        [&](std::vector<std::any>&& outputs) -> std::optional<int> {
            secondPathIsHole = !outputs.at(1).has_value();
            return outputs.at(0).has_value() ? std::optional<int>(std::any_cast<int>(outputs.at(0))) : std::nullopt;
        });

    auto path1 = nlohmann::json::parse(R"([ { "type": "DoubleHole" } ])");
    auto path2 = nlohmann::json::parse(R"([ { "type": "DropOddHole" } ])"); // drops odd input
    join->addPath(std::make_shared<AnyFilterChain>(path1));
    join->addPath(std::make_shared<AnyFilterChain>(path2));

    auto result = join->filter(3); // odd: path2 drops -> hole
    REQUIRE(result.has_value());
    REQUIRE(*result == 6);
    REQUIRE(secondPathIsHole);
}

TEST_CASE("JoinFilter rejects a Void-terminated path", "[JoinFilter]")
{
    static FilterRegistrar<VoidSinkFilter> registerVoidSinkJoin("VoidSinkJoin");

    auto join = std::make_shared<JoinFilter<int, int>>(
        [](std::vector<std::any>&&) -> std::optional<int> { return 0; });

    auto voidPath = nlohmann::json::parse(R"([ { "type": "VoidSinkJoin" } ])");
    REQUIRE_THROWS_AS(join->addPath(std::make_shared<AnyFilterChain>(voidPath)), std::runtime_error);
}

TEST_CASE("JoinFilter rejects a path whose input type differs from the join input", "[JoinFilter]")
{
    static FilterRegistrar<ToStringFilter> registerToStringJoin("ToStringJoin");

    // Join scatters int, but the path expects int and outputs string; input
    // type matches, so instead build a join over a mismatched input type.
    auto join = std::make_shared<JoinFilter<std::string, std::string>>(
        [](std::vector<std::any>&&) -> std::optional<std::string> { return std::string{}; });

    auto intPath = nlohmann::json::parse(R"([ { "type": "ToStringJoin" } ])"); // expects int
    REQUIRE_THROWS_AS(join->addPath(std::make_shared<AnyFilterChain>(intPath)), std::runtime_error);
}

TEST_CASE("registerJoinFilter builds a join from JSON with a C++ combiner", "[JoinFilter]")
{
    static FilterRegistrar<DoubleFilter> registerDoubleRegJoin("DoubleRegJoin");

    registerJoinFilter<int, int>("SumJoin", [](std::vector<std::any>&& outputs) -> std::optional<int> {
        int sum = 0;
        for (auto& out : outputs)
        {
            if (out.has_value())
            {
                sum += std::any_cast<int>(out);
            }
        }
        return sum;
    });

    auto config = nlohmann::json::parse(R"({
        "paths": [
            [ { "type": "DoubleRegJoin" } ],
            [ { "type": "DoubleRegJoin" }, { "type": "DoubleRegJoin" } ]
        ]
    })");

    auto anyJoin = FilterRegistry::instance().create("SumJoin", config);
    auto result  = anyJoin->filter(std::any(5));
    REQUIRE(result.has_value());
    REQUIRE(std::any_cast<int>(*result) == 30); // 5*2 + 5*2*2
}

TEST_CASE("validateGraph accepts a well-formed chain", "[GraphValidator]")
{
    static FilterRegistrar<DoubleFilter>   registerVDouble("VDouble");
    static FilterRegistrar<ToStringFilter> registerVToString("VToString");

    auto config = nlohmann::json::parse(R"([
        { "type": "VDouble" },
        { "type": "VToString" }
    ])");

    REQUIRE(validateGraph(config).empty());
}

TEST_CASE("validateGraph reports an unknown type with a suggestion and location", "[GraphValidator]")
{
    static FilterRegistrar<DoubleFilter> registerVDoubleSuggest("VDoubleSuggest");

    auto config = nlohmann::json::parse(R"([
        { "type": "VDoubleSuggst" }
    ])");

    auto diagnostics = validateGraph(config);
    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].pointer == "/0");
    REQUIRE(diagnostics[0].message.find("unknown filter type 'VDoubleSuggst'") != std::string::npos);
    REQUIRE(diagnostics[0].message.find("did you mean 'VDoubleSuggest'") != std::string::npos);
}

TEST_CASE("validateGraph reports an adjacent type mismatch", "[GraphValidator]")
{
    static FilterRegistrar<DoubleFilter>   registerVDoubleMm("VDoubleMm");
    static FilterRegistrar<ToStringFilter> registerVToStringMm("VToStringMm");

    // VToStringMm outputs string; VDoubleMm expects int -> mismatch at /1.
    auto config = nlohmann::json::parse(R"([
        { "type": "VToStringMm" },
        { "type": "VDoubleMm" }
    ])");

    auto diagnostics = validateGraph(config);
    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].pointer == "/1");
    REQUIRE(diagnostics[0].message.find("expects input type") != std::string::npos);
}

TEST_CASE("validateGraph reports a bad type inside a composite sub-path", "[GraphValidator]")
{
    static FilterRegistrar<DoubleFilter> registerVDoubleFan("VDoubleFan");
    static const bool                    registerVFanout = [] {
        registerFanoutFilter<int>("VFanout");
        return true;
    }();
    (void)registerVFanout;

    auto config = nlohmann::json::parse(R"([
        { "type": "VFanout", "config": { "branches": [
            [ { "type": "NopeStage" } ]
        ] } }
    ])");

    auto diagnostics = validateGraph(config);
    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].pointer == "/0/config/branches/0/0");
    REQUIRE(diagnostics[0].message.find("unknown filter type 'NopeStage'") != std::string::npos);
}

TEST_CASE("validateGraph reports a stage missing its type field", "[GraphValidator]")
{
    auto config = nlohmann::json::parse(R"([
        { "config": { "minLength": 3 } }
    ])");

    auto diagnostics = validateGraph(config);
    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].pointer == "/0");
    REQUIRE(diagnostics[0].message.find("missing a string \"type\"") != std::string::npos);
}

TEST_CASE("validateGraph reports bad/missing config for a leaf stage", "[GraphValidator]")
{
    static FilterRegistrar<DoubleFilter> registerNeedsK(
        "NeedsK",
        [](const nlohmann::json& config) {
            config.at("k"); // throws if the required key is absent
            return std::make_shared<DoubleFilter>();
        });

    auto config = nlohmann::json::parse(R"([
        { "type": "NeedsK" }
    ])");

    auto diagnostics = validateGraph(config);
    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].pointer == "/0");
    REQUIRE(diagnostics[0].message.find("could not construct 'NeedsK'") != std::string::npos);
}

TEST_CASE("parseGraphProgram parses a linear chain", "[GraphDsl]")
{
    static FilterRegistrar<DoubleFilter> registerDslDouble("DslDouble");

    auto program = filterGraph::dsl::parseGraphProgram("in -> DslDouble -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.stages.size() == 1);
    REQUIRE(program.stages[0].type == "DslDouble");
    REQUIRE(program.stages[0].inputs == std::vector<std::string>{"in"});
    REQUIRE(program.outputs.size() == 1);
    REQUIRE(program.outputs[0].index == 0);
}

TEST_CASE("parseGraphProgram parses config args on a stage", "[GraphDsl]")
{
    static FilterRegistrar<DoubleFilter> registerDslArgs(
        "DslArgs",
        [](const nlohmann::json&) { return std::make_shared<DoubleFilter>(); });

    auto program = filterGraph::dsl::parseGraphProgram(R"(in -> DslArgs(min=3, label="hi", flag=true) -> out)");
    REQUIRE(program.ok());
    REQUIRE(program.stages.size() == 1);
    REQUIRE(program.stages[0].config.at("min").get<int>() == 3);
    REQUIRE(program.stages[0].config.at("label").get<std::string>() == "hi");
    REQUIRE(program.stages[0].config.at("flag").get<bool>() == true);
}

TEST_CASE("parseGraphProgram records ordered, keyed outputs", "[GraphDsl]")
{
    static FilterRegistrar<DoubleFilter>   registerDslText("DslText");
    static FilterRegistrar<ToStringFilter> registerDslStats("DslStats");

    auto program = filterGraph::dsl::parseGraphProgram(
        "in -> DslText  -> out.text\n"
        "in -> DslStats -> out.stats\n");
    REQUIRE(program.ok());
    REQUIRE(program.outputs.size() == 2);
    REQUIRE(program.outputs[0].index == 0);
    REQUIRE(program.outputs[0].key.value() == "text");
    REQUIRE(program.outputs[1].index == 1);
    REQUIRE(program.outputs[1].key.value() == "stats");
}

TEST_CASE("parseGraphProgram handles fan-in via a group", "[GraphDsl]")
{
    static FilterRegistrar<DoubleFilter> registerDslP1("DslP1");
    static FilterRegistrar<DoubleFilter> registerDslP2("DslP2");
    static const bool                    registerDslMerge = [] {
        registerMergeFilter<int>("DslMerge", [](MergeInputs&&) -> std::optional<int> { return 0; });
        return true;
    }();
    (void)registerDslMerge;

    auto program = filterGraph::dsl::parseGraphProgram(
        "in      -> DslP1 -> a\n"
        "in      -> DslP2 -> b\n"
        "(a, b)  -> DslMerge -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.stages.size() == 3);
    const auto& merge = program.stages.back();
    REQUIRE(merge.type == "DslMerge");
    REQUIRE(merge.inputs == std::vector<std::string>{"a", "b"});
}

TEST_CASE("parseGraphProgram treats a stage routed to end as a dead-end", "[GraphDsl]")
{
    static FilterRegistrar<DoubleFilter> registerDslSink("DslSink");
    static FilterRegistrar<DoubleFilter> registerDslMain("DslMain");

    auto program = filterGraph::dsl::parseGraphProgram(
        "in -> DslSink -> end\n"
        "in -> DslMain -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.deadEnds.size() == 1);
}

TEST_CASE("parseGraphProgram reports an unknown stage with a location", "[GraphDsl]")
{
    auto program = filterGraph::dsl::parseGraphProgram("in -> NoSuchStage -> out\n");
    REQUIRE_FALSE(program.ok());
    const bool reported = std::any_of(
        program.diagnostics.begin(), program.diagnostics.end(),
        [](const filterGraph::dsl::TextDiagnostic& d) {
            return d.message.find("unknown stage type 'NoSuchStage'") != std::string::npos && d.loc.line == 1;
        });
    REQUIRE(reported);
}

TEST_CASE("parseGraphProgram reports an undefined edge reference", "[GraphDsl]")
{
    static FilterRegistrar<DoubleFilter> registerDslUndef("DslUndef");

    auto program = filterGraph::dsl::parseGraphProgram("missing -> DslUndef -> out\n");
    REQUIRE_FALSE(program.ok());
    const bool reported = std::any_of(
        program.diagnostics.begin(), program.diagnostics.end(),
        [](const filterGraph::dsl::TextDiagnostic& d) {
            return d.message.find("edge 'missing' is used but never produced") != std::string::npos;
        });
    REQUIRE(reported);
}

TEST_CASE("parseGraphProgram reports an edge with multiple producers", "[GraphDsl]")
{
    static FilterRegistrar<DoubleFilter> registerDslA("DslA");
    static FilterRegistrar<DoubleFilter> registerDslB("DslB");
    static FilterRegistrar<DoubleFilter> registerDslC("DslC");

    auto program = filterGraph::dsl::parseGraphProgram(
        "in -> DslA -> dup\n"
        "in -> DslB -> dup\n"
        "dup -> DslC -> out\n");
    REQUIRE_FALSE(program.ok());
    const bool reported = std::any_of(
        program.diagnostics.begin(), program.diagnostics.end(),
        [](const filterGraph::dsl::TextDiagnostic& d) {
            return d.message.find("is produced by 2 stages") != std::string::npos;
        });
    REQUIRE(reported);
}

TEST_CASE("parseGraphProgram rejects a statement without a stage", "[GraphDsl]")
{
    auto program = filterGraph::dsl::parseGraphProgram("in -> out\n");
    REQUIRE_FALSE(program.ok());
    const bool reported = std::any_of(
        program.diagnostics.begin(), program.diagnostics.end(),
        [](const filterGraph::dsl::TextDiagnostic& d) {
            return d.message.find("must alternate edge -> stage -> edge") != std::string::npos;
        });
    REQUIRE(reported);
}

TEST_CASE("toMermaid renders the parsed graph", "[GraphDsl]")
{
    static FilterRegistrar<DoubleFilter> registerDslMermaid("DslMermaid");

    auto program = filterGraph::dsl::parseGraphProgram("in -> DslMermaid -> out\n");
    auto mermaid = filterGraph::dsl::toMermaid(program);
    REQUIRE(mermaid.find("flowchart LR") != std::string::npos);
    REQUIRE(mermaid.find("DslMermaid") != std::string::npos);
}

TEST_CASE("lexy parser smoke test", "[GraphDslLexy]")
{
    static FilterRegistrar<DoubleFilter> registerLexySmoke("LexySmoke");

    auto program = filterGraph::dsl::parseGraphProgramLexy("in -> LexySmoke -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.stages.size() == 1);
}

TEST_CASE("parseGraphProgramLexy parses a linear chain", "[GraphDslLexy]")
{
    static FilterRegistrar<DoubleFilter> registerLexyDouble("LexyDouble");

    auto program = filterGraph::dsl::parseGraphProgramLexy("in -> LexyDouble -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.stages.size() == 1);
    REQUIRE(program.stages[0].type == "LexyDouble");
    REQUIRE(program.stages[0].inputs == std::vector<std::string>{"in"});
    REQUIRE(program.outputs.size() == 1);
    REQUIRE(program.outputs[0].index == 0);
}

TEST_CASE("parseGraphProgramLexy parses config args on a stage", "[GraphDslLexy]")
{
    static FilterRegistrar<DoubleFilter> registerLexyArgs(
        "LexyArgs",
        [](const nlohmann::json&) { return std::make_shared<DoubleFilter>(); });

    auto program = filterGraph::dsl::parseGraphProgramLexy(R"(in -> LexyArgs(min=3, label="hi", flag=true) -> out)");
    REQUIRE(program.ok());
    REQUIRE(program.stages.size() == 1);
    REQUIRE(program.stages[0].config.at("min").get<int>() == 3);
    REQUIRE(program.stages[0].config.at("label").get<std::string>() == "hi");
    REQUIRE(program.stages[0].config.at("flag").get<bool>() == true);
}

TEST_CASE("parseGraphProgramLexy records ordered, keyed outputs", "[GraphDslLexy]")
{
    static FilterRegistrar<DoubleFilter>   registerLexyText("LexyText");
    static FilterRegistrar<ToStringFilter> registerLexyStats("LexyStats");

    auto program = filterGraph::dsl::parseGraphProgramLexy(
        "in -> LexyText  -> out.text\n"
        "in -> LexyStats -> out.stats\n");
    REQUIRE(program.ok());
    REQUIRE(program.outputs.size() == 2);
    REQUIRE(program.outputs[0].index == 0);
    REQUIRE(program.outputs[0].key.value() == "text");
    REQUIRE(program.outputs[1].index == 1);
    REQUIRE(program.outputs[1].key.value() == "stats");
}

TEST_CASE("parseGraphProgramLexy handles fan-in via a group", "[GraphDslLexy]")
{
    static FilterRegistrar<DoubleFilter> registerLexyP1("LexyP1");
    static FilterRegistrar<DoubleFilter> registerLexyP2("LexyP2");
    static const bool                    registerLexyMerge = [] {
        registerMergeFilter<int>("LexyMerge", [](MergeInputs&&) -> std::optional<int> { return 0; });
        return true;
    }();
    (void)registerLexyMerge;

    auto program = filterGraph::dsl::parseGraphProgramLexy(
        "in      -> LexyP1 -> a\n"
        "in      -> LexyP2 -> b\n"
        "(a, b)  -> LexyMerge -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.stages.size() == 3);
    const auto& merge = program.stages.back();
    REQUIRE(merge.type == "LexyMerge");
    REQUIRE(merge.inputs == std::vector<std::string>{"a", "b"});
}

TEST_CASE("parseGraphProgramLexy treats a stage routed to end as a dead-end", "[GraphDslLexy]")
{
    static FilterRegistrar<DoubleFilter> registerLexySink("LexySink");
    static FilterRegistrar<DoubleFilter> registerLexyMain("LexyMain");

    auto program = filterGraph::dsl::parseGraphProgramLexy(
        "in -> LexySink -> end\n"
        "in -> LexyMain -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.deadEnds.size() == 1);
}

TEST_CASE("parseGraphProgramLexy reports an unknown stage with a location", "[GraphDslLexy]")
{
    auto program = filterGraph::dsl::parseGraphProgramLexy("in -> NoSuch -> out\n");
    REQUIRE_FALSE(program.ok());
    const bool reported = std::any_of(
        program.diagnostics.begin(), program.diagnostics.end(),
        [](const filterGraph::dsl::TextDiagnostic& d) {
            return d.message.find("unknown stage type 'NoSuch'") != std::string::npos;
        });
    REQUIRE(reported);
}

TEST_CASE("parseGraphProgramLexy reports an undefined edge reference", "[GraphDslLexy]")
{
    static FilterRegistrar<DoubleFilter> registerLexyUndef("LexyUndef");

    auto program = filterGraph::dsl::parseGraphProgramLexy("missing -> LexyUndef -> out\n");
    REQUIRE_FALSE(program.ok());
    const bool reported = std::any_of(
        program.diagnostics.begin(), program.diagnostics.end(),
        [](const filterGraph::dsl::TextDiagnostic& d) {
            return d.message.find("edge 'missing' is used but never produced") != std::string::npos;
        });
    REQUIRE(reported);
}

TEST_CASE("parseGraphProgramLexy matches parseGraphProgram on the same input", "[GraphDslLexy]")
{
    static FilterRegistrar<DoubleFilter>   registerLexyParityA("LexyParityA");
    static FilterRegistrar<ToStringFilter> registerLexyParityB("LexyParityB");

    const std::string source =
        "in -> LexyParityA -> mid\n"
        "mid -> LexyParityB -> out.text\n"
        "in -> LexyParityA -> out.raw\n";

    auto handWritten = filterGraph::dsl::parseGraphProgram(source);
    auto lexy        = filterGraph::dsl::parseGraphProgramLexy(source);

    REQUIRE(lexy.stages.size() == handWritten.stages.size());
    REQUIRE(lexy.outputs.size() == handWritten.outputs.size());
}

// ---------------------------------------------------------------------------
// DslFilterGraph: running graphs described in the text DSL
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string>& runLog()
{
    static std::vector<std::string> log;
    return log;
}

// Appends "<label>:<value>" to runLog() and passes the value on unchanged.
class RecordFilter : public MessageFilter<int>
{
public:
    explicit RecordFilter(std::string label)
        : mLabel(std::move(label))
    {
    }

    std::optional<int> filter(int&& value) override
    {
        runLog().push_back(std::format("{}:{}", mLabel, value));
        return value;
    }

private:
    std::string mLabel;
};

static FilterRegistrar<DoubleFilter>   registerRunDouble("RunDouble");
static FilterRegistrar<ToStringFilter> registerRunToString("RunToString");
static FilterRegistrar<DropOddFilter>  registerRunDropOdd("RunDropOdd");
static FilterRegistrar<VoidSinkFilter> registerRunVoidSink("RunVoidSink");

static FilterRegistrar<RecordFilter> registerRunRecord("RunRecord", [](const nlohmann::json& config) {
    return std::make_shared<RecordFilter>(config.value("label", std::string{}));
});

static FilterRegistrar<DoubleFilter> registerRunNeedsK("RunNeedsK", [](const nlohmann::json& config) {
    (void)config.at("k").get<int>();
    return std::make_shared<DoubleFilter>();
});

// Describes its fan-in slots, e.g. "8+4 (0 holes)".
static const bool sRegisterRunDescribe = [] {
    registerMergeFilter<std::string>("RunDescribe", [](MergeInputs&& inputs) -> std::optional<std::string> {
        std::string joined;
        std::size_t holes = 0;
        for (const auto& input : inputs)
        {
            if (!input.has_value())
            {
                ++holes;
                continue;
            }
            if (!joined.empty())
            {
                joined += '+';
            }
            joined += std::to_string(std::any_cast<int>(input));
        }
        return std::format("{} ({} hole{})", joined, holes, holes == 1 ? "" : "s");
    });
    return true;
}();

// A DSL graph is itself a MessageFilter, so it can be registered as a stage.
static FilterRegistrar<DslFilterGraph<int, int>> registerRunQuadruple("RunQuadruple", [](const nlohmann::json&) {
    return std::make_shared<DslFilterGraph<int, int>>("in -> RunDouble -> twice -> RunDouble -> out");
});

template <typename InputType, typename OutputType>
std::vector<dsl::TextDiagnostic> buildErrors(std::string_view text)
{
    try
    {
        DslFilterGraph<InputType, OutputType> graph(text);
    }
    catch (const GraphError& error)
    {
        return error.diagnostics();
    }
    return {};
}

bool mentions(const std::vector<dsl::TextDiagnostic>& diagnostics, std::string_view text)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const dsl::TextDiagnostic& d) {
        return d.message.find(text) != std::string::npos;
    });
}

} // namespace

TEST_CASE("DslFilterGraph runs a chain and converts types", "[DslFilterGraph]")
{
    DslFilterGraph<int, std::string> graph("in -> RunDouble -> doubled -> RunToString -> out");
    REQUIRE(graph.filter(21).value() == "42");
}

TEST_CASE("DslFilterGraph propagates a drop downstream", "[DslFilterGraph]")
{
    DslFilterGraph<int, std::string> graph("in -> RunDropOdd -> even -> RunToString -> out");
    REQUIRE(graph.filter(4).value() == "4");
    REQUIRE_FALSE(graph.filter(3).has_value());
}

TEST_CASE("DslFilterGraph gives every reader of an edge its own copy, in source order", "[DslFilterGraph]")
{
    runLog().clear();
    DslFilterGraph<int, int> graph(R"(
        in -> RunRecord(label=tap) -> end
        in -> RunDouble -> doubled -> RunRecord(label=main) -> out
    )");

    REQUIRE(graph.filter(5).value() == 10);
    REQUIRE(runLog() == std::vector<std::string>{"tap:5", "main:10"});
}

TEST_CASE("DslFilterGraph runs a stage only after its inputs are produced", "[DslFilterGraph]")
{
    runLog().clear();
    DslFilterGraph<int, int> graph(R"(
        doubled -> RunRecord(label=late) -> out
        in -> RunDouble -> doubled
    )");

    REQUIRE(graph.filter(2).value() == 4);
    REQUIRE(runLog() == std::vector<std::string>{"late:4"});
}

TEST_CASE("DslFilterGraph merges fan-in edges and leaves holes for dropped paths", "[DslFilterGraph]")
{
    DslFilterGraph<int, std::string> graph(R"(
        in -> RunDouble -> doubled
        in -> RunDropOdd -> even
        (doubled, even) -> RunDescribe -> out
    )");

    REQUIRE(graph.filter(4).value() == "8+4 (0 holes)");
    REQUIRE(graph.filter(3).value() == "6 (1 hole)");
}

TEST_CASE("DslFilterGraph skips a merge when every input was dropped", "[DslFilterGraph]")
{
    DslFilterGraph<int, std::string> graph(R"(
        in -> RunDropOdd -> a
        in -> RunDropOdd -> b
        (a, b) -> RunDescribe -> out
    )");

    REQUIRE_FALSE(graph.filter(3).has_value());
}

TEST_CASE("DslFilterGraph returns several keyed outputs as GraphOutputs", "[DslFilterGraph]")
{
    DslFilterGraph<int> graph(R"(
        in -> RunDouble   -> out.doubled
        in -> RunToString -> out.text
        in -> RunDropOdd  -> out.even
    )");

    auto outputs = graph.filter(3);
    REQUIRE(outputs.has_value());
    REQUIRE(outputs->size() == 3);
    REQUIRE(outputs->get<int>("doubled").value() == 6);
    REQUIRE(outputs->get<std::string>(1).value() == "3");
    REQUIRE_FALSE(outputs->has("even"));
    REQUIRE_THROWS_AS(outputs->get<int>("missing"), std::out_of_range);
    REQUIRE_THROWS_AS(outputs->get<std::string>("doubled"), std::bad_any_cast);

    REQUIRE(outputs->take<std::string>("text").value() == "3");
    REQUIRE_FALSE(outputs->has("text"));

    DslFilterGraph<int> allDropped("in -> RunDropOdd -> out.a\nin -> RunDropOdd -> out.b\n");
    REQUIRE_FALSE(allDropped.filter(3).has_value());
}

TEST_CASE("DslFilterGraph with a Void output runs a sink-only graph", "[DslFilterGraph]")
{
    runLog().clear();
    DslFilterGraph<int, Void> graph("in -> RunRecord(label=sink) -> end\nin -> RunVoidSink -> end\n");

    REQUIRE(graph.filter(7).has_value());
    REQUIRE(runLog() == std::vector<std::string>{"sink:7"});
}

TEST_CASE("DslFilterGraph nests a registered DSL graph as a stage", "[DslFilterGraph]")
{
    DslFilterGraph<int, int> graph("in -> RunQuadruple -> out");
    REQUIRE(graph.filter(3).value() == 12);

    JsonFilterGraph<int, int> jsonGraph(nlohmann::json::parse(R"([ { "type": "RunQuadruple" } ])"));
    REQUIRE(jsonGraph.filter(3).value() == 12);
}

TEST_CASE("DslFilterGraph runs a program parsed by the lexy front-end", "[DslFilterGraph]")
{
    DslFilterGraph<int, std::string> graph(
        dsl::parseGraphProgramLexy("in -> RunDouble -> doubled -> RunToString -> out\n"));
    REQUIRE(graph.filter(21).value() == "42");
}

TEST_CASE("DslFilterGraph reports every build problem at once, located and sorted", "[DslFilterGraph]")
{
    const auto diagnostics = buildErrors<int, int>("in -> RunToString -> text -> RunDouble -> out\n"
                                                   "in -> RunNeedsK -> end\n"
                                                   "in -> RunDescribe -> end\n");

    REQUIRE(diagnostics.size() == 3);
    REQUIRE(diagnostics[0].loc.line == 1);
    REQUIRE(diagnostics[0].loc.column == 30);
    REQUIRE(diagnostics[0].message.find("stage 'RunDouble' expects input type") != std::string::npos);
    REQUIRE(diagnostics[0].message.find("edge 'text' carries") != std::string::npos);
    REQUIRE(diagnostics[1].loc.line == 2);
    REQUIRE(diagnostics[1].message.find("could not construct 'RunNeedsK'") != std::string::npos);
    REQUIRE(diagnostics[2].loc.line == 3);
    REQUIRE(diagnostics[2].message.find("'RunDescribe' is a merge stage") != std::string::npos);

    try
    {
        DslFilterGraph<int, int> graph("in -> RunNeedsK -> out");
        FAIL("expected a GraphError");
    }
    catch (const GraphError& error)
    {
        REQUIRE(std::string(error.what()).find("1:7: could not construct 'RunNeedsK'") != std::string::npos);
    }
}

TEST_CASE("DslFilterGraph checks fan-in, Void and the graph input type", "[DslFilterGraph]")
{
    REQUIRE(mentions(buildErrors<int, int>("in -> RunDouble -> a\nin -> RunDouble -> b\n(a, b) -> RunDouble -> out"),
                     "cannot follow a fan-in group"));
    REQUIRE(mentions(buildErrors<int, GraphOutputs>("in -> RunVoidSink -> out"), "must route to 'end'"));
    REQUIRE(mentions(buildErrors<std::string, int>("in -> RunDouble -> out"), "but the graph input 'in' carries"));
}

TEST_CASE("DslFilterGraph checks the output boundary against its OutputType", "[DslFilterGraph]")
{
    REQUIRE(mentions(buildErrors<int, int>("in -> RunDouble -> out.a\nin -> RunDouble -> out.b"),
                     "needs exactly one '-> out'"));
    REQUIRE(mentions(buildErrors<int, int>("in -> RunDouble -> end"), "needs one '-> out', but the graph has none"));
    REQUIRE(mentions(buildErrors<int, int>("in -> RunToString -> out"), "but 'out' receives"));
    REQUIRE(mentions(buildErrors<int, GraphOutputs>("in -> RunDouble -> end"), "GraphOutputs needs at least one"));
    REQUIRE(mentions(buildErrors<int, Void>("in -> RunDouble -> out"), "route it to 'end' instead"));
    REQUIRE(buildErrors<int, int>("in -> RunDouble -> out").empty());
}

TEST_CASE("validateDslGraph returns diagnostics without throwing", "[DslFilterGraph]")
{
    REQUIRE(validateDslGraph<int, std::string>("in -> RunDouble -> d -> RunToString -> out").empty());

    const auto diagnostics = validateDslGraph<int, int>("in -> RunDoubel -> out\n\nin -> RunNeedsK -> end\n");
    REQUIRE(diagnostics.size() == 2);
    REQUIRE(dsl::formatDiagnostic(diagnostics[0]).starts_with("1:7: unknown stage type 'RunDoubel'"));
    REQUIRE(diagnostics[0].message.find("did you mean 'RunDouble'?") != std::string::npos);
    REQUIRE(diagnostics[1].loc.line == 3);
}

TEST_CASE("toMermaid generates node ids and labels outputs, config and dead ends", "[GraphDsl]")
{
    auto program = dsl::parseGraphProgram("in -> RunDouble -> out.twice\nin -> RunRecord(label=tap) -> end\n");
    REQUIRE(program.ok());

    const std::string mermaid = dsl::toMermaid(program);
    REQUIRE(mermaid.starts_with("flowchart LR\n"));
    REQUIRE(mermaid.find("e0([\"in\"])") != std::string::npos);
    REQUIRE(mermaid.find("([\"out.twice\"])") != std::string::npos);
    REQUIRE(mermaid.find("s1[\"RunRecord<br/>label=tap\"]") != std::string::npos);
    REQUIRE(mermaid.find("d0[[\"end\"]]") != std::string::npos);
    REQUIRE(mermaid.find("s1 --> d0") != std::string::npos);
    REQUIRE(mermaid.find('$') == std::string::npos);
}

TEST_CASE("toDot renders the same picture as toMermaid", "[GraphDsl]")
{
    auto program = dsl::parseGraphProgram("in -> RunDouble -> out.twice\nin -> RunRecord(label=tap) -> end\n");
    REQUIRE(program.ok());

    const std::string dot = dsl::toDot(program);
    REQUIRE(dot.starts_with("digraph filterGraph {\n    rankdir=LR;\n"));
    REQUIRE(dot.ends_with("}\n"));
    REQUIRE(dot.find("e0 [shape=ellipse, label=\"in\"];") != std::string::npos);
    REQUIRE(dot.find("[shape=ellipse, label=\"out.twice\"];") != std::string::npos);
    REQUIRE(dot.find("s1 [shape=box, label=\"RunRecord\\nlabel=tap\"];") != std::string::npos);
    REQUIRE(dot.find("d0 [shape=box, peripheries=2, label=\"end\"];") != std::string::npos);
    REQUIRE(dot.find("e0 -> s0;") != std::string::npos);
    REQUIRE(dot.find("s1 -> d0;") != std::string::npos);
    REQUIRE(dot.find('$') == std::string::npos);
}

TEST_CASE("toAscii lists fan-out as siblings and a merge under its group", "[GraphDsl]")
{
    auto program = dsl::parseGraphProgram("in -> RunDouble -> msg\n"
                                          "msg -> RunDropOdd -> valid\n"
                                          "(msg, valid) -> RunDescribe -> out.stats\n");
    REQUIRE(program.ok());

    REQUIRE(dsl::toAscii(program) == "in\n"
                                     "`-> RunDouble -> msg\n"
                                     "    +-> RunDropOdd -> valid\n"
                                     "    |   `-> RunDescribe (merge, see below)\n"
                                     "    `-> RunDescribe (merge, see below)\n"
                                     "\n"
                                     "(msg, valid)\n"
                                     "`-> RunDescribe -> out.stats\n");
}

TEST_CASE("toAscii shows arguments, dead ends and outputs, in plain or Unicode characters", "[GraphDsl]")
{
    auto program = dsl::parseGraphProgram("in -> RunRecord(n=3, s=\"x\") -> end\nin -> RunDouble -> out\n");
    REQUIRE(program.ok());

    REQUIRE(dsl::toAscii(program) == "in\n"
                                     "+-> RunRecord(n=3, s=\"x\") -> end\n"
                                     "`-> RunDouble -> out\n");
    REQUIRE(dsl::toAscii(program, dsl::AsciiStyle::unicode) ==
            "in\n"
            "\xE2\x94\x9C\xE2\x94\x80\xE2\x96\xBA RunRecord(n=3, s=\"x\") \xE2\x94\x80\xE2\x96\xBA end\n"
            "\xE2\x94\x94\xE2\x94\x80\xE2\x96\xBA RunDouble \xE2\x94\x80\xE2\x96\xBA out\n");
}

TEST_CASE("toAscii roots a tree at a graph input read only by a group", "[GraphDsl]")
{
    const auto program = dsl::parseGraphProgram("(in.orders, in.quotes) -> X -> out\n");

    REQUIRE(dsl::toAscii(program) == "in.orders\n"
                                     "`-> X (merge, see below)\n"
                                     "\n"
                                     "in.quotes\n"
                                     "`-> X (merge, see below)\n"
                                     "\n"
                                     "(in.orders, in.quotes)\n"
                                     "`-> X -> out\n");
}

TEST_CASE("toAscii prints every stage of an invalid program once", "[GraphDsl]")
{
    auto program = dsl::parseGraphProgram("a -> X -> b\nb -> Y -> a\n");
    REQUIRE_FALSE(program.ok());

    REQUIRE(dsl::toAscii(program) == "a\n"
                                     "`-> X -> b\n"
                                     "    `-> Y -> a\n"
                                     "        `-> X (see above)\n");
}

TEST_CASE("toDot escapes quotes and backslashes in labels", "[GraphDsl]")
{
    auto program = dsl::parseGraphProgram("in -> RunRecord(label=\"say \\\"hi\\\" \\\\ bye\") -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.stages[0].config["label"] == "say \"hi\" \\ bye");

    const std::string dot = dsl::toDot(program);
    REQUIRE(dot.find("label=\"RunRecord\\nlabel=say \\\"hi\\\" \\\\ bye\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Parser diagnostics: the hand-written and lexy parsers must agree exactly
// ---------------------------------------------------------------------------

namespace {

// Everything a parse produces, as text, so two parses can be compared at once.
std::string describeProgram(const dsl::GraphProgram& program)
{
    std::string text;
    for (const auto& stage : program.stages)
    {
        text += std::format("stage {} @{}:{} config={} inputs=", stage.type, stage.loc.line, stage.loc.column,
                            stage.config.dump());
        for (const auto& input : stage.inputs)
        {
            text += input + ',';
        }
        text += " slots=";
        for (const auto& slot : stage.slotNames)
        {
            text += slot + ',';
        }
        text += std::format(" output={} fanIn={}\n", stage.output.value_or("<end>"), stage.fanIn);
    }
    for (const auto& output : program.outputs)
    {
        text += std::format("output {} key={} index={} @{}:{}\n", output.edge, output.key.value_or("<none>"),
                            output.index, output.loc.line, output.loc.column);
    }
    for (const auto& input : program.inputs)
    {
        text += std::format("input {} key={} @{}:{}\n", input.edge, input.key.value_or("<none>"), input.loc.line,
                            input.loc.column);
    }
    text += std::format("deadEnds={}\n", program.deadEnds.size());
    text += dsl::formatDiagnostics(program.diagnostics);
    return text;
}

// Parses `source` with the lexy parser and the deprecated hand-written parser,
// requires identical results, and returns the diagnostics.
std::vector<dsl::TextDiagnostic> parseWithBoth(std::string_view source)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    const auto handWritten = dsl::parseGraphProgramHandwritten(source);
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    const auto lexy = dsl::parseGraphProgram(source);
    REQUIRE(describeProgram(lexy) == describeProgram(handWritten));
    return lexy.diagnostics;
}

} // namespace

TEST_CASE("Both parsers report the same located syntax errors", "[GraphDslErrors]")
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"in -> RunDouble RunDouble -> out", "1:17: expected '->' but found 'RunDouble'"},
        {"in -> RunDouble ->", "1:19: expected an edge, stage, or group but found end of line"},
        {"in -> RunDouble -> # comment", "1:20: expected an edge, stage, or group but found end of line"},
        {"-> RunDouble -> out", "1:1: expected an edge, stage, or group but found '->'"},
        {R"(in -> RunRecord(label="tap) -> out)", "1:23: unterminated string literal"},
        {"in -> RunRecord(label=tap -> out", "1:27: expected an argument name but found '->'"},
        {"in -> RunRecord(label=tap", "1:16: unterminated argument list; missing ')'"},
        {"in -> RunRecord(3=tap) -> out", "1:17: expected an argument name but found '3'"},
        {"in -> RunRecord(label tap) -> out", "1:23: expected '=' after argument 'label' but found 'tap'"},
        {"in -> RunRecord(label=) -> out", "1:23: expected an argument value but found ')'"},
        {"in -> RunRecord(label=1.2.3) -> out", "1:23: malformed number '1.2.3'"},
        {"in -> RunRecord(n=99999999999999999999) -> out", "1:19: number '99999999999999999999' is out of range"},
        {"(a, b -> RunDouble -> out", "1:7: expected an edge name in group but found '->'"},
        {"(a, b", "1:1: unterminated fan-in group; missing ')'"},
        {"() -> RunDouble -> out", "1:1: a fan-in group needs at least one edge"},
        {"in -> RunDouble -> out.3", "1:24: expected a key name after '.' but found '3'"},
        {"in -> Run@Double -> out", "1:10: unexpected character '@'"},
        {"in -> RunDouble -> -", "1:20: unexpected character '-'"},
        {"in -> RunDouble -> \xC3\xA9", "1:20: unexpected byte 0xC3"},
        {"in -> RunDouble RunDouble -> out\r\n", "1:17: expected '->' but found 'RunDouble'"},
        {"(a: -> RunDouble -> out", "1:5: expected an edge name after 'a:' but found '->'"},
        {"(a: 3) -> RunDouble -> out", "1:5: expected an edge name after 'a:' but found '3'"},
        {"(a b: c) -> RunDouble -> out", "1:1: a fan-in group names either all of its slots or none, e.g. '(a: x, b: y)'"},
        {"(a: x, a: y) -> RunDouble -> out", "1:1: slot 'a' is named twice in the fan-in group"},
        {"in -> RunDouble: -> out", "1:16: expected '->' but found ':'"},
        {"in -> RunDouble -> q.x -> RunDouble -> out", "1:20: only 'in' and 'out' take a '.<key>', not 'q.x'"},
        {"in. -> RunDouble -> out", "1:5: expected a key name after '.' but found '->'"},
        {"(in., b) -> RunDouble -> out", "1:5: expected a key name after '.' but found ','"},
        {"(a: in.) -> RunDouble -> out", "1:8: expected a key name after '.' but found ')'"},
        {"(msg.x, y) -> RunDouble -> out", "1:2: only 'in' and 'out' take a '.<key>', not 'msg.x'"},
        {"(a: x, b: q.k) -> RunDouble -> out", "1:11: only 'in' and 'out' take a '.<key>', not 'q.k'"},
        {"(out, y) -> RunDouble -> out", "1:2: 'out' may only appear as the last term of a statement"},
        {"(y, out.k) -> RunDouble -> out", "1:5: 'out' may only appear as the last term of a statement"},
        {"(end, y) -> RunDouble -> out", "1:2: 'end' may only appear as the last term of a statement"},
    };

    for (const auto& [source, expected] : cases)
    {
        INFO("source: " << source);
        REQUIRE(dsl::formatDiagnostics(parseWithBoth(source)) == expected);
    }
}

TEST_CASE("A syntax error ends only its own statement", "[GraphDslErrors]")
{
    const auto diagnostics = parseWithBoth("in -> RunDouble RunDouble -> x\n"
                                           "in -> RunDouble -> y -> RunRecord(label=\"tap) -> out\n"
                                           "in -> RunDouble -> out\n");

    REQUIRE(dsl::formatDiagnostics(diagnostics)
            == "1:17: expected '->' but found 'RunDouble'\n2:41: unterminated string literal");
}

TEST_CASE("Both parsers agree on graph inputs inside fan-in groups", "[GraphDslErrors]")
{
    parseWithBoth("(in.orders, in.quotes) -> RunDouble -> out\n");
    parseWithBoth("(quote: in . quotes, order: in.orders) -> RunDouble -> out\n");
    parseWithBoth("in.orders -> RunDouble -> a\n(x: a, y: in.quotes) -> RunDouble -> out\n");
    parseWithBoth("in -> RunDouble -> a\n(x: in, y: a) -> RunDouble -> out\n");
}

TEST_CASE("Both parsers agree on strings, numbers and comments", "[GraphDslErrors]")
{
    const char* source = R"dsl(# a comment line
in -> RunRecord(label="#1 \"quoted\" \\ done", ratio=0.5, n=-3, on=false, word=tap) -> recorded   # trailing
recorded -> RunDouble -> out.twice
)dsl";

    REQUIRE(parseWithBoth(source).empty());

    const auto program = dsl::parseGraphProgramLexy(source);
    REQUIRE(program.stages.size() == 2);
    const auto& config = program.stages[0].config;
    REQUIRE(config.at("label").get<std::string>() == R"(#1 "quoted" \ done)");
    REQUIRE(config.at("ratio").get<double>() == 0.5);
    REQUIRE(config.at("n").get<int>() == -3);
    REQUIRE(config.at("on").get<bool>() == false);
    REQUIRE(config.at("word").get<std::string>() == "tap");
    REQUIRE(program.stages[0].loc.line == 2);
    REQUIRE(program.stages[0].loc.column == 7);
    REQUIRE(program.stages[1].loc.column == 13);
}
