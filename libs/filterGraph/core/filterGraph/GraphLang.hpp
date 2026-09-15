#pragma once

#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphValidator.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Text DSL (Model B) for describing a filter graph as a named-edge DAG.
//
// A program is a set of newline-separated statements; each statement is an
// alternating chain of edges and stages joined by "->":
//
//   in            -> Parse            -> msg
//   msg           -> Format           -> out.text
//   msg           -> Summary          -> out.stats
//   (a, b)        -> Merge            -> c
//   msg           -> Store            -> end
//
// - even positions are edges (identifiers) or the reserved boundary nodes
//   `in` (graph input), `out` / `out.<key>` (results), `end` (dead-end/Void);
// - odd positions are stage applications: a REGISTERED filter/merge name,
//   optionally with config args `Name(k=v, k2="s", k3=true)`;
// - fan-out = reuse an edge name as a source in several statements;
// - fan-in = a source group `(a, b) -> Merge -> c`.
//
// This front-end only parses/validates/visualizes; executing a Model-B graph
// needs runtime edge-routing that is intentionally out of scope here.
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
    std::optional<std::string> output;  // produced edge name; nullopt => routed to out/end
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

namespace detail {

inline bool isReserved(std::string_view name)
{
    return name == "in" || name == "out" || name == "end";
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
    Newline,
    EndOfInput
};

struct Token
{
    TokenKind   kind;
    std::string text;
    SourceLoc   loc;
};

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
            const char c = mSource[mPos];

            if (c == '\n')
            {
                tokens.push_back(make(TokenKind::Newline, "\n"));
                advance();
                continue;
            }
            if (c == '\r' || c == ' ' || c == '\t')
            {
                advance();
                continue;
            }
            if (c == '#')
            {
                while (mPos < mSource.size() && mSource[mPos] != '\n')
                {
                    advance();
                }
                continue;
            }
            if (c == '-' && mPos + 1 < mSource.size() && mSource[mPos + 1] == '>')
            {
                tokens.push_back(make(TokenKind::Arrow, "->"));
                advance();
                advance();
                continue;
            }
            if (c == '(')
            {
                tokens.push_back(make(TokenKind::LParen, "("));
                advance();
                continue;
            }
            if (c == ')')
            {
                tokens.push_back(make(TokenKind::RParen, ")"));
                advance();
                continue;
            }
            if (c == ',')
            {
                tokens.push_back(make(TokenKind::Comma, ","));
                advance();
                continue;
            }
            if (c == '=')
            {
                tokens.push_back(make(TokenKind::Equals, "="));
                advance();
                continue;
            }
            if (c == '.')
            {
                tokens.push_back(make(TokenKind::Dot, "."));
                advance();
                continue;
            }
            if (c == '"')
            {
                tokens.push_back(lexString());
                continue;
            }
            if (isDigit(c) || (c == '-' && mPos + 1 < mSource.size() && isDigit(mSource[mPos + 1])))
            {
                tokens.push_back(lexNumber());
                continue;
            }
            if (isIdentStart(c))
            {
                tokens.push_back(lexIdentifier());
                continue;
            }

            // Unknown character: record and skip so lexing can continue.
            mErrors.push_back({currentLoc(), std::format("unexpected character '{}'", c)});
            advance();
        }
        tokens.push_back(make(TokenKind::EndOfInput, ""));
        return tokens;
    }

    const std::vector<TextDiagnostic>& errors() const
    {
        return mErrors;
    }

private:
    Token make(TokenKind kind, std::string text) const
    {
        return Token{kind, std::move(text), currentLoc()};
    }

    SourceLoc currentLoc() const
    {
        return SourceLoc{mLine, mColumn};
    }

    void advance()
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

    Token lexString()
    {
        const SourceLoc start = currentLoc();
        advance(); // opening quote
        std::string value;
        while (mPos < mSource.size() && mSource[mPos] != '"')
        {
            if (mSource[mPos] == '\\' && mPos + 1 < mSource.size())
            {
                advance();
            }
            value += mSource[mPos];
            advance();
        }
        if (mPos < mSource.size())
        {
            advance(); // closing quote
        }
        else
        {
            mErrors.push_back({start, "unterminated string literal"});
        }
        return Token{TokenKind::String, std::move(value), start};
    }

    Token lexNumber()
    {
        const SourceLoc start = currentLoc();
        std::string     value;
        if (mSource[mPos] == '-')
        {
            value += '-';
            advance();
        }
        while (mPos < mSource.size() && (isDigit(mSource[mPos]) || mSource[mPos] == '.'))
        {
            value += mSource[mPos];
            advance();
        }
        return Token{TokenKind::Number, std::move(value), start};
    }

    Token lexIdentifier()
    {
        const SourceLoc start = currentLoc();
        std::string     value;
        while (mPos < mSource.size() && isIdentPart(mSource[mPos]))
        {
            value += mSource[mPos];
            advance();
        }
        return Token{TokenKind::Identifier, std::move(value), start};
    }

    std::string_view            mSource;
    std::size_t                 mPos    = 0;
    std::size_t                 mLine   = 1;
    std::size_t                 mColumn = 1;
    std::vector<TextDiagnostic> mErrors;
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
        }
    }

    // Emit one stage node per stage term, wiring input(s) from the previous
    // edge term and the produced edge from the next edge term.
    for (std::size_t i = 1; i < terms.size(); i += 2)
    {
        StageNode node;
        node.type   = terms[i].name;
        node.config = terms[i].config;
        node.loc    = terms[i].loc;

        const Term& source = terms[i - 1];
        if (source.kind == Term::Kind::Group)
        {
            node.inputs = source.edges;
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

class Parser
{
public:
    Parser(std::vector<Token> tokens, std::vector<TextDiagnostic> lexErrors)
        : mTokens(std::move(tokens))
        , mDiagnostics(std::move(lexErrors))
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

            auto terms = parseStatement();
            if (!terms.empty())
            {
                buildStatement(terms, program, outputIndex, mDiagnostics);
            }
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

    void error(const SourceLoc& loc, std::string message)
    {
        mDiagnostics.push_back({loc, std::move(message)});
    }

    // Parses a single line into a flat list of terms (until newline / EOF).
    std::vector<Term> parseStatement()
    {
        std::vector<Term> terms;
        bool              expectArrow = false;

        while (peek().kind != TokenKind::Newline && peek().kind != TokenKind::EndOfInput)
        {
            if (expectArrow)
            {
                if (peek().kind != TokenKind::Arrow)
                {
                    error(peek().loc, std::format("expected '->' but found '{}'", peek().text));
                    // Skip to end of line to recover.
                    while (peek().kind != TokenKind::Newline && peek().kind != TokenKind::EndOfInput)
                    {
                        next();
                    }
                    break;
                }
                next(); // consume arrow
                expectArrow = false;
                continue;
            }

            if (auto term = parseTerm())
            {
                terms.push_back(std::move(*term));
                expectArrow = true;
            }
            else
            {
                break;
            }
        }
        return terms;
    }

    std::optional<Term> parseTerm()
    {
        const Token& token = peek();

        if (token.kind == TokenKind::LParen)
        {
            return parseGroup();
        }
        if (token.kind == TokenKind::Identifier)
        {
            return parseNamedTerm();
        }

        error(token.loc, std::format("expected an edge, stage, or group but found '{}'", token.text));
        next();
        return std::nullopt;
    }

    Term parseGroup()
    {
        Term term;
        term.kind = Term::Kind::Group;
        term.loc  = peek().loc;
        next(); // (

        while (peek().kind != TokenKind::RParen && peek().kind != TokenKind::Newline
               && peek().kind != TokenKind::EndOfInput)
        {
            if (peek().kind == TokenKind::Identifier)
            {
                term.edges.push_back(peek().text);
                next();
            }
            else if (peek().kind == TokenKind::Comma)
            {
                next();
            }
            else
            {
                error(peek().loc, std::format("expected an edge name in group but found '{}'", peek().text));
                next();
            }
        }
        if (peek().kind == TokenKind::RParen)
        {
            next();
        }
        else
        {
            error(term.loc, "unterminated fan-in group; missing ')'");
        }
        return term;
    }

    Term parseNamedTerm()
    {
        Term term;
        term.loc         = peek().loc;
        term.name        = peek().text;
        next();

        // Optional `.key` (currently only meaningful for `out`).
        if (peek().kind == TokenKind::Dot)
        {
            next();
            if (peek().kind == TokenKind::Identifier)
            {
                term.key = peek().text;
                next();
            }
            else
            {
                error(peek().loc, "expected a key name after '.'");
            }
        }

        // A trailing `(...)` makes this a stage application with config args.
        if (peek().kind == TokenKind::LParen)
        {
            term.kind   = Term::Kind::Stage;
            term.config = parseConfigArgs();
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

    nlohmann::json parseConfigArgs()
    {
        nlohmann::json config = nlohmann::json::object();
        next(); // (

        while (peek().kind != TokenKind::RParen && peek().kind != TokenKind::Newline
               && peek().kind != TokenKind::EndOfInput)
        {
            if (peek().kind == TokenKind::Comma)
            {
                next();
                continue;
            }
            if (peek().kind != TokenKind::Identifier)
            {
                error(peek().loc, std::format("expected a config key but found '{}'", peek().text));
                next();
                continue;
            }
            const std::string key = peek().text;
            next();
            if (peek().kind != TokenKind::Equals)
            {
                error(peek().loc, std::format("expected '=' after config key '{}'", key));
                continue;
            }
            next(); // =
            config[key] = parseConfigValue();
        }
        if (peek().kind == TokenKind::RParen)
        {
            next();
        }
        else
        {
            error(peek().loc, "unterminated config argument list; missing ')'");
        }
        return config;
    }

    nlohmann::json parseConfigValue()
    {
        const Token& token = peek();
        if (token.kind == TokenKind::String)
        {
            next();
            return token.text;
        }
        if (token.kind == TokenKind::Number)
        {
            next();
            if (token.text.find('.') != std::string::npos)
            {
                return std::stod(token.text);
            }
            return static_cast<std::int64_t>(std::stoll(token.text));
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
        error(token.loc, std::format("expected a config value but found '{}'", token.text));
        next();
        return nullptr;
    }

    std::vector<Token>          mTokens;
    std::size_t                 mPos = 0;
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

// Parses and structurally validates a Model-B graph program from text. All
// problems (lexer, parser, and semantic) are collected into
// GraphProgram::diagnostics, each located by line/column.
inline GraphProgram parseGraphProgram(std::string_view text)
{
    detail::Lexer lexer(text);
    auto          tokens = lexer.tokenize();

    detail::Parser parser(std::move(tokens), lexer.errors());
    GraphProgram   program = parser.parse();

    detail::validateProgram(program);
    return program;
}

// Renders a parsed program as a Mermaid flowchart so the DAG can be visualized.
inline std::string toMermaid(const GraphProgram& program)
{
    std::string out = "flowchart LR\n";

    std::size_t nodeId = 0;
    for (const auto& stage : program.stages)
    {
        const std::string id = std::format("n{}", nodeId++);
        for (const auto& input : stage.inputs)
        {
            out += std::format("    {}([{}]) --> {}[{}]\n", input, input, id, stage.type);
        }
        if (stage.output)
        {
            out += std::format("    {}[{}] --> {}([{}])\n", id, stage.type, *stage.output, *stage.output);
        }
        else
        {
            out += std::format("    {}[{}] --> end([end])\n", id, stage.type);
        }
    }
    return out;
}

} // namespace filterGraph::dsl
