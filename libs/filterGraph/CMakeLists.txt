project(filterGraph)

add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_sources(${PROJECT_NAME}
    INTERFACE
    core/filterGraph/Void.hpp
    core/filterGraph/MessageFilter.hpp
    core/filterGraph/FilterGraph.hpp
    core/filterGraph/AnyMessageFilter.hpp
    core/filterGraph/FilterRegistry.hpp
    core/filterGraph/AnyFilterChain.hpp
    core/filterGraph/JsonFilterGraph.hpp
    core/filterGraph/FanoutFilter.hpp
)

target_link_libraries(${PROJECT_NAME}
    INTERFACE
        nlohmann_json::nlohmann_json
)

target_include_directories(
    ${PROJECT_NAME}
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_20)
