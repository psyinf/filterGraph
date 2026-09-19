#pragma once

#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphValidator.hpp>

#include <lexy/action/scan.hpp>
#include <lexy/dsl.hpp>
#include <lexy/error.hpp>
#include <lexy/input/string_input.hpp>

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
#include <utility>
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
//   `in` / `in.<key>` (the graph input, or one of several named inputs),
//   `out` / `out.<key>` (results), `end` (dead-end: the value is discarded);
// - odd positions are stage applications: a REGISTERED filter/merge name,
//   optionally with config args `Name(k=v, k2="s", k3=true)`;
// - fan-out = reuse an edge name as a source in several statements;
// - fan-in = a source group `(a, b) -> Merge -> c`, where Merge is a merge
//   stage (see registerMergeFilter in MergeFilter.hpp); the group may name the
//   merge's slots, `(raw: a, checked: b) -> Merge -> c`, which matches them by
//   name instead of by position;
// - `#` starts a comment that runs to the end of the line.
//
// This header parses (with a lexy-based parser) and structurally validates a
// program into a node/edge IR (GraphProgram) and renders it with toMermaid or
// toDot;
// DslFilterGraph instantiates, type-checks and runs it. The original
// hand-written parser is deprecated (GraphLangHandwritten.hpp).
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
    std::vector<std::string>   slotNames; // per input, from `(name: edge, ...)`; empty for a positional group
    SourceLoc                  loc;
};

// One graph input the program reads: `in`, or a named `in.<key>`, located at
// its first use.
struct InputBinding
{
    std::string                edge; // "in" or "in.<key>"
    std::optional<std::string> key;  // name from `in.<key>`
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
    std::vector<InputBinding>   inputs;   // distinct graph inputs, in order of first use
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

// The edge a graph input arrives on: `in`, or `in.<key>` for a named input.
inline bool isInputEdge(std::string_view edge)
{
    return edge == "in" || edge.starts_with("in.");
}

// ------------------------- shared lexical helpers ------------------------
//
// The parser below and the deprecated hand-written parser
// (GraphLangHandwritten.hpp) classify input and produce literal values and
// error descriptions through these helpers, so they report identical
// diagnostics.

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
    if (c == '(' || c == ')' || c == ',' || c == '=' || c == '.' || c == ':')
    {
        return {false, std::format("'{}'", c)};
    }
    return {true, unexpectedCharacter(c)};
}

// -------------------------------- terms ----------------------------------

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
    std::optional<std::string> key;     // for `in.<key>` / `out.<key>`
    nlohmann::json             config;  // for Stage
    std::vector<std::string>   edges;   // for Group
    std::vector<std::string>   slotNames; // for Group: per edge, from `name: edge`; "" when unnamed
    SourceLoc                  loc;
};

// Checks the slot names of a fan-in group: all slots are named or none, and no
// name is used twice.
inline bool checkSlotNames(const Term& group, std::vector<TextDiagnostic>& diags)
{
    const auto named = std::count_if(group.slotNames.begin(), group.slotNames.end(),
                                     [](const std::string& name) { return !name.empty(); });
    if (named == 0)
    {
        return true;
    }
    if (static_cast<std::size_t>(named) != group.slotNames.size())
    {
        diags.push_back({group.loc, "a fan-in group names either all of its slots or none, e.g. '(a: x, b: y)'"});
        return false;
    }
    std::unordered_set<std::string> seen;
    for (const auto& name : group.slotNames)
    {
        if (!seen.insert(name).second)
        {
            diags.push_back({group.loc, std::format("slot '{}' is named twice in the fan-in group", name)});
            return false;
        }
    }
    return true;
}

// Interprets a flat term list (edge, stage, edge, ...) into stage nodes, wiring
// each stage's inputs from the previous edge/group and its output edge from the
// next edge/boundary. Shared by every parser.
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
            if (terms[i].kind == Term::Kind::Group && !checkSlotNames(terms[i], diags))
            {
                return;
            }
        }

        // Only the graph's inputs and outputs are keyed.
        if (terms[i].key && terms[i].name != "in" && terms[i].name != "out")
        {
            diags.push_back({terms[i].loc,
                             std::format("only 'in' and 'out' take a '.<key>', not '{}.{}'", terms[i].name,
                                         *terms[i].key)});
            return;
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
            if (!source.slotNames.front().empty())
            {
                node.slotNames = source.slotNames;
            }
        }
        else if (source.name == "in")
        {
            const std::string edge = source.key ? "in." + *source.key : std::string{"in"};
            const bool        seen = std::any_of(program.inputs.begin(), program.inputs.end(),
                                                 [&](const InputBinding& input) { return input.edge == edge; });
            if (!seen)
            {
                program.inputs.push_back({edge, source.key, source.loc});
            }
            node.inputs = {edge};
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
            if (!isInputEdge(input) && !produced)
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

// ------------------------------- parser ----------------------------------
//
// The DSL is newline-significant, so parseGraphProgram runs a lexy scanner over
// each line. Syntax errors are raised through lexy's error callback, which
// records them as located TextDiagnostics; a syntax error ends its statement,
// so each line reports at most one.

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
            // `edge`, or `name: edge` for a named slot.
            std::string edge = captureIdent();
            std::string slot;
            skipBlank();
            if (sc.branch(ld::lit_c<':'>))
            {
                skipBlank();
                if (!sc.peek(ld::ascii::alpha_underscore))
                {
                    return unexpected(std::format("expected an edge name after '{}:'", edge));
                }
                slot = std::exchange(edge, captureIdent());
            }
            term.edges.push_back(std::move(edge));
            term.slotNames.push_back(std::move(slot));
        }
    };

    auto parseNamed = [&] {
        Term term;
        term.loc  = locOf(sc.position());
        term.name = captureIdent();

        // Optional `.key` (only meaningful for `in` and `out`).
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

// Parses and structurally validates a graph program from text. All problems
// (syntax and structure) are collected into GraphProgram::diagnostics, each
// located by line/column and sorted by location.
inline GraphProgram parseGraphProgram(std::string_view text)
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

namespace detail {

// The name each edge is shown under in a rendering: output edges (internally
// "$outN") become `out`, `out.<key>` or `out[<index>]`; every other edge keeps
// its own name.
inline std::unordered_map<std::string, std::string> edgeDisplayNames(const GraphProgram& program)
{
    std::unordered_map<std::string, std::string> names;
    for (const auto& output : program.outputs)
    {
        names[output.edge] = output.key                   ? "out." + *output.key
                           : program.outputs.size() > 1 ? std::format("out[{}]", output.index)
                                                        : std::string{"out"};
    }
    return names;
}

inline std::string displayName(const std::unordered_map<std::string, std::string>& names, const std::string& edge)
{
    auto it = names.find(edge);
    return it != names.end() ? it->second : edge;
}

// A stage's config as "key=value" lines, strings unquoted, for diagram labels.
inline std::vector<std::string> configLines(const StageNode& stage)
{
    std::vector<std::string> lines;
    if (stage.config.is_object())
    {
        for (const auto& item : stage.config.items())
        {
            const std::string value = item.value().is_string() ? item.value().get<std::string>() : item.value().dump();
            lines.push_back(item.key() + '=' + value);
        }
    }
    return lines;
}

// Escapes text for a quoted DOT string.
inline std::string dotLabel(std::string_view text)
{
    std::string label;
    for (char c : text)
    {
        if (c == '"' || c == '\\')
        {
            label += '\\';
            label += c;
        }
        else if (c == '\n')
        {
            label += "\\n";
        }
        else
        {
            label += c;
        }
    }
    return label;
}

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
    const auto displayNames = detail::edgeDisplayNames(program);

    std::string                                  nodes;
    std::string                                  links;
    std::unordered_map<std::string, std::string> edgeIds;

    auto edgeId = [&](const std::string& edge) -> std::string {
        if (auto it = edgeIds.find(edge); it != edgeIds.end())
        {
            return it->second;
        }
        std::string id = std::format("e{}", edgeIds.size());
        nodes += std::format("    {}([\"{}\"])\n", id, detail::mermaidLabel(detail::displayName(displayNames, edge)));
        edgeIds.emplace(edge, id);
        return id;
    };

    std::size_t deadEnds = 0;
    for (std::size_t i = 0; i < program.stages.size(); ++i)
    {
        const StageNode& stage = program.stages[i];

        std::string label = detail::mermaidLabel(stage.type);
        for (const auto& line : detail::configLines(stage))
        {
            label += "<br/>" + detail::mermaidLabel(line);
        }
        nodes += std::format("    s{}[\"{}\"]\n", i, label);

        for (std::size_t slot = 0; slot < stage.inputs.size(); ++slot)
        {
            const std::string& input = stage.inputs[slot];
            if (stage.slotNames.empty())
            {
                links += std::format("    {} --> s{}\n", edgeId(input), i);
            }
            else
            {
                links += std::format("    {} -->|\"{}\"| s{}\n", edgeId(input),
                                     detail::mermaidLabel(stage.slotNames[slot]), i);
            }
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

// Renders a parsed program as a Graphviz DOT digraph: the same picture as
// toMermaid, with the same node ids. Stages are boxes (listing their config),
// edges are ellipses, every `-> end` gets its own double-bordered box, and the
// links of a named group carry the slot names. Render it with `dot -Tsvg`, or
// in a console with `graph-easy --as=boxart`.
inline std::string toDot(const GraphProgram& program)
{
    const auto displayNames = detail::edgeDisplayNames(program);

    std::string                                  nodes;
    std::string                                  links;
    std::unordered_map<std::string, std::string> edgeIds;

    auto edgeId = [&](const std::string& edge) -> std::string {
        if (auto it = edgeIds.find(edge); it != edgeIds.end())
        {
            return it->second;
        }
        std::string id = std::format("e{}", edgeIds.size());
        nodes += std::format("    {} [shape=ellipse, label=\"{}\"];\n", id,
                             detail::dotLabel(detail::displayName(displayNames, edge)));
        edgeIds.emplace(edge, id);
        return id;
    };

    std::size_t deadEnds = 0;
    for (std::size_t i = 0; i < program.stages.size(); ++i)
    {
        const StageNode& stage = program.stages[i];

        std::string label = detail::dotLabel(stage.type);
        for (const auto& line : detail::configLines(stage))
        {
            label += "\\n" + detail::dotLabel(line);
        }
        nodes += std::format("    s{} [shape=box, label=\"{}\"];\n", i, label);

        for (std::size_t slot = 0; slot < stage.inputs.size(); ++slot)
        {
            const std::string& input = stage.inputs[slot];
            if (stage.slotNames.empty())
            {
                links += std::format("    {} -> s{};\n", edgeId(input), i);
            }
            else
            {
                links += std::format("    {} -> s{} [label=\"{}\"];\n", edgeId(input), i,
                                     detail::dotLabel(stage.slotNames[slot]));
            }
        }
        if (stage.output)
        {
            links += std::format("    s{} -> {};\n", i, edgeId(*stage.output));
        }
        else
        {
            nodes += std::format("    d{} [shape=box, peripheries=2, label=\"end\"];\n", deadEnds);
            links += std::format("    s{} -> d{};\n", i, deadEnds);
            ++deadEnds;
        }
    }
    return "digraph filterGraph {\n    rankdir=LR;\n" + nodes + links + "}\n";
}

} // namespace filterGraph::dsl
