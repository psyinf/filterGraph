#pragma once

#include <filterGraph/core/filterGraph/GraphLang.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// DEPRECATED: the original hand-written tokenizer and recursive-descent parser
// for the graph DSL. dsl::parseGraphProgram (GraphLang.hpp) now uses the
// lexy-based parser, which produces the same IR and diagnostics; the tests
// check the two against each other. This parser will be removed in a future
// release.
namespace filterGraph::dsl {

namespace detail::handwritten {

enum class TokenKind
{
    Identifier,
    Number,
    String,
    Arrow,     // ->
    LParen,    // (
    RParen,    // )
    Comma,     // ,
    Equals,    // =
    Dot,       // .
    Colon,     // :
    Invalid,   // an unexpected character or an unterminated string
    Newline,
    EndOfInput
};

struct Token
{
    TokenKind   kind;
    std::string text;       // source text; the unescaped value for strings
    SourceLoc   loc;
    std::size_t offset = 0; // position in the source
};

class Lexer
{
public:
    explicit Lexer(std::string_view source)
        : mSource(source)
    {
    }

    std::vector<Token> tokenize()
    {
        std::vector<Token> tokens;
        while (mPos < mSource.size())
        {
            const char             c    = mSource[mPos];
            const std::string_view rest = mSource.substr(mPos);

            if (c == '\n')
            {
                tokens.push_back(endOfLine(TokenKind::Newline));
                advance(1);
                continue;
            }
            if (c == '\r' || c == ' ' || c == '\t')
            {
                advance(1);
                continue;
            }
            if (c == '#')
            {
                mCommentStart = Mark{currentLoc(), mPos};
                advance(std::min(rest.find('\n'), rest.size()));
                continue;
            }
            if (rest.starts_with("->"))
            {
                tokens.push_back(take(TokenKind::Arrow, 2));
                continue;
            }
            if (c == '(' || c == ')' || c == ',' || c == '=' || c == '.' || c == ':')
            {
                const TokenKind kind = c == '(' ? TokenKind::LParen
                                     : c == ')' ? TokenKind::RParen
                                     : c == ',' ? TokenKind::Comma
                                     : c == '=' ? TokenKind::Equals
                                     : c == '.' ? TokenKind::Dot
                                                : TokenKind::Colon;
                tokens.push_back(take(kind, 1));
                continue;
            }
            if (c == '"')
            {
                if (auto literal = scanStringLiteral(rest))
                {
                    Token token = take(TokenKind::String, literal->length);
                    token.text  = std::move(literal->value);
                    tokens.push_back(std::move(token));
                }
                else
                {
                    tokens.push_back(take(TokenKind::Invalid, std::min(rest.find('\n'), rest.size())));
                }
                continue;
            }
            if (const std::size_t length = numberLength(rest))
            {
                tokens.push_back(take(TokenKind::Number, length));
                continue;
            }
            if (isIdentStart(c))
            {
                std::size_t length = 1;
                while (length < rest.size() && isIdentPart(rest[length]))
                {
                    ++length;
                }
                tokens.push_back(take(TokenKind::Identifier, length));
                continue;
            }
            tokens.push_back(take(TokenKind::Invalid, 1));
        }
        tokens.push_back(endOfLine(TokenKind::EndOfInput));
        return tokens;
    }

private:
    struct Mark
    {
        SourceLoc   loc;
        std::size_t offset;
    };

    // A token of `length` characters starting at the current position.
    Token take(TokenKind kind, std::size_t length)
    {
        Token token{kind, std::string(mSource.substr(mPos, length)), currentLoc(), mPos};
        advance(length);
        return token;
    }

    // Newline / end of input, located where the line's content ends: at its
    // comment, if it has one.
    Token endOfLine(TokenKind kind)
    {
        const Mark mark = mCommentStart.value_or(Mark{currentLoc(), mPos});
        mCommentStart.reset();
        return Token{kind, {}, mark.loc, mark.offset};
    }

    SourceLoc currentLoc() const
    {
        return SourceLoc{mLine, mColumn};
    }

    void advance(std::size_t count)
    {
        for (; count > 0 && mPos < mSource.size(); --count)
        {
            if (mSource[mPos] == '\n')
            {
                ++mLine;
                mColumn = 1;
            }
            else
            {
                ++mColumn;
            }
            ++mPos;
        }
    }

    std::string_view    mSource;
    std::size_t         mPos    = 0;
    std::size_t         mLine   = 1;
    std::size_t         mColumn = 1;
    std::optional<Mark> mCommentStart;
};

// Recursive-descent parser over the token stream. A syntax error ends its
// statement: the rest of the line is skipped and the statement is not built,
// so each line reports at most one syntax error.
class Parser
{
public:
    Parser(std::string_view source, std::vector<Token> tokens)
        : mSource(source)
        , mTokens(std::move(tokens))
    {
    }

    GraphProgram parse()
    {
        GraphProgram program;
        std::size_t  outputIndex = 0;

        while (peek().kind != TokenKind::EndOfInput)
        {
            if (peek().kind == TokenKind::Newline)
            {
                next();
                continue;
            }

            mFailed    = false;
            auto terms = parseStatement();
            if (mFailed)
            {
                while (!atEndOfLine())
                {
                    next();
                }
                continue;
            }
            buildStatement(terms, program, outputIndex, mDiagnostics);
        }

        program.diagnostics = std::move(mDiagnostics);
        return program;
    }

private:
    const Token& peek() const
    {
        return mTokens[mPos];
    }

    const Token& next()
    {
        return mTokens[mPos < mTokens.size() - 1 ? mPos++ : mPos];
    }

    bool atEndOfLine() const
    {
        return peek().kind == TokenKind::Newline || peek().kind == TokenKind::EndOfInput;
    }

    void error(const SourceLoc& loc, std::string message)
    {
        mDiagnostics.push_back({loc, std::move(message)});
        mFailed = true;
    }

    // Reports what was found at the current token instead of `expected`, or
    // why the input there is invalid.
    void unexpected(std::string_view expected)
    {
        const Found found = describeFound(mSource.substr(peek().offset));
        error(peek().loc, found.invalid ? found.text : std::format("{} but found {}", expected, found.text));
    }

    // Parses a single line into a flat list of terms.
    std::vector<Term> parseStatement()
    {
        std::vector<Term> terms;
        while (true)
        {
            Term term = parseTerm();
            if (mFailed)
            {
                return terms;
            }
            terms.push_back(std::move(term));
            if (atEndOfLine())
            {
                return terms;
            }
            if (peek().kind != TokenKind::Arrow)
            {
                unexpected("expected '->'");
                return terms;
            }
            next();
        }
    }

    Term parseTerm()
    {
        if (peek().kind == TokenKind::LParen)
        {
            return parseGroup();
        }
        if (peek().kind == TokenKind::Identifier)
        {
            return parseNamedTerm();
        }
        unexpected("expected an edge, stage, or group");
        return {};
    }

    Term parseGroup()
    {
        Term term;
        term.kind = Term::Kind::Group;
        term.loc  = peek().loc;
        next(); // (

        while (true)
        {
            if (peek().kind == TokenKind::RParen)
            {
                next();
                return term;
            }
            if (atEndOfLine())
            {
                error(term.loc, "unterminated fan-in group; missing ')'");
                return term;
            }
            if (peek().kind == TokenKind::Comma)
            {
                next();
                continue;
            }
            if (peek().kind != TokenKind::Identifier)
            {
                unexpected("expected an edge name in group");
                return term;
            }
            // `edge`, or `name: edge` for a named slot.
            std::string edge = next().text;
            std::string slot;
            if (peek().kind == TokenKind::Colon)
            {
                next();
                if (peek().kind != TokenKind::Identifier)
                {
                    unexpected(std::format("expected an edge name after '{}:'", edge));
                    return term;
                }
                slot = std::exchange(edge, next().text);
            }
            term.edges.push_back(std::move(edge));
            term.slotNames.push_back(std::move(slot));
        }
    }

    Term parseNamedTerm()
    {
        Term term;
        term.loc  = peek().loc;
        term.name = peek().text;
        next();

        // Optional `.key` (currently only meaningful for `out`).
        if (peek().kind == TokenKind::Dot)
        {
            next();
            if (peek().kind != TokenKind::Identifier)
            {
                unexpected("expected a key name after '.'");
                return term;
            }
            term.key = peek().text;
            next();
        }

        // A trailing `(...)` makes this a stage application with arguments.
        if (peek().kind == TokenKind::LParen)
        {
            term.kind   = Term::Kind::Stage;
            term.config = parseArguments();
            return term;
        }

        if (isReserved(term.name))
        {
            term.kind = (term.name == "in") ? Term::Kind::Edge : Term::Kind::Boundary;
        }
        else
        {
            // Positional classification (edge vs stage) is resolved later; mark
            // as Edge here and let buildStatement reinterpret odd positions.
            term.kind = Term::Kind::Edge;
        }
        return term;
    }

    nlohmann::json parseArguments()
    {
        nlohmann::json  config = nlohmann::json::object();
        const SourceLoc open   = peek().loc;
        next(); // (

        while (true)
        {
            if (peek().kind == TokenKind::RParen)
            {
                next();
                return config;
            }
            if (atEndOfLine())
            {
                error(open, "unterminated argument list; missing ')'");
                return config;
            }
            if (peek().kind == TokenKind::Comma)
            {
                next();
                continue;
            }
            if (peek().kind != TokenKind::Identifier)
            {
                unexpected("expected an argument name");
                return config;
            }
            const std::string key = peek().text;
            next();
            if (peek().kind != TokenKind::Equals)
            {
                unexpected(std::format("expected '=' after argument '{}'", key));
                return config;
            }
            next(); // =
            nlohmann::json value = parseValue();
            if (mFailed)
            {
                return config;
            }
            config[key] = std::move(value);
        }
    }

    nlohmann::json parseValue()
    {
        const Token& token = peek();
        if (token.kind == TokenKind::String)
        {
            next();
            return token.text;
        }
        if (token.kind == TokenKind::Number)
        {
            std::string numberError;
            auto        number = numberValue(token.text, numberError);
            if (!number)
            {
                error(token.loc, std::move(numberError));
                return nullptr;
            }
            next();
            return std::move(*number);
        }
        if (token.kind == TokenKind::Identifier)
        {
            next();
            if (token.text == "true")
            {
                return true;
            }
            if (token.text == "false")
            {
                return false;
            }
            return token.text; // bareword treated as string
        }
        unexpected("expected an argument value");
        return nullptr;
    }

    std::string_view            mSource;
    std::vector<Token>          mTokens;
    std::size_t                 mPos    = 0;
    bool                        mFailed = false; // the current statement has a syntax error
    std::vector<TextDiagnostic> mDiagnostics;
};

} // namespace detail::handwritten

// Parses a graph program with the deprecated hand-written parser. Produces the
// same result as parseGraphProgram.
[[deprecated("the hand-written DSL parser is deprecated; use dsl::parseGraphProgram, which gives the same result")]]
inline GraphProgram parseGraphProgramHandwritten(std::string_view text)
{
    detail::handwritten::Parser parser(text, detail::handwritten::Lexer(text).tokenize());
    GraphProgram                program = parser.parse();

    detail::validateProgram(program);
    detail::sortDiagnostics(program.diagnostics);
    return program;
}

} // namespace filterGraph::dsl
