include (cmake/setup_cpm.cmake)

CPMAddPackage(
    NAME Catch2

    GITHUB_REPOSITORY "catchorg/Catch2"
    GIT_TAG v3.5.2
     OPTIONS
    "CATCH_BUILD_TESTING OFF"
    "CATCH_BUILD_EXAMPLES OFF"
    "CATCH_BUILD_EXTRA_TESTS OFF"
    "CATCH_BUILD_FUZZERS OFF"
)

CPMAddPackage(
    NAME nlohmann_json
    GITHUB_REPOSITORY "nlohmann/json"
    GIT_TAG v3.11.3
    OPTIONS
    "JSON_BuildTests OFF"
)

CPMAddPackage(
    NAME lexy
    GITHUB_REPOSITORY "foonathan/lexy"
    GIT_TAG v2025.05.0
    OPTIONS
    "LEXY_BUILD_EXAMPLES OFF"
    "LEXY_BUILD_TESTS OFF"
    "LEXY_BUILD_BENCHMARKS OFF"
    "LEXY_BUILD_DOCS OFF"
    "LEXY_BUILD_PACKAGE OFF"
)
