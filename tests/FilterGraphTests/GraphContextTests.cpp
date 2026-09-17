#include <filterGraph/core/filterGraph/AnyMessageFilter.hpp>
#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FanoutFilter.hpp>
#include <filterGraph/core/filterGraph/FilterGraph.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphContext.hpp>
#include <filterGraph/core/filterGraph/JoinFilter.hpp>
#include <filterGraph/core/filterGraph/JsonFilterGraph.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <any>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace filterGraph;

namespace {

struct FrameNo
{
    std::uint64_t value{};
};

struct SourceName
{
    std::string value;
};

struct Counter
{
    int value{};
};

class AppContext : public GraphContext
{
public:
    std::string sessionId = "session-1";
};

class OtherContext : public GraphContext
{
};

struct SeenFrame
{
    std::uint64_t value{};
};

// Publishes each message as the current frame number.
class PublishFrameFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override
    {
        context().set(FrameNo{static_cast<std::uint64_t>(value)});
        return value;
    }
};

// Replaces the message by the published frame number (0 if none was published).
class ReadFrameFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&&) override
    {
        return static_cast<int>(context().getOr(FrameNo{}).value);
    }
};

// Records the frame number it saw, for side branches whose output is discarded.
class RecordFrameFilter : public MessageFilter<int>
{
public:
    std::optional<int> filter(int&& value) override
    {
        context().set(SeenFrame{context().getOr(FrameNo{}).value});
        return value;
    }
};

std::shared_ptr<AnyMessageFilter> erase(std::shared_ptr<MessageFilter<int>> filter)
{
    return std::make_shared<AnyMessageFilterAdapter<MessageFilter<int>>>(std::move(filter));
}

} // namespace

TEST_CASE("A stage outside a graph has an empty context of its own", "[GraphContext]")
{
    ReadFrameFilter read;
    REQUIRE(read.sharedContext() != nullptr);
    REQUIRE_FALSE(read.context().contains<FrameNo>());
    REQUIRE(read.filter(5) == 0);

    read.context().set(FrameNo{4});
    REQUIRE(read.filter(5) == 4);

    ReadFrameFilter other;
    REQUIRE(other.sharedContext() != read.sharedContext());
    REQUIRE_FALSE(other.context().contains<FrameNo>());
}

TEST_CASE("A graph gives its stages a shared context without setContext", "[GraphContext]")
{
    FilterGraph<PublishFrameFilter, ReadFrameFilter> graph(
        std::make_shared<PublishFrameFilter>(), std::make_shared<ReadFrameFilter>());

    REQUIRE(graph.sharedContext() != nullptr);
    REQUIRE(graph.filter(5) == 5);
    REQUIRE(graph.context().get<FrameNo>()->value == 5);
}

TEST_CASE("setContext(nullptr) installs a fresh empty context", "[GraphContext]")
{
    ReadFrameFilter read;
    read.context().set(FrameNo{3});

    read.setContext(nullptr);

    REQUIRE(read.sharedContext() != nullptr);
    REQUIRE_FALSE(read.context().contains<FrameNo>());
}

TEST_CASE("FilterGraph hands its context to every stage", "[GraphContext]")
{
    FilterGraph<PublishFrameFilter, ReadFrameFilter> graph(
        std::make_shared<PublishFrameFilter>(), std::make_shared<ReadFrameFilter>());

    auto ctx = std::make_shared<GraphContext>();
    graph.setContext(ctx);

    REQUIRE(graph.sharedContext() == ctx);
    REQUIRE(graph.filter(5) == 5);
    REQUIRE(ctx->get<FrameNo>()->value == 5);
}

TEST_CASE("DslFilterGraph lets stages exchange values through the context", "[GraphContext]")
{
    static FilterRegistrar<PublishFrameFilter> registerPublish("CtxPublish");
    static FilterRegistrar<ReadFrameFilter>    registerRead("CtxRead");

    DslFilterGraph<int, int> graph(R"(
        in -> CtxPublish -> published
        published -> CtxRead -> out
    )");

    auto ctx = std::make_shared<GraphContext>();
    graph.setContext(ctx);

    REQUIRE(graph.filter(7) == 7);
    REQUIRE(graph.filter(8) == 8);
    REQUIRE(ctx->get<FrameNo>()->value == 8);
}

TEST_CASE("Nested JSON fanout and join paths receive the context", "[GraphContext]")
{
    static FilterRegistrar<PublishFrameFilter> registerPublish("CtxJsonPublish");
    static FilterRegistrar<ReadFrameFilter>    registerRead("CtxJsonRead");
    static FilterRegistrar<RecordFrameFilter>  registerRecord("CtxJsonRecord");
    registerFanoutFilter<int>("CtxJsonFanout");
    registerJoinFilter<int, int>("CtxJsonJoin", [](std::vector<std::any>&& slots) -> std::optional<int> {
        return std::any_cast<int>(slots.at(0));
    });

    JsonFilterGraph<int, int> graph(nlohmann::json::parse(R"([
        { "type": "CtxJsonPublish" },
        { "type": "CtxJsonFanout", "config": { "branches": [ [ { "type": "CtxJsonRecord" } ] ] } },
        { "type": "CtxJsonJoin",   "config": { "paths":    [ [ { "type": "CtxJsonRead" } ] ] } }
    ])"));

    auto ctx = std::make_shared<GraphContext>();
    graph.setContext(ctx);

    REQUIRE(graph.filter(11) == 11);
    REQUIRE(ctx->get<SeenFrame>()->value == 11);
}

TEST_CASE("A receiver added after setContext still receives the context", "[GraphContext]")
{
    auto ctx = std::make_shared<GraphContext>();
    ctx->set(FrameNo{9});

    FanoutFilter<int> fanout;
    fanout.setContext(ctx);
    fanout.addReceiver(erase(std::make_shared<RecordFrameFilter>()));

    REQUIRE(fanout.filter(3) == 3);
    REQUIRE(ctx->get<SeenFrame>()->value == 9);
}

TEST_CASE("GraphContext can be extended and recovered polymorphically", "[GraphContext]")
{
    std::shared_ptr<GraphContext> ctx = std::make_shared<AppContext>();
    ctx->set(FrameNo{5});

    REQUIRE(ctx->as<AppContext>() != nullptr);
    REQUIRE(ctx->as<AppContext>()->sessionId == "session-1");
    REQUIRE(ctx->as<OtherContext>() == nullptr);
    REQUIRE(ctx->as<AppContext>()->get<FrameNo>()->value == 5);

    std::shared_ptr<const GraphContext> constCtx = ctx;
    REQUIRE(constCtx->as<AppContext>() != nullptr);
}

TEST_CASE("GraphContext returns nothing for unset types", "[GraphContext]")
{
    GraphContext ctx;

    REQUIRE_FALSE(ctx.contains<FrameNo>());
    REQUIRE_FALSE(ctx.get<FrameNo>().has_value());
    REQUIRE(ctx.getOr(FrameNo{7}).value == 7);
    REQUIRE_FALSE(ctx.erase<FrameNo>());
}

TEST_CASE("GraphContext stores one value per type", "[GraphContext]")
{
    GraphContext ctx;
    ctx.set(FrameNo{42});
    ctx.set(SourceName{"camera"});

    REQUIRE(ctx.contains<FrameNo>());
    REQUIRE(ctx.get<FrameNo>()->value == 42);
    REQUIRE(ctx.get<SourceName>()->value == "camera");

    ctx.set(FrameNo{43});
    REQUIRE(ctx.get<FrameNo>()->value == 43);
    REQUIRE(ctx.getOr(FrameNo{0}).value == 43);
}

TEST_CASE("GraphContext hands out copies, not references", "[GraphContext]")
{
    GraphContext ctx;
    ctx.set(SourceName{"camera"});

    auto copy  = *ctx.get<SourceName>();
    copy.value = "changed";

    REQUIRE(ctx.get<SourceName>()->value == "camera");
}

TEST_CASE("GraphContext erase and clear remove values", "[GraphContext]")
{
    GraphContext ctx;
    ctx.set(FrameNo{1});
    ctx.set(SourceName{"camera"});

    REQUIRE(ctx.erase<FrameNo>());
    REQUIRE_FALSE(ctx.contains<FrameNo>());
    REQUIRE(ctx.contains<SourceName>());

    ctx.clear();
    REQUIRE_FALSE(ctx.contains<SourceName>());
}

TEST_CASE("GraphContext update modifies existing values only", "[GraphContext]")
{
    GraphContext ctx;

    bool called = false;
    REQUIRE_FALSE(ctx.update<Counter>([&](Counter&) { called = true; }));
    REQUIRE_FALSE(called);

    ctx.set(Counter{1});
    REQUIRE(ctx.update<Counter>([](Counter& counter) { ++counter.value; }));
    REQUIRE(ctx.get<Counter>()->value == 2);
}

TEST_CASE("GraphContext shares non-copyable payloads via shared_ptr", "[GraphContext]")
{
    GraphContext ctx;
    ctx.set(std::make_shared<std::vector<int>>(std::vector{1, 2, 3}));

    auto shared = ctx.get<std::shared_ptr<std::vector<int>>>();
    REQUIRE(shared.has_value());
    (*shared)->push_back(4);

    REQUIRE((*ctx.get<std::shared_ptr<std::vector<int>>>())->size() == 4);
}

TEST_CASE("GraphContext update is atomic across threads", "[GraphContext]")
{
    GraphContext ctx;
    ctx.set(Counter{0});

    constexpr int threads    = 8;
    constexpr int increments = 1000;

    std::vector<std::jthread> workers;
    for (int t = 0; t < threads; ++t)
    {
        workers.emplace_back([&] {
            for (int i = 0; i < increments; ++i)
            {
                ctx.update<Counter>([](Counter& counter) { ++counter.value; });
            }
        });
    }
    workers.clear();

    REQUIRE(ctx.get<Counter>()->value == threads * increments);
}
