#include <filterGraph/core/filterGraph/AnyFilterChain.hpp>
#include <filterGraph/core/filterGraph/FanoutFilter.hpp>
#include <filterGraph/core/filterGraph/FilterGraph.hpp>
#include <filterGraph/core/filterGraph/GraphLang.hpp>
#include <filterGraph/core/filterGraph/GraphLangLexy.hpp>
#include <filterGraph/core/filterGraph/GraphValidator.hpp>
#include <filterGraph/core/filterGraph/JoinFilter.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>
#include <filterGraph/core/filterGraph/Void.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
        registerJoinFilter<int, int>("DslMerge",
                                     [](std::vector<std::any>&&) -> std::optional<int> { return 0; });
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
        registerJoinFilter<int, int>("LexyMerge",
                                     [](std::vector<std::any>&&) -> std::optional<int> { return 0; });
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
