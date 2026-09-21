#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphParameters.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace filterGraph;

namespace {

// Adds `amount` to its input.
class AddFilter : public MessageFilter<int>
{
public:
    explicit AddFilter(int amount)
        : mAmount(amount)
    {
    }

    std::optional<int> filter(int&& value) override
    {
        return value + mAmount;
    }

private:
    int mAmount;
};

// Drops values outside [min, max]; the bounds come as one nested object,
// `range={"min": ..., "max": ...}`.
class ClampFilter : public MessageFilter<int>
{
public:
    ClampFilter(int min, int max)
        : mMin(min)
        , mMax(max)
    {
    }

    std::optional<int> filter(int&& value) override
    {
        return value < mMin || value > mMax ? std::nullopt : std::optional<int>(value);
    }

private:
    int mMin;
    int mMax;
};

static FilterRegistrar<AddFilter> registerAdd("Add", [](const nlohmann::json& config) {
    return std::make_shared<AddFilter>(config.at("amount").get<int>());
});

static FilterRegistrar<ClampFilter> registerClamp("Clamp", [](const nlohmann::json& config) {
    const auto& range = config.at("range");
    return std::make_shared<ClampFilter>(range.at("min").get<int>(), range.at("max").get<int>());
});

std::vector<std::string> messages(const std::vector<dsl::TextDiagnostic>& diagnostics)
{
    std::vector<std::string> texts;
    for (const auto& diagnostic : diagnostics)
    {
        texts.push_back(dsl::formatDiagnostic(diagnostic));
    }
    return texts;
}

// A fresh directory for graph and parameter files, removed at the end of a test.
class TempDir
{
public:
    TempDir()
    {
        // Test cases may run in parallel processes, so the name is random.
        std::random_device device;
        mPath = std::filesystem::temp_directory_path() /
                ("filterGraphParameterTests-" + std::to_string(device()) + "-" + std::to_string(device()));
        std::filesystem::remove_all(mPath);
        std::filesystem::create_directories(mPath);
    }

    ~TempDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(mPath, ignored);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path write(const std::string& name, std::string_view content) const
    {
        const auto    path = mPath / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream << content;
        return path;
    }

private:
    std::filesystem::path mPath;
};

} // namespace

TEST_CASE("A $name argument takes its value from the parameters", "[Parameters]")
{
    DslFilterGraph<int, int> graph(dsl::parseGraphProgram("in -> Add(amount=$step) -> out",
                                                          nlohmann::json{{"step", 5}}));
    REQUIRE(graph.filter(1) == 6);
}

TEST_CASE("Dots in a parameter name walk into nested objects", "[Parameters]")
{
    const nlohmann::json parameters = {{"tracker", {{"step", 2}, {"window", {{"min", 0}, {"max", 10}}}}}};
    DslFilterGraph<int, int> graph(dsl::parseGraphProgram(
        "in -> Add(amount=$tracker.step) -> stepped -> Clamp(range=$tracker.window) -> out", parameters));

    REQUIRE(graph.filter(3) == 5);
    REQUIRE(graph.filter(9) == std::nullopt); // 11 is out of range
}

TEST_CASE("Parameters mix with literal arguments, and the last value for a key wins", "[Parameters]")
{
    const nlohmann::json parameters = {{"step", 5}};
    DslFilterGraph<int, int> literalLast(dsl::parseGraphProgram("in -> Add(amount=$step, amount=1) -> out", parameters));
    DslFilterGraph<int, int> parameterLast(dsl::parseGraphProgram("in -> Add(amount=1, amount=$step) -> out", parameters));

    REQUIRE(literalLast.filter(0) == 1);
    REQUIRE(parameterLast.filter(0) == 5);
}

TEST_CASE("Parameters the graph does not use are not an error", "[Parameters]")
{
    const nlohmann::json parameters = {{"step", 1}, {"otherGraph", {{"threshold", 0.5}}}};
    REQUIRE(validateDslGraph<int, int>(dsl::parseGraphProgram("in -> Add(amount=$step) -> out", parameters)).empty());
}

TEST_CASE("An unknown parameter is located and a close name is suggested", "[Parameters]")
{
    const auto program =
        dsl::parseGraphProgram("in -> Add(amount=$tracker.stp) -> out", nlohmann::json{{"tracker", {{"step", 1}}}});

    REQUIRE(messages(program.diagnostics) ==
            std::vector<std::string>{"1:18: unknown parameter '$tracker.stp' — did you mean '$tracker.step'?"});
    // Reported once, not again as "not set" when the graph is built.
    REQUIRE(messages(validateDslGraph<int, int>(program)) == messages(program.diagnostics));
}

TEST_CASE("A graph whose parameters were never bound reports each as not set", "[Parameters]")
{
    const auto diagnostics = validateDslGraph<int, int>("in -> Add(amount=$step) -> added -> Add(amount=$step) -> out");

    REQUIRE(diagnostics.size() == 2);
    REQUIRE(dsl::formatDiagnostic(diagnostics[0]).starts_with("1:18: parameter '$step' is not set;"));
    REQUIRE(dsl::formatDiagnostic(diagnostics[1]).starts_with("1:48: parameter '$step' is not set;"));
    REQUIRE_THROWS_AS((DslFilterGraph<int, int>("in -> Add(amount=$step) -> out")), GraphError);
}

TEST_CASE("Renderings show unbound parameters by name and bound ones by value", "[Parameters]")
{
    constexpr std::string_view text = "in -> Add(amount=$step) -> out";

    const auto unbound = dsl::parseGraphProgram(text);
    REQUIRE(dsl::toAscii(unbound) == "in\n`-> Add(amount=$step) -> out\n");
    REQUIRE(dsl::toMermaid(unbound).find("amount=$step") != std::string::npos);
    REQUIRE(dsl::toDot(unbound).find("amount=$step") != std::string::npos);

    const auto bound = dsl::parseGraphProgram(text, nlohmann::json{{"step", 3}});
    REQUIRE(dsl::toAscii(bound) == "in\n`-> Add(amount=3) -> out\n");
}

TEST_CASE("Malformed parameter references are syntax errors", "[Parameters]")
{
    REQUIRE(messages(dsl::parseGraphProgram("in -> Add(amount=$) -> out").diagnostics) ==
            std::vector<std::string>{"1:19: expected a parameter name after '$' but found ')'"});
    REQUIRE(messages(dsl::parseGraphProgram("in -> Add(amount=$a.) -> out").diagnostics) ==
            std::vector<std::string>{"1:21: expected a name after '$a.' but found ')'"});
    REQUIRE(messages(dsl::parseGraphProgram("in -> Add(amount=$a b) -> out").diagnostics) ==
            std::vector<std::string>{"1:22: expected '=' after argument 'b' but found ')'"});
}

TEST_CASE("A params line names a parameter file and is not a statement", "[Parameters]")
{
    const auto program = dsl::parseGraphProgram("params \"common.json\"\n"
                                                "  params \"graph.json\"  # overrides common.json\n"
                                                "in -> Add(amount=1) -> out\n");
    REQUIRE(program.ok());
    REQUIRE(program.stages.size() == 1);
    REQUIRE(program.parameterFiles.size() == 2);
    REQUIRE(program.parameterFiles[0].path == "common.json");
    REQUIRE(program.parameterFiles[1].path == "graph.json");
    REQUIRE(program.parameterFiles[1].loc.line == 2);
    REQUIRE(program.parameterFiles[1].loc.column == 10);
}

TEST_CASE("An edge named params still works", "[Parameters]")
{
    DslFilterGraph<int, int> graph("in -> Add(amount=1) -> params\n"
                                   "params -> Add(amount=2) -> out\n");
    REQUIRE(graph.filter(0) == 3);
}

TEST_CASE("Malformed params lines are syntax errors", "[Parameters]")
{
    REQUIRE(messages(dsl::parseGraphProgram("params \"tuning.json").diagnostics) ==
            std::vector<std::string>{"1:8: unterminated string literal"});
    REQUIRE(messages(dsl::parseGraphProgram("params \"tuning.json\" -> Add -> out").diagnostics) ==
            std::vector<std::string>{"1:22: expected end of line after the parameter file but found '->'"});
}

TEST_CASE("Two graph files share one parameter file", "[Parameters]")
{
    TempDir dir;
    dir.write("tuning.json", R"({ "step": 10, "window": { "min": 0, "max": 100 } })");
    const auto add   = dir.write("add.fg", "params \"tuning.json\"\nin -> Add(amount=$step) -> out\n");
    const auto clamp = dir.write("clamp.fg", "params \"tuning.json\"\nin -> Clamp(range=$window) -> out\n");

    DslFilterGraph<int, int> addGraph(dsl::loadGraphProgram(add));
    DslFilterGraph<int, int> clampGraph(dsl::loadGraphProgram(clamp));

    REQUIRE(addGraph.filter(1) == 11);
    REQUIRE(clampGraph.filter(50) == 50);
    REQUIRE(clampGraph.filter(101) == std::nullopt);
}

TEST_CASE("Parameter files are relative to the graph file and override in order", "[Parameters]")
{
    TempDir dir;
    dir.write("shared/common.json", R"({ "step": 1, "window": { "min": 0, "max": 10 } })");
    dir.write("graphs/local.json", R"({ "window": { "max": 20 } })");
    const auto graph = dir.write("graphs/graph.fg", "params \"../shared/common.json\"\n"
                                                    "params \"local.json\"\n"
                                                    "in -> Add(amount=$step) -> added\n"
                                                    "added -> Clamp(range=$window) -> out\n");

    DslFilterGraph<int, int> merged(dsl::loadGraphProgram(graph));
    REQUIRE(merged.filter(14) == 15); // max 20 from local.json, min 0 kept from common.json

    DslFilterGraph<int, int> overridden(dsl::loadGraphProgram(graph, nlohmann::json{{"step", 100}}));
    REQUIRE(overridden.filter(0) == std::nullopt); // 100 > 20
}

TEST_CASE("A parameter file that cannot be read is reported at its params line", "[Parameters]")
{
    TempDir dir;
    dir.write("broken.json", R"({ "step": )");
    dir.write("list.json", "[1, 2]");
    const auto graph = dir.write("graph.fg", "params \"missing.json\"\n"
                                             "params \"broken.json\"\n"
                                             "params \"list.json\"\n"
                                             "in -> Add(amount=$step) -> out\n");

    const auto program = dsl::loadGraphProgram(graph);
    const auto texts   = messages(program.diagnostics);
    REQUIRE(texts.size() == 3); // no follow-on "unknown parameter '$step'"
    REQUIRE(texts[0].starts_with("1:8: cannot open parameter file '"));
    REQUIRE(texts[1].starts_with("2:8: parameter file '"));
    REQUIRE(texts[1].find("is not valid JSON: ") != std::string::npos);
    REQUIRE(texts[2].starts_with("3:8: parameter file '"));
    REQUIRE(texts[2].ends_with("must hold a JSON object, not array"));
    REQUIRE(messages(validateDslGraph<int, int>(program)) == texts);
}

TEST_CASE("A missing graph file throws", "[Parameters]")
{
    TempDir dir;
    REQUIRE_THROWS_AS(dsl::loadGraphProgram(dir.write("tuning.json", "{}").parent_path() / "missing.fg"),
                      std::runtime_error);
}

TEST_CASE("loadParameters binds a graph given as text", "[Parameters]")
{
    TempDir    dir;
    const auto tuning = dir.write("tuning.json", R"({ "step": 7 })");

    DslFilterGraph<int, int> graph(dsl::parseGraphProgram("in -> Add(amount=$step) -> out", dsl::loadParameters(tuning)));
    REQUIRE(graph.filter(0) == 7);
    REQUIRE_THROWS_AS(dsl::loadParameters(dir.write("list.json", "[]")), std::runtime_error);
    dsl::GraphProgram program;
    REQUIRE_THROWS_AS(dsl::bindParameters(program, nlohmann::json::array()), std::invalid_argument);
}
