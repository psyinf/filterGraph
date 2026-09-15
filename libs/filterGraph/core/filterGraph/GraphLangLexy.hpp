#pragma once

#include <filterGraph/core/filterGraph/GraphLang.hpp>

#include <lexy/action/scan.hpp>
#include <lexy/dsl.hpp>
#include <lexy/error.hpp>
#include <lexy/input/string_input.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A lexy-based parser producing the SAME GraphProgram IR and the SAME
// diagnostics as the hand-written parser in GraphLang.hpp; the tests check the
// two parsers against each other.
//
// The DSL is newline-significant, so the driver runs a lexy scanner over each
// line. Syntax errors are raised through lexy's error callback, which records
// them as located TextDiagnostics; a syntax error ends its statement, so each
// line reports at most one. Token classification, literal values and the
// "but found ..." descriptions come from helpers shared with the hand-written
// parser, so both report identical messages.
namespace filterGraph::dsl {

namespace detail {
namespace lexy_impl {

namespace ld = lexy::dsl;

// Identifier: [A-Za-z_][A-Za-z0-9_]*
inline constexpr auto identToken =
    ld::token(ld::ascii::alpha_underscore + ld::while_(ld::ascii::alpha_digit_underscore));

// A run of spaces/tabs (and a stray CR) between tokens.
inline constexpr auto blankToken = ld::token(ld::while_(ld::ascii::blank / ld::lit_c<'\r'>));

// Ends a chunk of string contents: the closing quote or the start of an escape.
inline constexpr auto stringStop = ld::literal_set(ld::lit_c<'"'>, ld::lit_c<'\\'>);

// The tag of every syntax error raised by this parser. Its message is prepared
// right before the error is raised (LineErrors::pendingMessage).
struct syntax_error
{
    static constexpr auto name = "syntax error";
};

// Records the errors lexy reports for one line as located diagnostics.
struct LineErrors
{
    const char*                  lineBegin;
    std::size_t                  lineNo;
    std::vector<TextDiagnostic>* diagnostics;
    std::string                  pendingMessage;

    void record(const char* position, std::string message)
    {
        const auto column = static_cast<std::size_t>(position - lineBegin) + 1;
        diagnostics->push_back({SourceLoc{lineNo, column}, std::move(message)});
    }
};

// The lexy error callback. Syntax errors carry their prepared message; lexy's
// own error kinds are formatted generically, so no error is ever reported
// without a location.
struct ErrorCallback
{
    using return_type = void;

    LineErrors* errors;

    template <typename Input, typename Reader>
    void operator()(const lexy::error_context<Input>&, const lexy::error<Reader, void>& error) const
    {
        std::string message = errors->pendingMessage.empty() ? std::string(error.message())
                                                             : std::exchange(errors->pendingMessage, std::string{});
        errors->record(error.position(), std::move(message));
    }

    template <typename Input, typename Reader>
    void operator()(const lexy::error_context<Input>&, const lexy::error<Reader, lexy::expected_literal>& error) const
    {
        errors->record(error.position(),
                       std::format("expected '{}'", std::string_view(error.string(), error.length())));
    }

    template <typename Input, typename Reader>
    void operator()(const lexy::error_context<Input>&, const lexy::error<Reader, lexy::expected_keyword>& error) const
    {
        errors->record(error.position(),
                       std::format("expected keyword '{}'", std::string_view(error.string(), error.length())));
    }

    template <typename Input, typename Reader>
    void operator()(const lexy::error_context<Input>&,
                    const lexy::error<Reader, lexy::expected_char_class>& error) const
    {
        errors->record(error.position(), std::format("expected {}", error.name()));
    }
};

// Parses one line into a flat term list. Returns false after reporting a syntax
// error, in which case the statement must not be built.
inline bool parseStatementLine(std::string_view             lineText,
                               std::size_t                  lineNo,
                               std::vector<Term>&           terms,
                               std::vector<TextDiagnostic>& diagnostics)
{
    LineErrors errors{lineText.data(), lineNo, &diagnostics, {}};
    auto       input = lexy::string_input(lineText.data(), lineText.size());
    auto       sc    = lexy::scan(input, ErrorCallback{&errors});

    const char* const lineEnd = lineText.data() + lineText.size();

    auto locOf = [&](const char* position) {
        return SourceLoc{lineNo, static_cast<std::size_t>(position - lineText.data()) + 1};
    };
    auto rest = [&] {
        return std::string_view(sc.position(), static_cast<std::size_t>(lineEnd - sc.position()));
    };
    auto skipBlank = [&] {
        sc.parse(blankToken);
    };
    auto atEndOfLine = [&] {
        return sc.is_at_eof() || sc.peek(ld::lit_c<'#'>);
    };
    auto captureIdent = [&] {
        auto lexeme = sc.capture(identToken).value();
        return std::string(lexeme.begin(), lexeme.end());
    };

    // Raises a syntax error at `position`; scanning stops.
    auto fail = [&](const char* position, std::string message) {
        errors.pendingMessage = std::move(message);
        sc.fatal_error(syntax_error{}, position);
        return false;
    };

    // Reports what was found at the current position instead of `expected`,
    // or why the input there is invalid.
    auto unexpected = [&](std::string_view expected) {
        const Found found = describeFound(rest());
        return fail(sc.position(), found.invalid ? found.text : std::format("{} but found {}", expected, found.text));
    };

    auto parseValue = [&](nlohmann::json& value) {
        const char* const      start = sc.position();
        const std::string_view text  = rest();

        if (sc.peek(ld::lit_c<'"'>))
        {
            auto literal = scanStringLiteral(text);
            if (!literal)
            {
                return fail(start, "unterminated string literal");
            }
            // Consume chunk by chunk up to the closing quote; an escaped quote
            // or backslash is consumed on its own so it cannot end a chunk.
            sc.parse(ld::lit_c<'"'>);
            while (sc)
            {
                sc.parse(ld::until(stringStop));
                if (!sc || sc.position()[-1] == '"')
                {
                    break;
                }
                if (!sc.branch(ld::lit_c<'"'>))
                {
                    sc.branch(ld::lit_c<'\\'>);
                }
            }
            value = std::move(literal->value);
            return static_cast<bool>(sc);
        }

        if (const std::size_t length = numberLength(text))
        {
            std::string error;
            auto        number = numberValue(text.substr(0, length), error);
            if (!number)
            {
                return fail(start, std::move(error));
            }
            for (std::size_t i = 0; i < length; ++i)
            {
                sc.parse(ld::ascii::character);
            }
            value = std::move(*number);
            return true;
        }

        if (sc.peek(ld::ascii::alpha_underscore))
        {
            const std::string word = captureIdent();
            value = word == "true" ? nlohmann::json(true) : word == "false" ? nlohmann::json(false) : nlohmann::json(word);
            return true;
        }

        return unexpected("expected an argument value");
    };

    auto parseArgs = [&](nlohmann::json& config) {
        const char* const open = sc.position();
        sc.parse(ld::lit_c<'('>);
        config = nlohmann::json::object();
        while (true)
        {
            skipBlank();
            if (sc.branch(ld::lit_c<')'>))
            {
                return true;
            }
            if (atEndOfLine())
            {
                return fail(open, "unterminated argument list; missing ')'");
            }
            if (sc.branch(ld::lit_c<','>))
            {
                continue;
            }
            if (!sc.peek(ld::ascii::alpha_underscore))
            {
                return unexpected("expected an argument name");
            }
            const std::string key = captureIdent();
            skipBlank();
            if (!sc.branch(ld::lit_c<'='>))
            {
                return unexpected(std::format("expected '=' after argument '{}'", key));
            }
            skipBlank();
            nlohmann::json value;
            if (!parseValue(value))
            {
                return false;
            }
            config[key] = std::move(value);
        }
    };

    auto parseGroup = [&] {
        const char* const open = sc.position();
        Term              term;
        term.kind = Term::Kind::Group;
        term.loc  = locOf(open);
        sc.parse(ld::lit_c<'('>);
        while (true)
        {
            skipBlank();
            if (sc.branch(ld::lit_c<')'>))
            {
                terms.push_back(std::move(term));
                return true;
            }
            if (atEndOfLine())
            {
                return fail(open, "unterminated fan-in group; missing ')'");
            }
            if (sc.branch(ld::lit_c<','>))
            {
                continue;
            }
            if (!sc.peek(ld::ascii::alpha_underscore))
            {
                return unexpected("expected an edge name in group");
            }
            term.edges.push_back(captureIdent());
        }
    };

    auto parseNamed = [&] {
        Term term;
        term.loc  = locOf(sc.position());
        term.name = captureIdent();

        // Optional `.key` (only meaningful for `out`).
        skipBlank();
        if (sc.branch(ld::lit_c<'.'>))
        {
            skipBlank();
            if (!sc.peek(ld::ascii::alpha_underscore))
            {
                return unexpected("expected a key name after '.'");
            }
            term.key = captureIdent();
            skipBlank();
        }

        // A trailing `(...)` makes this a stage application with arguments.
        if (sc.peek(ld::lit_c<'('>))
        {
            term.kind = Term::Kind::Stage;
            if (!parseArgs(term.config))
            {
                return false;
            }
        }
        else if (isReserved(term.name))
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

    auto parseTerm = [&] {
        skipBlank();
        if (sc.peek(ld::lit_c<'('>))
        {
            return parseGroup();
        }
        if (sc.peek(ld::ascii::alpha_underscore))
        {
            return parseNamed();
        }
        return unexpected("expected an edge, stage, or group");
    };

    skipBlank();
    if (atEndOfLine())
    {
        return true; // blank or comment-only line
    }
    if (!parseTerm())
    {
        return false;
    }
    while (true)
    {
        skipBlank();
        if (atEndOfLine())
        {
            return true;
        }
        if (!sc.branch(LEXY_LIT("->")))
        {
            return unexpected("expected '->'");
        }
        if (!parseTerm())
        {
            return false;
        }
    }
}

} // namespace lexy_impl
} // namespace detail

// Parses and structurally validates a graph program from text using lexy,
// producing the same IR and diagnostics as parseGraphProgram.
inline GraphProgram parseGraphProgramLexy(std::string_view text)
{
    GraphProgram program;
    std::size_t  outputIndex = 0;
    std::size_t  lineNo      = 0;
    std::size_t  pos         = 0;

    while (pos <= text.size())
    {
        const std::size_t      newline = text.find('\n', pos);
        const std::string_view line =
            (newline == std::string_view::npos) ? text.substr(pos) : text.substr(pos, newline - pos);
        ++lineNo;

        std::vector<detail::Term> terms;
        if (detail::lexy_impl::parseStatementLine(line, lineNo, terms, program.diagnostics) && !terms.empty())
        {
            detail::buildStatement(terms, program, outputIndex, program.diagnostics);
        }

        if (newline == std::string_view::npos)
        {
            break;
        }
        pos = newline + 1;
    }

    detail::validateProgram(program);
    detail::sortDiagnostics(program.diagnostics);
    return program;
}

} // namespace filterGraph::dsl
