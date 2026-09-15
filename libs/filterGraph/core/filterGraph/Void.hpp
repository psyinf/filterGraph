#pragma once

namespace filterGraph {

// Void is an explicit terminal marker for a path: a stage declared as
// MessageFilter<InputType, Void> states "I intentionally produce no output;
// the chain ends here". It is a first-class type (carried through std::any
// like any other message), so it is distinct from a stage returning
// std::nullopt, which means "this message was dropped / could not be
// processed" rather than "this is a deliberate dead-end".
//
// A Void stage must be the last stage in a path; AnyFilterChain rejects any
// stage placed after it at construction time.
struct Void
{
    friend bool operator==(const Void&, const Void&) = default;
};

} // namespace filterGraph
