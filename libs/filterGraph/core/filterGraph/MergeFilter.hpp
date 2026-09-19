#pragma once

#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/MergeStage.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <nlohmann/json.hpp>

#include <any>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace filterGraph {

// MergeInputs (the slots of a fan-in group) and MergeStage (how a merge
// declares its slot types) live in MergeStage.hpp.

// MergeFilter<OutputType> is the fan-in stage of the text DSL: it combines the
// values arriving on several edges into a single OutputType via a
// caller-supplied combiner. It is an ordinary MessageFilter<MergeInputs,
// OutputType>, so it lives in the same FilterRegistry as every other stage; the
// DSL runtime recognises a merge by its MergeInputs input type and gathers the
// group's edges into it.
//
// The combiner has the same signature as JoinFilter's, so a combiner written
// for a JSON Join can be reused as a DSL merge unchanged. Its slots are untyped;
// they are named if `slotNames` is given (see MergeStage::mergeInputNames).
template <typename OutputType>
class MergeFilter
    : public MessageFilter<MergeInputs, OutputType>
    , public MergeStage
{
public:
    using Combiner = std::function<std::optional<OutputType>(MergeInputs&&)>;

    explicit MergeFilter(Combiner combiner, std::vector<std::string> slotNames = {})
        : mCombiner(std::move(combiner))
        , mSlotNames(std::move(slotNames))
    {
    }

    std::optional<OutputType> filter(MergeInputs&& inputs) override
    {
        return mCombiner(std::move(inputs));
    }

    std::vector<std::string> mergeInputNames() const override
    {
        return mSlotNames;
    }

private:
    Combiner                 mCombiner;
    std::vector<std::string> mSlotNames;
};

// Registers MergeFilter<OutputType> under `name` for use as a DSL fan-in stage:
//
//     (a, b) -> Name -> merged
//
// The combiner is copied into every instance of the stage, so state it captures
// is shared by all of them; see TypedMergeFilter/UniformMergeFilter (or a
// hand-written merge stage) for a merge with per-instance state.
template <typename OutputType>
void registerMergeFilter(const std::string& name, typename MergeFilter<OutputType>::Combiner combiner)
{
    FilterRegistrar<MergeFilter<OutputType>> registrar(
        name,
        [combiner = std::move(combiner)](const nlohmann::json&) {
            return std::make_shared<MergeFilter<OutputType>>(combiner);
        });
}

// Registers a merge with named slots, which a DSL group may match by name:
//
//     registerMergeFilter<Stats>("Summarize", {"raw", "checked"}, combiner);
//     (checked: valid, raw: msg) -> Summarize -> stats
//
// The combiner receives the slots in the order of `slotNames`, whatever the
// order of the group.
template <typename OutputType>
void registerMergeFilter(const std::string&                         name,
                         std::vector<std::string>                   slotNames,
                         typename MergeFilter<OutputType>::Combiner combiner)
{
    FilterRegistrar<MergeFilter<OutputType>> registrar(
        name,
        [combiner = std::move(combiner), slotNames = std::move(slotNames)](const nlohmann::json&) {
            return std::make_shared<MergeFilter<OutputType>>(combiner, slotNames);
        });
}

// TypedMergeFilter<OutputType, InputTypes...> is a merge stage that declares
// the type of every one of its slots, so that the DSL checks the edges of its
// fan-in group when the graph is built instead of failing with a
// std::bad_any_cast on the first message.
//
// The slots arrive as std::optionals, in slot order: an empty optional is a
// hole left by a path that dropped the message. Override mergeInputNames() to
// name the slots, so that a group can match them by name, in any order (see
// MergeStage). Implement merge():
//
//     class Summarize : public TypedMergeFilter<Stats, Message, Valid>
//     {
//         std::optional<Stats> merge(std::optional<Message>&& message,
//                                    std::optional<Valid>&& valid) override { ... }
//     };
template <typename OutputType, typename... InputTypes>
class TypedMergeFilter
    : public MessageFilter<MergeInputs, OutputType>
    , public MergeStage
{
public:
    static_assert(sizeof...(InputTypes) > 0, "a TypedMergeFilter needs at least one slot");

    // One argument per slot, in group order; std::nullopt for a dropped path.
    virtual std::optional<OutputType> merge(std::optional<InputTypes>&&... inputs) = 0;

    std::optional<OutputType> filter(MergeInputs&& inputs) final
    {
        if (inputs.size() != sizeof...(InputTypes))
        {
            throw std::invalid_argument(std::format("TypedMergeFilter: expected {} slots but received {}",
                                                    sizeof...(InputTypes),
                                                    inputs.size()));
        }
        return mergeSlots(inputs, std::index_sequence_for<InputTypes...>{});
    }

    MergeSlotTypes mergeInputTypes() const override
    {
        return {{std::type_index(typeid(InputTypes))...}, false};
    }

private:
    template <std::size_t... Slot>
    std::optional<OutputType> mergeSlots(MergeInputs& inputs, std::index_sequence<Slot...>)
    {
        // The pack expansion names each slot's type by position, so the
        // arguments are built in slot order regardless of evaluation order.
        return merge(slot<std::tuple_element_t<Slot, std::tuple<InputTypes...>>>(inputs[Slot])...);
    }

    template <typename SlotType>
    static std::optional<SlotType> slot(std::any& value)
    {
        if (!value.has_value())
        {
            return std::nullopt; // hole: the path feeding this slot dropped the message
        }
        return std::any_cast<SlotType>(std::move(value));
    }
};

// UniformMergeFilter<InputType, OutputType> is a merge stage whose slots all
// carry the same type, for any number of slots: N parallel variants of one
// computation, combined into a single result.
template <typename InputType, typename OutputType>
class UniformMergeFilter
    : public MessageFilter<MergeInputs, OutputType>
    , public MergeStage
{
public:
    // One entry per slot, in group order; std::nullopt for a dropped path.
    virtual std::optional<OutputType> merge(std::vector<std::optional<InputType>>&& inputs) = 0;

    std::optional<OutputType> filter(MergeInputs&& inputs) final
    {
        std::vector<std::optional<InputType>> slots;
        slots.reserve(inputs.size());
        for (auto& input : inputs)
        {
            slots.push_back(input.has_value() ? std::optional<InputType>(std::any_cast<InputType>(std::move(input)))
                                              : std::nullopt);
        }
        return merge(std::move(slots));
    }

    MergeSlotTypes mergeInputTypes() const override
    {
        return {{std::type_index(typeid(InputType))}, true};
    }
};

namespace detail {

// A TypedMergeFilter that calls a caller-supplied function, so that a typed
// merge can be registered from a lambda instead of a subclass.
template <typename OutputType, typename... InputTypes>
class CallableTypedMergeFilter : public TypedMergeFilter<OutputType, InputTypes...>
{
public:
    using Merger = std::function<std::optional<OutputType>(std::optional<InputTypes>&&...)>;

    explicit CallableTypedMergeFilter(Merger merger, std::vector<std::string> slotNames = {})
        : mMerger(std::move(merger))
        , mSlotNames(std::move(slotNames))
    {
    }

    std::optional<OutputType> merge(std::optional<InputTypes>&&... inputs) override
    {
        return mMerger(std::move(inputs)...);
    }

    std::vector<std::string> mergeInputNames() const override
    {
        return mSlotNames;
    }

private:
    Merger                   mMerger;
    std::vector<std::string> mSlotNames;
};

} // namespace detail

// Registers a typed merge stage built from a function, the typed counterpart of
// registerMergeFilter:
//
//     registerTypedMergeFilter<Stats, Message, Valid>(
//         "Summarize",
//         [](std::optional<Message>&& message, std::optional<Valid>&& valid) -> std::optional<Stats> { ... });
//
// As with registerMergeFilter, the function is copied into every instance of
// the stage, so state it captures is shared by all of them.
template <typename OutputType, typename... InputTypes>
void registerTypedMergeFilter(const std::string&                                                          name,
                              typename detail::CallableTypedMergeFilter<OutputType, InputTypes...>::Merger merger)
{
    using Stage = detail::CallableTypedMergeFilter<OutputType, InputTypes...>;
    FilterRegistrar<Stage> registrar(name, [merger = std::move(merger)](const nlohmann::json&) {
        return std::make_shared<Stage>(merger);
    });
}

// Registers a typed merge with named slots, one name per slot type:
//
//     registerTypedMergeFilter<Stats, Message, Valid>("Summarize", {"message", "valid"}, merger);
//     (valid: checked, message: msg) -> Summarize -> stats
template <typename OutputType, typename... InputTypes>
void registerTypedMergeFilter(const std::string&                                                          name,
                              std::vector<std::string>                                                    slotNames,
                              typename detail::CallableTypedMergeFilter<OutputType, InputTypes...>::Merger merger)
{
    using Stage = detail::CallableTypedMergeFilter<OutputType, InputTypes...>;
    FilterRegistrar<Stage> registrar(
        name, [merger = std::move(merger), slotNames = std::move(slotNames)](const nlohmann::json&) {
            return std::make_shared<Stage>(merger, slotNames);
        });
}

} // namespace filterGraph
