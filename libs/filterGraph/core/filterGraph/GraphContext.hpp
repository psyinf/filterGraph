#pragma once

#include <any>
#include <concepts>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <typeindex>
#include <unordered_map>

namespace filterGraph {

// A value stored in a GraphContext: a plain (non-cv, non-reference) copyable
// type. The type itself is the key, so wrap primitives in a dedicated struct
// (struct FrameNo { std::uint64_t value; };) instead of storing a bare int
// that unrelated stages would silently share. Non-copyable or heavy payloads
// can be stored as std::shared_ptr<T>.
template <typename T>
concept ContextValue = std::same_as<T, std::remove_cvref_t<T>> && std::copy_constructible<T>;

class GraphContext;

template <typename T>
concept ContextType = std::derived_from<T, GraphContext>;

// GraphContext is a graph-scoped, type-keyed blackboard: any stage may publish
// a value and any other stage may read it without knowing who wrote it. There
// is at most one value per type.
//
// Values are copied in and out, so no reference to stored state escapes the
// internal lock; this keeps the context safe to share between concurrently
// running paths. Use update() for an atomic read-modify-write.
//
// GraphContext is a polymorphic base: an application may derive its own
// context with dedicated members, hand it to the graph as a GraphContext, and
// recover it in stages via as<Derived>(). Members added by a derived class are
// not covered by the internal lock. Prefer the type-keyed store for stages
// meant to be reusable, as as<Derived>() couples a stage to that type.
class GraphContext
{
public:
    GraphContext()                               = default;
    GraphContext(const GraphContext&)            = delete;
    GraphContext& operator=(const GraphContext&) = delete;
    virtual ~GraphContext()                      = default;

    // Returns this context as Derived, or nullptr if it is not one.
    template <ContextType Derived>
    [[nodiscard]] Derived* as() noexcept
    {
        return dynamic_cast<Derived*>(this);
    }

    template <ContextType Derived>
    [[nodiscard]] const Derived* as() const noexcept
    {
        return dynamic_cast<const Derived*>(this);
    }

    // Stores value, replacing any previous value of the same type.
    template <ContextValue T>
    void set(T value)
    {
        std::unique_lock lock(mMutex);
        mValues.insert_or_assign(std::type_index(typeid(T)), std::any(std::move(value)));
    }

    // Returns a copy of the stored value, or std::nullopt if none was set.
    template <ContextValue T>
    [[nodiscard]] std::optional<T> get() const
    {
        std::shared_lock lock(mMutex);
        if (const auto* value = findLocked<T>())
        {
            return *value;
        }
        return std::nullopt;
    }

    // Returns a copy of the stored value, or fallback if none was set.
    template <ContextValue T>
    [[nodiscard]] T getOr(T fallback) const
    {
        std::shared_lock lock(mMutex);
        if (const auto* value = findLocked<T>())
        {
            return *value;
        }
        return fallback;
    }

    template <ContextValue T>
    [[nodiscard]] bool contains() const
    {
        std::shared_lock lock(mMutex);
        return mValues.contains(std::type_index(typeid(T)));
    }

    // Removes the value of type T; returns whether one was present.
    template <ContextValue T>
    bool erase()
    {
        std::unique_lock lock(mMutex);
        return mValues.erase(std::type_index(typeid(T))) > 0;
    }

    // Atomically modifies an existing value in place; returns false (without
    // calling fn) if no value of type T is present. fn runs under the context's
    // lock and must not access the context itself.
    template <ContextValue T, std::invocable<T&> Fn>
    bool update(Fn&& fn)
    {
        std::unique_lock lock(mMutex);
        auto it = mValues.find(std::type_index(typeid(T)));
        if (it == mValues.end())
        {
            return false;
        }
        std::invoke(std::forward<Fn>(fn), *std::any_cast<T>(&it->second));
        return true;
    }

    void clear()
    {
        std::unique_lock lock(mMutex);
        mValues.clear();
    }

private:
    template <ContextValue T>
    const T* findLocked() const
    {
        auto it = mValues.find(std::type_index(typeid(T)));
        return it == mValues.end() ? nullptr : std::any_cast<T>(&it->second);
    }

    mutable std::shared_mutex                      mMutex;
    std::unordered_map<std::type_index, std::any> mValues;
};

} // namespace filterGraph
