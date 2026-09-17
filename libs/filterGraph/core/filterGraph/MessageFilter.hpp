#pragma once

#include <filterGraph/core/filterGraph/GraphContext.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace filterGraph {

// Base interface for a single processing stage. A MessageFilter consumes an
// InputType (by rvalue reference, so it may move from / mutate it freely) and
// produces an std::optional<OutputType>: returning std::nullopt allows a
// stage to short-circuit (terminate) a chain early, e.g. when a message
// should be dropped/filtered out.
//
// A stage may read and publish side-channel data through context(), the
// GraphContext of the graph it runs in. A context always exists: every stage
// starts with its own empty one, every graph hands its context to its stages
// when it is built, and setContext() replaces it (e.g. with an application's
// derived context). So context() never needs a null check; a stage used
// outside a graph simply talks to a context nobody else sees.
template <typename InputType, typename OutputType = InputType>
class MessageFilter
{
public:
    using InType  = InputType;
    using OutType = OutputType;

    virtual ~MessageFilter()                                    = default;
    virtual std::optional<OutputType> filter(InputType&& input) = 0;

    // Composite stages override this to forward the context to their inner
    // stages. Not meant to be called while messages are being processed.
    // Passing nullptr installs a fresh empty context rather than none, so the
    // "there is always a context" invariant holds unconditionally.
    virtual void setContext(std::shared_ptr<GraphContext> context)
    {
        mContext = context ? std::move(context) : std::make_shared<GraphContext>();
    }

    // The context this stage runs in. Never null.
    [[nodiscard]] GraphContext& context() noexcept
    {
        return *mContext;
    }

    [[nodiscard]] const GraphContext& context() const noexcept
    {
        return *mContext;
    }

    // The same context as a shared_ptr, for composites handing it to stages
    // they own.
    [[nodiscard]] const std::shared_ptr<GraphContext>& sharedContext() const noexcept
    {
        return mContext;
    }

private:
    std::shared_ptr<GraphContext> mContext = std::make_shared<GraphContext>();
};

// Generic terminal stage for a FilterGraph. Consumes InputType via a caller
// supplied sink callback (e.g. writing to a socket, logging, storage, ...)
// and returns a simple status code, so it can sit at the end of a chain
// without needing to produce a further transformable output.
//
// Configured entirely from the outside: no subclassing required, just pass a
// callable to the constructor.
template <typename InputType>
class SinkFilter : public MessageFilter<InputType, int>
{
public:
    using Sink = std::function<void(const InputType&)>;

    explicit SinkFilter(Sink sink)
        : mSink(std::move(sink))
    {
    }

    std::optional<int> filter(InputType&& data) override
    {
        mSink(data);
        return 0;
    }

private:
    Sink mSink;
};

} // namespace filterGraph
