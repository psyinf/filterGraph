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
// GraphContext of the graph it runs in. The graph hands it over once via
// setContext() before messages are processed; a stage used outside a graph, or
// in a graph without a context, sees nullptr.
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
    virtual void setContext(std::shared_ptr<GraphContext> context)
    {
        mContext = std::move(context);
    }

    [[nodiscard]] const std::shared_ptr<GraphContext>& context() const noexcept
    {
        return mContext;
    }

private:
    std::shared_ptr<GraphContext> mContext;
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
