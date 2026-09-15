#pragma once

#include <filterGraph/core/filterGraph/GraphLang.hpp>

#include <lexy/action/scan.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Prototype: a lexy-based parser producing the SAME GraphProgram IR as the
// hand-written parser in GraphLang.hpp, kept alongside it for comparison.
//
// The DSL is newline-significant, so the C++ driver splits the input into
// lines (stripping `#` comments and blank lines) and runs a lexy scanner over
// each statement. The scanner uses lexy's imperative scanning interface, which
// classifies each term exactly like `parseNamedTerm`/`parseGroup` in
// GraphLang.hpp so the shared `buildStatement`/`validateProgram` helpers yield
// identical output.
namespace filterGraph::dsl {

namespace detail {
namespace lexy_impl {

namespace ld = lexy::dsl;

// Identifier: [A-Za-z_][A-Za-z0-9_]* as a capturable token.
inline constexpr auto identToken =
    ld::token(ld::ascii::alpha_underscore + ld::while_(ld::ascii::alpha_digit_underscore));

// A run of spaces/tabs (and a stray CR) to discard between tokens.
inline constexpr auto blankToken = ld::token(ld::while_(ld::ascii::blank / ld::lit_c<'\r'>));

// Parses one DSL statement line into a flat term list. Returns false on a
// syntax error (the caller then records a generic "syntax error" diagnostic).
inline bool parseStatementLine(std::string_view lineText, std::size_t lineNo, std::vector<Term>& terms)
{
    auto input = lexy::string_input(lineText.data(), lineText.size());
    auto sc    = lexy::scan(input, lexy::noop);

    const SourceLoc loc{lineNo, 1};

    auto skipBlank = [&] { sc.discard(blankToken); };

    auto captureIdent = [&]() -> std::optional<std::string> {
        auto result = sc.capture(identToken);
        if (!result)
        {
            return std::nullopt;
        }
        auto lexeme = result.value();
        return std::string(lexeme.begin(), lexeme.end());
    };

    auto parseValue = [&]() -> nlohmann::json {
        skipBlank();
        if (sc.peek(ld::lit_c<'"'>))
        {
            sc.parse(ld::lit_c<'"'>);
            auto result = sc.capture(ld::token(ld::while_(ld::ascii::character - ld::lit_c<'"'>)));
            sc.parse(ld::lit_c<'"'>);
            if (!result)
            {
                return nullptr;
            }
            auto lexeme = result.value();
            return std::string(lexeme.begin(), lexeme.end());
        }
        if (sc.peek(ld::lit_c<'-'>) || sc.peek(ld::ascii::digit))
        {
            auto result = sc.capture(ld::token(ld::if_(ld::lit_c<'-'>) + ld::digits<>));
            if (!result)
            {
                return nullptr;
            }
            auto              lexeme = result.value();
            const std::string text(lexeme.begin(), lexeme.end());
            return static_cast<std::int64_t>(std::stoll(text));
        }
        // Bareword: true/false become bools, anything else a string.
        auto word = captureIdent();
        if (!word)
        {
            return nullptr;
        }
        if (*word == "true")
        {
            return true;
        }
        if (*word == "false")
        {
            return false;
        }
        return *word;
    };

    auto parseConfigArgs = [&]() -> nlohmann::json {
        nlohmann::json config = nlohmann::json::object();
        sc.parse(ld::lit_c<'('>);
        while (true)
        {
            skipBlank();
            if (sc.peek(ld::lit_c<')'>) || sc.is_at_eof())
            {
                break;
            }
            auto key = captureIdent();
            if (!key)
            {
                sc.fatal_error("syntax error", sc.position());
                break;
            }
            skipBlank();
            if (!sc.branch(ld::lit_c<'='>))
            {
                sc.fatal_error("syntax error", sc.position());
                break;
            }
            config[*key] = parseValue();
            skipBlank();
            sc.branch(ld::lit_c<','>); // optional separator
        }
        sc.parse(ld::lit_c<')'>);
        return config;
    };

    auto parseGroup = [&]() -> bool {
        Term term;
        term.kind = Term::Kind::Group;
        term.loc  = loc;
        sc.parse(ld::lit_c<'('>);
        while (true)
        {
            skipBlank();
            if (sc.peek(ld::lit_c<')'>) || sc.is_at_eof())
            {
                break;
            }
            if (sc.branch(ld::lit_c<','>))
            {
                continue;
            }
            auto edge = captureIdent();
            if (!edge)
            {
                sc.fatal_error("syntax error", sc.position());
                return false;
            }
            term.edges.push_back(std::move(*edge));
        }
        if (!sc.branch(ld::lit_c<')'>))
        {
            sc.fatal_error("syntax error", sc.position());
            return false;
        }
        terms.push_back(std::move(term));
        return true;
    };

    auto parseNamed = [&]() -> bool {
        Term term;
        term.loc  = loc;
        auto name = captureIdent();
        if (!name)
        {
            sc.fatal_error("syntax error", sc.position());
            return false;
        }
        term.name = std::move(*name);

        // Optional `.key` (only meaningful for `out`).
        if (sc.peek(ld::lit_c<'.'>))
        {
            sc.parse(ld::lit_c<'.'>);
            auto key = captureIdent();
            if (!key)
            {
                sc.fatal_error("syntax error", sc.position());
                return false;
            }
            term.key = std::move(*key);
        }

        // A trailing `(...)` makes this a stage application with config args.
        skipBlank();
        if (sc.peek(ld::lit_c<'('>))
        {
            term.kind   = Term::Kind::Stage;
            term.config = parseConfigArgs();
            terms.push_back(std::move(term));
            return true;
        }

        if (isReserved(term.name))
        {
            term.kind = (term.name == "in") ? Term::Kind::Edge : Term::Kind::Boundary;
        }
        else
        {
            // Positional edge/stage classification is resolved by buildStatement.
            term.kind = Term::Kind::Edge;
        }
        terms.push_back(std::move(term));
        return true;
    };

    auto parseTerm = [&]() -> bool {
        skipBlank();
        if (sc.peek(ld::lit_c<'('>))
        {
            return parseGroup();
        }
        if (sc.peek(ld::ascii::alpha_underscore))
        {
            return parseNamed();
        }
        sc.fatal_error("syntax error", sc.position());
        return false;
    };

    skipBlank();
    if (sc.is_at_eof())
    {
        return true; // empty statement
    }
    if (!parseTerm())
    {
        return false;
    }
    while (true)
    {
        skipBlank();
        if (sc.is_at_eof())
        {
            break;
        }
        if (!sc.branch(LEXY_LIT("->")))
        {
            sc.fatal_error("syntax error", sc.position());
            return false;
        }
        if (!parseTerm())
        {
            return false;
        }
    }
    return static_cast<bool>(sc);
}

} // namespace lexy_impl
} // namespace detail

// Parses and structurally validates a Model-B graph program from text using a
// lexy grammar, producing the SAME IR as parseGraphProgram.
inline GraphProgram parseGraphProgramLexy(std::string_view text)
{
    GraphProgram program;
    std::size_t  outputIndex = 0;
    std::size_t  lineNo      = 0;
    std::size_t  pos         = 0;

    while (pos <= text.size())
    {
        const std::size_t      newline = text.find('\n', pos);
        const std::string_view raw =
            (newline == std::string_view::npos) ? text.substr(pos) : text.substr(pos, newline - pos);
        ++lineNo;

        // Strip a `#` comment to end-of-line.
        const std::size_t hash = raw.find('#');
        std::string_view  line = (hash == std::string_view::npos) ? raw : raw.substr(0, hash);

        const bool blank = line.find_first_not_of(" \t\r") == std::string_view::npos;
        if (!blank)
        {
            std::vector<detail::Term> terms;
            if (detail::lexy_impl::parseStatementLine(line, lineNo, terms) && !terms.empty())
            {
                detail::buildStatement(terms, program, outputIndex, program.diagnostics);
            }
            else
            {
                program.diagnostics.push_back({SourceLoc{lineNo, 1}, "syntax error"});
            }
        }

        if (newline == std::string_view::npos)
        {
            break;
        }
        pos = newline + 1;
    }

    detail::validateProgram(program);
    return program;
}

} // namespace filterGraph::dsl
