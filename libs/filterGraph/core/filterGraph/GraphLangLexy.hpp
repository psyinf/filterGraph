#pragma once

#include <filterGraph/core/filterGraph/GraphLang.hpp>

#include <string_view>

// The lexy-based parser now lives in GraphLang.hpp and backs
// dsl::parseGraphProgram. This header keeps the name introduced in 0.2.0
// working.
namespace filterGraph::dsl {

// Same as parseGraphProgram.
inline GraphProgram parseGraphProgramLexy(std::string_view text)
{
    return parseGraphProgram(text);
}

} // namespace filterGraph::dsl
