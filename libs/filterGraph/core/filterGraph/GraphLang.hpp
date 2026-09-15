#pragma once

#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphValidator.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Text DSL for describing a filter graph as a named-edge DAG. This is the
// preferred way to describe a runtime graph; DslFilterGraph (DslFilterGraph.hpp)
// builds and runs one.
//
// A program is a set of newline-separated statements; each statement is an
// alternating chain of edges and stages joined by "->":
//
//   in            -> Parse            -> msg
//   msg           -> Format           -> out.text
//   msg           -> Summary          -> sum
//   (msg, sum)    -> Merge            -> out.stats
//   msg           -> Store            -> end
//
// - even positions are edges (identifiers) or the reserved boundary nodes
//   `in` (graph input), `out` / `out.<key>` (results), `end` (dead-end: the
//   value is discarded);
// - odd positions are stage applications: a REGISTERED filter/merge name,
//   optionally with config args `Name(k=v, k2="s", k3=true)`;
// - fan-out = reuse an edge name as a source in several statements;
// - fan-in = a source group `(a, b) -> Merge -> c`, where Merge is a merge
//   stage (see registerMergeFilter in MergeFilter.hpp);
// - `#` starts a comment that runs to the end of the line.
//
// This header parses and structurally validates a program into a node/edge IR
// (GraphProgram) and renders it with toMermaid; DslFilterGraph instantiates,
// type-checks and runs it. GraphLangLexy.hpp provides an alternative lexy-based
// parser with identical output, including diagnostics.
namespace filterGraph::dsl {

struct SourceLoc
{
    std::size_t line   = 1;
    std::size_t column = 1;
};

struct TextDiagnostic
{
    SourceLoc   loc;
    std::string message;
};

// A stage application: consumes one or more input edges (several = fan-in) and
// either produces a named edge, or terminates the path at `out`/`end`.
struct StageNode
{
    std::string                type;    // registered filter/merge name
    nlohmann::json             config;  // parsed config args
    std::vector<std::string>   inputs;  // source edge names ("in" allowed)
    std::optional<std::string> output;  // produced edge name ("$outN" for `out`); nullopt => routed to end
    bool                       fanIn = false; // inputs came from a group `(a, b) -> Stage`
    SourceLoc                  loc;
};

// One `-> out` (or `-> out.<key>`), in DSL order (index is positional).
struct OutputBinding
{
    std::string                edge;   // internal edge feeding this output
    std::optional<std::string> key;    // name from `out.<key>`
    std::size_t                index;  // positional slot
    SourceLoc                  loc;
};

struct GraphProgram
{
    std::vector<StageNode>      stages;
    std::vector<OutputBinding>  outputs;
    std::vector<std::string>    deadEnds; // edges routed to `end`
    std::vector<TextDiagnostic> diagnostics;

    bool ok() const
    {
        return diagnostics.empty();
    }
};

// Formats a diagnostic as "line:column: message".
inline std::string formatDiagnostic(const TextDiagnostic& diagnostic)
{
    return std::format("{}:{}: {}", diagnostic.loc.line, diagnostic.loc.column, diagnostic.message);
}

// Formats diagnostics one per line.
inline std::string formatDiagnostics(const std::vector<TextDiagnostic>& diagnostics)
{
    std::string text;
    for (const auto& diagnostic : diagnostics)
    {
        if (!text.empty())
        {
            text += '\n';
        }
        text += formatDiagnostic(diagnostic);
    }
    return text;
}

namespace detail {

inline void sortDiagnostics(std::vector<TextDiagnostic>& diagnostics)
{
    std::stable_sort(diagnostics.begin(), diagnostics.end(), [](const TextDiagnostic& a, const TextDiagnostic& b) {
        return a.loc.line != b.loc.line ? a.loc.line < b.loc.line : a.loc.column < b.loc.column;
    });
}

inline bool isReserved(std::string_view name)
{
    return name == "in" || name == "out" || name == "end";
}

// ------------------------- shared lexical helpers ------------------------
//
// Both front-ends (the hand-written parser below and the lexy parser in
// GraphLangLexy.hpp) classify input and produce literal values and error
// descriptions through these helpers, so they report identical diagnostics.

inline bool isIdentStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool isIdentPart(char c)
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

inline bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

struct StringLiteral
{
    std::string value;  // with escapes resolved
    std::size_t length; // in the source, including both quotes
};

// `rest` starts at an opening quote. A backslash takes the next character
// literally. Returns std::nullopt if the line ends before the closing quote.
inline std::optional<StringLiteral> scanStringLiteral(std::string_view rest)
{
    std::string value;
    std::size_t i = 1;
    while (i < rest.size() && rest[i] != '"' && rest[i] != '\n')
    {
        if (rest[i] == '\\' && i + 1 < rest.size() && rest[i + 1] != '\n')
        {
            ++i;
        }
        value += rest[i];
        ++i;
    }
    if (i >= rest.size() || rest[i] != '"')
    {
        return std::nullopt;
    }
    return StringLiteral{std::move(value), i + 1};
}

// Length of the number token at the start of `rest` (an optional '-', a digit,
// then digits and dots), or 0 if `rest` does not start with a number.
inline std::size_t numberLength(std::string_view rest)
{
    std::size_t i = 0;
    if (!rest.empty() && rest[0] == '-')
    {
        i = 1;
    }
    if (i >= rest.size() || !isDigit(rest[i]))
    {
        return 0;
    }
    while (i < rest.size() && (isDigit(rest[i]) || rest[i] == '.'))
    {
        ++i;
    }
    return i;
}

// Converts a number token to JSON: an integer, or a double if it has a
// fractional part. On failure returns std::nullopt and sets `error`.
inline std::optional<nlohmann::json> numberValue(std::string_view text, std::string& error)
{
    const std::string number(text);
    const std::size_t dot = number.find('.');
    if (dot != std::string::npos && (dot + 1 == number.size() || number.find('.', dot + 1) != std::string::npos))
    {
        error = std::format("malformed number '{}'", number);
        return std::optional<nlohmann::json>{};
    }
    try
    {
        if (dot != std::string::npos)
        {
            return nlohmann::json(std::stod(number));
        }
        return nlohmann::json(static_cast<std::int64_t>(std::stoll(number)));
    }
    catch (const std::out_of_range&)
    {
        error = std::format("number '{}' is out of range", number);
        return std::optional<nlohmann::json>{};
    }
}

inline std::string unexpectedCharacter(char c)
{
    const auto code = static_cast<unsigned char>(c);
    if (code >= 0x20 && code < 0x7F)
    {
        return std::format("unexpected character '{}'", c);
    }
    return std::format("unexpected byte 0x{:02X}", static_cast<unsigned>(code));
}

// Describes the input at the start of `rest` for an "expected ... but found
// ..." message. If that input is not a valid token at all, `invalid` is set and
// `text` is the complete error message instead.
struct Found
{
    bool        invalid = false;
    std::string text;
};

inline Found describeFound(std::string_view rest)
{
    rest = rest.substr(0, rest.find('\n'));
    if (rest.empty() || rest.front() == '#')
    {
        return {false, "end of line"};
    }
    if (rest.starts_with("->"))
    {
        return {false, "'->'"};
    }

    const char c = rest.front();
    if (c == '"')
    {
        if (scanStringLiteral(rest))
        {
            return {false, "a string literal"};
        }
        return {true, "unterminated string literal"};
    }
    if (const std::size_t length = numberLength(rest))
    {
        return {false, std::format("'{}'", rest.substr(0, length))};
    }
    if (isIdentStart(c))
    {
        std::size_t length = 1;
        while (length < rest.size() && isIdentPart(rest[length]))
        {
            ++length;
        }
        return {false, std::format("'{}'", rest.substr(0, length))};
    }
    if (c == '(' || c == ')' || c == ',' || c == '=' || c == '.')
    {
        return {false, std::format("'{}'", c)};
    }
    return {true, unexpectedCharacter(c)};
}

// ------------------------------- tokenizer -------------------------------

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
            if (c == '(' || c == ')' || c == ',' || c == '=' || c == '.')
            {
                const TokenKind kind = c == '(' ? TokenKind::LParen
                                     : c == ')' ? TokenKind::RParen
                                     : c == ',' ? TokenKind::Comma
                                     : c == '=' ? TokenKind::Equals
                                                : TokenKind::Dot;
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

// -------------------------------- parser ---------------------------------

// A single term in a statement, already classified by the parser.
struct Term
{
    enum class Kind
    {
        Edge,   // a plain edge identifier, or `in`
        Boundary, // `out` / `out.<key>` / `end`
        Stage,  // a stage application (name + config)
        Group   // a fan-in group of edges
    };

    Kind                       kind = Kind::Edge;
    std::string                name;    // edge/stage/boundary name
    std::optional<std::string> key;     // for `out.<key>`
    nlohmann::json             config;  // for Stage
    std::vector<std::string>   edges;   // for Group
    SourceLoc                  loc;
};

// Interprets a flat term list (edge, stage, edge, ...) into stage nodes, wiring
// each stage's inputs from the previous edge/group and its output edge from the
// next edge/boundary. Shared by every front-end (hand-written or lexy).
inline void buildStatement(std::vector<Term>&           terms,
                           GraphProgram&                program,
                           std::size_t&                 outputIndex,
                           std::vector<TextDiagnostic>& diags)
{
    if (terms.size() < 3 || (terms.size() % 2) == 0)
    {
        diags.push_back({terms.front().loc,
                         "a statement must alternate edge -> stage -> edge (e.g. 'in -> Stage -> out')"});
        return;
    }

    // Reclassify odd positions as stages (unless already a stage app).
    for (std::size_t i = 0; i < terms.size(); ++i)
    {
        const bool oddPosition = (i % 2) == 1;
        if (oddPosition)
        {
            if (terms[i].kind == Term::Kind::Group || terms[i].kind == Term::Kind::Boundary)
            {
                diags.push_back({terms[i].loc,
                                 std::format("expected a stage at this position but found '{}'", terms[i].name)});
                return;
            }
            terms[i].kind = Term::Kind::Stage;
        }
        else
        {
            if (terms[i].kind == Term::Kind::Stage)
            {
                diags.push_back({terms[i].loc,
                                 std::format("unexpected stage '{}' where an edge was expected", terms[i].name)});
                return;
            }
            // `in` only valid at the very start; out/end only at the very end.
            const bool first = (i == 0);
            const bool last  = (i + 1 == terms.size());
            if (terms[i].name == "in" && !first)
            {
                diags.push_back({terms[i].loc, "'in' may only appear as the first term of a statement"});
                return;
            }
            if ((terms[i].kind == Term::Kind::Boundary) && !last)
            {
                diags.push_back({terms[i].loc,
                                 std::format("'{}' may only appear as the last term of a statement", terms[i].name)});
                return;
            }
            if (terms[i].kind == Term::Kind::Group && !first)
            {
                diags.push_back({terms[i].loc, "a fan-in group may only appear as the first term of a statement"});
                return;
            }
            if (terms[i].kind == Term::Kind::Group && terms[i].edges.empty())
            {
                diags.push_back({terms[i].loc, "a fan-in group needs at least one edge"});
                return;
            }
        }
    }

    // Emit one stage node per stage term, wiring input(s) from the previous
    // edge term and the produced edge from the next edge term.
    for (std::size_t i = 1; i < terms.size(); i += 2)
    {
        StageNode node;
        node.type = terms[i].name;
        // A stage without `(...)` gets an empty object, as a JSON stage without "config" does.
        node.config = terms[i].config.is_null() ? nlohmann::json::object() : terms[i].config;
        node.loc    = terms[i].loc;

        const Term& source = terms[i - 1];
        if (source.kind == Term::Kind::Group)
        {
            node.inputs = source.edges;
            node.fanIn  = true;
        }
        else
        {
            node.inputs = {source.name};
        }

        const Term& sink = terms[i + 1];
        if (sink.kind == Term::Kind::Boundary && sink.name == "end")
        {
            const std::string deadEdge = std::format("$end{}", program.deadEnds.size());
            program.deadEnds.push_back(deadEdge);
            node.output = std::nullopt;
        }
        else if (sink.kind == Term::Kind::Boundary && sink.name == "out")
        {
            const std::string outEdge = std::format("$out{}", outputIndex);
            program.outputs.push_back({outEdge, sink.key, outputIndex, sink.loc});
            node.output = outEdge;
            ++outputIndex;
        }
        else
        {
            node.output = sink.name; // intermediate named edge
        }

        program.stages.push_back(std::move(node));
    }
}

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
            term.edges.push_back(peek().text);
            next();
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

// ------------------------------ validation -------------------------------

inline void validateProgram(GraphProgram& program)
{
    auto& registry = FilterRegistry::instance();

    // Map each produced edge to the node(s) that produce it.
    std::unordered_map<std::string, std::size_t> producerCount;
    for (const auto& stage : program.stages)
    {
        if (stage.output)
        {
            ++producerCount[*stage.output];
        }
    }

    std::unordered_set<std::string> consumed;

    for (const auto& stage : program.stages)
    {
        // Unknown stage type (reuse the JSON validator's suggestion logic).
        if (!registry.contains(stage.type))
        {
            std::string message = std::format("unknown stage type '{}'", stage.type);
            if (auto suggestion = filterGraph::detail::closestName(stage.type, registry.registeredNames()))
            {
                message += std::format(" — did you mean '{}'?", *suggestion);
            }
            program.diagnostics.push_back({stage.loc, message});
        }

        for (const auto& input : stage.inputs)
        {
            consumed.insert(input);
            const bool produced = producerCount.find(input) != producerCount.end();
            if (input != "in" && !produced)
            {
                program.diagnostics.push_back(
                    {stage.loc, std::format("edge '{}' is used but never produced", input)});
            }
        }
    }

    // Edges with more than one producer.
    for (const auto& [edge, count] : producerCount)
    {
        if (count > 1)
        {
            program.diagnostics.push_back(
                {SourceLoc{}, std::format("edge '{}' is produced by {} stages (must be exactly one)", edge, count)});
        }
    }

    // Produced edges that nothing consumes and that aren't routed to out/end.
    for (const auto& stage : program.stages)
    {
        if (!stage.output)
        {
            continue; // routed to end
        }
        const bool isOutput = std::any_of(program.outputs.begin(), program.outputs.end(),
                                          [&](const OutputBinding& o) { return o.edge == *stage.output; });
        if (!isOutput && consumed.find(*stage.output) == consumed.end())
        {
            program.diagnostics.push_back(
                {stage.loc, std::format("edge '{}' is produced but never consumed (route it to a stage, out, or end)",
                                        *stage.output)});
        }
    }

    // Duplicate output keys.
    std::unordered_set<std::string> seenKeys;
    for (const auto& output : program.outputs)
    {
        if (output.key && !seenKeys.insert(*output.key).second)
        {
            program.diagnostics.push_back(
                {output.loc, std::format("duplicate output key '{}'", *output.key)});
        }
    }

    // Cycle detection over the edge dependency graph (DFS).
    std::unordered_map<std::string, std::vector<std::string>> dependsOn; // output edge -> input edges
    for (const auto& stage : program.stages)
    {
        if (stage.output)
        {
            auto& deps = dependsOn[*stage.output];
            deps.insert(deps.end(), stage.inputs.begin(), stage.inputs.end());
        }
    }

    enum class Mark
    {
        None,
        InProgress,
        Done
    };
    std::unordered_map<std::string, Mark> marks;
    bool                                  cycleReported = false;

    auto visit = [&](const std::string& edge, auto&& self) -> void {
        if (cycleReported)
        {
            return;
        }
        Mark& mark = marks[edge];
        if (mark == Mark::Done)
        {
            return;
        }
        if (mark == Mark::InProgress)
        {
            program.diagnostics.push_back({SourceLoc{}, std::format("cycle detected involving edge '{}'", edge)});
            cycleReported = true;
            return;
        }
        mark = Mark::InProgress;
        auto it = dependsOn.find(edge);
        if (it != dependsOn.end())
        {
            for (const auto& dep : it->second)
            {
                self(dep, self);
            }
        }
        mark = Mark::Done;
    };

    for (const auto& [edge, deps] : dependsOn)
    {
        visit(edge, visit);
    }
}

} // namespace detail

// Parses and structurally validates a graph program from text. All problems
// (syntax and structure) are collected into GraphProgram::diagnostics, each
// located by line/column and sorted by location.
inline GraphProgram parseGraphProgram(std::string_view text)
{
    detail::Parser parser(text, detail::Lexer(text).tokenize());
    GraphProgram   program = parser.parse();

    detail::validateProgram(program);
    detail::sortDiagnostics(program.diagnostics);
    return program;
}

namespace detail {

// Escapes the characters that would end or confuse a quoted Mermaid label.
inline std::string mermaidLabel(std::string_view text)
{
    std::string label;
    for (char c : text)
    {
        if (c == '"')
        {
            label += "#quot;";
        }
        else if (c == '<')
        {
            label += "#lt;";
        }
        else if (c == '>')
        {
            label += "#gt;";
        }
        else
        {
            label += c;
        }
    }
    return label;
}

} // namespace detail

// Renders a parsed program as a Mermaid flowchart: stages are boxes (listing
// their config), edges are rounded nodes, and every `-> end` gets its own
// terminal node. Node ids are generated, so edge names never clash with Mermaid
// keywords such as `end`.
inline std::string toMermaid(const GraphProgram& program)
{
    std::unordered_map<std::string, std::string> outputLabels;
    for (const auto& output : program.outputs)
    {
        outputLabels[output.edge] = output.key                   ? "out." + *output.key
                                  : program.outputs.size() > 1 ? std::format("out[{}]", output.index)
                                                               : std::string{"out"};
    }

    std::string                                  nodes;
    std::string                                  links;
    std::unordered_map<std::string, std::string> edgeIds;

    auto edgeId = [&](const std::string& edge) -> std::string {
        if (auto it = edgeIds.find(edge); it != edgeIds.end())
        {
            return it->second;
        }
        std::string id    = std::format("e{}", edgeIds.size());
        auto        label = outputLabels.find(edge);
        nodes += std::format("    {}([\"{}\"])\n", id,
                             detail::mermaidLabel(label != outputLabels.end() ? label->second : edge));
        edgeIds.emplace(edge, id);
        return id;
    };

    std::size_t deadEnds = 0;
    for (std::size_t i = 0; i < program.stages.size(); ++i)
    {
        const StageNode& stage = program.stages[i];

        std::string label = detail::mermaidLabel(stage.type);
        if (stage.config.is_object())
        {
            for (const auto& item : stage.config.items())
            {
                const std::string value = item.value().is_string() ? item.value().get<std::string>() : item.value().dump();
                label += std::format("<br/>{}={}", detail::mermaidLabel(item.key()), detail::mermaidLabel(value));
            }
        }
        nodes += std::format("    s{}[\"{}\"]\n", i, label);

        for (const auto& input : stage.inputs)
        {
            links += std::format("    {} --> s{}\n", edgeId(input), i);
        }
        if (stage.output)
        {
            links += std::format("    s{} --> {}\n", i, edgeId(*stage.output));
        }
        else
        {
            nodes += std::format("    d{}[[\"end\"]]\n", deadEnds);
            links += std::format("    s{} --> d{}\n", i, deadEnds);
            ++deadEnds;
        }
    }
    return "flowchart LR\n" + nodes + links;
}

} // namespace filterGraph::dsl
