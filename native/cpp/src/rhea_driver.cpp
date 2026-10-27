#include "rhea/rhea_c_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace rhea {

static constexpr const char* RHEA_VERSION = "0.13.1";
static constexpr size_t MAX_RECURSION_DEPTH = 256;
static constexpr size_t MAX_PARSE_DEPTH = 256;
static constexpr size_t MAX_VM_STEPS = 50'000'000;
static constexpr int64_t MAX_RANGE_ELEMENTS = 10'000'000;
static constexpr int64_t MAX_SLEEP_MS = 3'600'000;
static constexpr size_t MAX_TYPE_DEPTH = 32;

// ============================================================================
// 1. Source Spans, Edit Distance & Beginner-Friendly Diagnostics
// ============================================================================

struct Span {
    size_t line = 1;
    size_t col = 1;
    size_t length = 1;
};

struct Diagnostic {
    std::string code;
    std::string severity;
    std::string message;
    std::string filename;
    Span span;
    std::string why;
    std::string hint;
    std::string example;

    std::string render(const std::string& source) const {
        std::ostringstream out;
        out << severity << "[" << code << "]: " << message << "\n";
        out << "  --> " << filename << ":" << span.line << ":" << span.col << "\n";

        std::istringstream iss(source);
        std::string line_text;
        size_t current_line = 0;
        while (std::getline(iss, line_text)) {
            ++current_line;
            if (current_line == span.line) {
                out << "   |\n";
                out << std::setw(3) << span.line << " | " << line_text << "\n";
                out << "   | ";
                for (size_t i = 1; i < span.col; ++i) out << ' ';
                size_t caret_len = std::max<size_t>(1, span.length);
                for (size_t i = 0; i < caret_len; ++i) out << '^';
                out << "\n";
                break;
            }
        }
        if (!why.empty()) out << "   = why: " << why << "\n";
        if (!hint.empty()) out << "   = help: " << hint << "\n";
        if (!example.empty()) out << "   = example: " << example << "\n";
        return out.str();
    }
};

struct RheaException : public std::runtime_error {
    Diagnostic diag;
    explicit RheaException(Diagnostic d)
        : std::runtime_error(d.message), diag(std::move(d)) {}
};

static size_t levenshtein_distance(const std::string& a, const std::string& b) {
    const size_t m = a.size();
    const size_t n = b.size();
    std::vector<std::vector<size_t>> dp(m + 1, std::vector<size_t>(n + 1, 0));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = i;
    for (size_t j = 0; j <= n; ++j) dp[0][j] = j;
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({
                dp[i - 1][j] + 1,
                dp[i][j - 1] + 1,
                dp[i - 1][j - 1] + cost
            });
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                dp[i][j] = std::min(dp[i][j], dp[i - 2][j - 2] + 1); // Damerau transposition
            }
        }
    }
    return dp[m][n];
}

static bool differs_only_by_digits(const std::string& a, const std::string& b) {
    auto strip_digits = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (!std::isdigit(static_cast<unsigned char>(c))) out.push_back(c);
        }
        return out;
    };
    return strip_digits(a) == strip_digits(b);
}

template <typename Iterable>
static std::string find_closest_match(const std::string& target, const Iterable& candidates, size_t max_dist = 2) {
    if (target.size() < 2) return "";
    std::string best;
    size_t best_d = max_dist + 1;
    for (const auto& c : candidates) {
        if (c.empty() || c.rfind("__", 0) == 0) continue;
        size_t d = levenshtein_distance(target, c);
        if (d > 0 && d <= max_dist && d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
}

static Diagnostic make_diag(
    std::string code,
    std::string message,
    std::string filename,
    Span span,
    std::string hint = "",
    std::string why = "",
    std::string example = ""
) {
    if (why.empty()) {
        if (code == "E0001") {
            why = "The source file contains a character, number format, or string literal that is not valid Rhea syntax.";
        } else if (code == "E0002") {
            why = "The parser encountered an unexpected token, missing delimiter, or duplicate declaration.";
        } else if (code == "E0003") {
            why = "Rhea is statically typed and checks that operators, generic parameters, and functions receive compatible types.";
        } else if (code == "E0004") {
            why = "This identifier or field has not been defined in the current scope or appears to be a misspelling.";
        } else if (code == "E0005") {
            why = "Variables created with 'name = value' or 'let name = value' are immutable by default to prevent accidental state changes.";
        } else if (code == "E0006") {
            why = "Each function or closure call must pass the exact number of arguments declared in its signature.";
        } else if (code == "E0007") {
            why = "Every function that declares a non-Unit return type must return a value of that type along all control-flow paths.";
        } else if (code == "E0008") {
            why = "'break' and 'continue' statements are only valid inside 'while', 'for', or 'parallel for' loops.";
        } else if (code == "E0009") {
            why = "Imported modules must exist, avoid circular dependencies, stay within the project root, and respect 'priv fn' visibility.";
        } else if (code == "E1001") {
            why = "Dividing or taking the remainder by zero is mathematically undefined.";
        } else if (code == "E1002") {
            why = "64-bit signed integers must stay within [-9,223,372,036,854,775,808 .. 9,223,372,036,854,775,807].";
        } else if (code == "E1003") {
            why = "List indices must be between 0 and len(list) - 1, and map keys must exist when read.";
        } else if (code == "E1004") {
            why = "Runtime execution limits protect the process against infinite recursion and unbounded memory allocation.";
        } else if (code == "E1005") {
            why = "A runtime assertion ('assert' or 'assert_eq') evaluated to false.";
        } else if (code == "E1006") {
            why = "The input string or file content could not be parsed or opened in the expected format.";
        } else if (code == "E1008") {
            why = "Sleep durations must be non-negative and at most 3,600,000 ms (1 hour).";
        } else if (code == "E2001") {
            why = "The bytecode verifier detected an invalid constant index, jump target, or function reference before VM execution.";
        } else if (code == "E2002") {
            why = "The selected execution backend does not support this language construct or dynamic runtime module.";
        } else if (code == "E2003") {
            why = "The native C++20 compiler failed while compiling or linking the generated native binary.";
        }
    }
    if (example.empty()) {
        if (code == "E0005") {
            example = "mut count = 0\n               count = count + 1";
        } else if (code == "E0004") {
            example = "name = \"Rhea\"\n               print(name)";
        } else if (code == "E0003") {
            example = "total = 10 + 5          // Int + Int\n               label = \"Score: \" + str(total)";
        } else if (code == "E0007") {
            example = "fn compute(x: Int) -> Int {\n                   if x > 0 { return x }\n                   return 0\n               }";
        } else if (code == "E1008") {
            example = "sleep(1s)     // or sleep(500ms)";
        }
    }
    return Diagnostic{
        std::move(code),
        "error",
        std::move(message),
        std::move(filename),
        span,
        std::move(why),
        std::move(hint),
        std::move(example)
    };
}

// ============================================================================
// 2. Lexical Analysis
// ============================================================================

enum class TokenKind {
    Eof,
    Int,
    Float,
    Str,
    Ident,
    // Keywords
    KwFn,
    KwLet,
    KwMut,
    KwConst,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwIn,
    KwReturn,
    KwTrue,
    KwFalse,
    KwStruct,
    KwEnum,
    KwImport,
    KwModule,
    KwPub,
    KwPriv,
    KwMatch,
    KwTry,
    KwCatch,
    KwBreak,
    KwContinue,
    KwParallel,
    KwPipeline,
    // Operators & Delimiters
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    Eq,
    EqEq,
    Bang,
    BangEq,
    Lt,
    LtEq,
    Gt,
    GtEq,
    AndAnd,
    OrOr,
    Pipe,
    PipeGreater,
    Arrow,
    FatArrow,
    DotDot,
    DotDotEq,
    Colon,
    ColonColon,
    Comma,
    Dot,
    Semi,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string lexeme;
    int64_t int_val = 0;
    double float_val = 0.0;
    bool is_duration = false;
    Span span;
};

static std::string token_kind_name(TokenKind k) {
    switch (k) {
        case TokenKind::Eof: return "EOF";
        case TokenKind::Int: return "Int";
        case TokenKind::Float: return "Float";
        case TokenKind::Str: return "String";
        case TokenKind::Ident: return "Identifier";
        case TokenKind::KwFn: return "fn";
        case TokenKind::KwLet: return "let";
        case TokenKind::KwMut: return "mut";
        case TokenKind::KwConst: return "const";
        case TokenKind::KwIf: return "if";
        case TokenKind::KwElse: return "else";
        case TokenKind::KwWhile: return "while";
        case TokenKind::KwFor: return "for";
        case TokenKind::KwIn: return "in";
        case TokenKind::KwReturn: return "return";
        case TokenKind::KwTrue: return "true";
        case TokenKind::KwFalse: return "false";
        case TokenKind::KwStruct: return "struct";
        case TokenKind::KwEnum: return "enum";
        case TokenKind::KwImport: return "import";
        case TokenKind::KwModule: return "module";
        case TokenKind::KwPub: return "pub";
        case TokenKind::KwPriv: return "priv";
        case TokenKind::KwMatch: return "match";
        case TokenKind::KwTry: return "try";
        case TokenKind::KwCatch: return "catch";
        case TokenKind::KwBreak: return "break";
        case TokenKind::KwContinue: return "continue";
        case TokenKind::KwParallel: return "parallel";
        case TokenKind::KwPipeline: return "pipeline";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::PlusEq: return "+=";
        case TokenKind::MinusEq: return "-=";
        case TokenKind::StarEq: return "*=";
        case TokenKind::SlashEq: return "/=";
        case TokenKind::Eq: return "=";
        case TokenKind::EqEq: return "==";
        case TokenKind::Bang: return "!";
        case TokenKind::BangEq: return "!=";
        case TokenKind::Lt: return "<";
        case TokenKind::LtEq: return "<=";
        case TokenKind::Gt: return ">";
        case TokenKind::GtEq: return ">=";
        case TokenKind::AndAnd: return "&&";
        case TokenKind::OrOr: return "||";
        case TokenKind::Pipe: return "|";
        case TokenKind::PipeGreater: return "|>";
        case TokenKind::Arrow: return "->";
        case TokenKind::FatArrow: return "=>";
        case TokenKind::DotDot: return "..";
        case TokenKind::DotDotEq: return "..=";
        case TokenKind::Colon: return ":";
        case TokenKind::ColonColon: return "::";
        case TokenKind::Comma: return ",";
        case TokenKind::Dot: return ".";
        case TokenKind::Semi: return ";";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::LBrace: return "{";
        case TokenKind::RBrace: return "}";
        case TokenKind::LBracket: return "[";
        case TokenKind::RBracket: return "]";
    }
    return "?";
}

class Lexer {
public:
    Lexer(std::string filename, std::string source)
        : filename_(std::move(filename)), src_(std::move(source)) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r') {
                advance();
                continue;
            }
            if (c == '\n') {
                advance_line();
                continue;
            }
            if (c == '/' && peek(1) == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                Span start_span{line_, col_, 2};
                advance(2);
                bool closed = false;
                while (pos_ < src_.size()) {
                    if (src_[pos_] == '*' && peek(1) == '/') {
                        advance(2);
                        closed = true;
                        break;
                    }
                    if (src_[pos_] == '\n') advance_line();
                    else advance();
                }
                if (!closed) {
                    throw RheaException(make_diag(
                        "E0001", "unterminated block comment", filename_, start_span,
                        "Close the block comment with '*/'."
                    ));
                }
                continue;
            }

            Span sp{line_, col_, 1};

            if (std::isdigit(static_cast<unsigned char>(c))) {
                std::string num_str;
                bool is_float = false;
                size_t start_col = col_;
                while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
                    if (src_[pos_] != '_') num_str.push_back(src_[pos_]);
                    advance();
                }
                if (pos_ < src_.size() && src_[pos_] == '.' && peek(1) != '.' &&
                    std::isdigit(static_cast<unsigned char>(peek(1)))) {
                    is_float = true;
                    num_str.push_back('.');
                    advance();
                    while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
                        if (src_[pos_] != '_') num_str.push_back(src_[pos_]);
                        advance();
                    }
                }
                if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
                    is_float = true;
                    num_str.push_back(src_[pos_]);
                    advance();
                    if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                        num_str.push_back(src_[pos_]);
                        advance();
                    }
                    if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                        throw RheaException(make_diag(
                            "E0001", "invalid scientific notation exponent", filename_,
                            Span{line_, start_col, col_ - start_col},
                            "Add digits after 'e' or 'E', such as 1.0e-5."
                        ));
                    }
                    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                        num_str.push_back(src_[pos_]);
                        advance();
                    }
                }

                auto is_ident_cont = [](char ch) {
                    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
                };
                if (pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_]))) {
                    if (src_[pos_] == 'm' && peek(1) == 's' && !is_ident_cont(peek(2))) {
                        advance(2);
                        Span dsp{line_, start_col, col_ - start_col};
                        double v = 0.0;
                        try {
                            v = std::stod(num_str);
                        } catch (...) {
                            throw RheaException(make_diag(
                                "E1002", "duration literal '" + num_str + "ms' overflows numeric range",
                                filename_, dsp, "Use a duration between 0ms and 3600000ms (1 hour)."
                            ));
                        }
                        if (!std::isfinite(v) || v < -9.0e15 || v > 9.0e15) {
                            throw RheaException(make_diag(
                                "E1002", "duration literal '" + num_str + "ms' overflows 64-bit integer milliseconds",
                                filename_, dsp, "Use a duration between 0ms and 3600000ms (1 hour)."
                            ));
                        }
                        int64_t ms = static_cast<int64_t>(std::llround(v));
                        Token tok{TokenKind::Int, num_str + "ms", ms, 0.0, true, dsp};
                        tokens.push_back(std::move(tok));
                        continue;
                    } else if (src_[pos_] == 's' && !is_ident_cont(peek(1))) {
                        advance(1);
                        Span dsp{line_, start_col, col_ - start_col};
                        double v = 0.0;
                        try {
                            v = std::stod(num_str);
                        } catch (...) {
                            throw RheaException(make_diag(
                                "E1002", "duration literal '" + num_str + "s' overflows numeric range",
                                filename_, dsp, "Use a duration between 0s and 3600s (1 hour)."
                            ));
                        }
                        double scaled = v * 1000.0;
                        if (!std::isfinite(scaled) || scaled < -9.0e15 || scaled > 9.0e15) {
                            throw RheaException(make_diag(
                                "E1002", "duration literal '" + num_str + "s' overflows 64-bit integer milliseconds",
                                filename_, dsp, "Use a duration between 0s and 3600s (1 hour)."
                            ));
                        }
                        int64_t ms = static_cast<int64_t>(std::llround(scaled));
                        Token tok{TokenKind::Int, num_str + "s", ms, 0.0, true, dsp};
                        tokens.push_back(std::move(tok));
                        continue;
                    } else {
                        std::string bad_suffix;
                        while (pos_ < src_.size() && is_ident_cont(src_[pos_])) {
                            bad_suffix.push_back(src_[pos_]);
                            advance();
                        }
                        throw RheaException(make_diag(
                            "E0001",
                            "invalid numeric suffix '" + bad_suffix + "' after number '" + num_str + "'",
                            filename_,
                            Span{line_, start_col, col_ - start_col},
                            "Use 's' (seconds) or 'ms' (milliseconds) for durations, or start variable names with a letter."
                        ));
                    }
                }

                Token tok;
                tok.lexeme = num_str;
                tok.span = Span{line_, start_col, col_ - start_col};
                if (is_float) {
                    tok.kind = TokenKind::Float;
                    try {
                        tok.float_val = std::stod(num_str);
                    } catch (...) {
                        throw RheaException(make_diag(
                            "E1002", "floating-point literal '" + num_str + "' overflows 64-bit IEEE-754 Float",
                            filename_, tok.span,
                            "Use a floating-point literal within [-1.797e308, 1.797e308]."
                        ));
                    }
                    if (!std::isfinite(tok.float_val)) {
                        throw RheaException(make_diag(
                            "E1002", "floating-point literal '" + num_str + "' overflows 64-bit IEEE-754 Float",
                            filename_, tok.span,
                            "Use a floating-point literal within [-1.797e308, 1.797e308]."
                        ));
                    }
                } else {
                    tok.kind = TokenKind::Int;
                    if (num_str == "9223372036854775808" && !tokens.empty() && tokens.back().kind == TokenKind::Minus) {
                        tokens.pop_back();
                        tok.lexeme = "-9223372036854775808";
                        tok.int_val = std::numeric_limits<int64_t>::min();
                        tokens.push_back(std::move(tok));
                        continue;
                    }
                    try {
                        size_t idx = 0;
                        long long v = std::stoll(num_str, &idx, 10);
                        tok.int_val = static_cast<int64_t>(v);
                    } catch (const std::out_of_range&) {
                        throw RheaException(make_diag(
                            "E1002", "integer literal '" + num_str + "' overflows 64-bit signed Int",
                            filename_, tok.span,
                            "Use a value within [-9223372036854775808, 9223372036854775807] or a Float literal."
                        ));
                    }
                }
                tokens.push_back(std::move(tok));
                continue;
            }

            if (c == '"') {
                size_t start_line = line_;
                size_t start_col = col_;
                advance();
                std::string s;
                bool closed = false;
                while (pos_ < src_.size()) {
                    char ch = src_[pos_];
                    if (ch == '"') {
                        advance();
                        closed = true;
                        break;
                    }
                    if (ch == '\n') break;
                    if (ch == '\\') {
                        advance();
                        if (pos_ >= src_.size()) break;
                        char esc = src_[pos_];
                        switch (esc) {
                            case 'n': s.push_back('\n'); break;
                            case 't': s.push_back('\t'); break;
                            case 'r': s.push_back('\r'); break;
                            case '\\': s.push_back('\\'); break;
                            case '"': s.push_back('"'); break;
                            case '{': s.push_back('{'); break;
                            case '}': s.push_back('}'); break;
                            default: s.push_back(esc); break;
                        }
                        advance();
                    } else {
                        s.push_back(ch);
                        advance();
                    }
                }
                if (!closed) {
                    throw RheaException(make_diag(
                        "E0001", "unterminated string literal", filename_,
                        Span{start_line, start_col, col_ - start_col},
                        "Add a closing double quote '\"' at the end of the string."
                    ));
                }
                Token tok{TokenKind::Str, s, 0, 0.0, false, Span{start_line, start_col, col_ - start_col}};
                tokens.push_back(std::move(tok));
                continue;
            }

            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t start_col = col_;
                std::string ident;
                while (pos_ < src_.size() &&
                       (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
                    ident.push_back(src_[pos_]);
                    advance();
                }
                Token tok{classify_keyword(ident), ident, 0, 0.0, false, Span{line_, start_col, col_ - start_col}};
                tokens.push_back(std::move(tok));
                continue;
            }

            if (c == '.' && peek(1) == '.' && peek(2) == '=') {
                tokens.push_back(Token{TokenKind::DotDotEq, "..=", 0, 0.0, false, Span{line_, col_, 3}});
                advance(3);
                continue;
            }

            char n = peek(1);
            if (c == '.' && n == '.') {
                tokens.push_back(Token{TokenKind::DotDot, "..", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '|' && n == '>') {
                tokens.push_back(Token{TokenKind::PipeGreater, "|>", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '-' && n == '>') {
                tokens.push_back(Token{TokenKind::Arrow, "->", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '=' && n == '>') {
                tokens.push_back(Token{TokenKind::FatArrow, "=>", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == ':' && n == ':') {
                tokens.push_back(Token{TokenKind::ColonColon, "::", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '=' && n == '=') {
                tokens.push_back(Token{TokenKind::EqEq, "==", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '!' && n == '=') {
                tokens.push_back(Token{TokenKind::BangEq, "!=", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '<' && n == '=') {
                tokens.push_back(Token{TokenKind::LtEq, "<=", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '>' && n == '=') {
                tokens.push_back(Token{TokenKind::GtEq, ">=", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '&' && n == '&') {
                tokens.push_back(Token{TokenKind::AndAnd, "&&", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '|' && n == '|') {
                tokens.push_back(Token{TokenKind::OrOr, "||", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '+' && n == '=') {
                tokens.push_back(Token{TokenKind::PlusEq, "+=", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '-' && n == '=') {
                tokens.push_back(Token{TokenKind::MinusEq, "-=", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '*' && n == '=') {
                tokens.push_back(Token{TokenKind::StarEq, "*=", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }
            if (c == '/' && n == '=') {
                tokens.push_back(Token{TokenKind::SlashEq, "/=", 0, 0.0, false, Span{line_, col_, 2}});
                advance(2);
                continue;
            }

            TokenKind single = TokenKind::Eof;
            switch (c) {
                case '+': single = TokenKind::Plus; break;
                case '-': single = TokenKind::Minus; break;
                case '*': single = TokenKind::Star; break;
                case '/': single = TokenKind::Slash; break;
                case '%': single = TokenKind::Percent; break;
                case '=': single = TokenKind::Eq; break;
                case '!': single = TokenKind::Bang; break;
                case '<': single = TokenKind::Lt; break;
                case '>': single = TokenKind::Gt; break;
                case '|': single = TokenKind::Pipe; break;
                case ':': single = TokenKind::Colon; break;
                case ',': single = TokenKind::Comma; break;
                case '.': single = TokenKind::Dot; break;
                case ';': single = TokenKind::Semi; break;
                case '(': single = TokenKind::LParen; break;
                case ')': single = TokenKind::RParen; break;
                case '{': single = TokenKind::LBrace; break;
                case '}': single = TokenKind::RBrace; break;
                case '[': single = TokenKind::LBracket; break;
                case ']': single = TokenKind::RBracket; break;
                default:
                    throw RheaException(make_diag(
                        "E0001",
                        std::string("unexpected character '") + c + "'",
                        filename_,
                        sp,
                        "Remove or replace the invalid character."
                    ));
            }
            tokens.push_back(Token{single, std::string(1, c), 0, 0.0, false, sp});
            advance();
        }
        tokens.push_back(Token{TokenKind::Eof, "", 0, 0.0, false, Span{line_, col_, 1}});
        return tokens;
    }

private:
    char peek(size_t offset) const {
        return (pos_ + offset < src_.size()) ? src_[pos_ + offset] : '\0';
    }
    void advance(size_t n = 1) {
        pos_ += n;
        col_ += n;
    }
    void advance_line() {
        ++pos_;
        ++line_;
        col_ = 1;
    }
    static TokenKind classify_keyword(const std::string& s) {
        if (s == "fn") return TokenKind::KwFn;
        if (s == "let") return TokenKind::KwLet;
        if (s == "mut") return TokenKind::KwMut;
        if (s == "const") return TokenKind::KwConst;
        if (s == "if") return TokenKind::KwIf;
        if (s == "else") return TokenKind::KwElse;
        if (s == "while") return TokenKind::KwWhile;
        if (s == "for") return TokenKind::KwFor;
        if (s == "in") return TokenKind::KwIn;
        if (s == "return") return TokenKind::KwReturn;
        if (s == "true") return TokenKind::KwTrue;
        if (s == "false") return TokenKind::KwFalse;
        if (s == "struct") return TokenKind::KwStruct;
        if (s == "enum") return TokenKind::KwEnum;
        if (s == "import") return TokenKind::KwImport;
        if (s == "module") return TokenKind::KwModule;
        if (s == "pub") return TokenKind::KwPub;
        if (s == "priv") return TokenKind::KwPriv;
        if (s == "match") return TokenKind::KwMatch;
        if (s == "try") return TokenKind::KwTry;
        if (s == "catch") return TokenKind::KwCatch;
        if (s == "break") return TokenKind::KwBreak;
        if (s == "continue") return TokenKind::KwContinue;
        if (s == "parallel") return TokenKind::KwParallel;
        if (s == "pipeline") return TokenKind::KwPipeline;
        return TokenKind::Ident;
    }

    std::string filename_;
    std::string src_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t col_ = 1;
};

// ============================================================================
// 3. Abstract Syntax Tree (AST)
// ============================================================================

struct Expr;
struct Stmt;
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;

struct Param {
    std::string name;
    std::string type_name;
    Span span;
};

struct MatchArm {
    std::string pattern_enum;
    std::string pattern_variant;
    std::string binding_name;
    ExprPtr literal_expr;
    bool is_wildcard = false;
    bool is_binding = false;
    bool is_enum_variant = false;
    ExprPtr body_expr;
    std::vector<StmtPtr> body_stmts;
    Span span;
};

struct Expr {
    enum class Kind {
        IntLit,
        FloatLit,
        BoolLit,
        StrLit,
        Ident,
        Unary,
        Binary,
        Call,
        ListLit,
        MapLit,
        Index,
        FieldAccess,
        StructInit,
        EnumVariant,
        Lambda,
        Match
    };

    Kind kind = Kind::IntLit;
    Span span;
    int64_t int_val = 0;
    double float_val = 0.0;
    bool bool_val = false;
    bool is_duration = false;
    std::string text;
    std::string op;
    ExprPtr lhs;
    ExprPtr rhs;
    std::vector<ExprPtr> args;
    std::vector<std::string> generic_args;
    std::vector<std::pair<ExprPtr, ExprPtr>> map_entries;
    std::vector<std::pair<std::string, ExprPtr>> struct_fields;
    std::vector<Param> lambda_params;
    std::string lambda_ret_type = "Any";
    std::vector<StmtPtr> lambda_body_stmts;
    size_t lambda_id = 0;
    std::string enum_name;
    std::string variant_name;
    std::vector<MatchArm> match_arms;
    std::string inferred_type = "Any";
};

struct Stmt {
    enum class Kind {
        Let,
        Const,
        Assign,
        If,
        While,
        For,
        ParallelFor,
        Block,
        Return,
        Break,
        Continue,
        TryCatch,
        Expr
    };

    Kind kind = Kind::Expr;
    Span span;
    std::string name;
    std::string type_ann;
    bool mutable_binding = false;
    bool is_implicit_let = false;
    std::string assign_op = "=";
    ExprPtr target;
    ExprPtr expr;
    ExprPtr cond;
    std::vector<StmtPtr> then_body;
    std::vector<StmtPtr> else_body;
    std::vector<StmtPtr> body;
    std::string catch_var;
    std::vector<StmtPtr> catch_body;
};

struct FnDecl {
    std::string name;
    std::vector<std::string> type_params;
    std::vector<Param> params;
    std::string return_type = "Unit";
    bool explicit_return_type = false;
    bool is_private = false;
    std::string origin_module;
    std::vector<StmtPtr> body;
    Span span;
};

struct StructDecl {
    std::string name;
    std::vector<std::string> type_params;
    std::vector<Param> fields;
    Span span;
};

struct EnumVariantDecl {
    std::string name;
    std::vector<std::string> payload_types;
};

struct EnumDecl {
    std::string name;
    std::vector<EnumVariantDecl> variants;
    Span span;
};

struct PipelineDecl {
    std::string name;
    std::vector<std::string> stages;
    Span span;
};

struct ImportDecl {
    std::string path;
    Span span;
};

struct Program {
    std::string module_name;
    std::vector<ImportDecl> imports;
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<PipelineDecl> pipelines;
    std::vector<StmtPtr> constants;
    std::vector<FnDecl> functions;
    std::vector<StmtPtr> top_stmts;
};

// ============================================================================
// 4. Recursive-Descent Parser (with Generics, Closures, Modules, Implicit Return)
// ============================================================================

class Parser {
public:
    Parser(std::string filename, std::vector<Token> tokens)
        : filename_(std::move(filename)), tokens_(std::move(tokens)) {}

    Program parse_program() {
        Program prog;
        while (!is_at_end()) {
            if (match(TokenKind::Semi)) continue;
            if (check(TokenKind::KwModule)) {
                advance();
                prog.module_name = parse_module_path();
                match(TokenKind::Semi);
            } else if (check(TokenKind::KwImport)) {
                Span sp = peek().span;
                advance();
                prog.imports.push_back(ImportDecl{parse_module_path(), sp});
                match(TokenKind::Semi);
            } else if (check(TokenKind::KwPub) || check(TokenKind::KwPriv)) {
                bool is_priv = check(TokenKind::KwPriv);
                advance();
                if (check(TokenKind::KwFn)) {
                    FnDecl fn = parse_fn_decl();
                    fn.is_private = is_priv;
                    prog.functions.push_back(std::move(fn));
                } else if (check(TokenKind::KwStruct)) {
                    prog.structs.push_back(parse_struct_decl());
                } else {
                    throw RheaException(make_diag(
                        "E0002", "expected 'fn' or 'struct' after visibility modifier", filename_, peek().span
                    ));
                }
            } else if (check(TokenKind::KwStruct)) {
                prog.structs.push_back(parse_struct_decl());
            } else if (check(TokenKind::KwEnum)) {
                prog.enums.push_back(parse_enum_decl());
            } else if (check(TokenKind::KwPipeline)) {
                prog.pipelines.push_back(parse_pipeline_decl());
            } else if (check(TokenKind::KwFn)) {
                prog.functions.push_back(parse_fn_decl());
            } else if (check(TokenKind::KwConst)) {
                StmtPtr c = parse_stmt();
                prog.constants.push_back(c);
                prog.top_stmts.push_back(c);
            } else {
                prog.top_stmts.push_back(parse_stmt());
            }
        }
        return prog;
    }

private:
    std::string parse_module_path() {
        std::string mod;
        if (match(TokenKind::DotDot) || match(TokenKind::Slash)) {
            mod = previous().lexeme;
        } else {
            Token first = consume_ident_or_keyword("Expected module name");
            mod = first.lexeme;
        }
        while (match(TokenKind::ColonColon) || match(TokenKind::Dot) || match(TokenKind::Slash) || match(TokenKind::DotDot)) {
            if (previous().kind == TokenKind::DotDot) {
                mod += "::..";
                continue;
            }
            if (match(TokenKind::DotDot)) {
                mod += "::..";
                continue;
            }
            Token part = consume_ident_or_keyword("Expected module path segment");
            mod += "::" + part.lexeme;
        }
        return mod;
    }

    std::vector<std::string> parse_optional_type_params() {
        std::vector<std::string> tparams;
        if (match(TokenKind::Lt)) {
            do {
                Token tp = consume(TokenKind::Ident, "Expected generic type parameter name");
                tparams.push_back(tp.lexeme);
            } while (match(TokenKind::Comma));
            consume(TokenKind::Gt, "Expected '>' after generic type parameters");
        }
        return tparams;
    }

    StructDecl parse_struct_decl() {
        Span sp = peek().span;
        consume(TokenKind::KwStruct, "Expected 'struct'");
        std::string name = consume(TokenKind::Ident, "Expected struct name").lexeme;
        std::vector<std::string> tparams = parse_optional_type_params();
        consume(TokenKind::LBrace, "Expected '{' after struct name");
        std::vector<Param> fields;
        while (!check(TokenKind::RBrace) && !is_at_end()) {
            Span fsp = peek().span;
            std::string fname = consume(TokenKind::Ident, "Expected field name").lexeme;
            consume(TokenKind::Colon, "Expected ':' after field name");
            std::string ftype = parse_type_name();
            fields.push_back(Param{fname, ftype, fsp});
            if (!match(TokenKind::Comma)) match(TokenKind::Semi);
        }
        consume(TokenKind::RBrace, "Expected '}' after struct fields");
        return StructDecl{name, std::move(tparams), std::move(fields), sp};
    }

    EnumDecl parse_enum_decl() {
        Span sp = peek().span;
        consume(TokenKind::KwEnum, "Expected 'enum'");
        std::string name = consume(TokenKind::Ident, "Expected enum name").lexeme;
        consume(TokenKind::LBrace, "Expected '{' after enum name");
        std::vector<EnumVariantDecl> variants;
        while (!check(TokenKind::RBrace) && !is_at_end()) {
            std::string vname = consume(TokenKind::Ident, "Expected enum variant name").lexeme;
            std::vector<std::string> payloads;
            if (match(TokenKind::LParen)) {
                if (!check(TokenKind::RParen)) {
                    payloads.push_back(parse_type_name());
                    while (match(TokenKind::Comma)) {
                        payloads.push_back(parse_type_name());
                    }
                }
                consume(TokenKind::RParen, "Expected ')' after enum variant payload");
            }
            variants.push_back(EnumVariantDecl{vname, std::move(payloads)});
            if (!match(TokenKind::Comma)) match(TokenKind::Semi);
        }
        consume(TokenKind::RBrace, "Expected '}' after enum variants");
        return EnumDecl{name, std::move(variants), sp};
    }

    PipelineDecl parse_pipeline_decl() {
        Span sp = peek().span;
        consume(TokenKind::KwPipeline, "Expected 'pipeline'");
        std::string name = consume(TokenKind::Ident, "Expected pipeline name").lexeme;
        consume(TokenKind::LBrace, "Expected '{' after pipeline name");
        std::vector<std::string> stages;
        while (!check(TokenKind::RBrace) && !is_at_end()) {
            Token st = consume_ident_or_keyword("Expected stage name in pipeline");
            stages.push_back(st.lexeme);
            if (!match(TokenKind::Arrow) && !match(TokenKind::PipeGreater) && !match(TokenKind::Comma)) {
                match(TokenKind::Semi);
            }
        }
        consume(TokenKind::RBrace, "Expected '}' after pipeline stages");
        return PipelineDecl{name, std::move(stages), sp};
    }

    static void apply_implicit_return(std::vector<StmtPtr>& body) {
        if (body.empty()) return;
        StmtPtr& last = body.back();
        if (last->kind == Stmt::Kind::Expr) {
            auto ret = std::make_shared<Stmt>();
            ret->kind = Stmt::Kind::Return;
            ret->span = last->span;
            ret->expr = last->expr;
            last = ret;
        } else if (last->kind == Stmt::Kind::If && !last->else_body.empty()) {
            apply_implicit_return(last->then_body);
            apply_implicit_return(last->else_body);
        }
    }

    FnDecl parse_fn_decl() {
        Span sp = peek().span;
        consume(TokenKind::KwFn, "Expected 'fn'");
        std::string name = consume(TokenKind::Ident, "Expected function name after 'fn'").lexeme;
        std::vector<std::string> tparams = parse_optional_type_params();
        consume(TokenKind::LParen, "Expected '(' after function name");
        std::vector<Param> params;
        if (!check(TokenKind::RParen)) {
            do {
                Span psp = peek().span;
                std::string pname = consume(TokenKind::Ident, "Expected parameter name").lexeme;
                std::string ptype = "Any";
                if (match(TokenKind::Colon)) {
                    ptype = parse_type_name();
                }
                params.push_back(Param{pname, ptype, psp});
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RParen, "Expected ')' after function parameters");
        std::string ret_type = "Unit";
        bool explicit_ret = false;
        if (match(TokenKind::Arrow)) {
            ret_type = parse_type_name();
            explicit_ret = true;
        }
        std::vector<StmtPtr> body = parse_block();
        if (ret_type != "Unit") {
            apply_implicit_return(body);
        }
        return FnDecl{name, std::move(tparams), std::move(params), ret_type, explicit_ret, false, "", std::move(body), sp};
    }

    std::string parse_type_name(size_t depth = 0) {
        if (depth > MAX_TYPE_DEPTH) {
            throw RheaException(make_diag(
                "E0002", "generic type nesting exceeds maximum depth (32)", filename_, peek().span
            ));
        }
        Token t = consume_ident_or_keyword("Expected type name");
        std::string base = t.lexeme;
        if (match(TokenKind::Lt)) {
            base += "<";
            base += parse_type_name(depth + 1);
            while (match(TokenKind::Comma)) {
                base += ", " + parse_type_name(depth + 1);
            }
            consume(TokenKind::Gt, "Expected '>' after generic type arguments");
            base += ">";
        }
        return base;
    }

    std::vector<StmtPtr> parse_block() {
        if (++parse_depth_ > MAX_PARSE_DEPTH) {
            --parse_depth_;
            throw RheaException(make_diag(
                "E0002", "block nesting depth exceeds maximum limit (256)", filename_, peek().span
            ));
        }
        consume(TokenKind::LBrace, "Expected '{' to start block");
        std::vector<StmtPtr> stmts;
        while (!check(TokenKind::RBrace) && !is_at_end()) {
            if (match(TokenKind::Semi)) continue;
            stmts.push_back(parse_stmt());
        }
        consume(TokenKind::RBrace, "Expected '}' to close block");
        --parse_depth_;
        return stmts;
    }

    StmtPtr parse_stmt() {
        Span sp = peek().span;

        // Standalone lexical block `{ ... }`
        // Disambiguate from Map literal `{ "k": v }`: if `{` is empty `{}` or has `expr :`, it's a Map literal; otherwise a block!
        if (check(TokenKind::LBrace)) {
            bool is_map = false;
            if (peek_next().kind == TokenKind::RBrace) {
                is_map = false; // empty block `{}`
            } else if (current_ + 2 < tokens_.size() && tokens_[current_ + 2].kind == TokenKind::Colon) {
                is_map = true;
            }
            if (!is_map) {
                auto st = std::make_shared<Stmt>();
                st->kind = Stmt::Kind::Block;
                st->span = sp;
                st->body = parse_block();
                return st;
            }
        }

        if (match(TokenKind::KwLet)) {
            bool is_mut = match(TokenKind::KwMut);
            std::string name = consume(TokenKind::Ident, "Expected variable name after 'let'").lexeme;
            std::string type_ann;
            if (match(TokenKind::Colon)) {
                type_ann = parse_type_name();
            }
            consume(TokenKind::Eq, "Expected '=' in variable declaration");
            ExprPtr val = parse_expr();
            match(TokenKind::Semi);
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::Let;
            st->span = sp;
            st->name = name;
            st->type_ann = type_ann;
            st->mutable_binding = is_mut;
            st->expr = expand_interpolated_string(val);
            return st;
        }

        if (match(TokenKind::KwMut)) {
            std::string name = consume(TokenKind::Ident, "Expected variable name after 'mut'").lexeme;
            std::string type_ann;
            if (match(TokenKind::Colon)) {
                type_ann = parse_type_name();
            }
            consume(TokenKind::Eq, "Expected '=' after mutable variable declaration");
            ExprPtr val = parse_expr();
            match(TokenKind::Semi);
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::Let;
            st->span = sp;
            st->name = name;
            st->type_ann = type_ann;
            st->mutable_binding = true;
            st->expr = expand_interpolated_string(val);
            return st;
        }

        if (match(TokenKind::KwConst)) {
            std::string name = consume(TokenKind::Ident, "Expected constant name after 'const'").lexeme;
            std::string type_ann;
            if (match(TokenKind::Colon)) {
                type_ann = parse_type_name();
            }
            consume(TokenKind::Eq, "Expected '=' in constant declaration");
            ExprPtr val = parse_expr();
            match(TokenKind::Semi);
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::Const;
            st->span = sp;
            st->name = name;
            st->type_ann = type_ann;
            st->mutable_binding = false;
            st->expr = expand_interpolated_string(val);
            return st;
        }

        if (match(TokenKind::KwIf)) {
            ExprPtr cond = parse_expr();
            std::vector<StmtPtr> then_b = parse_block();
            std::vector<StmtPtr> else_b;
            if (match(TokenKind::KwElse)) {
                if (check(TokenKind::KwIf)) {
                    else_b.push_back(parse_stmt());
                } else {
                    else_b = parse_block();
                }
            }
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::If;
            st->span = sp;
            st->cond = cond;
            st->then_body = std::move(then_b);
            st->else_body = std::move(else_b);
            return st;
        }

        if (match(TokenKind::KwWhile)) {
            ExprPtr cond = parse_expr();
            std::vector<StmtPtr> body = parse_block();
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::While;
            st->span = sp;
            st->cond = cond;
            st->body = std::move(body);
            return st;
        }

        if (match(TokenKind::KwParallel)) {
            consume(TokenKind::KwFor, "Expected 'for' after 'parallel'");
            std::string var_name = consume(TokenKind::Ident, "Expected loop variable name").lexeme;
            consume(TokenKind::KwIn, "Expected 'in' after loop variable");
            ExprPtr iter = parse_expr();
            std::vector<StmtPtr> body = parse_block();
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::ParallelFor;
            st->span = sp;
            st->name = var_name;
            st->expr = iter;
            st->body = std::move(body);
            return st;
        }

        if (match(TokenKind::KwFor)) {
            std::string var_name = consume(TokenKind::Ident, "Expected loop variable name after 'for'").lexeme;
            consume(TokenKind::KwIn, "Expected 'in' after loop variable");
            ExprPtr iter = parse_expr();
            std::vector<StmtPtr> body = parse_block();
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::For;
            st->span = sp;
            st->name = var_name;
            st->expr = iter;
            st->body = std::move(body);
            return st;
        }

        if (match(TokenKind::KwReturn)) {
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::Return;
            st->span = sp;
            if (!check(TokenKind::Semi) && !check(TokenKind::RBrace) && !is_at_end()) {
                st->expr = expand_interpolated_string(parse_expr());
            }
            match(TokenKind::Semi);
            return st;
        }

        if (match(TokenKind::KwBreak)) {
            match(TokenKind::Semi);
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::Break;
            st->span = sp;
            return st;
        }

        if (match(TokenKind::KwContinue)) {
            match(TokenKind::Semi);
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::Continue;
            st->span = sp;
            return st;
        }

        if (match(TokenKind::KwTry)) {
            std::vector<StmtPtr> try_b = parse_block();
            consume(TokenKind::KwCatch, "Expected 'catch' after 'try' block");
            std::string err_var = "err";
            if (check(TokenKind::Ident)) {
                err_var = advance().lexeme;
            }
            std::vector<StmtPtr> catch_b = parse_block();
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::TryCatch;
            st->span = sp;
            st->body = std::move(try_b);
            st->catch_var = err_var;
            st->catch_body = std::move(catch_b);
            return st;
        }

        if (check(TokenKind::Ident) && peek_next().kind == TokenKind::Colon) {
            std::string name = advance().lexeme;
            advance();
            std::string type_ann = parse_type_name();
            consume(TokenKind::Eq, "Expected '=' after type annotation in variable declaration");
            ExprPtr val = parse_expr();
            match(TokenKind::Semi);
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::Let;
            st->span = sp;
            st->name = name;
            st->type_ann = type_ann;
            st->mutable_binding = false;
            st->is_implicit_let = true;
            st->expr = expand_interpolated_string(val);
            return st;
        }

        ExprPtr lhs = parse_expr();
        if (check(TokenKind::Eq) || check(TokenKind::PlusEq) || check(TokenKind::MinusEq) ||
            check(TokenKind::StarEq) || check(TokenKind::SlashEq)) {
            std::string aop = advance().lexeme;
            ExprPtr rhs = expand_interpolated_string(parse_expr());
            match(TokenKind::Semi);
            auto st = std::make_shared<Stmt>();
            st->kind = Stmt::Kind::Assign;
            st->span = sp;
            st->assign_op = aop;
            st->target = lhs;
            if (lhs->kind == Expr::Kind::Ident) {
                st->name = lhs->text;
            }
            st->expr = rhs;
            return st;
        }

        match(TokenKind::Semi);
        auto st = std::make_shared<Stmt>();
        st->kind = Stmt::Kind::Expr;
        st->span = sp;
        st->expr = expand_interpolated_string(lhs);
        return st;
    }

    ExprPtr expand_interpolated_string(ExprPtr e) {
        if (!e || e->kind != Expr::Kind::StrLit) return e;
        const std::string& s = e->text;
        if (s.find('{') == std::string::npos || s.find('}') == std::string::npos) return e;

        std::vector<ExprPtr> parts;
        size_t i = 0;
        std::string literal_buf;
        while (i < s.size()) {
            if (s[i] == '{') {
                size_t close = s.find('}', i + 1);
                if (close != std::string::npos && close > i + 1) {
                    std::string inner = s.substr(i + 1, close - i - 1);
                    bool valid_ident = true;
                    for (size_t k = 0; k < inner.size(); ++k) {
                        char ch = inner[k];
                        if (k == 0 && !(std::isalpha(static_cast<unsigned char>(ch)) || ch == '_')) {
                            valid_ident = false; break;
                        }
                        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
                            valid_ident = false; break;
                        }
                    }
                    if (valid_ident) {
                        if (!literal_buf.empty()) {
                            auto lit = std::make_shared<Expr>();
                            lit->kind = Expr::Kind::StrLit;
                            lit->span = e->span;
                            lit->text = literal_buf;
                            parts.push_back(lit);
                            literal_buf.clear();
                        }
                        auto id = std::make_shared<Expr>();
                        id->kind = Expr::Kind::Ident;
                        id->span = e->span;
                        id->text = inner;
                        auto str_call = std::make_shared<Expr>();
                        str_call->kind = Expr::Kind::Call;
                        str_call->span = e->span;
                        str_call->text = "str";
                        str_call->args.push_back(id);
                        parts.push_back(str_call);
                        i = close + 1;
                        continue;
                    }
                }
            }
            literal_buf.push_back(s[i]);
            ++i;
        }
        if (parts.empty()) return e;
        if (!literal_buf.empty()) {
            auto lit = std::make_shared<Expr>();
            lit->kind = Expr::Kind::StrLit;
            lit->span = e->span;
            lit->text = literal_buf;
            parts.push_back(lit);
        }
        ExprPtr acc = parts[0];
        for (size_t p = 1; p < parts.size(); ++p) {
            auto bin = std::make_shared<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->span = e->span;
            bin->op = "+";
            bin->lhs = acc;
            bin->rhs = parts[p];
            acc = bin;
        }
        return acc;
    }

    ExprPtr parse_expr() {
        if (++parse_depth_ > MAX_PARSE_DEPTH) {
            --parse_depth_;
            throw RheaException(make_diag(
                "E0002", "expression nesting depth exceeds maximum limit (256)", filename_, peek().span
            ));
        }
        ExprPtr res = parse_pipeline();
        --parse_depth_;
        return res;
    }

    ExprPtr parse_pipeline() {
        ExprPtr left = parse_logic_or();
        while (match(TokenKind::PipeGreater)) {
            Span sp = previous().span;
            ExprPtr right = parse_logic_or();
            if (right->kind == Expr::Kind::Ident) {
                auto call = std::make_shared<Expr>();
                call->kind = Expr::Kind::Call;
                call->span = sp;
                call->text = right->text;
                call->args.push_back(left);
                left = call;
            } else if (right->kind == Expr::Kind::Call) {
                right->args.insert(right->args.begin(), left);
                left = right;
            } else {
                throw RheaException(make_diag(
                    "E0002", "right-hand side of '|>' pipeline operator must be a function or call",
                    filename_, sp,
                    "Use 'value |> func' or 'value |> func(extra_arg)'."
                ));
            }
        }
        return left;
    }

    ExprPtr parse_logic_or() {
        ExprPtr left = parse_logic_and();
        while (match(TokenKind::OrOr)) {
            Span sp = previous().span;
            ExprPtr right = parse_logic_and();
            auto bin = std::make_shared<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->span = sp;
            bin->op = "||";
            bin->lhs = left;
            bin->rhs = right;
            left = bin;
        }
        return left;
    }

    ExprPtr parse_logic_and() {
        ExprPtr left = parse_equality();
        while (match(TokenKind::AndAnd)) {
            Span sp = previous().span;
            ExprPtr right = parse_equality();
            auto bin = std::make_shared<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->span = sp;
            bin->op = "&&";
            bin->lhs = left;
            bin->rhs = right;
            left = bin;
        }
        return left;
    }

    ExprPtr parse_equality() {
        ExprPtr left = parse_comparison();
        while (check(TokenKind::EqEq) || check(TokenKind::BangEq)) {
            Token op_tok = advance();
            ExprPtr right = parse_comparison();
            auto bin = std::make_shared<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->span = op_tok.span;
            bin->op = op_tok.lexeme;
            bin->lhs = left;
            bin->rhs = right;
            left = bin;
        }
        return left;
    }

    ExprPtr parse_comparison() {
        ExprPtr left = parse_range();
        while (check(TokenKind::Lt) || check(TokenKind::LtEq) ||
               check(TokenKind::Gt) || check(TokenKind::GtEq)) {
            Token op_tok = advance();
            ExprPtr right = parse_range();
            auto bin = std::make_shared<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->span = op_tok.span;
            bin->op = op_tok.lexeme;
            bin->lhs = left;
            bin->rhs = right;
            left = bin;
        }
        return left;
    }

    ExprPtr parse_range() {
        ExprPtr left = parse_term();
        if (check(TokenKind::DotDot) || check(TokenKind::DotDotEq)) {
            Token op_tok = advance();
            ExprPtr right = parse_term();
            auto bin = std::make_shared<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->span = op_tok.span;
            bin->op = op_tok.lexeme;
            bin->lhs = left;
            bin->rhs = right;
            return bin;
        }
        return left;
    }

    ExprPtr parse_term() {
        ExprPtr left = parse_factor();
        while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
            Token op_tok = advance();
            ExprPtr right = parse_factor();
            auto bin = std::make_shared<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->span = op_tok.span;
            bin->op = op_tok.lexeme;
            bin->lhs = left;
            bin->rhs = right;
            left = expand_interpolated_string(bin);
        }
        return left;
    }

    ExprPtr parse_factor() {
        ExprPtr left = parse_unary();
        while (check(TokenKind::Star) || check(TokenKind::Slash) || check(TokenKind::Percent)) {
            Token op_tok = advance();
            ExprPtr right = parse_unary();
            auto bin = std::make_shared<Expr>();
            bin->kind = Expr::Kind::Binary;
            bin->span = op_tok.span;
            bin->op = op_tok.lexeme;
            bin->lhs = left;
            bin->rhs = right;
            left = bin;
        }
        return left;
    }

    ExprPtr parse_unary() {
        if (check(TokenKind::Minus) || check(TokenKind::Bang)) {
            Token op_tok = advance();
            ExprPtr operand = parse_unary();
            auto u = std::make_shared<Expr>();
            u->kind = Expr::Kind::Unary;
            u->span = op_tok.span;
            u->op = op_tok.lexeme;
            u->lhs = operand;
            return u;
        }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        ExprPtr expr = parse_primary();
        while (true) {
            if (match(TokenKind::LParen)) {
                Span sp = previous().span;
                std::vector<ExprPtr> args;
                if (!check(TokenKind::RParen)) {
                    do {
                        args.push_back(expand_interpolated_string(parse_expr()));
                    } while (match(TokenKind::Comma));
                }
                consume(TokenKind::RParen, "Expected ')' after argument list");
                if (expr->kind == Expr::Kind::Ident) {
                    auto call = std::make_shared<Expr>();
                    call->kind = Expr::Kind::Call;
                    call->span = expr->span;
                    call->text = expr->text;
                    call->generic_args = expr->generic_args;
                    call->args = std::move(args);
                    expr = call;
                } else if (expr->kind == Expr::Kind::FieldAccess && expr->lhs &&
                           expr->lhs->kind == Expr::Kind::Ident) {
                    auto call = std::make_shared<Expr>();
                    call->kind = Expr::Kind::Call;
                    call->span = sp;
                    call->text = expr->lhs->text + "." + expr->text;
                    call->args = std::move(args);
                    expr = call;
                } else if (expr->kind == Expr::Kind::EnumVariant) {
                    expr->args = std::move(args);
                } else {
                    throw RheaException(make_diag(
                        "E0002", "expression is not callable", filename_, sp,
                        "Call a named function, closure variable, or module function."
                    ));
                }
            } else if (match(TokenKind::LBracket)) {
                Span sp = previous().span;
                ExprPtr idx = parse_expr();
                consume(TokenKind::RBracket, "Expected ']' after index expression");
                auto ie = std::make_shared<Expr>();
                ie->kind = Expr::Kind::Index;
                ie->span = sp;
                ie->lhs = expr;
                ie->rhs = idx;
                expr = ie;
            } else if (match(TokenKind::Dot)) {
                Span sp = previous().span;
                Token member = consume_ident_or_keyword("Expected field or method name after '.'");
                auto fa = std::make_shared<Expr>();
                fa->kind = Expr::Kind::FieldAccess;
                fa->span = sp;
                fa->lhs = expr;
                fa->text = member.lexeme;
                expr = fa;
            } else if (match(TokenKind::ColonColon)) {
                Span sp = previous().span;
                Token seg = consume_ident_or_keyword("Expected identifier after '::'");
                if (expr->kind == Expr::Kind::Ident &&
                    (expr->text == "std" || expr->text.rfind("std::", 0) == 0)) {
                    expr->text += "::" + seg.lexeme;
                } else if (expr->kind == Expr::Kind::Ident && !expr->text.empty() &&
                           std::isupper(static_cast<unsigned char>(expr->text[0]))) {
                    auto ev = std::make_shared<Expr>();
                    ev->kind = Expr::Kind::EnumVariant;
                    ev->span = sp;
                    ev->enum_name = expr->text;
                    ev->variant_name = seg.lexeme;
                    expr = ev;
                } else if (expr->kind == Expr::Kind::Ident) {
                    // Module-qualified identifier: `math_utils::triple` -> `math_utils.triple`
                    expr->text += "." + seg.lexeme;
                } else {
                    throw RheaException(make_diag(
                        "E0002", "invalid '::' path qualifier", filename_, sp,
                        "Use 'std::module.func(...)', 'module::func(...)', or 'EnumName::Variant'."
                    ));
                }
            } else {
                break;
            }
        }
        return expr;
    }

    ExprPtr parse_primary() {
        Token t = peek();
        if (match(TokenKind::Int)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::IntLit;
            e->span = t.span;
            e->int_val = t.int_val;
            e->is_duration = t.is_duration;
            return e;
        }
        if (match(TokenKind::Float)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::FloatLit;
            e->span = t.span;
            e->float_val = t.float_val;
            return e;
        }
        if (match(TokenKind::KwTrue)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::BoolLit;
            e->span = t.span;
            e->bool_val = true;
            return e;
        }
        if (match(TokenKind::KwFalse)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::BoolLit;
            e->span = t.span;
            e->bool_val = false;
            return e;
        }
        if (match(TokenKind::Str)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::StrLit;
            e->span = t.span;
            e->text = t.lexeme;
            return expand_interpolated_string(e);
        }
        if (match(TokenKind::LParen)) {
            ExprPtr inner = parse_expr();
            consume(TokenKind::RParen, "Expected ')' after grouped expression");
            return inner;
        }
        if (match(TokenKind::LBracket)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::ListLit;
            e->span = t.span;
            if (!check(TokenKind::RBracket)) {
                do {
                    if (check(TokenKind::RBracket)) break;
                    e->args.push_back(expand_interpolated_string(parse_expr()));
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RBracket, "Expected ']' after list elements");
            return e;
        }
        if (match(TokenKind::LBrace)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::MapLit;
            e->span = t.span;
            if (match(TokenKind::Colon)) {
                // `{:}` explicit empty map literal
            } else if (!check(TokenKind::RBrace)) {
                do {
                    if (check(TokenKind::RBrace)) break;
                    ExprPtr k = parse_expr();
                    consume(TokenKind::Colon, "Expected ':' between map key and value");
                    ExprPtr v = expand_interpolated_string(parse_expr());
                    e->map_entries.push_back({k, v});
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RBrace, "Expected '}' after map entries");
            return e;
        }
        // Zero-parameter closure `|| expr` or `|| { ... }`
        if (match(TokenKind::OrOr)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::Lambda;
            e->span = t.span;
            if (match(TokenKind::Arrow)) {
                e->lambda_ret_type = parse_type_name();
            }
            if (check(TokenKind::LBrace)) {
                e->lambda_body_stmts = parse_block();
                apply_implicit_return(e->lambda_body_stmts);
            } else {
                e->lhs = expand_interpolated_string(parse_expr());
            }
            return e;
        }
        // Parameterized closure `|x, y: Int| expr` or `|x: Int| -> Int { ... }`
        if (match(TokenKind::Pipe)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::Lambda;
            e->span = t.span;
            if (!check(TokenKind::Pipe)) {
                do {
                    Span psp = peek().span;
                    Token p = consume(TokenKind::Ident, "Expected closure parameter name");
                    std::string ptype = "Any";
                    if (match(TokenKind::Colon)) {
                        ptype = parse_type_name();
                    }
                    e->lambda_params.push_back(Param{p.lexeme, ptype, psp});
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::Pipe, "Expected closing '|' after closure parameters");
            if (match(TokenKind::Arrow)) {
                e->lambda_ret_type = parse_type_name();
            }
            if (check(TokenKind::LBrace)) {
                e->lambda_body_stmts = parse_block();
                apply_implicit_return(e->lambda_body_stmts);
            } else {
                e->lhs = expand_interpolated_string(parse_expr());
            }
            return e;
        }
        if (match(TokenKind::KwMatch)) {
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::Match;
            e->span = t.span;
            e->lhs = parse_expr();
            consume(TokenKind::LBrace, "Expected '{' after match value");
            while (!check(TokenKind::RBrace) && !is_at_end()) {
                MatchArm arm;
                arm.span = peek().span;
                if (check(TokenKind::Ident) && peek().lexeme == "_") {
                    advance();
                    arm.is_wildcard = true;
                } else if (check(TokenKind::Ident) && peek_next().kind == TokenKind::ColonColon) {
                    arm.is_enum_variant = true;
                    arm.pattern_enum = advance().lexeme;
                    advance();
                    arm.pattern_variant = consume(TokenKind::Ident, "Expected variant name").lexeme;
                    if (match(TokenKind::LParen)) {
                        if (!check(TokenKind::RParen)) {
                            arm.binding_name = consume(TokenKind::Ident, "Expected pattern binding variable").lexeme;
                        }
                        consume(TokenKind::RParen, "Expected ')' after pattern binding");
                    }
                } else if (check(TokenKind::Ident)) {
                    arm.is_binding = true;
                    arm.binding_name = advance().lexeme;
                } else {
                    arm.literal_expr = parse_primary();
                }
                consume(TokenKind::FatArrow, "Expected '=>' in match arm");
                if (check(TokenKind::LBrace)) {
                    arm.body_stmts = parse_block();
                } else {
                    arm.body_expr = expand_interpolated_string(parse_expr());
                }
                match(TokenKind::Comma);
                match(TokenKind::Semi);
                e->match_arms.push_back(std::move(arm));
            }
            consume(TokenKind::RBrace, "Expected '}' after match arms");
            return e;
        }
        if (match(TokenKind::Ident)) {
            if (check(TokenKind::LBrace) && !t.lexeme.empty() &&
                std::isupper(static_cast<unsigned char>(t.lexeme[0]))) {
                size_t saved = current_;
                advance();
                if (check(TokenKind::RBrace) ||
                    (check(TokenKind::Ident) && peek_next().kind == TokenKind::Colon)) {
                    auto st = std::make_shared<Expr>();
                    st->kind = Expr::Kind::StructInit;
                    st->span = t.span;
                    st->text = t.lexeme;
                    if (!check(TokenKind::RBrace)) {
                        do {
                            if (check(TokenKind::RBrace)) break;
                            std::string fn = consume(TokenKind::Ident, "Expected field name").lexeme;
                            consume(TokenKind::Colon, "Expected ':' after struct field name");
                            ExprPtr fv = expand_interpolated_string(parse_expr());
                            st->struct_fields.push_back({fn, fv});
                        } while (match(TokenKind::Comma));
                    }
                    consume(TokenKind::RBrace, "Expected '}' after struct initializer");
                    return st;
                }
                current_ = saved;
            }
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::Ident;
            e->span = t.span;
            e->text = t.lexeme;
            return e;
        }

        throw RheaException(make_diag(
            "E0002",
            "expected expression, found '" + token_kind_name(t.kind) + "'",
            filename_,
            t.span,
            "Provide a valid literal, variable name, function call, or parenthesized expression."
        ));
    }

    bool is_at_end() const { return peek().kind == TokenKind::Eof; }
    const Token& peek() const { return tokens_[current_]; }
    const Token& peek_next() const {
        if (current_ + 1 < tokens_.size()) return tokens_[current_ + 1];
        return tokens_.back();
    }
    const Token& previous() const { return tokens_[current_ - 1]; }
    bool check(TokenKind k) const { return !is_at_end() && peek().kind == k; }
    const Token& advance() {
        if (!is_at_end()) ++current_;
        return previous();
    }
    bool match(TokenKind k) {
        if (check(k)) {
            advance();
            return true;
        }
        return false;
    }
    const Token& consume(TokenKind k, const std::string& msg) {
        if (check(k)) return advance();
        throw RheaException(make_diag(
            "E0002",
            msg + " (found '" + token_kind_name(peek().kind) + "')",
            filename_,
            peek().span,
            "Check for missing delimiters, parentheses, or braces around this token."
        ));
    }
    const Token& consume_ident_or_keyword(const std::string& msg) {
        if (check(TokenKind::Ident)) {
            return advance();
        }
        if (!is_at_end() && !peek().lexeme.empty() &&
            std::isalpha(static_cast<unsigned char>(peek().lexeme[0]))) {
            return advance();
        }
        return consume(TokenKind::Ident, msg);
    }

    std::string filename_;
    std::vector<Token> tokens_;
    size_t current_ = 0;
    size_t parse_depth_ = 0;
};

// ============================================================================
// 5. Module Resolver (Stdlib Validation, Local Modules, Circular/Traversal Guards)
// ============================================================================

class ModuleResolver {
public:
    void resolve_imports(Program& root_prog, const std::string& root_file) {
        fs::path abs_root = fs::weakly_canonical(fs::absolute(root_file));
        active_stack_.push_back(abs_root.string());
        loaded_.insert(abs_root.string());
        resolve_program_imports(root_prog, abs_root, root_prog);
        active_stack_.pop_back();
    }

    const std::unordered_map<std::string, std::string>& private_functions() const {
        return private_functions_;
    }

private:
    static bool is_valid_std_module(const std::string& m) {
        static const std::unordered_set<std::string> kStdMods = {
            "std::io", "std::time", "std::math", "std::string", "std::collections",
            "std::fs", "std::env", "std::test", "std::testing", "std::ai",
            "std::random", "std::json", "std::path", "std::process",
            "std::log", "std::logging", "std::tensor", "std::autodiff", "std::ml"
        };
        return kStdMods.count(m) > 0;
    }

    static void rewrite_private_calls_in_expr(
        ExprPtr& e,
        const std::unordered_map<std::string, std::string>& priv_renames,
        const std::string& mod_alias,
        const std::unordered_set<std::string>& mod_fns
    ) {
        if (!e) return;
        rewrite_private_calls_in_expr(e->lhs, priv_renames, mod_alias, mod_fns);
        rewrite_private_calls_in_expr(e->rhs, priv_renames, mod_alias, mod_fns);
        for (auto& a : e->args) rewrite_private_calls_in_expr(a, priv_renames, mod_alias, mod_fns);
        for (auto& kv : e->map_entries) {
            rewrite_private_calls_in_expr(kv.first, priv_renames, mod_alias, mod_fns);
            rewrite_private_calls_in_expr(kv.second, priv_renames, mod_alias, mod_fns);
        }
        for (auto& sf : e->struct_fields) {
            rewrite_private_calls_in_expr(sf.second, priv_renames, mod_alias, mod_fns);
        }
        for (auto& st : e->lambda_body_stmts) {
            rewrite_private_calls_in_stmt(st, priv_renames, mod_alias, mod_fns);
        }
        if (e->kind == Expr::Kind::Call) {
            auto it = priv_renames.find(e->text);
            if (it != priv_renames.end()) {
                e->text = it->second;
            }
        }
    }

    static void rewrite_private_calls_in_stmt(
        StmtPtr& st,
        const std::unordered_map<std::string, std::string>& priv_renames,
        const std::string& mod_alias,
        const std::unordered_set<std::string>& mod_fns
    ) {
        if (!st) return;
        rewrite_private_calls_in_expr(st->target, priv_renames, mod_alias, mod_fns);
        rewrite_private_calls_in_expr(st->expr, priv_renames, mod_alias, mod_fns);
        rewrite_private_calls_in_expr(st->cond, priv_renames, mod_alias, mod_fns);
        for (auto& s : st->then_body) rewrite_private_calls_in_stmt(s, priv_renames, mod_alias, mod_fns);
        for (auto& s : st->else_body) rewrite_private_calls_in_stmt(s, priv_renames, mod_alias, mod_fns);
        for (auto& s : st->body) rewrite_private_calls_in_stmt(s, priv_renames, mod_alias, mod_fns);
        for (auto& s : st->catch_body) rewrite_private_calls_in_stmt(s, priv_renames, mod_alias, mod_fns);
    }

    void resolve_program_imports(Program& current_prog, const fs::path& current_file, Program& dest_root) {
        fs::path base_dir = current_file.parent_path();
        for (const auto& imp : current_prog.imports) {
            const std::string& mod_path = imp.path;
            if (mod_path.rfind("std::", 0) == 0) {
                if (!is_valid_std_module(mod_path)) {
                    static const std::vector<std::string> kAllStd = {
                        "std::io", "std::time", "std::math", "std::string", "std::collections",
                        "std::fs", "std::env", "std::test", "std::testing", "std::ai",
                        "std::random", "std::json", "std::path", "std::process", "std::log"
                    };
                    std::string sug = find_closest_match(mod_path, kAllStd, 2);
                    std::string msg = "unknown standard library module '" + mod_path + "'" +
                                      (sug.empty() ? "" : " (did you mean '" + sug + "'?)");
                    std::string help = sug.empty()
                        ? "Use a valid standard module such as 'std::io', 'std::math', 'std::time', 'std::string', 'std::collections', 'std::json', 'std::path', 'std::env', 'std::fs', 'std::log', or 'std::ai'."
                        : ("Replace '" + mod_path + "' with '" + sug + "'.");
                    throw RheaException(make_diag("E0009", msg, current_file.string(), imp.span, help));
                }
                continue;
            }

            // Reject path traversal (`..`)
            if (mod_path.find("..") != std::string::npos || (!mod_path.empty() && mod_path[0] == '/')) {
                throw RheaException(make_diag(
                    "E0009",
                    "invalid module path '" + mod_path + "': parent directory traversal ('..') is forbidden",
                    current_file.string(),
                    imp.span,
                    "Import modules using relative module names within the project workspace."
                ));
            }

            // Convert `a::b` -> `a/b.rhea`
            std::string rel_str;
            for (size_t i = 0; i < mod_path.size(); ++i) {
                if (i + 1 < mod_path.size() && mod_path[i] == ':' && mod_path[i + 1] == ':') {
                    rel_str.push_back('/');
                    ++i;
                } else {
                    rel_str.push_back(mod_path[i]);
                }
            }
            rel_str += ".rhea";

            std::vector<fs::path> candidates = {
                base_dir / rel_str,
                base_dir / "modules" / rel_str,
                fs::current_path() / rel_str
            };
            fs::path resolved_path;
            bool found = false;
            for (const auto& cand : candidates) {
                if (fs::exists(cand) && fs::is_regular_file(cand)) {
                    resolved_path = fs::weakly_canonical(fs::absolute(cand));
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw RheaException(make_diag(
                    "E0009",
                    "cannot find local module '" + mod_path + "' (looked for '" + (base_dir / rel_str).string() + "')",
                    current_file.string(),
                    imp.span,
                    "Verify the module filename and relative location."
                ));
            }

            std::string canon = resolved_path.string();
            // Circular import check
            auto cyc_it = std::find(active_stack_.begin(), active_stack_.end(), canon);
            if (cyc_it != active_stack_.end()) {
                std::ostringstream chain;
                for (auto it = cyc_it; it != active_stack_.end(); ++it) {
                    chain << fs::path(*it).filename().string() << " -> ";
                }
                chain << resolved_path.filename().string();
                throw RheaException(make_diag(
                    "E0009",
                    "circular module import detected: " + chain.str(),
                    current_file.string(),
                    imp.span,
                    "Break the dependency cycle by moving shared definitions into a third module."
                ));
            }

            if (loaded_.count(canon)) {
                continue; // Already linked
            }
            loaded_.insert(canon);
            active_stack_.push_back(canon);

            std::ifstream ifs(canon);
            std::ostringstream ss;
            ss << ifs.rdbuf();
            std::string mod_src = ss.str();

            Lexer lex(canon, mod_src);
            Parser parser(canon, lex.tokenize());
            Program sub_prog = parser.parse_program();

            // Recursively resolve transitive imports first
            resolve_program_imports(sub_prog, resolved_path, dest_root);
            active_stack_.pop_back();

            // Determine short module alias (last segment of import path)
            std::string mod_alias = mod_path;
            size_t last_sep = mod_path.rfind("::");
            if (last_sep != std::string::npos) {
                mod_alias = mod_path.substr(last_sep + 2);
            }

            // Collect private function renames for internal calls inside this module
            std::unordered_map<std::string, std::string> priv_renames;
            std::unordered_set<std::string> mod_fns;
            for (const auto& fn : sub_prog.functions) {
                mod_fns.insert(fn.name);
                if (fn.is_private) {
                    std::string internal_name = "__priv__" + mod_alias + "__" + fn.name;
                    priv_renames[fn.name] = internal_name;
                    private_functions_[fn.name] = mod_alias;
                    private_functions_[mod_alias + "." + fn.name] = mod_alias;
                }
            }

            for (auto& fn : sub_prog.functions) {
                for (auto& st : fn.body) {
                    rewrite_private_calls_in_stmt(st, priv_renames, mod_alias, mod_fns);
                }
                fn.origin_module = mod_alias;
                if (fn.is_private) {
                    FnDecl internal_fn = fn;
                    internal_fn.name = priv_renames[fn.name];
                    internal_fn.is_private = false;
                    dest_root.functions.push_back(std::move(internal_fn));
                } else {
                    dest_root.functions.push_back(fn);
                    FnDecl qualified_fn = fn;
                    qualified_fn.name = mod_alias + "__dot__" + fn.name;
                    dest_root.functions.push_back(std::move(qualified_fn));
                }
            }
            for (const auto& s : sub_prog.structs) dest_root.structs.push_back(s);
            for (const auto& en : sub_prog.enums) dest_root.enums.push_back(en);
            for (const auto& c : sub_prog.constants) dest_root.constants.push_back(c);
        }
    }

    std::vector<std::string> active_stack_;
    std::unordered_set<std::string> loaded_;
    std::unordered_map<std::string, std::string> private_functions_;
};

// ============================================================================
// 6. Static Type Checker (Shadowing Resolution, Typo Guard, Generics, Closures)
// ============================================================================

struct VarInfo {
    std::string type_name;
    std::string resolved_name;
    bool is_mutable = false;
    Span decl_span;
};

struct FnSig {
    std::vector<std::string> type_params;
    std::vector<std::string> param_types;
    std::string return_type;
    bool variadic = false;
    const FnDecl* decl_ptr = nullptr;
};

class TypeChecker {
public:
    explicit TypeChecker(
        std::string filename,
        std::unordered_map<std::string, std::string> private_fns = {}
    ) : filename_(std::move(filename)), private_fns_(std::move(private_fns)) {
        register_builtins();
    }

    void check_program(Program& prog) {
        std::unordered_set<std::string> seen_structs;
        for (const auto& s : prog.structs) {
            if (!seen_structs.insert(s.name).second) {
                throw RheaException(make_diag(
                    "E0002", "duplicate struct declaration '" + s.name + "'", filename_, s.span
                ));
            }
            std::unordered_map<std::string, std::string> fmap;
            for (const auto& f : s.fields) {
                if (!fmap.emplace(f.name, f.type_name).second) {
                    throw RheaException(make_diag(
                        "E0002", "duplicate field '" + f.name + "' in struct '" + s.name + "'", filename_, f.span
                    ));
                }
            }
            structs_[s.name] = std::move(fmap);
            struct_decls_[s.name] = s;
        }
        for (const auto& en : prog.enums) {
            std::unordered_map<std::string, std::vector<std::string>> vmap;
            for (const auto& v : en.variants) vmap[v.name] = v.payload_types;
            enums_[en.name] = std::move(vmap);
        }
        for (const auto& pl : prog.pipelines) {
            pipelines_[pl.name] = pl.stages;
        }

        std::unordered_set<std::string> seen_fns;
        for (const auto& fn : prog.functions) {
            if (!seen_fns.insert(fn.name).second) {
                throw RheaException(make_diag(
                    "E0002", "duplicate function declaration '" + fn.name + "'", filename_, fn.span,
                    "Rename or remove the duplicate function definition."
                ));
            }
            std::unordered_set<std::string> seen_params;
            FnSig sig;
            sig.type_params = fn.type_params;
            for (const auto& p : fn.params) {
                if (!seen_params.insert(p.name).second) {
                    throw RheaException(make_diag(
                        "E0002", "duplicate parameter '" + p.name + "' in function '" + fn.name + "'", filename_, p.span
                    ));
                }
                sig.param_types.push_back(p.type_name);
            }
            sig.return_type = fn.return_type;
            sig.decl_ptr = &fn;
            functions_[fn.name] = sig;
            // If function name has `__dot__`, also register `mod.fn`
            size_t dot_pos = fn.name.find("__dot__");
            if (dot_pos != std::string::npos) {
                std::string dotted = fn.name.substr(0, dot_pos) + "." + fn.name.substr(dot_pos + 7);
                functions_[dotted] = sig;
            }
        }

        scopes_.clear();
        var_name_counts_.clear();
        push_scope();
        for (auto& st : prog.top_stmts) {
            check_stmt(st, "Unit");
        }
        for (auto& fn : prog.functions) {
            var_name_counts_.clear();
            push_scope();
            active_type_params_.clear();
            for (const auto& tp : fn.type_params) active_type_params_.insert(tp);
            for (const auto& p : fn.params) {
                define_var(p.name, p.type_name, false, p.span);
            }
            for (auto& st : fn.body) {
                check_stmt(st, fn.return_type);
            }
            if (fn.return_type != "Unit" && !all_paths_return(fn.body)) {
                throw RheaException(make_diag(
                    "E0007",
                    "function '" + fn.name + "' declares return type " + fn.return_type +
                    ", but not all control paths return a value",
                    filename_,
                    fn.span,
                    "Ensure every branch ends with a 'return' statement or a final expression of type " + fn.return_type + "."
                ));
            }
            active_type_params_.clear();
            pop_scope();
        }
        pop_scope();
    }

private:
    static bool all_paths_return(const std::vector<StmtPtr>& stmts) {
        for (const auto& st : stmts) {
            if (!st) continue;
            if (st->kind == Stmt::Kind::Return) return true;
            if (st->kind == Stmt::Kind::Block && all_paths_return(st->body)) return true;
            if (st->kind == Stmt::Kind::If && !st->else_body.empty() &&
                all_paths_return(st->then_body) && all_paths_return(st->else_body)) {
                return true;
            }
        }
        return false;
    }

    void register_builtins() {
        functions_["print"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["println"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["print_inline"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["input"] = FnSig{{}, {}, "String", true, nullptr};
        functions_["input_int"] = FnSig{{}, {}, "Int", true, nullptr};
        functions_["input_float"] = FnSig{{}, {}, "Float", true, nullptr};
        functions_["try_parse_int"] = FnSig{{}, {"String"}, "Map<String, Any>", false, nullptr};
        functions_["try_parse_float"] = FnSig{{}, {"String"}, "Map<String, FloatAny>", false, nullptr};
        functions_["parse_int"] = FnSig{{}, {"String"}, "Int", false, nullptr};
        functions_["parse_float"] = FnSig{{}, {"String"}, "Float", false, nullptr};
        functions_["is_eof"] = FnSig{{}, {}, "Bool", false, nullptr};
        functions_["sleep"] = FnSig{{}, {"Int"}, "Unit", false, nullptr};
        functions_["now_ms"] = FnSig{{}, {}, "Int", false, nullptr};
        functions_["trim"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["lower"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["upper"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["len"] = FnSig{{}, {"Any"}, "Int", false, nullptr};
        functions_["str"] = FnSig{{}, {"Any"}, "String", false, nullptr};
        functions_["int"] = FnSig{{}, {"Any"}, "Int", false, nullptr};
        functions_["float"] = FnSig{{}, {"Any"}, "Float", false, nullptr};
        functions_["abs"] = FnSig{{}, {"Any"}, "Any", false, nullptr};
        functions_["sqrt"] = FnSig{{}, {"Float"}, "Float", false, nullptr};
        functions_["range"] = FnSig{{}, {}, "List<Int>", true, nullptr};
        functions_["push"] = FnSig{{}, {"Any", "Any"}, "Unit", false, nullptr};
        functions_["pop"] = FnSig{{}, {"Any"}, "Any", false, nullptr};
        functions_["contains"] = FnSig{{}, {"Any", "Any"}, "Bool", false, nullptr};
        functions_["reverse"] = FnSig{{}, {"Any"}, "Any", false, nullptr};
        functions_["sort"] = FnSig{{}, {"Any"}, "Any", false, nullptr};
        functions_["has_key"] = FnSig{{}, {"Any", "Any"}, "Bool", false, nullptr};
        functions_["keys"] = FnSig{{}, {"Any"}, "List<String>", false, nullptr};
        functions_["values"] = FnSig{{}, {"Any"}, "List<Any>", false, nullptr};
        functions_["remove_key"] = FnSig{{}, {"Any", "Any"}, "Bool", false, nullptr};
        functions_["eprint"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["eprintln"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["assert"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["assert_eq"] = FnSig{{}, {"Any", "Any"}, "Unit", false, nullptr};
        functions_["tensor"] = FnSig{{}, {}, "Tensor", true, nullptr};
        functions_["matmul"] = FnSig{{}, {"Tensor", "Tensor"}, "Tensor", false, nullptr};
        // Standard library module signatures (D-20)
        functions_["std::io.print"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["std::io.println"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["std::io.print_inline"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["std::io.eprint"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["std::io.eprintln"] = FnSig{{}, {}, "Unit", true, nullptr};
        functions_["std::io.read_line"] = FnSig{{}, {}, "String", true, nullptr};
        functions_["std::io.input"] = FnSig{{}, {}, "String", true, nullptr};
        functions_["std::io.is_eof"] = FnSig{{}, {}, "Bool", false, nullptr};
        functions_["std::io.try_parse_int"] = FnSig{{}, {"String"}, "Map<String, Any>", false, nullptr};
        functions_["std::io.try_parse_float"] = FnSig{{}, {"String"}, "Map<String, FloatAny>", false, nullptr};
        functions_["std::io.parse_int"] = FnSig{{}, {"String"}, "Int", false, nullptr};
        functions_["std::io.parse_float"] = FnSig{{}, {"String"}, "Float", false, nullptr};
        functions_["std::time.sleep_ms"] = FnSig{{}, {"Int"}, "Unit", false, nullptr};
        functions_["std::time.monotonic_ms"] = FnSig{{}, {}, "Int", false, nullptr};
        functions_["std::time.now_ms"] = FnSig{{}, {}, "Int", false, nullptr};
        functions_["std::string.len"] = FnSig{{}, {"String"}, "Int", false, nullptr};
        functions_["std::string.char_count"] = FnSig{{}, {"String"}, "Int", false, nullptr};
        functions_["std::string.trim"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::string.to_lower"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::string.to_upper"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::string.lower"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::string.upper"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::string.split"] = FnSig{{}, {"String", "String"}, "List<String>", false, nullptr};
        functions_["std::string.join"] = FnSig{{}, {"List<String>", "String"}, "String", false, nullptr};
        functions_["std::string.replace"] = FnSig{{}, {"String", "String", "String"}, "String", false, nullptr};
        functions_["std::string.contains"] = FnSig{{}, {"String", "String"}, "Bool", false, nullptr};
        functions_["std::string.starts_with"] = FnSig{{}, {"String", "String"}, "Bool", false, nullptr};
        functions_["std::string.ends_with"] = FnSig{{}, {"String", "String"}, "Bool", false, nullptr};
        functions_["std::collections.len"] = FnSig{{}, {"Any"}, "Int", false, nullptr};
        functions_["std::collections.push"] = FnSig{{}, {"Any", "Any"}, "Unit", false, nullptr};
        functions_["std::collections.pop"] = FnSig{{}, {"Any"}, "Any", false, nullptr};
        functions_["std::collections.contains"] = FnSig{{}, {"Any", "Any"}, "Bool", false, nullptr};
        functions_["std::collections.reverse"] = FnSig{{}, {"Any"}, "Any", false, nullptr};
        functions_["std::collections.sort"] = FnSig{{}, {"Any"}, "Any", false, nullptr};
        functions_["std::collections.has_key"] = FnSig{{}, {"Any", "Any"}, "Bool", false, nullptr};
        functions_["std::collections.keys"] = FnSig{{}, {"Any"}, "List<String>", false, nullptr};
        functions_["std::collections.values"] = FnSig{{}, {"Any"}, "List<Any>", false, nullptr};
        functions_["std::collections.remove_key"] = FnSig{{}, {"Any", "Any"}, "Bool", false, nullptr};
        functions_["std::collections.remove"] = FnSig{{}, {"Any", "Any"}, "Bool", false, nullptr};
        functions_["std::path.join"] = FnSig{{}, {"String", "String"}, "String", false, nullptr};
        functions_["std::path.basename"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::path.dirname"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::path.extension"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::path.stem"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::path.is_abs"] = FnSig{{}, {"String"}, "Bool", false, nullptr};
        functions_["std::env.get"] = FnSig{{}, {}, "String", true, nullptr};
        functions_["std::env.set"] = FnSig{{}, {"String", "String"}, "Bool", false, nullptr};
        functions_["std::env.has"] = FnSig{{}, {"String"}, "Bool", false, nullptr};
        functions_["std::env.args"] = FnSig{{}, {}, "List<String>", false, nullptr};
        functions_["std::fs.exists"] = FnSig{{}, {"String"}, "Bool", false, nullptr};
        functions_["std::fs.read_text"] = FnSig{{}, {"String"}, "String", false, nullptr};
        functions_["std::fs.read_lines"] = FnSig{{}, {"String"}, "List<String>", false, nullptr};
        functions_["std::fs.write_text"] = FnSig{{}, {"String", "String"}, "Bool", false, nullptr};
        functions_["std::fs.append_text"] = FnSig{{}, {"String", "String"}, "Bool", false, nullptr};
        functions_["std::fs.remove_file"] = FnSig{{}, {"String"}, "Bool", false, nullptr};
        functions_["std::fs.mkdir_p"] = FnSig{{}, {"String"}, "Bool", false, nullptr};
        functions_["std::log.info"] = FnSig{{}, {"Any"}, "Unit", false, nullptr};
        functions_["std::log.warn"] = FnSig{{}, {"Any"}, "Unit", false, nullptr};
        functions_["std::log.error"] = FnSig{{}, {"Any"}, "Unit", false, nullptr};
        functions_["std::json.stringify"] = FnSig{{}, {"Any"}, "String", false, nullptr};
        functions_["std::json.parse"] = FnSig{{}, {"String"}, "Any", false, nullptr};
    }

    static std::string base_type(const std::string& t) {
        size_t lt = t.find('<');
        if (lt != std::string::npos) return t.substr(0, lt);
        return t;
    }

    static std::vector<std::string> split_top_level_type_args(const std::string& s) {
        std::vector<std::string> res;
        int depth = 0;
        size_t start = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '<') ++depth;
            else if (s[i] == '>') --depth;
            else if (s[i] == ',' && depth == 0) {
                std::string part = s.substr(start, i - start);
                while (!part.empty() && part.front() == ' ') part.erase(part.begin());
                while (!part.empty() && part.back() == ' ') part.pop_back();
                res.push_back(part);
                start = i + 1;
            }
        }
        std::string last = s.substr(start);
        while (!last.empty() && last.front() == ' ') last.erase(last.begin());
        while (!last.empty() && last.back() == ' ') last.pop_back();
        if (!last.empty()) res.push_back(last);
        return res;
    }

    bool types_compatible(const std::string& expected, const std::string& actual) const {
        if (expected.empty() || actual.empty() || expected == "Any" || actual == "Any") return true;
        if (active_type_params_.count(expected) || active_type_params_.count(actual)) return true;
        if (expected == actual) return true;
        if (expected == "Fn" && actual.rfind("Fn(", 0) == 0) return true;
        if (actual == "Fn" && expected.rfind("Fn(", 0) == 0) return true;
        if (expected == "List<Any>" && base_type(actual) == "List") return true;
        if (actual == "List<Any>" && base_type(expected) == "List") return true;
        if (expected == "Map<String, Any>" && base_type(actual) == "Map") return true;
        if (actual == "Map<String, Any>" && base_type(expected) == "Map") return true;
        size_t e_lt = expected.find('<');
        size_t a_lt = actual.find('<');
        if (e_lt != std::string::npos && a_lt != std::string::npos &&
            expected.back() == '>' && actual.back() == '>') {
            if (expected.substr(0, e_lt) != actual.substr(0, a_lt)) return false;
            auto e_args = split_top_level_type_args(expected.substr(e_lt + 1, expected.size() - e_lt - 2));
            auto a_args = split_top_level_type_args(actual.substr(a_lt + 1, actual.size() - a_lt - 2));
            if (e_args.size() != a_args.size()) return false;
            for (size_t i = 0; i < e_args.size(); ++i) {
                if (!types_compatible(e_args[i], a_args[i])) return false;
            }
            return true;
        }
        return false;
    }

    static bool unify_type_pattern(
        const std::string& pattern,
        const std::string& concrete,
        const std::unordered_set<std::string>& tparams,
        std::unordered_map<std::string, std::string>& bindings
    ) {
        if (concrete == "Any") return true;
        if (tparams.count(pattern)) {
            auto it = bindings.find(pattern);
            if (it == bindings.end()) {
                bindings[pattern] = concrete;
                return true;
            }
            return it->second == concrete || it->second == "Any";
        }
        size_t p_lt = pattern.find('<');
        size_t c_lt = concrete.find('<');
        if (p_lt != std::string::npos && c_lt != std::string::npos &&
            pattern.back() == '>' && concrete.back() == '>') {
            std::string p_base = pattern.substr(0, p_lt);
            std::string c_base = concrete.substr(0, c_lt);
            if (p_base != c_base) return false;
            auto p_args = split_top_level_type_args(pattern.substr(p_lt + 1, pattern.size() - p_lt - 2));
            auto c_args = split_top_level_type_args(concrete.substr(c_lt + 1, concrete.size() - c_lt - 2));
            if (p_args.size() != c_args.size()) return false;
            for (size_t i = 0; i < p_args.size(); ++i) {
                if (!unify_type_pattern(p_args[i], c_args[i], tparams, bindings)) return false;
            }
            return true;
        }
        return pattern == concrete || pattern == "Any";
    }

    static std::string substitute_type_params(
        const std::string& pattern,
        const std::unordered_map<std::string, std::string>& bindings
    ) {
        auto it = bindings.find(pattern);
        if (it != bindings.end()) return it->second;
        size_t lt = pattern.find('<');
        if (lt != std::string::npos && pattern.back() == '>') {
            std::string base = pattern.substr(0, lt);
            auto args = split_top_level_type_args(pattern.substr(lt + 1, pattern.size() - lt - 2));
            std::string out = base + "<";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) out += ", ";
                out += substitute_type_params(args[i], bindings);
            }
            out += ">";
            return out;
        }
        return pattern;
    }

    void validate_generic_expr_ops(
        const ExprPtr& ex,
        const std::string& fn_name,
        const std::unordered_map<std::string, std::string>& bindings,
        Span call_span
    ) const {
        if (!ex) return;
        validate_generic_expr_ops(ex->lhs, fn_name, bindings, call_span);
        validate_generic_expr_ops(ex->rhs, fn_name, bindings, call_span);
        for (const auto& a : ex->args) validate_generic_expr_ops(a, fn_name, bindings, call_span);
        if (ex->kind == Expr::Kind::Unary && ex->op == "-" && ex->lhs) {
            std::string t = substitute_type_params(ex->lhs->inferred_type, bindings);
            if (t != "Int" && t != "Float" && t != "Any") {
                throw RheaException(make_diag(
                    "E0003",
                    "generic function '" + fn_name + "' cannot apply unary '-' to type " + t,
                    filename_, call_span
                ));
            }
        }
        if (ex->kind == Expr::Kind::Binary && ex->lhs && ex->rhs) {
            const std::string& bop = ex->op;
            std::string lt = substitute_type_params(ex->lhs->inferred_type, bindings);
            std::string rt = substitute_type_params(ex->rhs->inferred_type, bindings);
            if (bop == "+" || bop == "-" || bop == "*" || bop == "/") {
                if (bop == "+" && lt == "String" && rt == "String") return;
                if ((lt != "Int" && lt != "Float" && lt != "Tensor" && lt != "Any") ||
                    (rt != "Int" && rt != "Float" && rt != "Tensor" && rt != "Any")) {
                    throw RheaException(make_diag(
                        "E0003",
                        "generic function '" + fn_name + "' cannot apply operator '" + bop + "' to " + lt + " and " + rt,
                        filename_, call_span,
                        "Pass numeric arguments (Int or Float)" + std::string(bop == "+" ? " or String" : "") + " to '" + fn_name + "'."
                    ));
                }
            } else if (bop == "%") {
                if ((lt != "Int" && lt != "Any") || (rt != "Int" && rt != "Any")) {
                    throw RheaException(make_diag(
                        "E0003",
                        "generic function '" + fn_name + "' cannot apply modulo '%' to non-Int type " + lt,
                        filename_, call_span
                    ));
                }
            } else if (bop == "<" || bop == "<=" || bop == ">" || bop == ">=") {
                if ((lt != "Int" && lt != "Float" && lt != "String" && lt != "Any") ||
                    (rt != "Int" && rt != "Float" && rt != "String" && rt != "Any")) {
                    throw RheaException(make_diag(
                        "E0003",
                        "generic function '" + fn_name + "' cannot apply ordering operator '" + bop + "' to type " + lt,
                        filename_, call_span
                    ));
                }
            }
        }
    }

    void validate_generic_stmt_ops(
        const StmtPtr& st,
        const std::string& fn_name,
        const std::unordered_map<std::string, std::string>& bindings,
        Span call_span
    ) const {
        if (!st) return;
        validate_generic_expr_ops(st->target, fn_name, bindings, call_span);
        validate_generic_expr_ops(st->expr, fn_name, bindings, call_span);
        validate_generic_expr_ops(st->cond, fn_name, bindings, call_span);
        for (const auto& s : st->then_body) validate_generic_stmt_ops(s, fn_name, bindings, call_span);
        for (const auto& s : st->else_body) validate_generic_stmt_ops(s, fn_name, bindings, call_span);
        for (const auto& s : st->body) validate_generic_stmt_ops(s, fn_name, bindings, call_span);
        for (const auto& s : st->catch_body) validate_generic_stmt_ops(s, fn_name, bindings, call_span);
    }

    std::string find_typo_candidate(const std::string& target, bool only_mutable = false) const {
        if (target.size() < 4) return "";
        std::string best;
        size_t best_dist = 99;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            for (const auto& kv : *it) {
                if (only_mutable && !kv.second.is_mutable) continue;
                const std::string& cand = kv.first;
                if (cand.size() < 4) continue;
                if (differs_only_by_digits(target, cand)) continue;
                size_t d = levenshtein_distance(target, cand);
                if (d > 0 && d <= 1 && d < best_dist) {
                    best_dist = d;
                    best = cand;
                }
            }
        }
        return best;
    }

    void check_stmt(StmtPtr& st, const std::string& expected_ret) {
        if (!st) return;
        switch (st->kind) {
            case Stmt::Kind::Let:
            case Stmt::Kind::Const: {
                std::string inferred = check_expr(st->expr);
                if (!st->type_ann.empty() && !types_compatible(st->type_ann, inferred)) {
                    throw RheaException(make_diag(
                        "E0003",
                        "type mismatch in declaration of '" + st->name + "': expected " +
                        st->type_ann + ", found " + inferred,
                        filename_,
                        st->span,
                        "Change the value to match '" + st->type_ann + "' or update the type annotation."
                    ));
                }
                std::string final_t = st->type_ann.empty() ? inferred : st->type_ann;
                if (st->expr && (st->expr->kind == Expr::Kind::MapLit || st->expr->kind == Expr::Kind::ListLit) &&
                    !st->type_ann.empty()) {
                    st->expr->inferred_type = st->type_ann;
                }
                std::string resolved = define_var(st->name, final_t, st->mutable_binding, st->span);
                st->name = resolved;
                break;
            }
            case Stmt::Kind::Assign: {
                std::string rhs_t = check_expr(st->expr);
                if (st->target->kind == Expr::Kind::Ident) {
                    const std::string& vname = st->target->text;
                    VarInfo* info = lookup_var(vname);
                    if (!info) {
                        if (st->assign_op == "=") {
                            std::string typo_mut = find_typo_candidate(vname, true);
                            if (!typo_mut.empty()) {
                                throw RheaException(make_diag(
                                    "E0004",
                                    "assignment to undeclared identifier '" + vname + "' looks like a misspelling of mutable variable '" + typo_mut + "'",
                                    filename_,
                                    st->span,
                                    "Did you mean '" + typo_mut + " = ...'? If you intended to create a new variable named '" + vname + "', use 'let " + vname + " = ...'.",
                                    "To prevent silent bugs from typos when updating mutable state, Rhea flags undeclared assignments that are 1 edit away from an existing 'mut' variable.",
                                    typo_mut + " = new_value    // or: let " + vname + " = new_value"
                                ));
                            }
                            st->is_implicit_let = true;
                            st->mutable_binding = false;
                            std::string resolved = define_var(vname, rhs_t, false, st->span);
                            st->name = resolved;
                            st->target->text = resolved;
                            break;
                        }
                        throw RheaException(make_diag(
                            "E0004",
                            "cannot apply '" + st->assign_op + "' to undefined variable '" + vname + "'",
                            filename_,
                            st->span,
                            "Declare '" + vname + "' with 'mut " + vname + " = ...' before using '" + st->assign_op + "'."
                        ));
                    }
                    if (is_captured_from_outer_scope(vname)) {
                        throw RheaException(make_diag(
                            "E0005",
                            "cannot mutate captured outer variable '" + vname + "' inside a closure",
                            filename_,
                            st->span,
                            "Return the updated value from the closure (e.g. '" + vname + " = closure(" + vname + ")') instead of mutating '" + vname + "' in place.",
                            "Rhea closures capture enclosing variables by immutable lexical value snapshot at creation time to guarantee deterministic behavior across all backends."
                        ));
                    }
                    if (!info->is_mutable) {
                        throw RheaException(make_diag(
                            "E0005",
                            "cannot assign twice to immutable variable '" + vname + "'",
                            filename_,
                            st->span,
                            "Declare '" + vname + "' as mutable using 'mut " + vname + " = ...' or 'let mut " + vname + " = ...'."
                        ));
                    }
                    if (!types_compatible(info->type_name, rhs_t)) {
                        throw RheaException(make_diag(
                            "E0003",
                            "cannot assign value of type " + rhs_t + " to variable '" + vname + "' of type " + info->type_name,
                            filename_,
                            st->span,
                            "Assign a value of type '" + info->type_name + "' or convert it explicitly."
                        ));
                    }
                    st->name = info->resolved_name;
                    st->target->text = info->resolved_name;
                } else if (st->target->kind == Expr::Kind::Index || st->target->kind == Expr::Kind::FieldAccess) {
                    // Generalized single-level and multi-level lvalue chain (`xs[i]`, `matrix[i][j]`, `obj.items[i]`, `obj.inner.f`)
                    ExprPtr root = st->target;
                    while (root && (root->kind == Expr::Kind::Index || root->kind == Expr::Kind::FieldAccess)) {
                        root = root->lhs;
                    }
                    if (!root || root->kind != Expr::Kind::Ident) {
                        throw RheaException(make_diag(
                            "E0002", "invalid assignment target: left-hand side must be a variable, index, or struct field",
                            filename_, st->span
                        ));
                    }
                    const std::string orig_root = root->text;
                    VarInfo* rinfo = lookup_var(orig_root);
                    if (!rinfo) {
                        std::string typo = find_typo_candidate(orig_root, false);
                        throw RheaException(make_diag(
                            "E0004",
                            "undefined variable '" + orig_root + "'" + (typo.empty() ? "" : " (did you mean '" + typo + "'?)"),
                            filename_, root->span,
                            typo.empty() ? ("Declare 'mut " + orig_root + " = ...' before mutating it.") : ("Did you mean '" + typo + "'?")
                        ));
                    }
                    if (is_captured_from_outer_scope(orig_root)) {
                        throw RheaException(make_diag(
                            "E0005",
                            "cannot mutate captured outer variable '" + orig_root + "' inside a closure",
                            filename_, st->span,
                            "Pass '" + orig_root + "' as a parameter or return an updated value from the closure.",
                            "Rhea closures capture enclosing variables by immutable lexical value snapshot at creation time."
                        ));
                    }
                    if (!rinfo->is_mutable) {
                        throw RheaException(make_diag(
                            "E0005",
                            "cannot mutate element or field of immutable binding '" + orig_root + "'",
                            filename_, st->span,
                            "Declare '" + orig_root + "' as mutable using 'mut " + orig_root + " = ...'."
                        ));
                    }
                    std::string target_t = check_expr(st->target);
                    st->name = rinfo->resolved_name;
                    if (!types_compatible(target_t, rhs_t)) {
                        throw RheaException(make_diag(
                            "E0003",
                            "cannot assign value of type " + rhs_t + " to target of type " + target_t,
                            filename_, st->expr->span,
                            "Provide a value of type '" + target_t + "'."
                        ));
                    }
                    if (st->assign_op != "=") {
                        if (target_t != "Int" && target_t != "Float" && target_t != "Any" &&
                            !(st->assign_op == "+=" && target_t == "String")) {
                            throw RheaException(make_diag(
                                "E0003",
                                "compound assignment '" + st->assign_op + "' is not supported for target type " + target_t,
                                filename_, st->span
                            ));
                        }
                    }
                } else {
                    check_expr(st->target);
                }
                break;
            }
            case Stmt::Kind::If: {
                std::string ct = check_expr(st->cond);
                if (!types_compatible("Bool", ct)) {
                    throw RheaException(make_diag(
                        "E0003", "if condition must be Bool, found " + ct, filename_, st->cond->span,
                        "Use a comparison expression such as 'x > 0' or 'x == 10' that evaluates to Bool."
                    ));
                }
                push_scope();
                for (auto& s : st->then_body) check_stmt(s, expected_ret);
                pop_scope();
                if (!st->else_body.empty()) {
                    push_scope();
                    for (auto& s : st->else_body) check_stmt(s, expected_ret);
                    pop_scope();
                }
                break;
            }
            case Stmt::Kind::While: {
                std::string ct = check_expr(st->cond);
                if (!types_compatible("Bool", ct)) {
                    throw RheaException(make_diag(
                        "E0003", "while condition must be Bool, found " + ct, filename_, st->cond->span,
                        "Use a boolean condition such as 'while running {' or 'while count < 10 {'."
                    ));
                }
                ++loop_depth_;
                push_scope();
                for (auto& s : st->body) check_stmt(s, expected_ret);
                pop_scope();
                --loop_depth_;
                break;
            }
            case Stmt::Kind::For:
            case Stmt::Kind::ParallelFor: {
                std::string it = check_expr(st->expr);
                if (base_type(it) != "List" && it != "Any") {
                    throw RheaException(make_diag(
                        "E0003", "for loop iterable must be a List or range, found " + it,
                        filename_, st->expr->span,
                        "Iterate over a range like '1..=10' or a List like '[1, 2, 3]'."
                    ));
                }
                std::string elem_t = "Any";
                if (it.rfind("List<", 0) == 0 && it.back() == '>') {
                    elem_t = it.substr(5, it.size() - 6);
                }
                ++loop_depth_;
                push_scope();
                std::string resolved = define_var(st->name, elem_t, false, st->span);
                st->name = resolved;
                for (auto& s : st->body) check_stmt(s, expected_ret);
                pop_scope();
                --loop_depth_;
                break;
            }
            case Stmt::Kind::Block: {
                push_scope();
                for (auto& s : st->body) check_stmt(s, expected_ret);
                pop_scope();
                break;
            }
            case Stmt::Kind::Return: {
                std::string actual = st->expr ? check_expr(st->expr) : "Unit";
                if (!types_compatible(expected_ret, actual)) {
                    throw RheaException(make_diag(
                        "E0003",
                        "return type mismatch: expected " + expected_ret + ", found " + actual,
                        filename_,
                        st->span,
                        "Return a value of type '" + expected_ret + "' or change the function's return type."
                    ));
                }
                break;
            }
            case Stmt::Kind::Break:
            case Stmt::Kind::Continue: {
                if (loop_depth_ <= 0) {
                    throw RheaException(make_diag(
                        "E0008",
                        std::string(st->kind == Stmt::Kind::Break ? "'break'" : "'continue'") +
                        " cannot be used outside of a loop",
                        filename_,
                        st->span,
                        "Place 'break' and 'continue' inside a 'while' or 'for' loop."
                    ));
                }
                break;
            }
            case Stmt::Kind::TryCatch: {
                push_scope();
                for (auto& s : st->body) check_stmt(s, expected_ret);
                pop_scope();
                push_scope();
                std::string resolved = define_var(st->catch_var, "String", false, st->span);
                st->catch_var = resolved;
                for (auto& s : st->catch_body) check_stmt(s, expected_ret);
                pop_scope();
                break;
            }
            case Stmt::Kind::Expr:
                check_expr(st->expr);
                break;
        }
    }

    std::string check_expr(ExprPtr& e) {
        if (!e) return "Unit";
        switch (e->kind) {
            case Expr::Kind::IntLit:
                return e->inferred_type = "Int";
            case Expr::Kind::FloatLit:
                return e->inferred_type = "Float";
            case Expr::Kind::BoolLit:
                return e->inferred_type = "Bool";
            case Expr::Kind::StrLit:
                return e->inferred_type = "String";
            case Expr::Kind::Ident: {
                VarInfo* v = lookup_var(e->text);
                if (v) {
                    e->text = v->resolved_name;
                    return e->inferred_type = v->type_name;
                }
                if (functions_.count(e->text) || pipelines_.count(e->text)) {
                    return e->inferred_type = "Fn";
                }
                std::string typo = find_typo_candidate(e->text, false);
                std::string hint = typo.empty()
                    ? ("Check the spelling of '" + e->text + "' or assign it before reading it.")
                    : ("Did you mean '" + typo + "'?");
                throw RheaException(make_diag(
                    "E0004",
                    "undefined variable '" + e->text + "'" + (typo.empty() ? "" : " (did you mean '" + typo + "'?)"),
                    filename_,
                    e->span,
                    hint
                ));
            }
            case Expr::Kind::Unary: {
                std::string lt = check_expr(e->lhs);
                if (e->op == "-") {
                    if (lt != "Int" && lt != "Float" && lt != "Any" && !active_type_params_.count(lt)) {
                        throw RheaException(make_diag(
                            "E0003", "unary '-' requires Int or Float, found " + lt,
                            filename_, e->span, "Apply '-' only to numeric values."
                        ));
                    }
                    return e->inferred_type = lt;
                }
                if (e->op == "!") {
                    if (!types_compatible("Bool", lt)) {
                        throw RheaException(make_diag(
                            "E0003", "logical '!' requires Bool, found " + lt,
                            filename_, e->span, "Apply '!' only to boolean expressions."
                        ));
                    }
                    return e->inferred_type = "Bool";
                }
                return e->inferred_type = lt;
            }
            case Expr::Kind::Binary: {
                std::string lt = check_expr(e->lhs);
                std::string rt = check_expr(e->rhs);
                const std::string& op = e->op;
                if (op == ".." || op == "..=") {
                    if (!types_compatible("Int", lt) || !types_compatible("Int", rt)) {
                        throw RheaException(make_diag(
                            "E0003",
                            "range operator '" + op + "' requires Int bounds, found " + lt + " and " + rt,
                            filename_,
                            e->span,
                            "Use integer start and end bounds in ranges."
                        ));
                    }
                    return e->inferred_type = "List<Int>";
                }
                if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
                    if (op == "+" && lt == "String" && rt == "String") {
                        return e->inferred_type = "String";
                    }
                    if (lt == "Tensor" || rt == "Tensor") {
                        return e->inferred_type = "Tensor";
                    }
                    if (active_type_params_.count(lt) && lt == rt) {
                        return e->inferred_type = lt;
                    }
                    if (lt != "Any" && rt != "Any" && lt != rt) {
                        throw RheaException(make_diag(
                            "E0003",
                            "invalid binary operation '" + lt + " " + op + " " + rt + "' (mismatched types)",
                            filename_,
                            e->span,
                            (lt == "String" || rt == "String")
                                ? "Use 'str(value)' to convert numbers to String before concatenating with '+'."
                                : "Convert both operands to the same numeric type ('Int' or 'Float')."
                        ));
                    }
                    if (op == "%") {
                        if ((lt != "Int" && lt != "Any" && !active_type_params_.count(lt)) ||
                            (rt != "Int" && rt != "Any" && !active_type_params_.count(rt))) {
                            throw RheaException(make_diag(
                                "E0003",
                                "modulo operator '%' requires Int operands, found " + lt + " and " + rt,
                                filename_,
                                e->span,
                                "Use 'Int' operands with '%' or convert values with 'int(x)'."
                            ));
                        }
                        return e->inferred_type = "Int";
                    }
                    if (lt != "Int" && lt != "Float" && lt != "Any" && !active_type_params_.count(lt)) {
                        throw RheaException(make_diag(
                            "E0003",
                            "operator '" + op + "' is not supported for type " + lt,
                            filename_,
                            e->span,
                            "Use numeric operands ('Int' or 'Float') with arithmetic operators."
                        ));
                    }
                    return e->inferred_type = (lt == "Any" ? rt : lt);
                }
                if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                    if (lt != "Any" && rt != "Any" && lt != rt) {
                        throw RheaException(make_diag(
                            "E0003", "cannot compare " + lt + " and " + rt + " with '" + op + "'",
                            filename_, e->span, "Compare values of the same type."
                        ));
                    }
                    std::string eff_t = (lt == "Any") ? rt : lt;
                    if (eff_t != "Int" && eff_t != "Float" && eff_t != "String" &&
                        eff_t != "Any" && !active_type_params_.count(eff_t)) {
                        throw RheaException(make_diag(
                            "E0003",
                            "ordering operator '" + op + "' is not supported for type " + eff_t,
                            filename_,
                            e->span,
                            "Ordering comparisons ('<', '<=', '>', '>=') require Int, Float, or String operands."
                        ));
                    }
                    return e->inferred_type = "Bool";
                }
                if (op == "==" || op == "!=") {
                    if (lt != "Any" && rt != "Any" && !types_compatible(lt, rt)) {
                        throw RheaException(make_diag(
                            "E0003", "cannot compare " + lt + " and " + rt + " with '" + op + "'",
                            filename_, e->span, "Compare values of compatible types."
                        ));
                    }
                    return e->inferred_type = "Bool";
                }
                if (op == "&&" || op == "||") {
                    if (!types_compatible("Bool", lt) || !types_compatible("Bool", rt)) {
                        throw RheaException(make_diag(
                            "E0003",
                            "logical operator '" + op + "' requires Bool operands, found " + lt + " and " + rt,
                            filename_, e->span, "Ensure both sides of '" + op + "' evaluate to Bool."
                        ));
                    }
                    return e->inferred_type = "Bool";
                }
                return e->inferred_type = "Any";
            }
            case Expr::Kind::Call: {
                if ((e->text == "push" || e->text == "std::collections.push" ||
                     e->text == "pop" || e->text == "std::collections.pop" ||
                     e->text == "remove_key" || e->text == "std::collections.remove_key" ||
                     e->text == "std::collections.remove") &&
                    !e->args.empty() && e->args[0]->kind == Expr::Kind::Ident) {
                    const std::string& cname = e->args[0]->text;
                    VarInfo* cinfo = lookup_var(cname);
                    if (cinfo && is_captured_from_outer_scope(cname)) {
                        throw RheaException(make_diag(
                            "E0005",
                            "cannot mutate captured outer collection '" + cname + "' via '" + e->text + "' inside a closure",
                            filename_, e->span,
                            "Rhea closures capture variables by immutable value snapshot; return a new collection instead."
                        ));
                    }
                    if (cinfo && !cinfo->is_mutable) {
                        throw RheaException(make_diag(
                            "E0005",
                            "cannot mutate immutable collection '" + cname + "' via '" + e->text + "'",
                            filename_, e->span,
                            "Declare '" + cname + "' as mutable using 'mut " + cname + " = ...'."
                        ));
                    }
                }
                for (auto& a : e->args) check_expr(a);
                if (e->text == "push" || e->text == "std::collections.push") {
                    if (e->args.size() != 2) {
                        throw RheaException(make_diag("E0006", "push() expects 2 arguments (list, value)", filename_, e->span));
                    }
                    std::string lt = e->args[0]->inferred_type;
                    if (base_type(lt) != "List" && lt != "Any") {
                        throw RheaException(make_diag("E0003", "push() first argument must be List<T>, found " + lt, filename_, e->args[0]->span));
                    }
                    if (lt.rfind("List<", 0) == 0 && lt.back() == '>') {
                        std::string elem_t = lt.substr(5, lt.size() - 6);
                        if (!types_compatible(elem_t, e->args[1]->inferred_type)) {
                            throw RheaException(make_diag(
                                "E0003",
                                "cannot push value of type " + e->args[1]->inferred_type + " into " + lt,
                                filename_, e->args[1]->span
                            ));
                        }
                    }
                    return e->inferred_type = "Unit";
                }
                if (e->text == "pop" || e->text == "std::collections.pop") {
                    if (e->args.size() != 1) {
                        throw RheaException(make_diag("E0006", "pop() expects 1 argument (list)", filename_, e->span));
                    }
                    std::string lt = e->args[0]->inferred_type;
                    if (base_type(lt) != "List" && lt != "Any") {
                        throw RheaException(make_diag("E0003", "pop() requires List<T>, found " + lt, filename_, e->args[0]->span));
                    }
                    if (lt.rfind("List<", 0) == 0 && lt.back() == '>') {
                        return e->inferred_type = lt.substr(5, lt.size() - 6);
                    }
                    return e->inferred_type = "Any";
                }

                // Check if calling a private module function
                auto priv_it = private_fns_.find(e->text);
                if (priv_it != private_fns_.end()) {
                    throw RheaException(make_diag(
                        "E0009",
                        "function '" + e->text + "' is private ('priv fn') to module '" + priv_it->second + "'",
                        filename_,
                        e->span,
                        "Mark the function 'pub fn' in '" + priv_it->second + ".rhea' if it should be callable from outside."
                    ));
                }

                // Rewrite `mod.func` call to `mod__dot__func` if resolved from local module
                if (e->text.find('.') != std::string::npos && e->text.rfind("std::", 0) != 0) {
                    size_t dpos = e->text.find('.');
                    std::string mangled = e->text.substr(0, dpos) + "__dot__" + e->text.substr(dpos + 1);
                    if (functions_.count(mangled)) {
                        e->text = mangled;
                    }
                }

                if (e->text == "sleep") {
                    if (e->args.size() != 1) {
                        throw RheaException(make_diag(
                            "E0006", "sleep() expects 1 duration argument",
                            filename_, e->span, "Pass a duration such as 'sleep(1s)' or 'sleep(500ms)'."
                        ));
                    }
                    std::string at = e->args[0]->inferred_type;
                    if (!types_compatible("Int", at)) {
                        throw RheaException(make_diag(
                            "E0003", "sleep() requires a duration or Int milliseconds, found " + at,
                            filename_, e->span, "Use a duration literal like 'sleep(1s)' or 'sleep(500ms)'."
                        ));
                    }
                    return e->inferred_type = "Unit";
                }

                // Check if calling a local closure variable!
                VarInfo* cl_var = lookup_var(e->text);
                if (cl_var) {
                    e->text = cl_var->resolved_name;
                    auto cl_sig_it = closure_sigs_.find(cl_var->resolved_name);
                    if (cl_sig_it != closure_sigs_.end()) {
                        const FnSig& csig = cl_sig_it->second;
                        if (e->args.size() != csig.param_types.size()) {
                            throw RheaException(make_diag(
                                "E0006",
                                "closure '" + e->text + "' expects " + std::to_string(csig.param_types.size()) +
                                " argument(s), found " + std::to_string(e->args.size()),
                                filename_, e->span,
                                "Pass the exact number of arguments declared by the closure."
                            ));
                        }
                        for (size_t i = 0; i < e->args.size(); ++i) {
                            if (!types_compatible(csig.param_types[i], e->args[i]->inferred_type)) {
                                throw RheaException(make_diag(
                                    "E0003",
                                    "closure argument " + std::to_string(i + 1) + " expects " +
                                    csig.param_types[i] + ", found " + e->args[i]->inferred_type,
                                    filename_, e->args[i]->span
                                ));
                            }
                        }
                        return e->inferred_type = csig.return_type;
                    }
                    return e->inferred_type = "Any";
                }

                auto it = functions_.find(e->text);
                if (it != functions_.end()) {
                    const FnSig& sig = it->second;
                    if (!sig.variadic && e->args.size() != sig.param_types.size()) {
                        throw RheaException(make_diag(
                            "E0006",
                            "function '" + e->text + "' expects " + std::to_string(sig.param_types.size()) +
                            " argument(s), found " + std::to_string(e->args.size()),
                            filename_, e->span,
                            "Pass the exact number of arguments declared by '" + e->text + "'."
                        ));
                    }
                    if (!sig.type_params.empty()) {
                        // Generic function call unification!
                        std::unordered_set<std::string> tset(sig.type_params.begin(), sig.type_params.end());
                        std::unordered_map<std::string, std::string> bindings;
                        for (size_t i = 0; i < e->args.size(); ++i) {
                            std::string actual_t = e->args[i]->inferred_type;
                            if (!unify_type_pattern(sig.param_types[i], actual_t, tset, bindings)) {
                                throw RheaException(make_diag(
                                    "E0003",
                                    "generic type mismatch in argument " + std::to_string(i + 1) + " of '" +
                                    e->text + "': expected pattern '" + sig.param_types[i] +
                                    "' (with current bindings), found '" + actual_t + "'",
                                    filename_, e->args[i]->span,
                                    "Ensure arguments mapped to the same generic type parameter have identical types."
                                ));
                            }
                        }
                        // Validate generic body operations against concrete bound types
                        if (sig.decl_ptr) {
                            for (const auto& bst : sig.decl_ptr->body) {
                                validate_generic_stmt_ops(bst, e->text, bindings, e->span);
                            }
                        }
                        return e->inferred_type = substitute_type_params(sig.return_type, bindings);
                    }
                    if (!sig.variadic) {
                        for (size_t i = 0; i < e->args.size(); ++i) {
                            std::string at = e->args[i]->inferred_type;
                            if (!types_compatible(sig.param_types[i], at)) {
                                throw RheaException(make_diag(
                                    "E0003",
                                    "argument " + std::to_string(i + 1) + " of '" + e->text + "' expects " +
                                    sig.param_types[i] + ", found " + at,
                                    filename_, e->args[i]->span,
                                    "Pass a value of type '" + sig.param_types[i] + "'."
                                ));
                            }
                        }
                    }
                    return e->inferred_type = sig.return_type;
                }
                if (e->text.rfind("std::", 0) == 0) {
                    if (e->text == "std::io.read_line" || e->text == "std::io.input") return e->inferred_type = "String";
                    if (e->text == "std::io.is_eof") return e->inferred_type = "Bool";
                    if (e->text == "std::time.monotonic_ms" || e->text == "std::time.now_ms") return e->inferred_type = "Int";
                    if (e->text == "std::time.sleep_ms") return e->inferred_type = "Unit";
                    return e->inferred_type = "Any";
                }
                if (pipelines_.count(e->text)) {
                    return e->inferred_type = "Any";
                }
                std::vector<std::string> fn_cands;
                for (const auto& kv : functions_) fn_cands.push_back(kv.first);
                std::string fn_sug = find_closest_match(e->text, fn_cands, 2);
                std::string fn_msg = "undefined function '" + e->text + "'" +
                                     (fn_sug.empty() ? "" : " (did you mean '" + fn_sug + "'?)");
                std::string fn_help = fn_sug.empty()
                    ? ("Define 'fn " + e->text + "(...)' or import the module that provides it.")
                    : ("Did you mean '" + fn_sug + "(...)'?");
                throw RheaException(make_diag("E0004", fn_msg, filename_, e->span, fn_help));
            }
            case Expr::Kind::ListLit: {
                std::string elem = "Any";
                for (size_t i = 0; i < e->args.size(); ++i) {
                    std::string t = check_expr(e->args[i]);
                    if (elem == "Any") {
                        elem = t;
                    } else if (t != "Any" && elem != t) {
                        throw RheaException(make_diag(
                            "E0003",
                            "heterogeneous list literal: element 0 has type " + elem +
                            ", but element " + std::to_string(i) + " has type " + t,
                            filename_, e->args[i]->span,
                            "All elements in a Rhea List<T> must share the same type."
                        ));
                    }
                }
                return e->inferred_type = "List<" + elem + ">";
            }
            case Expr::Kind::MapLit: {
                std::string key_t = "Any";
                std::string val_t = "Any";
                for (size_t i = 0; i < e->map_entries.size(); ++i) {
                    std::string kt = check_expr(e->map_entries[i].first);
                    std::string vt = check_expr(e->map_entries[i].second);
                    if (kt != "String" && kt != "Int" && kt != "Any") {
                        throw RheaException(make_diag(
                            "E0003", "map keys must be String or Int, found " + kt,
                            filename_, e->map_entries[i].first->span
                        ));
                    }
                    if (key_t == "Any") key_t = kt;
                    else if (kt != "Any" && key_t != kt) {
                        throw RheaException(make_diag(
                            "E0003", "heterogeneous map literal keys: expected " + key_t + ", found " + kt,
                            filename_, e->map_entries[i].first->span
                        ));
                    }
                    if (val_t == "Any") val_t = vt;
                    else if (vt != "Any" && val_t != vt) {
                        throw RheaException(make_diag(
                            "E0003", "heterogeneous map literal values: entry 0 has type " + val_t +
                            ", but entry " + std::to_string(i) + " has type " + vt,
                            filename_, e->map_entries[i].second->span,
                            "Ensure all values in a Map<K, V> literal have the same type."
                        ));
                    }
                }
                if (key_t == "Any") key_t = "String";
                if (val_t == "Any") val_t = "Int";
                return e->inferred_type = "Map<" + key_t + ", " + val_t + ">";
            }
            case Expr::Kind::Index: {
                std::string lt = check_expr(e->lhs);
                std::string rt = check_expr(e->rhs);
                if (base_type(lt) == "List") {
                    if (!types_compatible("Int", rt)) {
                        throw RheaException(make_diag(
                            "E0003", "list index must be Int, found " + rt,
                            filename_, e->rhs->span, "Use an integer index between 0 and len(list) - 1."
                        ));
                    }
                    if (lt.rfind("List<", 0) == 0 && lt.back() == '>') {
                        return e->inferred_type = lt.substr(5, lt.size() - 6);
                    }
                } else if (base_type(lt) == "Map") {
                    if ((lt == "Map<String, Any>" || lt == "Map<String, FloatAny>") &&
                        e->rhs->kind == Expr::Kind::StrLit) {
                        if (e->rhs->text == "ok") return e->inferred_type = "Bool";
                        if (e->rhs->text == "value") return e->inferred_type = (lt == "Map<String, FloatAny>" ? "Float" : "Int");
                        if (e->rhs->text == "error") return e->inferred_type = "String";
                    }
                    if (lt.rfind("Map<", 0) == 0 && lt.back() == '>') {
                        std::string inner = lt.substr(4, lt.size() - 5);
                        size_t comma = inner.find(',');
                        if (comma != std::string::npos) {
                            std::string kt = inner.substr(0, comma);
                            std::string vt = inner.substr(comma + 1);
                            while (!vt.empty() && vt.front() == ' ') vt.erase(vt.begin());
                            if (!types_compatible(kt, rt)) {
                                throw RheaException(make_diag(
                                    "E0003", "map index key type mismatch: expected " + kt + ", found " + rt,
                                    filename_, e->rhs->span
                                ));
                            }
                            return e->inferred_type = vt;
                        }
                    }
                }
                return e->inferred_type = "Any";
            }
            case Expr::Kind::FieldAccess: {
                std::string lt = check_expr(e->lhs);
                std::string sbase = base_type(lt);
                auto sit = structs_.find(sbase);
                if (sit != structs_.end()) {
                    auto fit = sit->second.find(e->text);
                    if (fit == sit->second.end()) {
                        std::vector<std::string> fcands;
                        for (const auto& fk : sit->second) fcands.push_back(fk.first);
                        std::string fsug = find_closest_match(e->text, fcands, 2);
                        throw RheaException(make_diag(
                            "E0004",
                            "struct '" + sbase + "' has no field named '" + e->text + "'" +
                            (fsug.empty() ? "" : " (did you mean '" + fsug + "'?)"),
                            filename_, e->span,
                            fsug.empty() ? ("Check the field names declared in 'struct " + sbase + "'.")
                                         : ("Replace '." + e->text + "' with '." + fsug + "'.")
                        ));
                    }
                    // If generic struct `Box<Int>` or `Pair<Int, String>`, substitute all type parameters
                    auto sdecl_it = struct_decls_.find(sbase);
                    if (sdecl_it != struct_decls_.end() && !sdecl_it->second.type_params.empty()) {
                        size_t lpos = lt.find('<');
                        if (lpos != std::string::npos && lt.back() == '>') {
                            auto arg_types = split_top_level_type_args(lt.substr(lpos + 1, lt.size() - lpos - 2));
                            std::unordered_map<std::string, std::string> bmap;
                            for (size_t ti = 0; ti < sdecl_it->second.type_params.size() && ti < arg_types.size(); ++ti) {
                                bmap[sdecl_it->second.type_params[ti]] = arg_types[ti];
                            }
                            return e->inferred_type = substitute_type_params(fit->second, bmap);
                        }
                    }
                    return e->inferred_type = fit->second;
                }
                return e->inferred_type = "Any";
            }
            case Expr::Kind::StructInit: {
                auto sit = structs_.find(e->text);
                if (sit == structs_.end()) {
                    std::vector<std::string> scands;
                    for (const auto& sk : structs_) scands.push_back(sk.first);
                    std::string ssug = find_closest_match(e->text, scands, 2);
                    throw RheaException(make_diag(
                        "E0004",
                        "unknown struct '" + e->text + "'" + (ssug.empty() ? "" : " (did you mean '" + ssug + "'?)"),
                        filename_, e->span,
                        ssug.empty() ? ("Declare 'struct " + e->text + " { ... }' before initializing it.")
                                     : ("Did you mean '" + ssug + " { ... }'?")
                    ));
                }
                const StructDecl& sdecl = struct_decls_[e->text];
                std::unordered_set<std::string> tset(sdecl.type_params.begin(), sdecl.type_params.end());
                std::unordered_map<std::string, std::string> bindings;
                std::unordered_set<std::string> provided_fields;
                for (auto& f : e->struct_fields) {
                    std::string ft = check_expr(f.second);
                    auto fit = sit->second.find(f.first);
                    if (fit == sit->second.end()) {
                        throw RheaException(make_diag(
                            "E0004",
                            "struct '" + e->text + "' has no field named '" + f.first + "'",
                            filename_, f.second->span
                        ));
                    }
                    if (!provided_fields.insert(f.first).second) {
                        throw RheaException(make_diag(
                            "E0002",
                            "duplicate field '" + f.first + "' in initializer for struct '" + e->text + "'",
                            filename_, f.second->span
                        ));
                    }
                    if (!tset.empty()) {
                        if (!unify_type_pattern(fit->second, ft, tset, bindings)) {
                            throw RheaException(make_diag(
                                "E0003", "type mismatch in generic struct field '" + f.first + "'",
                                filename_, f.second->span
                            ));
                        }
                    } else if (!types_compatible(fit->second, ft)) {
                        throw RheaException(make_diag(
                            "E0003",
                            "field '" + f.first + "' of struct '" + e->text + "' expects " +
                            fit->second + ", found " + ft,
                            filename_, f.second->span
                        ));
                    }
                }
                if (provided_fields.size() != sit->second.size()) {
                    throw RheaException(make_diag(
                        "E0003",
                        "missing field(s) in initializer for struct '" + e->text + "'",
                        filename_, e->span
                    ));
                }
                if (!sdecl.type_params.empty()) {
                    std::string full_t = e->text + "<";
                    for (size_t ti = 0; ti < sdecl.type_params.size(); ++ti) {
                        if (ti > 0) full_t += ", ";
                        auto bit = bindings.find(sdecl.type_params[ti]);
                        full_t += (bit != bindings.end()) ? bit->second : "Any";
                    }
                    full_t += ">";
                    return e->inferred_type = full_t;
                }
                return e->inferred_type = e->text;
            }
            case Expr::Kind::EnumVariant: {
                for (auto& a : e->args) check_expr(a);
                auto eit = enums_.find(e->enum_name);
                if (eit == enums_.end()) {
                    throw RheaException(make_diag(
                        "E0004", "unknown enum '" + e->enum_name + "'",
                        filename_, e->span,
                        "Declare 'enum " + e->enum_name + " { ... }' before constructing its variants."
                    ));
                }
                auto vit = eit->second.find(e->variant_name);
                if (vit == eit->second.end()) {
                    throw RheaException(make_diag(
                        "E0004", "enum '" + e->enum_name + "' has no variant named '" + e->variant_name + "'",
                        filename_, e->span
                    ));
                }
                if (e->args.size() != vit->second.size()) {
                    throw RheaException(make_diag(
                        "E0006",
                        "enum variant '" + e->enum_name + "::" + e->variant_name + "' expects " +
                        std::to_string(vit->second.size()) + " payload argument(s), found " +
                        std::to_string(e->args.size()),
                        filename_, e->span
                    ));
                }
                for (size_t i = 0; i < e->args.size(); ++i) {
                    if (!types_compatible(vit->second[i], e->args[i]->inferred_type)) {
                        throw RheaException(make_diag(
                            "E0003",
                            "payload argument " + std::to_string(i + 1) + " of '" + e->enum_name + "::" +
                            e->variant_name + "' expects " + vit->second[i] + ", found " + e->args[i]->inferred_type,
                            filename_, e->args[i]->span
                        ));
                    }
                }
                return e->inferred_type = e->enum_name;
            }
            case Expr::Kind::Lambda: {
                e->lambda_id = ++lambda_counter_;
                closure_floor_stack_.push_back(scopes_.size());
                push_scope();
                FnSig csig;
                std::unordered_set<std::string> seen_lparams;
                for (auto& p : e->lambda_params) {
                    if (!seen_lparams.insert(p.name).second) {
                        pop_scope();
                        closure_floor_stack_.pop_back();
                        throw RheaException(make_diag(
                            "E0002", "duplicate parameter '" + p.name + "' in closure", filename_, p.span
                        ));
                    }
                    std::string resolved = define_var(p.name, p.type_name, false, p.span);
                    p.name = resolved;
                    csig.param_types.push_back(p.type_name);
                }
                std::string ret_t = e->lambda_ret_type;
                try {
                    if (e->lhs) {
                        std::string body_t = check_expr(e->lhs);
                        if (ret_t == "Any") ret_t = body_t;
                    } else {
                        for (auto& st : e->lambda_body_stmts) {
                            check_stmt(st, ret_t);
                            if (st->kind == Stmt::Kind::Return && st->expr && ret_t == "Any") {
                                ret_t = st->expr->inferred_type;
                            }
                        }
                    }
                } catch (...) {
                    pop_scope();
                    closure_floor_stack_.pop_back();
                    throw;
                }
                pop_scope();
                closure_floor_stack_.pop_back();
                csig.return_type = ret_t;
                last_lambda_sig_ = csig;
                return e->inferred_type = "Fn";
            }
            case Expr::Kind::Match: {
                std::string target_t = check_expr(e->lhs);
                std::string arm_t = "Any";
                for (auto& arm : e->match_arms) {
                    push_scope();
                    if (!arm.binding_name.empty()) {
                        std::string bind_t = arm.is_binding ? target_t : "Any";
                        if (arm.is_enum_variant) {
                            auto eit = enums_.find(arm.pattern_enum);
                            if (eit != enums_.end()) {
                                auto vit = eit->second.find(arm.pattern_variant);
                                if (vit != eit->second.end() && !vit->second.empty()) {
                                    bind_t = vit->second[0];
                                }
                            }
                        }
                        std::string resolved = define_var(arm.binding_name, bind_t, false, arm.span);
                        arm.binding_name = resolved;
                    }
                    if (arm.body_expr) {
                        std::string t = check_expr(arm.body_expr);
                        if (arm_t == "Any") arm_t = t;
                    } else {
                        for (auto& s : arm.body_stmts) check_stmt(s, "Any");
                    }
                    pop_scope();
                }
                return e->inferred_type = arm_t;
            }
        }
        return "Any";
    }

    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }

    std::string define_var(const std::string& raw_name, const std::string& type_name, bool is_mut, Span sp) {
        size_t count = ++var_name_counts_[raw_name];
        std::string resolved = (count == 1) ? raw_name : (raw_name + "__s" + std::to_string(count));
        scopes_.back()[raw_name] = VarInfo{type_name, resolved, is_mut, sp};
        if (type_name == "Fn" && last_lambda_sig_.has_value()) {
            closure_sigs_[resolved] = *last_lambda_sig_;
            last_lambda_sig_.reset();
        }
        return resolved;
    }

    VarInfo* lookup_var(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    bool is_captured_from_outer_scope(const std::string& name) const {
        if (closure_floor_stack_.empty()) return false;
        size_t floor_idx = closure_floor_stack_.back();
        for (size_t i = scopes_.size(); i > 0; --i) {
            size_t idx = i - 1;
            if (scopes_[idx].count(name)) {
                if (idx == 0) return false;
                return idx < floor_idx;
            }
        }
        return false;
    }

    std::string filename_;
    std::vector<size_t> closure_floor_stack_;
    std::unordered_map<std::string, std::string> private_fns_;
    std::vector<std::unordered_map<std::string, VarInfo>> scopes_;
    std::unordered_map<std::string, size_t> var_name_counts_;
    std::unordered_map<std::string, FnSig> functions_;
    std::unordered_map<std::string, FnSig> closure_sigs_;
    std::optional<FnSig> last_lambda_sig_;
    std::unordered_set<std::string> active_type_params_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> structs_;
    std::unordered_map<std::string, StructDecl> struct_decls_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> enums_;
    std::unordered_map<std::string, std::vector<std::string>> pipelines_;
    int loop_depth_ = 0;
    size_t lambda_counter_ = 0;
};

// ============================================================================
// 7. Runtime Values, Overflow-Safe Ranges, Interactive I/O & Monotonic Timing
// ============================================================================

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct TensorValue {
    std::vector<size_t> shape;
    std::vector<double> data;
};

struct ClosureValue {
    std::vector<Param> params;
    ExprPtr body_expr;
    std::vector<StmtPtr> body_stmts;
    std::string vm_fn_name;
    std::unordered_map<std::string, Value> captured;
};

struct Value {
    enum class Kind {
        Unit,
        Int,
        Float,
        Bool,
        Str,
        List,
        Map,
        Struct,
        Enum,
        Tensor,
        Closure
    };

    Kind kind = Kind::Unit;
    int64_t int_val = 0;
    double float_val = 0.0;
    bool bool_val = false;
    std::string str_val;
    std::vector<Value> list_val;
    std::map<std::string, Value> map_val;
    std::string type_tag;
    std::string variant_tag;
    std::shared_ptr<TensorValue> tensor_val;
    std::shared_ptr<ClosureValue> closure_val;

    static Value make_unit() { return Value{}; }
    static Value make_int(int64_t v) { Value x; x.kind = Kind::Int; x.int_val = v; return x; }
    static Value make_float(double v) { Value x; x.kind = Kind::Float; x.float_val = v; return x; }
    static Value make_bool(bool v) { Value x; x.kind = Kind::Bool; x.bool_val = v; return x; }
    static Value make_str(std::string v) { Value x; x.kind = Kind::Str; x.str_val = std::move(v); return x; }
    static Value make_list(std::vector<Value> v) { Value x; x.kind = Kind::List; x.list_val = std::move(v); return x; }
    static Value make_map(std::map<std::string, Value> v) { Value x; x.kind = Kind::Map; x.map_val = std::move(v); return x; }
    static Value make_tensor(std::vector<size_t> shape, std::vector<double> data) {
        Value x;
        x.kind = Kind::Tensor;
        x.tensor_val = std::make_shared<TensorValue>(TensorValue{std::move(shape), std::move(data)});
        return x;
    }

    std::string to_string() const {
        switch (kind) {
            case Kind::Unit: return "()";
            case Kind::Int: return std::to_string(int_val);
            case Kind::Float: {
                std::ostringstream oss;
                oss << std::setprecision(12) << float_val;
                std::string s = oss.str();
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
                return s;
            }
            case Kind::Bool: return bool_val ? "true" : "false";
            case Kind::Str: return str_val;
            case Kind::List: {
                std::ostringstream oss;
                oss << "[";
                for (size_t i = 0; i < list_val.size(); ++i) {
                    if (i > 0) oss << ", ";
                    oss << list_val[i].to_string();
                }
                oss << "]";
                return oss.str();
            }
            case Kind::Map: {
                std::ostringstream oss;
                oss << "{";
                size_t i = 0;
                for (const auto& kv : map_val) {
                    if (i++ > 0) oss << ", ";
                    oss << kv.first << ": " << kv.second.to_string();
                }
                oss << "}";
                return oss.str();
            }
            case Kind::Struct: {
                std::ostringstream oss;
                oss << type_tag << " {";
                size_t i = 0;
                for (const auto& kv : map_val) {
                    if (i++ > 0) oss << ", ";
                    oss << kv.first << ": " << kv.second.to_string();
                }
                oss << "}";
                return oss.str();
            }
            case Kind::Enum: {
                std::ostringstream oss;
                oss << type_tag << "::" << variant_tag;
                if (!list_val.empty()) {
                    oss << "(";
                    for (size_t i = 0; i < list_val.size(); ++i) {
                        if (i > 0) oss << ", ";
                        oss << list_val[i].to_string();
                    }
                    oss << ")";
                }
                return oss.str();
            }
            case Kind::Tensor: {
                if (!tensor_val) return "Tensor([])";
                std::ostringstream oss;
                oss << "Tensor(shape=[";
                for (size_t i = 0; i < tensor_val->shape.size(); ++i) {
                    if (i > 0) oss << "x";
                    oss << tensor_val->shape[i];
                }
                oss << "], data=[";
                for (size_t i = 0; i < tensor_val->data.size(); ++i) {
                    if (i > 0) oss << ", ";
                    oss << std::setprecision(6) << tensor_val->data[i];
                }
                oss << "])";
                return oss.str();
            }
            case Kind::Closure:
                return "<closure>";
        }
        return "()";
    }
};

static bool values_equal(const Value& a, const Value& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Value::Kind::Unit: return true;
        case Value::Kind::Int: return a.int_val == b.int_val;
        case Value::Kind::Float: return std::fabs(a.float_val - b.float_val) < 1e-12;
        case Value::Kind::Bool: return a.bool_val == b.bool_val;
        case Value::Kind::Str: return a.str_val == b.str_val;
        case Value::Kind::List:
            if (a.list_val.size() != b.list_val.size()) return false;
            for (size_t i = 0; i < a.list_val.size(); ++i) {
                if (!values_equal(a.list_val[i], b.list_val[i])) return false;
            }
            return true;
        case Value::Kind::Map:
        case Value::Kind::Struct:
            if (a.map_val.size() != b.map_val.size()) return false;
            for (const auto& kv : a.map_val) {
                auto bit = b.map_val.find(kv.first);
                if (bit == b.map_val.end() || !values_equal(kv.second, bit->second)) return false;
            }
            return true;
        case Value::Kind::Enum:
            return a.type_tag == b.type_tag && a.variant_tag == b.variant_tag &&
                   a.list_val.size() == b.list_val.size() &&
                   (a.list_val.empty() || values_equal(a.list_val[0], b.list_val[0]));
        default:
            return a.to_string() == b.to_string();
    }
}

static int64_t checked_add_i64(int64_t a, int64_t b, const std::string& file, Span sp) {
    int64_t res = 0;
    if (__builtin_add_overflow(a, b, &res)) {
        throw RheaException(make_diag(
            "E1002", "integer overflow in addition", file, sp,
            "Use Float for values exceeding 64-bit signed integer range."
        ));
    }
    return res;
}

static int64_t checked_sub_i64(int64_t a, int64_t b, const std::string& file, Span sp) {
    int64_t res = 0;
    if (__builtin_sub_overflow(a, b, &res)) {
        throw RheaException(make_diag(
            "E1002", "integer overflow in subtraction", file, sp,
            "Use Float for values exceeding 64-bit signed integer range."
        ));
    }
    return res;
}

static int64_t checked_mul_i64(int64_t a, int64_t b, const std::string& file, Span sp) {
    int64_t res = 0;
    if (__builtin_mul_overflow(a, b, &res)) {
        throw RheaException(make_diag(
            "E1002", "integer overflow in multiplication", file, sp,
            "Use Float for values exceeding 64-bit signed integer range."
        ));
    }
    return res;
}

static int64_t checked_div_i64(int64_t a, int64_t b, const std::string& file, Span sp) {
    if (b == 0) {
        throw RheaException(make_diag(
            "E1001", "division by zero", file, sp,
            "Check that the divisor is non-zero before dividing."
        ));
    }
    if (a == std::numeric_limits<int64_t>::min() && b == -1) {
        throw RheaException(make_diag(
            "E1002", "integer overflow in INT64_MIN / -1", file, sp,
            "Avoid dividing INT64_MIN by -1."
        ));
    }
    return a / b;
}

static int64_t checked_mod_i64(int64_t a, int64_t b, const std::string& file, Span sp) {
    if (b == 0) {
        throw RheaException(make_diag(
            "E1001", "modulo by zero", file, sp,
            "Check that the modulus is non-zero before applying '%'."
        ));
    }
    if (a == std::numeric_limits<int64_t>::min() && b == -1) return 0;
    return a % b;
}

// Priority F: Overflow-safe range construction (`INT64_MAX..=INT64_MAX` safe!)
static Value build_range_value(int64_t start, int64_t end, bool inclusive, const std::string& file, Span sp) {
    if (!inclusive && end <= start) return Value::make_list({});
    if (inclusive && end < start) return Value::make_list({});
    int64_t diff = 0;
    if (__builtin_sub_overflow(end, start, &diff) || diff < 0 || diff > MAX_RANGE_ELEMENTS) {
        throw RheaException(make_diag(
            "E1004", "range size exceeds maximum allocation limit (10,000,000 elements)",
            file, sp, "Use a smaller range or a while loop for large iteration counts."
        ));
    }
    int64_t count = inclusive ? (diff + 1) : diff;
    if (count > MAX_RANGE_ELEMENTS) {
        throw RheaException(make_diag(
            "E1004", "range size exceeds maximum allocation limit (10,000,000 elements)",
            file, sp
        ));
    }
    std::vector<Value> items;
    items.reserve(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
        items.push_back(Value::make_int(start + i));
    }
    return Value::make_list(std::move(items));
}

static std::string str_trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::string str_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string str_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static bool safe_parse_i64(const std::string& raw, int64_t* out_val, std::string* out_err) {
    std::string s = str_trim(raw);
    if (s.empty()) {
        if (out_err) *out_err = "empty input";
        return false;
    }
    size_t pos = 0;
    try {
        long long v = std::stoll(s, &pos, 10);
        if (pos != s.size()) {
            if (out_err) *out_err = "input contains non-digit characters";
            return false;
        }
        *out_val = static_cast<int64_t>(v);
        return true;
    } catch (const std::out_of_range&) {
        if (out_err) *out_err = "integer out of 64-bit range";
        return false;
    } catch (...) {
        if (out_err) *out_err = "not a valid integer";
        return false;
    }
}

static bool safe_parse_f64(const std::string& raw, double* out_val, std::string* out_err) {
    std::string s = str_trim(raw);
    if (s.empty()) {
        if (out_err) *out_err = "empty input";
        return false;
    }
    size_t pos = 0;
    try {
        double v = std::stod(s, &pos);
        if (pos != s.size() || !std::isfinite(v)) {
            if (out_err) *out_err = "not a finite floating-point number";
            return false;
        }
        *out_val = v;
        return true;
    } catch (...) {
        if (out_err) *out_err = "not a valid floating-point number";
        return false;
    }
}

static bool g_stdin_eof = false;
static std::vector<std::string> g_cli_args;

static std::string runtime_read_line(const std::string& prompt = "") {
    if (!prompt.empty()) {
        std::cout << prompt << std::flush;
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        g_stdin_eof = true;
        return "";
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (std::cin.eof()) {
        g_stdin_eof = true;
    }
    return line;
}

static bool runtime_is_eof() {
    if (g_stdin_eof || std::cin.eof()) return true;
    int ch = std::cin.peek();
    if (ch == EOF) {
        g_stdin_eof = true;
        return true;
    }
    return false;
}

static void runtime_sleep_ms(int64_t ms, const std::string& file, Span sp) {
    if (ms < 0 || ms > MAX_SLEEP_MS) {
        throw RheaException(make_diag(
            "E1008",
            "invalid sleep duration: " + std::to_string(ms) + "ms (must be between 0ms and 3600000ms)",
            file,
            sp,
            "Pass a non-negative duration such as 'sleep(1s)' or 'sleep(500ms)'."
        ));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static int64_t runtime_monotonic_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}


// UTF-8 code point counter
static int64_t utf8_codepoint_count(const std::string& s) {
    int64_t count = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

static std::string str_replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static std::string json_escape_str(const std::string& s) {
    std::ostringstream o;
    o << "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default: o << static_cast<char>(c); break;
        }
    }
    o << "\"";
    return o.str();
}

static std::string value_to_json(const Value& v) {
    switch (v.kind) {
        case Value::Kind::Unit: return "null";
        case Value::Kind::Int: return std::to_string(v.int_val);
        case Value::Kind::Float: return v.to_string();
        case Value::Kind::Bool: return v.bool_val ? "true" : "false";
        case Value::Kind::Str: return json_escape_str(v.str_val);
        case Value::Kind::List: {
            std::ostringstream o;
            o << "[";
            for (size_t i = 0; i < v.list_val.size(); ++i) {
                if (i > 0) o << ",";
                o << value_to_json(v.list_val[i]);
            }
            o << "]";
            return o.str();
        }
        case Value::Kind::Map:
        case Value::Kind::Struct: {
            std::ostringstream o;
            o << "{";
            size_t i = 0;
            for (const auto& kv : v.map_val) {
                if (i++ > 0) o << ",";
                o << json_escape_str(kv.first) << ":" << value_to_json(kv.second);
            }
            o << "}";
            return o.str();
        }
        default:
            return json_escape_str(v.to_string());
    }
}

static std::vector<Value> split_string_values(const std::string& s, const std::string& delim) {
    std::vector<Value> parts;
    if (delim.empty()) {
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            size_t clen = 1;
            if ((c & 0x80) == 0) clen = 1;
            else if ((c & 0xE0) == 0xC0) clen = 2;
            else if ((c & 0xF0) == 0xE0) clen = 3;
            else if ((c & 0xF8) == 0xF0) clen = 4;
            clen = std::min(clen, s.size() - i);
            parts.push_back(Value::make_str(s.substr(i, clen)));
            i += clen;
        }
        return parts;
    }
    size_t start = 0;
    while (true) {
        size_t pos = s.find(delim, start);
        if (pos == std::string::npos) {
            parts.push_back(Value::make_str(s.substr(start)));
            break;
        }
        parts.push_back(Value::make_str(s.substr(start, pos - start)));
        start = pos + delim.size();
    }
    return parts;
}

class SimpleJsonParser {
public:
    SimpleJsonParser(std::string src, std::string file, Span sp)
        : src_(std::move(src)), file_(std::move(file)), sp_(sp) {}

    Value parse() {
        skip_ws();
        Value v = parse_val();
        skip_ws();
        if (pos_ != src_.size()) {
            throw RheaException(make_diag("E1006", "unexpected trailing characters in JSON", file_, sp_));
        }
        return v;
    }

private:
    struct DepthGuard {
        size_t& d;
        explicit DepthGuard(size_t& depth, const std::string& file, Span sp) : d(depth) {
            if (++d > MAX_PARSE_DEPTH) {
                throw RheaException(make_diag(
                    "E1006",
                    "JSON nesting depth exceeds maximum allowed limit (" + std::to_string(MAX_PARSE_DEPTH) + ")",
                    file, sp, "Reduce JSON object/array nesting depth."
                ));
            }
        }
        ~DepthGuard() { --d; }
    };

    void skip_ws() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }

    Value parse_val() {
        DepthGuard guard(depth_, file_, sp_);
        skip_ws();
        if (pos_ >= src_.size()) {
            throw RheaException(make_diag("E1006", "unexpected end of JSON input", file_, sp_));
        }
        char c = src_[pos_];
        if (c == '"') return Value::make_str(parse_str());
        if (c == '{') return parse_obj();
        if (c == '[') return parse_arr();
        if (src_.compare(pos_, 4, "true") == 0) { pos_ += 4; return Value::make_bool(true); }
        if (src_.compare(pos_, 5, "false") == 0) { pos_ += 5; return Value::make_bool(false); }
        if (src_.compare(pos_, 4, "null") == 0) { pos_ += 4; return Value::make_unit(); }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_num();
        throw RheaException(make_diag("E1006", std::string("invalid JSON token starting with '") + c + "'", file_, sp_));
    }

    std::string parse_str() {
        ++pos_; // skip '"'
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= src_.size()) break;
                char esc = src_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        throw RheaException(make_diag("E1006", "unterminated JSON string", file_, sp_));
    }

    Value parse_num() {
        size_t start = pos_;
        bool is_flt = false;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            is_flt = true;
            ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        std::string s = src_.substr(start, pos_ - start);
        if (is_flt) return Value::make_float(std::stod(s));
        return Value::make_int(static_cast<int64_t>(std::stoll(s)));
    }

    Value parse_arr() {
        ++pos_; // '['
        std::vector<Value> items;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') { ++pos_; return Value::make_list(std::move(items)); }
        while (pos_ < src_.size()) {
            items.push_back(parse_val());
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < src_.size() && src_[pos_] == ']') { ++pos_; return Value::make_list(std::move(items)); }
            break;
        }
        throw RheaException(make_diag("E1006", "malformed JSON array", file_, sp_));
    }

    Value parse_obj() {
        ++pos_; // '{'
        std::map<std::string, Value> m;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') { ++pos_; return Value::make_map(std::move(m)); }
        while (pos_ < src_.size()) {
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != '"') {
                throw RheaException(make_diag("E1006", "expected string key in JSON object", file_, sp_));
            }
            std::string k = parse_str();
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                throw RheaException(make_diag("E1006", "expected ':' after key in JSON object", file_, sp_));
            }
            ++pos_;
            Value v = parse_val();
            m[k] = std::move(v);
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < src_.size() && src_[pos_] == '}') { ++pos_; return Value::make_map(std::move(m)); }
            break;
        }
        throw RheaException(make_diag("E1006", "malformed JSON object", file_, sp_));
    }

    std::string src_;
    std::string file_;
    Span sp_;
    size_t pos_ = 0;
    size_t depth_ = 0;
};

// ============================================================================
// 8. Tree-Walking Interpreter
// ============================================================================

struct ReturnSignal { Value val; };
struct BreakSignal {};
struct ContinueSignal {};

class Interpreter {
public:
    using DebugHook = std::function<void(const Stmt&, Interpreter&)>;

    explicit Interpreter(std::string filename) : filename_(std::move(filename)) {}

    void set_debug_hook(DebugHook hook) { debug_hook_ = std::move(hook); }

    Value run(const Program& prog) {
        prog_ = &prog;
        for (const auto& fn : prog.functions) {
            functions_[fn.name] = fn;
        }
        for (const auto& pl : prog.pipelines) {
            pipelines_[pl.name] = pl.stages;
        }
        scopes_.clear();
        call_stack_.clear();
        push_scope();

        for (const auto& st : prog.top_stmts) {
            exec_stmt(st);
        }
        global_scope_ = scopes_.front();

        auto it = functions_.find("main");
        if (it != functions_.end()) {
            return call_function(it->second, {}, it->second.span);
        }
        return Value::make_int(0);
    }

    const std::vector<std::unordered_map<std::string, Value>>& scopes() const { return scopes_; }
    const std::vector<std::string>& call_stack() const { return call_stack_; }

    Value eval_expr_public(const ExprPtr& e) { return eval_expr(e); }

private:
    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }

    void define_var(const std::string& name, Value v) {
        scopes_.back()[name] = std::move(v);
    }

    Value* lookup_var(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        auto gfound = global_scope_.find(name);
        if (gfound != global_scope_.end()) return &gfound->second;
        return nullptr;
    }

    Value call_function(const FnDecl& fn, const std::vector<Value>& args, Span call_span) {
        if (call_depth_ >= MAX_RECURSION_DEPTH) {
            throw RheaException(make_diag(
                "E1004",
                "maximum recursion depth (" + std::to_string(MAX_RECURSION_DEPTH) + ") exceeded in '" + fn.name + "'",
                filename_,
                call_span,
                "Ensure recursive functions have a reachable base case."
            ));
        }
        ++call_depth_;
        if (debug_hook_) {
            call_stack_.push_back(fn.name + " at line " + std::to_string(call_span.line));
        }
        auto saved_scopes = std::move(scopes_);
        scopes_.clear();
        scopes_.emplace_back();
        for (size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
            scopes_.back().emplace(fn.params[i].name, args[i]);
        }
        bool prev_has_return = has_return_;
        Value prev_return_val = std::move(return_val_);
        has_return_ = false;
        Value ret = Value::make_unit();
        try {
            for (const auto& st : fn.body) {
                exec_stmt(st);
                if (has_return_) {
                    ret = std::move(return_val_);
                    break;
                }
            }
        } catch (const ReturnSignal& sig) {
            ret = sig.val;
        } catch (...) {
            has_return_ = prev_has_return;
            return_val_ = std::move(prev_return_val);
            scopes_ = std::move(saved_scopes);
            if (debug_hook_) call_stack_.pop_back();
            --call_depth_;
            throw;
        }
        has_return_ = prev_has_return;
        return_val_ = std::move(prev_return_val);
        scopes_ = std::move(saved_scopes);
        if (debug_hook_) call_stack_.pop_back();
        --call_depth_;
        return ret;
    }

    Value invoke_closure(const std::shared_ptr<ClosureValue>& cl, const std::vector<Value>& args, Span call_span) {
        if (call_depth_ >= MAX_RECURSION_DEPTH) {
            throw RheaException(make_diag("E1004", "maximum recursion depth exceeded in closure", filename_, call_span));
        }
        ++call_depth_;
        if (debug_hook_) {
            call_stack_.push_back("<closure> at line " + std::to_string(call_span.line));
        }
        auto saved_scopes = std::move(scopes_);
        scopes_.clear();
        scopes_.emplace_back(cl->captured);
        push_scope();
        for (size_t i = 0; i < cl->params.size() && i < args.size(); ++i) {
            define_var(cl->params[i].name, args[i]);
        }
        bool prev_has_return = has_return_;
        Value prev_return_val = std::move(return_val_);
        has_return_ = false;
        Value ret = Value::make_unit();
        try {
            if (cl->body_expr) {
                ret = eval_expr(cl->body_expr);
            } else {
                for (const auto& st : cl->body_stmts) {
                    exec_stmt(st);
                    if (has_return_) {
                        ret = std::move(return_val_);
                        break;
                    }
                }
            }
        } catch (const ReturnSignal& sig) {
            ret = sig.val;
        } catch (...) {
            has_return_ = prev_has_return;
            return_val_ = std::move(prev_return_val);
            scopes_ = std::move(saved_scopes);
            if (debug_hook_) call_stack_.pop_back();
            --call_depth_;
            throw;
        }
        has_return_ = prev_has_return;
        return_val_ = std::move(prev_return_val);
        scopes_ = std::move(saved_scopes);
        if (debug_hook_) call_stack_.pop_back();
        --call_depth_;
        return ret;
    }

    void exec_stmt(const StmtPtr& st) {
        if (!st || has_return_) return;
        if (debug_hook_) debug_hook_(*st, *this);

        switch (st->kind) {
            case Stmt::Kind::Let:
            case Stmt::Kind::Const: {
                Value v = eval_expr(st->expr);
                define_var(st->name, std::move(v));
                break;
            }
            case Stmt::Kind::Assign: {
                if (st->target->kind == Expr::Kind::Index || st->target->kind == Expr::Kind::FieldAccess) {
                    std::vector<ExprPtr> chain;
                    ExprPtr cur_e = st->target;
                    while (cur_e && (cur_e->kind == Expr::Kind::Index || cur_e->kind == Expr::Kind::FieldAccess)) {
                        chain.push_back(cur_e);
                        cur_e = cur_e->lhs;
                    }
                    std::reverse(chain.begin(), chain.end());
                    if (!cur_e || cur_e->kind != Expr::Kind::Ident) {
                        throw RheaException(make_diag("E0002", "invalid assignment target", filename_, st->span));
                    }
                    Value* cur_val = lookup_var(cur_e->text);
                    if (!cur_val) {
                        throw RheaException(make_diag("E0004", "undefined variable '" + cur_e->text + "'", filename_, st->span));
                    }
                    // 1. Evaluate each index expression in left-to-right order FIRST
                    std::vector<Value> eval_indices(chain.size());
                    for (size_t i = 0; i < chain.size(); ++i) {
                        if (chain[i]->kind == Expr::Kind::Index) {
                            eval_indices[i] = eval_expr(chain[i]->rhs);
                        }
                    }
                    // 2. Evaluate RHS expression SECOND
                    Value rhs_chained = eval_expr(st->expr);
                    // 3. Traverse and mutate target in-place
                    for (size_t i = 0; i < chain.size(); ++i) {
                        bool is_last = (i + 1 == chain.size());
                        if (chain[i]->kind == Expr::Kind::FieldAccess) {
                            cur_val = &cur_val->map_val[chain[i]->text];
                        } else {
                            const Value& idx = eval_indices[i];
                            if (cur_val->kind == Value::Kind::List) {
                                if (idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= cur_val->list_val.size()) {
                                    throw RheaException(make_diag(
                                        "E1003", "list index out of bounds", filename_, chain[i]->span,
                                        "Use an index between 0 and len(list) - 1."
                                    ));
                                }
                                cur_val = &cur_val->list_val[static_cast<size_t>(idx.int_val)];
                            } else if (cur_val->kind == Value::Kind::Map) {
                                std::string k = idx.to_string();
                                if (!is_last || st->assign_op != "=") {
                                    auto it = cur_val->map_val.find(k);
                                    if (it == cur_val->map_val.end()) {
                                        throw RheaException(make_diag("E1003", "key '" + k + "' not found in map", filename_, chain[i]->span));
                                    }
                                    cur_val = &it->second;
                                } else {
                                    cur_val = &cur_val->map_val[k];
                                }
                            } else {
                                throw RheaException(make_diag("E0003", "type is not indexable", filename_, chain[i]->span));
                            }
                        }
                    }
                    if (st->assign_op == "=") *cur_val = std::move(rhs_chained);
                    else *cur_val = apply_binary(st->assign_op.substr(0, 1), *cur_val, rhs_chained, st->span);
                    break;
                }
                Value rhs = eval_expr(st->expr);
                if (st->is_implicit_let && st->target && st->target->kind == Expr::Kind::Ident) {
                    define_var(st->target->text, std::move(rhs));
                    break;
                }
                if (st->target->kind == Expr::Kind::Ident) {
                    Value* existing = lookup_var(st->target->text);
                    if (!existing) {
                        if (st->assign_op == "=") {
                            define_var(st->target->text, std::move(rhs));
                            break;
                        }
                        throw RheaException(make_diag(
                            "E0004", "undefined variable '" + st->target->text + "'", filename_, st->span
                        ));
                    }
                    if (st->assign_op == "=") {
                        *existing = std::move(rhs);
                    } else if (st->assign_op == "+=") {
                        *existing = apply_binary("+", *existing, rhs, st->span);
                    } else if (st->assign_op == "-=") {
                        *existing = apply_binary("-", *existing, rhs, st->span);
                    } else if (st->assign_op == "*=") {
                        *existing = apply_binary("*", *existing, rhs, st->span);
                    } else if (st->assign_op == "/=") {
                        *existing = apply_binary("/", *existing, rhs, st->span);
                    }
                } else if (st->target->kind == Expr::Kind::Index || st->target->kind == Expr::Kind::FieldAccess) {
                    std::vector<ExprPtr> chain;
                    ExprPtr cur_e = st->target;
                    while (cur_e && (cur_e->kind == Expr::Kind::Index || cur_e->kind == Expr::Kind::FieldAccess)) {
                        chain.push_back(cur_e);
                        cur_e = cur_e->lhs;
                    }
                    std::reverse(chain.begin(), chain.end());
                    if (!cur_e || cur_e->kind != Expr::Kind::Ident) {
                        throw RheaException(make_diag("E0002", "invalid assignment target", filename_, st->span));
                    }
                    Value* cur_val = lookup_var(cur_e->text);
                    if (!cur_val) {
                        throw RheaException(make_diag("E0004", "undefined variable '" + cur_e->text + "'", filename_, st->span));
                    }
                    // Evaluate each index expression along the chain exactly once in left-to-right order
                    std::vector<Value> eval_indices(chain.size());
                    for (size_t i = 0; i < chain.size(); ++i) {
                        if (chain[i]->kind == Expr::Kind::Index) {
                            eval_indices[i] = eval_expr(chain[i]->rhs);
                        }
                    }
                    for (size_t i = 0; i < chain.size(); ++i) {
                        bool is_last = (i + 1 == chain.size());
                        if (chain[i]->kind == Expr::Kind::FieldAccess) {
                            cur_val = &cur_val->map_val[chain[i]->text];
                        } else {
                            const Value& idx = eval_indices[i];
                            if (cur_val->kind == Value::Kind::List) {
                                if (idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= cur_val->list_val.size()) {
                                    throw RheaException(make_diag(
                                        "E1003", "list index out of bounds", filename_, chain[i]->span,
                                        "Use an index between 0 and len(list) - 1."
                                    ));
                                }
                                cur_val = &cur_val->list_val[static_cast<size_t>(idx.int_val)];
                            } else if (cur_val->kind == Value::Kind::Map) {
                                std::string k = idx.to_string();
                                if (!is_last || st->assign_op != "=") {
                                    auto it = cur_val->map_val.find(k);
                                    if (it == cur_val->map_val.end()) {
                                        throw RheaException(make_diag("E1003", "key '" + k + "' not found in map", filename_, chain[i]->span));
                                    }
                                    cur_val = &it->second;
                                } else {
                                    cur_val = &cur_val->map_val[k];
                                }
                            } else {
                                throw RheaException(make_diag("E0003", "type is not indexable", filename_, chain[i]->span));
                            }
                        }
                    }
                    if (st->assign_op == "=") {
                        *cur_val = std::move(rhs);
                    } else {
                        *cur_val = apply_binary(st->assign_op.substr(0, 1), *cur_val, rhs, st->span);
                    }
                }
                break;
            }
            case Stmt::Kind::If: {
                Value c = eval_expr(st->cond);
                if (c.bool_val) {
                    push_scope();
                    for (const auto& s : st->then_body) {
                        exec_stmt(s);
                        if (has_return_) break;
                    }
                    pop_scope();
                } else if (!st->else_body.empty()) {
                    push_scope();
                    for (const auto& s : st->else_body) {
                        exec_stmt(s);
                        if (has_return_) break;
                    }
                    pop_scope();
                }
                break;
            }
            case Stmt::Kind::While: {
                while (!has_return_ && eval_expr(st->cond).bool_val) {
                    push_scope();
                    try {
                        for (const auto& s : st->body) {
                            exec_stmt(s);
                            if (has_return_) break;
                        }
                    } catch (const BreakSignal&) {
                        pop_scope();
                        break;
                    } catch (const ContinueSignal&) {
                        pop_scope();
                        continue;
                    }
                    pop_scope();
                }
                break;
            }
            case Stmt::Kind::For:
            case Stmt::Kind::ParallelFor: {
                if (st->expr && st->expr->kind == Expr::Kind::Binary &&
                    (st->expr->op == ".." || st->expr->op == "..=")) {
                    int64_t start_v = eval_expr(st->expr->lhs).int_val;
                    int64_t end_v = eval_expr(st->expr->rhs).int_val;
                    bool incl = (st->expr->op == "..=");
                    if ((!incl && end_v <= start_v) || (incl && end_v < start_v)) break;
                    int64_t cur = start_v;
                    push_scope();
                    while (!has_return_) {
                        scopes_.back()[st->name] = Value::make_int(cur);
                        bool broken = false;
                        try {
                            for (const auto& s : st->body) {
                                exec_stmt(s);
                                if (has_return_) break;
                            }
                        } catch (const BreakSignal&) {
                            broken = true;
                        } catch (const ContinueSignal&) {
                            // continue to step
                        }
                        if (broken || has_return_) break;
                        if (incl) {
                            if (cur == end_v) break;
                            ++cur;
                        } else {
                            ++cur;
                            if (cur >= end_v) break;
                        }
                    }
                    pop_scope();
                    break;
                }
                Value iter = eval_expr(st->expr);
                if (iter.kind != Value::Kind::List) {
                    throw RheaException(make_diag(
                        "E0003", "for loop requires a List or range (e.g. 1..=5)", filename_, st->span
                    ));
                }
                push_scope();
                for (const auto& item : iter.list_val) {
                    if (has_return_) break;
                    scopes_.back()[st->name] = item;
                    bool broken = false;
                    try {
                        for (const auto& s : st->body) {
                            exec_stmt(s);
                            if (has_return_) break;
                        }
                    } catch (const BreakSignal&) {
                        broken = true;
                    } catch (const ContinueSignal&) {
                        continue;
                    }
                    if (broken) break;
                }
                pop_scope();
                break;
            }
            case Stmt::Kind::Block: {
                push_scope();
                for (const auto& s : st->body) {
                    exec_stmt(s);
                    if (has_return_) break;
                }
                pop_scope();
                break;
            }
            case Stmt::Kind::Return: {
                return_val_ = st->expr ? eval_expr(st->expr) : Value::make_unit();
                has_return_ = true;
                break;
            }
            case Stmt::Kind::Break:
                throw BreakSignal{};
            case Stmt::Kind::Continue:
                throw ContinueSignal{};
            case Stmt::Kind::TryCatch: {
                try {
                    push_scope();
                    for (const auto& s : st->body) exec_stmt(s);
                    pop_scope();
                } catch (const RheaException& ex) {
                    pop_scope();
                    push_scope();
                    define_var(st->catch_var, Value::make_str(ex.diag.message));
                    for (const auto& s : st->catch_body) exec_stmt(s);
                    pop_scope();
                }
                break;
            }
            case Stmt::Kind::Expr:
                eval_expr(st->expr);
                break;
        }
    }

    Value apply_binary(const std::string& op, const Value& l, const Value& r, Span sp) {
        if (op == "..") return build_range_value(l.int_val, r.int_val, false, filename_, sp);
        if (op == "..=") return build_range_value(l.int_val, r.int_val, true, filename_, sp);
        if (op == "+") {
            if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                return Value::make_int(checked_add_i64(l.int_val, r.int_val, filename_, sp));
            }
            if (l.kind == Value::Kind::Float || r.kind == Value::Kind::Float) {
                double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
                double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
                return Value::make_float(a + b);
            }
            if (l.kind == Value::Kind::Str && r.kind == Value::Kind::Str) {
                return Value::make_str(l.str_val + r.str_val);
            }
            if (l.kind == Value::Kind::Tensor && r.kind == Value::Kind::Tensor) {
                return tensor_binary_op(l, r, "add", sp);
            }
        }
        if (op == "-") {
            if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                return Value::make_int(checked_sub_i64(l.int_val, r.int_val, filename_, sp));
            }
            if (l.kind == Value::Kind::Float || r.kind == Value::Kind::Float) {
                double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
                double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
                return Value::make_float(a - b);
            }
            if (l.kind == Value::Kind::Tensor && r.kind == Value::Kind::Tensor) {
                return tensor_binary_op(l, r, "sub", sp);
            }
        }
        if (op == "*") {
            if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                return Value::make_int(checked_mul_i64(l.int_val, r.int_val, filename_, sp));
            }
            if (l.kind == Value::Kind::Float || r.kind == Value::Kind::Float) {
                double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
                double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
                return Value::make_float(a * b);
            }
            if (l.kind == Value::Kind::Tensor && r.kind == Value::Kind::Tensor) {
                return tensor_binary_op(l, r, "mul", sp);
            }
        }
        if (op == "/") {
            if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                return Value::make_int(checked_div_i64(l.int_val, r.int_val, filename_, sp));
            }
            double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
            double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
            if (std::fabs(b) == 0.0) {
                throw RheaException(make_diag("E1001", "division by zero", filename_, sp));
            }
            return Value::make_float(a / b);
        }
        if (op == "%") {
            return Value::make_int(checked_mod_i64(l.int_val, r.int_val, filename_, sp));
        }
        if (op == "==") return Value::make_bool(values_equal(l, r));
        if (op == "!=") return Value::make_bool(!values_equal(l, r));
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                if (op == "<") return Value::make_bool(l.int_val < r.int_val);
                if (op == "<=") return Value::make_bool(l.int_val <= r.int_val);
                if (op == ">") return Value::make_bool(l.int_val > r.int_val);
                return Value::make_bool(l.int_val >= r.int_val);
            }
            if (l.kind == Value::Kind::Str && r.kind == Value::Kind::Str) {
                if (op == "<") return Value::make_bool(l.str_val < r.str_val);
                if (op == "<=") return Value::make_bool(l.str_val <= r.str_val);
                if (op == ">") return Value::make_bool(l.str_val > r.str_val);
                return Value::make_bool(l.str_val >= r.str_val);
            }
            double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
            double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
            if (op == "<") return Value::make_bool(a < b);
            if (op == "<=") return Value::make_bool(a <= b);
            if (op == ">") return Value::make_bool(a > b);
            return Value::make_bool(a >= b);
        }
        throw RheaException(make_diag("E0003", "unsupported binary operator '" + op + "'", filename_, sp));
    }

    Value tensor_binary_op(const Value& a, const Value& b, const std::string& kind, Span sp) {
        if (!a.tensor_val || !b.tensor_val) {
            throw RheaException(make_diag("E1007", "null tensor operand", filename_, sp));
        }
        rhea_TensorF64 ta{}, tb{}, tc{};
        rhea_tensor_from_buffer(
            a.tensor_val->data.data(), a.tensor_val->data.size(),
            a.tensor_val->shape.data(), a.tensor_val->shape.size(), &ta
        );
        rhea_tensor_from_buffer(
            b.tensor_val->data.data(), b.tensor_val->data.size(),
            b.tensor_val->shape.data(), b.tensor_val->shape.size(), &tb
        );
        rhea_Status st = RHEA_OK;
        if (kind == "add") st = rhea_tensor_add(&ta, &tb, &tc);
        else if (kind == "sub") st = rhea_tensor_sub(&ta, &tb, &tc);
        else if (kind == "mul") st = rhea_tensor_mul(&ta, &tb, &tc);
        else if (kind == "matmul") st = rhea_tensor_matmul(&ta, &tb, &tc);
        rhea_tensor_free(&ta);
        rhea_tensor_free(&tb);
        if (st != RHEA_OK) {
            throw RheaException(make_diag(
                "E1007", std::string("tensor operation failed: ") + rhea_status_message(st),
                filename_, sp
            ));
        }
        std::vector<size_t> shape(tc.shape, tc.shape + tc.ndim);
        std::vector<double> data(tc.data, tc.data + tc.numel);
        rhea_tensor_free(&tc);
        return Value::make_tensor(std::move(shape), std::move(data));
    }

    Value eval_expr(const ExprPtr& e) {
        if (!e) return Value::make_unit();
        switch (e->kind) {
            case Expr::Kind::IntLit: return Value::make_int(e->int_val);
            case Expr::Kind::FloatLit: return Value::make_float(e->float_val);
            case Expr::Kind::BoolLit: return Value::make_bool(e->bool_val);
            case Expr::Kind::StrLit: return Value::make_str(e->text);
            case Expr::Kind::Ident: {
                Value* v = lookup_var(e->text);
                if (v) return *v;
                throw RheaException(make_diag("E0004", "undefined variable '" + e->text + "'", filename_, e->span));
            }
            case Expr::Kind::Unary: {
                Value v = eval_expr(e->lhs);
                if (e->op == "-") {
                    if (v.kind == Value::Kind::Int) {
                        return Value::make_int(checked_sub_i64(0, v.int_val, filename_, e->span));
                    }
                    return Value::make_float(-v.float_val);
                }
                if (e->op == "!") return Value::make_bool(!v.bool_val);
                return v;
            }
            case Expr::Kind::Binary: {
                if (e->op == "&&") {
                    Value l = eval_expr(e->lhs);
                    if (!l.bool_val) return Value::make_bool(false);
                    return Value::make_bool(eval_expr(e->rhs).bool_val);
                }
                if (e->op == "||") {
                    Value l = eval_expr(e->lhs);
                    if (l.bool_val) return Value::make_bool(true);
                    return Value::make_bool(eval_expr(e->rhs).bool_val);
                }
                Value l = eval_expr(e->lhs);
                Value r = eval_expr(e->rhs);
                return apply_binary(e->op, l, r, e->span);
            }
            case Expr::Kind::ListLit: {
                std::vector<Value> items;
                for (const auto& a : e->args) items.push_back(eval_expr(a));
                return Value::make_list(std::move(items));
            }
            case Expr::Kind::MapLit: {
                std::map<std::string, Value> m;
                for (const auto& kv : e->map_entries) {
                    Value k = eval_expr(kv.first);
                    Value v = eval_expr(kv.second);
                    m[k.to_string()] = std::move(v);
                }
                return Value::make_map(std::move(m));
            }
            case Expr::Kind::Index: {
                if (e->lhs->kind == Expr::Kind::Ident) {
                    const Value* cptr = lookup_var(e->lhs->text);
                    Value idx = eval_expr(e->rhs);
                    if (cptr) {
                        if (cptr->kind == Value::Kind::List) {
                            if (idx.kind != Value::Kind::Int || idx.int_val < 0 ||
                                static_cast<size_t>(idx.int_val) >= cptr->list_val.size()) {
                                throw RheaException(make_diag(
                                    "E1003",
                                    "list index " + idx.to_string() + " out of bounds for length " +
                                    std::to_string(cptr->list_val.size()),
                                    filename_, e->span
                                ));
                            }
                            return cptr->list_val[static_cast<size_t>(idx.int_val)];
                        }
                        if (cptr->kind == Value::Kind::Map) {
                            std::string k = idx.to_string();
                            auto it = cptr->map_val.find(k);
                            if (it == cptr->map_val.end()) {
                                throw RheaException(make_diag("E1003", "key '" + k + "' not found in map", filename_, e->span));
                            }
                            return it->second;
                        }
                    }
                }
                Value container = eval_expr(e->lhs);
                Value idx = eval_expr(e->rhs);
                if (container.kind == Value::Kind::List) {
                    if (idx.kind != Value::Kind::Int || idx.int_val < 0 ||
                        static_cast<size_t>(idx.int_val) >= container.list_val.size()) {
                        throw RheaException(make_diag(
                            "E1003",
                            "list index " + idx.to_string() + " out of bounds for length " +
                            std::to_string(container.list_val.size()),
                            filename_, e->span
                        ));
                    }
                    return container.list_val[static_cast<size_t>(idx.int_val)];
                }
                if (container.kind == Value::Kind::Map) {
                    std::string k = idx.to_string();
                    auto it = container.map_val.find(k);
                    if (it == container.map_val.end()) {
                        throw RheaException(make_diag("E1003", "key '" + k + "' not found in map", filename_, e->span));
                    }
                    return it->second;
                }
                throw RheaException(make_diag("E0003", "type is not indexable", filename_, e->span));
            }
            case Expr::Kind::FieldAccess: {
                if (e->lhs->kind == Expr::Kind::Ident) {
                    const Value* optr = lookup_var(e->lhs->text);
                    if (optr) {
                        if (optr->kind == Value::Kind::Struct || optr->kind == Value::Kind::Map) {
                            auto it = optr->map_val.find(e->text);
                            if (it != optr->map_val.end()) return it->second;
                        }
                        if (optr->kind == Value::Kind::Tensor && optr->tensor_val) {
                            if (e->text == "numel") return Value::make_int(static_cast<int64_t>(optr->tensor_val->data.size()));
                            if (e->text == "ndim") return Value::make_int(static_cast<int64_t>(optr->tensor_val->shape.size()));
                        }
                    }
                }
                Value obj = eval_expr(e->lhs);
                if (obj.kind == Value::Kind::Struct || obj.kind == Value::Kind::Map) {
                    auto it = obj.map_val.find(e->text);
                    if (it != obj.map_val.end()) return it->second;
                }
                if (obj.kind == Value::Kind::Tensor && obj.tensor_val) {
                    if (e->text == "numel") return Value::make_int(static_cast<int64_t>(obj.tensor_val->data.size()));
                    if (e->text == "ndim") return Value::make_int(static_cast<int64_t>(obj.tensor_val->shape.size()));
                }
                throw RheaException(make_diag("E0004", "unknown field '" + e->text + "'", filename_, e->span));
            }
            case Expr::Kind::StructInit: {
                Value s;
                s.kind = Value::Kind::Struct;
                s.type_tag = e->text;
                for (const auto& f : e->struct_fields) {
                    s.map_val[f.first] = eval_expr(f.second);
                }
                return s;
            }
            case Expr::Kind::EnumVariant: {
                Value ev;
                ev.kind = Value::Kind::Enum;
                ev.type_tag = e->enum_name;
                ev.variant_tag = e->variant_name;
                for (const auto& a : e->args) ev.list_val.push_back(eval_expr(a));
                return ev;
            }
            case Expr::Kind::Lambda: {
                Value cl;
                cl.kind = Value::Kind::Closure;
                cl.closure_val = std::make_shared<ClosureValue>();
                cl.closure_val->params = e->lambda_params;
                cl.closure_val->body_expr = e->lhs;
                cl.closure_val->body_stmts = e->lambda_body_stmts;
                for (const auto& sc : scopes_) {
                    for (const auto& kv : sc) {
                        cl.closure_val->captured[kv.first] = kv.second;
                    }
                }
                return cl;
            }
            case Expr::Kind::Match: {
                Value target = eval_expr(e->lhs);
                for (const auto& arm : e->match_arms) {
                    bool matched = false;
                    push_scope();
                    if (arm.is_wildcard) {
                        matched = true;
                    } else if (arm.is_enum_variant) {
                        if (target.kind == Value::Kind::Enum &&
                            target.type_tag == arm.pattern_enum &&
                            target.variant_tag == arm.pattern_variant) {
                            matched = true;
                            if (!arm.binding_name.empty() && !target.list_val.empty()) {
                                define_var(arm.binding_name, target.list_val[0]);
                            }
                        }
                    } else if (arm.is_binding) {
                        matched = true;
                        define_var(arm.binding_name, target);
                    } else if (arm.literal_expr) {
                        Value lit = eval_expr(arm.literal_expr);
                        matched = values_equal(target, lit);
                    }
                    if (matched) {
                        Value res = Value::make_unit();
                        if (arm.body_expr) {
                            res = eval_expr(arm.body_expr);
                        } else {
                            for (const auto& st : arm.body_stmts) exec_stmt(st);
                        }
                        pop_scope();
                        return res;
                    }
                    pop_scope();
                }
                return Value::make_unit();
            }
            case Expr::Kind::Call:
                return eval_call(e);
        }
        return Value::make_unit();
    }

    Value eval_call(const ExprPtr& e) {
        const std::string& fn = e->text;

        if ((fn == "push" || fn == "std::collections.push") && e->args.size() == 2 &&
            e->args[0]->kind == Expr::Kind::Ident) {
            Value* target_list = lookup_var(e->args[0]->text);
            Value item = eval_expr(e->args[1]);
            if (target_list && target_list->kind == Value::Kind::List) {
                target_list->list_val.push_back(std::move(item));
                return Value::make_unit();
            }
        }
        if ((fn == "pop" || fn == "std::collections.pop") && e->args.size() == 1 &&
            e->args[0]->kind == Expr::Kind::Ident) {
            Value* target_list = lookup_var(e->args[0]->text);
            if (!target_list || target_list->kind != Value::Kind::List || target_list->list_val.empty()) {
                throw RheaException(make_diag("E1003", "pop() on empty list", filename_, e->span));
            }
            Value last = std::move(target_list->list_val.back());
            target_list->list_val.pop_back();
            return last;
        }
        if ((fn == "remove_key" || fn == "std::collections.remove_key" || fn == "std::collections.remove") &&
            e->args.size() == 2 && e->args[0]->kind == Expr::Kind::Ident) {
            Value* target_map = lookup_var(e->args[0]->text);
            Value k = eval_expr(e->args[1]);
            if (target_map && target_map->kind == Value::Kind::Map) {
                bool removed = target_map->map_val.erase(k.to_string()) > 0;
                return Value::make_bool(removed);
            }
        }

        std::vector<Value> args;
        for (const auto& a : e->args) args.push_back(eval_expr(a));

        if (fn == "print" || fn == "println" || fn == "std::io.print" || fn == "std::io.println") {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].to_string();
            }
            std::cout << "\n" << std::flush;
            return Value::make_unit();
        }
        if (fn == "print_inline" || fn == "std::io.print_inline") {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].to_string();
            }
            std::cout << std::flush;
            return Value::make_unit();
        }
        if (fn == "input" || fn == "std::io.input" || fn == "std::io.read_line") {
            std::string prompt = args.empty() ? "" : args[0].to_string();
            return Value::make_str(runtime_read_line(prompt));
        }
        if (fn == "is_eof" || fn == "std::io.is_eof") {
            return Value::make_bool(runtime_is_eof());
        }
        if (fn == "try_parse_int" || fn == "std::io.try_parse_int") {
            std::string raw = args.empty() ? "" : args[0].to_string();
            int64_t val = 0;
            std::string err;
            bool ok = safe_parse_i64(raw, &val, &err);
            std::map<std::string, Value> res;
            res["ok"] = Value::make_bool(ok);
            res["value"] = Value::make_int(ok ? val : 0);
            res["error"] = Value::make_str(ok ? "" : err);
            return Value::make_map(std::move(res));
        }
        if (fn == "try_parse_float" || fn == "std::io.try_parse_float") {
            std::string raw = args.empty() ? "" : args[0].to_string();
            double val = 0.0;
            std::string err;
            bool ok = safe_parse_f64(raw, &val, &err);
            std::map<std::string, Value> res;
            res["ok"] = Value::make_bool(ok);
            res["value"] = Value::make_float(ok ? val : 0.0);
            res["error"] = Value::make_str(ok ? "" : err);
            return Value::make_map(std::move(res));
        }
        if (fn == "parse_int" || fn == "std::io.parse_int") {
            std::string raw = args.empty() ? "" : args[0].to_string();
            int64_t val = 0;
            std::string err;
            if (!safe_parse_i64(raw, &val, &err)) {
                throw RheaException(make_diag("E1006", "cannot parse '" + raw + "' as Int: " + err, filename_, e->span));
            }
            return Value::make_int(val);
        }
        if (fn == "parse_float" || fn == "std::io.parse_float") {
            std::string raw = args.empty() ? "" : args[0].to_string();
            double val = 0.0;
            std::string err;
            if (!safe_parse_f64(raw, &val, &err)) {
                throw RheaException(make_diag("E1006", "cannot parse '" + raw + "' as Float: " + err, filename_, e->span));
            }
            return Value::make_float(val);
        }
        if (fn == "input_int" || fn == "std::io.read_int") {
            std::string prompt = args.empty() ? "" : args[0].to_string();
            std::string line = runtime_read_line(prompt);
            int64_t val = 0;
            std::string err;
            if (!safe_parse_i64(line, &val, &err)) {
                throw RheaException(make_diag("E1006", "invalid integer input '" + line + "': " + err, filename_, e->span));
            }
            return Value::make_int(val);
        }
        if (fn == "input_float" || fn == "std::io.read_float") {
            std::string prompt = args.empty() ? "" : args[0].to_string();
            std::string line = runtime_read_line(prompt);
            double val = 0.0;
            std::string err;
            if (!safe_parse_f64(line, &val, &err)) {
                throw RheaException(make_diag("E1006", "invalid float input '" + line + "': " + err, filename_, e->span));
            }
            return Value::make_float(val);
        }
        if (fn == "sleep" || fn == "std::time.sleep_ms") {
            int64_t ms = args.empty() ? 0 : args[0].int_val;
            runtime_sleep_ms(ms, filename_, e->span);
            return Value::make_unit();
        }
        if (fn == "now_ms" || fn == "std::time.monotonic_ms" || fn == "std::time.now_ms") {
            return Value::make_int(runtime_monotonic_ms());
        }
        if (fn == "trim" || fn == "std::string.trim") return Value::make_str(str_trim(args[0].to_string()));
        if (fn == "lower" || fn == "std::string.to_lower") return Value::make_str(str_lower(args[0].to_string()));
        if (fn == "upper" || fn == "std::string.to_upper") return Value::make_str(str_upper(args[0].to_string()));
        if (fn == "std::string.contains") {
            return Value::make_bool(args[0].to_string().find(args[1].to_string()) != std::string::npos);
        }
        if (fn == "std::string.split") {
            return Value::make_list(split_string_values(args[0].to_string(), args[1].to_string()));
        }
        if (fn == "len" || fn == "std::collections.len" || fn == "std::string.len") {
            const Value& v = args[0];
            if (v.kind == Value::Kind::List) return Value::make_int(static_cast<int64_t>(v.list_val.size()));
            if (v.kind == Value::Kind::Str) return Value::make_int(static_cast<int64_t>(v.str_val.size()));
            if (v.kind == Value::Kind::Map) return Value::make_int(static_cast<int64_t>(v.map_val.size()));
            if (v.kind == Value::Kind::Tensor && v.tensor_val) return Value::make_int(static_cast<int64_t>(v.tensor_val->data.size()));
            return Value::make_int(0);
        }
        if (fn == "str") return Value::make_str(args[0].to_string());
        if (fn == "int") {
            if (args[0].kind == Value::Kind::Int) return args[0];
            if (args[0].kind == Value::Kind::Float) return Value::make_int(static_cast<int64_t>(args[0].float_val));
            int64_t v = 0;
            std::string err;
            if (!safe_parse_i64(args[0].to_string(), &v, &err)) {
                throw RheaException(make_diag("E1006", "cannot convert '" + args[0].to_string() + "' to Int", filename_, e->span));
            }
            return Value::make_int(v);
        }
        if (fn == "float") {
            if (args[0].kind == Value::Kind::Float) return args[0];
            if (args[0].kind == Value::Kind::Int) return Value::make_float(static_cast<double>(args[0].int_val));
            double v = 0.0;
            std::string err;
            if (!safe_parse_f64(args[0].to_string(), &v, &err)) {
                throw RheaException(make_diag("E1006", "cannot convert '" + args[0].to_string() + "' to Float", filename_, e->span));
            }
            return Value::make_float(v);
        }
        if (fn == "abs" || fn == "std::math.abs") {
            if (args[0].kind == Value::Kind::Int) return Value::make_int(std::llabs(args[0].int_val));
            return Value::make_float(std::fabs(args[0].float_val));
        }
        if (fn == "sqrt" || fn == "std::math.sqrt") {
            double v = (args[0].kind == Value::Kind::Float) ? args[0].float_val : static_cast<double>(args[0].int_val);
            return Value::make_float(std::sqrt(v));
        }
        if (fn == "std::math.sin") {
            double v = (args[0].kind == Value::Kind::Float) ? args[0].float_val : static_cast<double>(args[0].int_val);
            return Value::make_float(std::sin(v));
        }
        if (fn == "std::math.cos") {
            double v = (args[0].kind == Value::Kind::Float) ? args[0].float_val : static_cast<double>(args[0].int_val);
            return Value::make_float(std::cos(v));
        }
        if (fn == "std::math.exp") {
            double v = (args[0].kind == Value::Kind::Float) ? args[0].float_val : static_cast<double>(args[0].int_val);
            return Value::make_float(std::exp(v));
        }
        if (fn == "std::math.log") {
            double v = (args[0].kind == Value::Kind::Float) ? args[0].float_val : static_cast<double>(args[0].int_val);
            return Value::make_float(std::log(v));
        }
        if (fn == "std::math.clamp") {
            double x = (args[0].kind == Value::Kind::Float) ? args[0].float_val : static_cast<double>(args[0].int_val);
            double lo = (args[1].kind == Value::Kind::Float) ? args[1].float_val : static_cast<double>(args[1].int_val);
            double hi = (args[2].kind == Value::Kind::Float) ? args[2].float_val : static_cast<double>(args[2].int_val);
            if (args[0].kind == Value::Kind::Int && args[1].kind == Value::Kind::Int && args[2].kind == Value::Kind::Int) {
                return Value::make_int(std::clamp(args[0].int_val, args[1].int_val, args[2].int_val));
            }
            return Value::make_float(std::clamp(x, lo, hi));
        }
        if (fn == "range") {
            int64_t start = (args.size() == 1) ? 0 : args[0].int_val;
            int64_t end = (args.size() == 1) ? args[0].int_val : args[1].int_val;
            return build_range_value(start, end, false, filename_, e->span);
        }
        if (fn == "assert" || fn == "std::test.assert") {
            if (!args[0].bool_val) {
                std::string msg = (args.size() > 1) ? args[1].to_string() : "assertion failed";
                throw RheaException(make_diag("E1005", msg, filename_, e->span));
            }
            return Value::make_unit();
        }
        if (fn == "assert_eq" || fn == "std::test.assert_eq") {
            if (!values_equal(args[0], args[1])) {
                throw RheaException(make_diag(
                    "E1005",
                    "assert_eq failed: left (" + args[0].to_string() + ") != right (" + args[1].to_string() + ")",
                    filename_, e->span
                ));
            }
            return Value::make_unit();
        }
        if (fn == "eprint" || fn == "eprintln" || fn == "std::io.eprint" || fn == "std::io.eprintln") {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) std::cerr << " ";
                std::cerr << args[i].to_string();
            }
            if (fn == "eprintln" || fn == "std::io.eprintln") std::cerr << "\n";
            std::cerr << std::flush;
            return Value::make_unit();
        }
        if (fn == "has_key" || fn == "std::collections.has_key") {
            return Value::make_bool(args[0].map_val.count(args[1].to_string()) > 0);
        }
        if (fn == "keys" || fn == "std::collections.keys") {
            std::vector<Value> ks;
            for (const auto& kv : args[0].map_val) ks.push_back(Value::make_str(kv.first));
            return Value::make_list(std::move(ks));
        }
        if (fn == "values" || fn == "std::collections.values") {
            std::vector<Value> vs;
            for (const auto& kv : args[0].map_val) vs.push_back(kv.second);
            return Value::make_list(std::move(vs));
        }
        if (fn == "contains" || fn == "std::collections.contains") {
            if (args[0].kind == Value::Kind::List) {
                for (const auto& item : args[0].list_val) {
                    if (values_equal(item, args[1])) return Value::make_bool(true);
                }
                return Value::make_bool(false);
            }
            return Value::make_bool(args[0].to_string().find(args[1].to_string()) != std::string::npos);
        }
        if (fn == "reverse" || fn == "std::collections.reverse") {
            std::vector<Value> rev = args[0].list_val;
            std::reverse(rev.begin(), rev.end());
            return Value::make_list(std::move(rev));
        }
        if (fn == "sort" || fn == "std::collections.sort") {
            std::vector<Value> sorted = args[0].list_val;
            std::sort(sorted.begin(), sorted.end(), [](const Value& a, const Value& b) {
                if (a.kind == Value::Kind::Int && b.kind == Value::Kind::Int) return a.int_val < b.int_val;
                if (a.kind == Value::Kind::Float && b.kind == Value::Kind::Float) return a.float_val < b.float_val;
                return a.to_string() < b.to_string();
            });
            return Value::make_list(std::move(sorted));
        }
        if (fn == "std::string.char_count") {
            return Value::make_int(utf8_codepoint_count(args[0].to_string()));
        }
        if (fn == "std::string.join") {
            std::string delim = (args.size() > 1) ? args[1].to_string() : "";
            std::ostringstream oss;
            for (size_t i = 0; i < args[0].list_val.size(); ++i) {
                if (i > 0) oss << delim;
                oss << args[0].list_val[i].to_string();
            }
            return Value::make_str(oss.str());
        }
        if (fn == "std::string.starts_with") {
            std::string s = args[0].to_string(), p = args[1].to_string();
            return Value::make_bool(s.rfind(p, 0) == 0);
        }
        if (fn == "std::string.ends_with") {
            std::string s = args[0].to_string(), sfx = args[1].to_string();
            return Value::make_bool(s.size() >= sfx.size() && s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0);
        }
        if (fn == "std::string.replace") {
            return Value::make_str(str_replace_all(args[0].to_string(), args[1].to_string(), args[2].to_string()));
        }
        if (fn == "std::json.stringify") {
            return Value::make_str(value_to_json(args[0]));
        }
        if (fn == "std::json.parse") {
            SimpleJsonParser jp(args[0].to_string(), filename_, e->span);
            return jp.parse();
        }
        if (fn == "std::path.join") {
            return Value::make_str((fs::path(args[0].to_string()) / fs::path(args[1].to_string())).string());
        }
        if (fn == "std::path.basename") {
            return Value::make_str(fs::path(args[0].to_string()).filename().string());
        }
        if (fn == "std::path.dirname") {
            return Value::make_str(fs::path(args[0].to_string()).parent_path().string());
        }
        if (fn == "std::path.extension") {
            return Value::make_str(fs::path(args[0].to_string()).extension().string());
        }
        if (fn == "std::env.get") {
            const char* val = std::getenv(args[0].to_string().c_str());
            if (val) return Value::make_str(val);
            return Value::make_str(args.size() > 1 ? args[1].to_string() : "");
        }
        if (fn == "std::env.set") {
            int rc = setenv(args[0].to_string().c_str(), args[1].to_string().c_str(), 1);
            return Value::make_bool(rc == 0);
        }
        if (fn == "std::env.args") {
            std::vector<Value> av;
            for (const auto& s : g_cli_args) av.push_back(Value::make_str(s));
            return Value::make_list(std::move(av));
        }
        if (fn == "std::log.info") {
            std::cout << "[INFO] " << args[0].to_string() << "\n" << std::flush;
            return Value::make_unit();
        }
        if (fn == "std::log.warn") {
            std::cerr << "[WARN] " << args[0].to_string() << "\n" << std::flush;
            return Value::make_unit();
        }
        if (fn == "std::log.error") {
            std::cerr << "[ERROR] " << args[0].to_string() << "\n" << std::flush;
            return Value::make_unit();
        }
        if (fn == "std::fs.write_text") {
            std::ofstream ofs(args[0].to_string());
            ofs << args[1].to_string();
            return Value::make_bool(ofs.good());
        }
        if (fn == "std::fs.append_text") {
            std::ofstream ofs(args[0].to_string(), std::ios::app);
            ofs << args[1].to_string();
            return Value::make_bool(ofs.good());
        }
        if (fn == "std::fs.read_text") {
            std::ifstream ifs(args[0].to_string());
            if (!ifs) {
                throw RheaException(make_diag("E1006", "cannot read non-existent file '" + args[0].to_string() + "'", filename_, e->span));
            }
            std::ostringstream ss;
            ss << ifs.rdbuf();
            return Value::make_str(ss.str());
        }
        if (fn == "std::fs.read_lines") {
            std::ifstream ifs(args[0].to_string());
            if (!ifs) {
                throw RheaException(make_diag("E1006", "cannot read non-existent file '" + args[0].to_string() + "'", filename_, e->span));
            }
            std::vector<Value> lines;
            std::string ln;
            while (std::getline(ifs, ln)) {
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                lines.push_back(Value::make_str(ln));
            }
            return Value::make_list(std::move(lines));
        }
        if (fn == "std::fs.exists") {
            return Value::make_bool(fs::exists(args[0].to_string()));
        }
        if (fn == "std::fs.remove_file") {
            std::error_code ec;
            bool ok = fs::remove(args[0].to_string(), ec);
            return Value::make_bool(ok);
        }

        if (fn == "tensor" || fn == "std::ai.tensor") {
            if (args.size() == 2 && args[0].kind == Value::Kind::List && args[1].kind == Value::Kind::List) {
                std::vector<size_t> shape;
                for (const auto& s : args[0].list_val) shape.push_back(static_cast<size_t>(s.int_val));
                std::vector<double> data;
                for (const auto& d : args[1].list_val) {
                    data.push_back(d.kind == Value::Kind::Float ? d.float_val : static_cast<double>(d.int_val));
                }
                rhea_TensorF64 native_t{};
                rhea_Status st = rhea_tensor_from_buffer(data.data(), data.size(), shape.data(), shape.size(), &native_t);
                if (st != RHEA_OK) {
                    throw RheaException(make_diag("E1007", rhea_status_message(st), filename_, e->span));
                }
                rhea_tensor_free(&native_t);
                return Value::make_tensor(std::move(shape), std::move(data));
            }
            if (args.size() == 1 && args[0].kind == Value::Kind::List) {
                if (!args[0].list_val.empty() && args[0].list_val[0].kind == Value::Kind::List) {
                    size_t rows = args[0].list_val.size();
                    size_t cols = args[0].list_val[0].list_val.size();
                    std::vector<double> data;
                    for (const auto& r : args[0].list_val) {
                        for (const auto& v : r.list_val) {
                            data.push_back(v.kind == Value::Kind::Float ? v.float_val : static_cast<double>(v.int_val));
                        }
                    }
                    return Value::make_tensor({rows, cols}, std::move(data));
                }
                std::vector<double> data;
                for (const auto& v : args[0].list_val) {
                    data.push_back(v.kind == Value::Kind::Float ? v.float_val : static_cast<double>(v.int_val));
                }
                size_t n = data.size();
                return Value::make_tensor({n}, std::move(data));
            }
        }
        if (fn == "matmul" || fn == "std::ai.matmul") {
            return tensor_binary_op(args[0], args[1], "matmul", e->span);
        }
        if (fn == "std::ai.relu" || fn == "std::ai.sigmoid" || fn == "std::ai.tanh" || fn == "std::ai.softmax") {
            const Value& a = args[0];
            rhea_TensorF64 ta{}, tb{};
            rhea_tensor_from_buffer(a.tensor_val->data.data(), a.tensor_val->data.size(), a.tensor_val->shape.data(), a.tensor_val->shape.size(), &ta);
            if (fn == "std::ai.relu") rhea_tensor_relu(&ta, &tb);
            else if (fn == "std::ai.sigmoid") rhea_tensor_sigmoid(&ta, &tb);
            else if (fn == "std::ai.tanh") rhea_tensor_tanh(&ta, &tb);
            else rhea_tensor_softmax_last_dim(&ta, &tb);
            std::vector<size_t> shape(tb.shape, tb.shape + tb.ndim);
            std::vector<double> data(tb.data, tb.data + tb.numel);
            rhea_tensor_free(&ta);
            rhea_tensor_free(&tb);
            return Value::make_tensor(std::move(shape), std::move(data));
        }
        if (fn == "std::ai.sum" || fn == "std::ai.mean" || fn == "std::ai.l2_norm") {
            const Value& a = args[0];
            rhea_TensorF64 ta{};
            rhea_tensor_from_buffer(a.tensor_val->data.data(), a.tensor_val->data.size(), a.tensor_val->shape.data(), a.tensor_val->shape.size(), &ta);
            double out = 0.0;
            if (fn == "std::ai.sum") rhea_tensor_sum(&ta, &out);
            else if (fn == "std::ai.mean") rhea_tensor_mean(&ta, &out);
            else rhea_tensor_l2_norm(&ta, &out);
            rhea_tensor_free(&ta);
            return Value::make_float(out);
        }
        if (fn == "std::ai.mse_loss") {
            const Value& p = args[0];
            const Value& t = args[1];
            rhea_TensorF64 tp{}, tt{}, tg{};
            rhea_tensor_from_buffer(p.tensor_val->data.data(), p.tensor_val->data.size(), p.tensor_val->shape.data(), p.tensor_val->shape.size(), &tp);
            rhea_tensor_from_buffer(t.tensor_val->data.data(), t.tensor_val->data.size(), t.tensor_val->shape.data(), t.tensor_val->shape.size(), &tt);
            double loss = 0.0;
            rhea_Status st = rhea_mse_loss_and_grad(&tp, &tt, &loss, &tg);
            rhea_tensor_free(&tp);
            rhea_tensor_free(&tt);
            rhea_tensor_free(&tg);
            if (st != RHEA_OK) throw RheaException(make_diag("E1007", rhea_status_message(st), filename_, e->span));
            return Value::make_float(loss);
        }
        if (fn == "std::ai.train_xor_demo") {
            size_t epochs = args.empty() ? 400 : static_cast<size_t>(args[0].int_val);
            double xor_x[8] = {0,0, 0,1, 1,0, 1,1};
            double xor_y[4] = {0, 1, 1, 0};
            size_t sx[2] = {4, 2}, sy[2] = {4, 1};
            rhea_TensorF64 X{}, Y{};
            rhea_tensor_from_buffer(xor_x, 8, sx, 2, &X);
            rhea_tensor_from_buffer(xor_y, 4, sy, 2, &Y);
            rhea_MLP2Layer mlp{};
            rhea_mlp_create(2, 16, 1, 1337ULL, &mlp);
            double init_l = 0.0, final_l = 0.0;
            rhea_mlp_train_epochs(&mlp, &X, &Y, 0.03, epochs, &init_l, &final_l);
            rhea_mlp_free(&mlp);
            rhea_tensor_free(&X);
            rhea_tensor_free(&Y);
            return Value::make_float(final_l);
        }

        // Check if calling a closure variable in scope first
        Value* cl_var = lookup_var(fn);
        if (cl_var && cl_var->kind == Value::Kind::Closure && cl_var->closure_val) {
            return invoke_closure(cl_var->closure_val, args, e->span);
        }

        auto fit = functions_.find(fn);
        if (fit != functions_.end()) {
            return call_function(fit->second, args, e->span);
        }

        auto pit = pipelines_.find(fn);
        if (pit != pipelines_.end() && !args.empty()) {
            Value acc = args[0];
            for (const auto& stage : pit->second) {
                auto sfit = functions_.find(stage);
                if (sfit != functions_.end()) {
                    acc = call_function(sfit->second, {acc}, e->span);
                }
            }
            return acc;
        }

        throw RheaException(make_diag("E0004", "undefined function '" + fn + "'", filename_, e->span));
    }

    std::string filename_;
    const Program* prog_ = nullptr;
    std::unordered_map<std::string, FnDecl> functions_;
    std::unordered_map<std::string, std::vector<std::string>> pipelines_;
    std::unordered_map<std::string, Value> global_scope_;
    std::vector<std::unordered_map<std::string, Value>> scopes_;
    std::vector<std::string> call_stack_;
    size_t call_depth_ = 0;
    bool has_return_ = false;
    Value return_val_;
    DebugHook debug_hook_;
};

// ============================================================================
// 9. Stack-Based Bytecode Compiler & Virtual Machine (`CallFrame` + Closures)
// ============================================================================

enum class OpCode : uint8_t {
    LoadConst,
    LoadLocal,
    StoreLocal,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Not,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    RangeExcl,
    RangeIncl,
    MakeList,
    MakeMap,
    MakeStruct,
    FieldGet,
    FieldSet,
    IndexGet,
    IndexSet,
    MakeClosure,
    Print,
    PrintInline,
    InputLine,
    SleepMs,
    NowMs,
    Jump,
    JumpIfFalse,
    CallFn,
    CallBuiltin,
    Pop,
    Return,
    Halt
};

struct Instruction {
    OpCode op = OpCode::Halt;
    int32_t operand = 0;
    std::string sym;
    Span span;
};

struct VMFunctionMeta {
    size_t entry_ip = 0;
    std::vector<std::string> params;
};

struct BytecodeChunk {
    std::vector<Value> constants;
    std::vector<Instruction> code;
    std::unordered_map<std::string, VMFunctionMeta> functions;

    int32_t add_const(Value v) {
        constants.push_back(std::move(v));
        return static_cast<int32_t>(constants.size() - 1);
    }

    std::string disassemble() const {
        std::ostringstream out;
        out << "=== Rhea Bytecode Disassembly (" << code.size() << " instructions) ===\n";
        for (size_t i = 0; i < code.size(); ++i) {
            const auto& ins = code[i];
            out << std::setw(4) << std::setfill('0') << i << std::setfill(' ') << "  ";
            switch (ins.op) {
                case OpCode::LoadConst:
                    out << "LOAD_CONST      " << ins.operand << " (" << constants[ins.operand].to_string() << ")";
                    break;
                case OpCode::LoadLocal: out << "LOAD_LOCAL      " << ins.sym; break;
                case OpCode::StoreLocal: out << "STORE_LOCAL     " << ins.sym; break;
                case OpCode::Add: out << "ADD"; break;
                case OpCode::Sub: out << "SUB"; break;
                case OpCode::Mul: out << "MUL"; break;
                case OpCode::Div: out << "DIV"; break;
                case OpCode::Mod: out << "MOD"; break;
                case OpCode::Neg: out << "NEG"; break;
                case OpCode::Not: out << "NOT"; break;
                case OpCode::Eq: out << "EQ"; break;
                case OpCode::Ne: out << "NE"; break;
                case OpCode::Lt: out << "LT"; break;
                case OpCode::Le: out << "LE"; break;
                case OpCode::Gt: out << "GT"; break;
                case OpCode::Ge: out << "Ge"; break;
                case OpCode::RangeExcl: out << "RANGE_EXCL"; break;
                case OpCode::RangeIncl: out << "RANGE_INCL"; break;
                case OpCode::MakeList: out << "MAKE_LIST       " << ins.operand; break;
                case OpCode::MakeMap: out << "MAKE_MAP        " << ins.operand; break;
                case OpCode::MakeStruct: out << "MAKE_STRUCT     " << ins.sym << " (" << ins.operand << ")"; break;
                case OpCode::FieldGet: out << "FIELD_GET       " << ins.sym; break;
                case OpCode::FieldSet: out << "FIELD_SET       " << ins.sym; break;
                case OpCode::IndexGet: out << "INDEX_GET"; break;
                case OpCode::IndexSet: out << "INDEX_SET"; break;
                case OpCode::MakeClosure: out << "MAKE_CLOSURE    " << ins.sym; break;
                case OpCode::Print: out << "PRINT           " << ins.operand; break;
                case OpCode::PrintInline: out << "PRINT_INLINE    " << ins.operand; break;
                case OpCode::InputLine: out << "INPUT_LINE      " << ins.operand; break;
                case OpCode::SleepMs: out << "SLEEP_MS"; break;
                case OpCode::NowMs: out << "NOW_MS"; break;
                case OpCode::Jump: out << "JUMP            -> " << ins.operand; break;
                case OpCode::JumpIfFalse: out << "JUMP_IF_FALSE   -> " << ins.operand; break;
                case OpCode::CallFn: out << "CALL_FN         " << ins.sym << " (" << ins.operand << ")"; break;
                case OpCode::CallBuiltin: out << "CALL_BUILTIN    " << ins.sym << " (" << ins.operand << ")"; break;
                case OpCode::Pop: out << "POP"; break;
                case OpCode::Return: out << "RETURN"; break;
                case OpCode::Halt: out << "HALT"; break;
            }
            out << "\n";
        }
        return out.str();
    }
};

class BytecodeCompiler {
public:
    BytecodeChunk compile(const Program& prog) {
        BytecodeChunk chunk;
        for (const auto& fn : prog.functions) {
            user_fns_.insert(fn.name);
        }

        // Compile top-level statements + snapshot globals + call to main() first, then HALT
        for (const auto& st : prog.top_stmts) {
            compile_stmt(chunk, st);
        }
        chunk.code.push_back(Instruction{OpCode::CallBuiltin, 0, "__snapshot_globals", Span{1, 1, 1}});
        chunk.code.push_back(Instruction{OpCode::Pop, 0, "", Span{1, 1, 1}});
        if (user_fns_.count("main")) {
            chunk.code.push_back(Instruction{OpCode::CallFn, 0, "main", Span{1, 1, 1}});
        }
        chunk.code.push_back(Instruction{OpCode::Halt, 0, "", Span{1, 1, 1}});

        // Compile all user-defined functions as real callable procedures
        for (const auto& fn : prog.functions) {
            VMFunctionMeta meta;
            meta.entry_ip = chunk.code.size();
            for (const auto& p : fn.params) meta.params.push_back(p.name);
            chunk.functions[fn.name] = std::move(meta);

            for (const auto& st : fn.body) {
                compile_stmt(chunk, st);
            }
            // Implicit trailing unit return if execution falls off the end
            int32_t u = chunk.add_const(Value::make_unit());
            chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", fn.span});
            chunk.code.push_back(Instruction{OpCode::Return, 0, "", fn.span});
        }

        // Compile any collected lambda procedures (copy shared_ptr by value so nested lambdas pushing to pending_lambdas_ never invalidate `lam`)
        for (size_t i = 0; i < pending_lambdas_.size(); ++i) {
            ExprPtr lam = pending_lambdas_[i];
            std::string lname = "__lambda_" + std::to_string(lam->lambda_id);
            VMFunctionMeta meta;
            meta.entry_ip = chunk.code.size();
            for (const auto& p : lam->lambda_params) meta.params.push_back(p.name);
            chunk.functions[lname] = std::move(meta);

            if (lam->lhs) {
                compile_expr(chunk, lam->lhs);
                chunk.code.push_back(Instruction{OpCode::Return, 0, "", lam->span});
            } else {
                for (const auto& st : lam->lambda_body_stmts) {
                    compile_stmt(chunk, st);
                }
                int32_t u = chunk.add_const(Value::make_unit());
                chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", lam->span});
                chunk.code.push_back(Instruction{OpCode::Return, 0, "", lam->span});
            }
        }
        return chunk;
    }

private:
    struct LoopPatchCtx {
        std::vector<size_t> break_jumps;
        std::vector<size_t> continue_jumps;
    };

    void compile_stmt(BytecodeChunk& chunk, const StmtPtr& st) {
        if (!st) return;
        switch (st->kind) {
            case Stmt::Kind::Let:
            case Stmt::Kind::Const:
                compile_expr(chunk, st->expr);
                chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, st->name, st->span});
                break;
            case Stmt::Kind::Assign:
                if (st->target->kind == Expr::Kind::Ident) {
                    if (st->assign_op == "=") {
                        compile_expr(chunk, st->expr);
                        chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, st->target->text, st->span});
                    } else {
                        chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, st->target->text, st->span});
                        compile_expr(chunk, st->expr);
                        OpCode op = OpCode::Add;
                        if (st->assign_op == "-=") op = OpCode::Sub;
                        else if (st->assign_op == "*=") op = OpCode::Mul;
                        else if (st->assign_op == "/=") op = OpCode::Div;
                        chunk.code.push_back(Instruction{op, 0, "", st->span});
                        chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, st->target->text, st->span});
                    }
                } else if (st->target->kind == Expr::Kind::Index || st->target->kind == Expr::Kind::FieldAccess) {
                    std::vector<ExprPtr> chain;
                    ExprPtr cur_e = st->target;
                    while (cur_e && (cur_e->kind == Expr::Kind::Index || cur_e->kind == Expr::Kind::FieldAccess)) {
                        chain.push_back(cur_e);
                        cur_e = cur_e->lhs;
                    }
                    std::reverse(chain.begin(), chain.end());
                    std::string kinds;
                    for (const auto& step : chain) {
                        if (step->kind == Expr::Kind::FieldAccess) {
                            kinds.push_back('F');
                            int32_t kc = chunk.add_const(Value::make_str(step->text));
                            chunk.code.push_back(Instruction{OpCode::LoadConst, kc, "", step->span});
                        } else {
                            kinds.push_back('I');
                            compile_expr(chunk, step->rhs);
                        }
                    }
                    compile_expr(chunk, st->expr);
                    std::string desc = "__lvalue_set|" + cur_e->text + "|" + st->assign_op + "|" + kinds;
                    chunk.code.push_back(Instruction{
                        OpCode::CallBuiltin, static_cast<int32_t>(chain.size() + 1), desc, st->span
                    });
                    chunk.code.push_back(Instruction{OpCode::Pop, 0, "", st->span});
                }
                break;
            case Stmt::Kind::Block:
                for (const auto& s : st->body) compile_stmt(chunk, s);
                break;
            case Stmt::Kind::If: {
                compile_expr(chunk, st->cond);
                size_t jfalse = chunk.code.size();
                chunk.code.push_back(Instruction{OpCode::JumpIfFalse, 0, "", st->span});
                for (const auto& s : st->then_body) compile_stmt(chunk, s);
                if (st->else_body.empty()) {
                    chunk.code[jfalse].operand = static_cast<int32_t>(chunk.code.size());
                } else {
                    size_t jend = chunk.code.size();
                    chunk.code.push_back(Instruction{OpCode::Jump, 0, "", st->span});
                    chunk.code[jfalse].operand = static_cast<int32_t>(chunk.code.size());
                    for (const auto& s : st->else_body) compile_stmt(chunk, s);
                    chunk.code[jend].operand = static_cast<int32_t>(chunk.code.size());
                }
                break;
            }
            case Stmt::Kind::While: {
                loop_stack_.emplace_back();
                size_t loop_start = chunk.code.size();
                compile_expr(chunk, st->cond);
                size_t jexit = chunk.code.size();
                chunk.code.push_back(Instruction{OpCode::JumpIfFalse, 0, "", st->span});
                for (const auto& s : st->body) compile_stmt(chunk, s);
                for (size_t cj : loop_stack_.back().continue_jumps) {
                    chunk.code[cj].operand = static_cast<int32_t>(loop_start);
                }
                chunk.code.push_back(Instruction{OpCode::Jump, static_cast<int32_t>(loop_start), "", st->span});
                size_t after_loop = chunk.code.size();
                chunk.code[jexit].operand = static_cast<int32_t>(after_loop);
                for (size_t bj : loop_stack_.back().break_jumps) {
                    chunk.code[bj].operand = static_cast<int32_t>(after_loop);
                }
                loop_stack_.pop_back();
                break;
            }
            case Stmt::Kind::For:
            case Stmt::Kind::ParallelFor: {
                loop_stack_.emplace_back();
                size_t lid = ++loop_id_;
                if (st->expr && st->expr->kind == Expr::Kind::Binary &&
                    (st->expr->op == ".." || st->expr->op == "..=")) {
                    std::string cur_tmp = "__for_cur_" + std::to_string(lid);
                    std::string end_tmp = "__for_end_" + std::to_string(lid);
                    std::string done_tmp = "__for_done_" + std::to_string(lid);
                    bool incl = (st->expr->op == "..=");
                    compile_expr(chunk, st->expr->lhs);
                    chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, cur_tmp, st->span});
                    compile_expr(chunk, st->expr->rhs);
                    chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, end_tmp, st->span});
                    int32_t false_c = chunk.add_const(Value::make_bool(false));
                    chunk.code.push_back(Instruction{OpCode::LoadConst, false_c, "", st->span});
                    chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, done_tmp, st->span});

                    size_t loop_top = chunk.code.size();
                    std::string rdesc = "__range_iter_next|" + cur_tmp + "|" + end_tmp + "|" +
                                        done_tmp + "|" + st->name + "|" + (incl ? "1" : "0");
                    chunk.code.push_back(Instruction{OpCode::CallBuiltin, 0, rdesc, st->span});
                    size_t jexit = chunk.code.size();
                    chunk.code.push_back(Instruction{OpCode::JumpIfFalse, 0, "", st->span});

                    for (const auto& s : st->body) compile_stmt(chunk, s);

                    for (size_t cj : loop_stack_.back().continue_jumps) {
                        chunk.code[cj].operand = static_cast<int32_t>(loop_top);
                    }
                    chunk.code.push_back(Instruction{OpCode::Jump, static_cast<int32_t>(loop_top), "", st->span});
                    size_t after_loop = chunk.code.size();
                    chunk.code[jexit].operand = static_cast<int32_t>(after_loop);
                    for (size_t bj : loop_stack_.back().break_jumps) {
                        chunk.code[bj].operand = static_cast<int32_t>(after_loop);
                    }
                    loop_stack_.pop_back();
                    break;
                }
                std::string list_tmp = "__for_list_" + std::to_string(lid);
                std::string idx_tmp = "__for_idx_" + std::to_string(lid);
                std::string len_tmp = "__for_len_" + std::to_string(lid);
                compile_expr(chunk, st->expr);
                chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, list_tmp, st->span});
                chunk.code.push_back(Instruction{OpCode::CallBuiltin, 0, "__len_local|" + list_tmp, st->span});
                chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, len_tmp, st->span});
                int32_t zero_c = chunk.add_const(Value::make_int(0));
                chunk.code.push_back(Instruction{OpCode::LoadConst, zero_c, "", st->span});
                chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, idx_tmp, st->span});

                size_t loop_top = chunk.code.size();
                chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, idx_tmp, st->span});
                chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, len_tmp, st->span});
                chunk.code.push_back(Instruction{OpCode::Lt, 0, "", st->span});
                size_t jexit = chunk.code.size();
                chunk.code.push_back(Instruction{OpCode::JumpIfFalse, 0, "", st->span});

                chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, idx_tmp, st->span});
                chunk.code.push_back(Instruction{OpCode::CallBuiltin, 1, "__index_local|" + list_tmp, st->span});
                chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, st->name, st->span});

                for (const auto& s : st->body) compile_stmt(chunk, s);

                size_t step_ip = chunk.code.size();
                for (size_t cj : loop_stack_.back().continue_jumps) {
                    chunk.code[cj].operand = static_cast<int32_t>(step_ip);
                }
                chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, idx_tmp, st->span});
                int32_t one_c = chunk.add_const(Value::make_int(1));
                chunk.code.push_back(Instruction{OpCode::LoadConst, one_c, "", st->span});
                chunk.code.push_back(Instruction{OpCode::Add, 0, "", st->span});
                chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, idx_tmp, st->span});
                chunk.code.push_back(Instruction{OpCode::Jump, static_cast<int32_t>(loop_top), "", st->span});
                size_t after_loop = chunk.code.size();
                chunk.code[jexit].operand = static_cast<int32_t>(after_loop);
                for (size_t bj : loop_stack_.back().break_jumps) {
                    chunk.code[bj].operand = static_cast<int32_t>(after_loop);
                }
                loop_stack_.pop_back();
                break;
            }
            case Stmt::Kind::Break: {
                if (!loop_stack_.empty()) {
                    size_t j = chunk.code.size();
                    chunk.code.push_back(Instruction{OpCode::Jump, 0, "", st->span});
                    loop_stack_.back().break_jumps.push_back(j);
                }
                break;
            }
            case Stmt::Kind::Continue: {
                if (!loop_stack_.empty()) {
                    size_t j = chunk.code.size();
                    chunk.code.push_back(Instruction{OpCode::Jump, 0, "", st->span});
                    loop_stack_.back().continue_jumps.push_back(j);
                }
                break;
            }
            case Stmt::Kind::Return:
                if (st->expr) compile_expr(chunk, st->expr);
                else {
                    int32_t c = chunk.add_const(Value::make_unit());
                    chunk.code.push_back(Instruction{OpCode::LoadConst, c, "", st->span});
                }
                chunk.code.push_back(Instruction{OpCode::Return, 0, "", st->span});
                break;
            case Stmt::Kind::Expr:
                compile_expr(chunk, st->expr);
                chunk.code.push_back(Instruction{OpCode::Pop, 0, "", st->span});
                break;
            default:
                break;
        }
    }

    void compile_expr(BytecodeChunk& chunk, const ExprPtr& e) {
        if (!e) return;
        switch (e->kind) {
            case Expr::Kind::IntLit: {
                int32_t c = chunk.add_const(Value::make_int(e->int_val));
                chunk.code.push_back(Instruction{OpCode::LoadConst, c, "", e->span});
                break;
            }
            case Expr::Kind::FloatLit: {
                int32_t c = chunk.add_const(Value::make_float(e->float_val));
                chunk.code.push_back(Instruction{OpCode::LoadConst, c, "", e->span});
                break;
            }
            case Expr::Kind::BoolLit: {
                int32_t c = chunk.add_const(Value::make_bool(e->bool_val));
                chunk.code.push_back(Instruction{OpCode::LoadConst, c, "", e->span});
                break;
            }
            case Expr::Kind::StrLit: {
                int32_t c = chunk.add_const(Value::make_str(e->text));
                chunk.code.push_back(Instruction{OpCode::LoadConst, c, "", e->span});
                break;
            }
            case Expr::Kind::Ident:
                chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, e->text, e->span});
                break;
            case Expr::Kind::Unary:
                compile_expr(chunk, e->lhs);
                if (e->op == "-") chunk.code.push_back(Instruction{OpCode::Neg, 0, "", e->span});
                else if (e->op == "!") chunk.code.push_back(Instruction{OpCode::Not, 0, "", e->span});
                break;
            case Expr::Kind::Binary:
                if (e->op == "&&") {
                    compile_expr(chunk, e->lhs);
                    size_t jfalse = chunk.code.size();
                    chunk.code.push_back(Instruction{OpCode::JumpIfFalse, 0, "", e->span});
                    compile_expr(chunk, e->rhs);
                    size_t jend = chunk.code.size();
                    chunk.code.push_back(Instruction{OpCode::Jump, 0, "", e->span});
                    chunk.code[jfalse].operand = static_cast<int32_t>(chunk.code.size());
                    int32_t fc = chunk.add_const(Value::make_bool(false));
                    chunk.code.push_back(Instruction{OpCode::LoadConst, fc, "", e->span});
                    chunk.code[jend].operand = static_cast<int32_t>(chunk.code.size());
                    break;
                }
                if (e->op == "||") {
                    compile_expr(chunk, e->lhs);
                    chunk.code.push_back(Instruction{OpCode::Not, 0, "", e->span});
                    size_t jfalse = chunk.code.size();
                    chunk.code.push_back(Instruction{OpCode::JumpIfFalse, 0, "", e->span});
                    compile_expr(chunk, e->rhs);
                    size_t jend = chunk.code.size();
                    chunk.code.push_back(Instruction{OpCode::Jump, 0, "", e->span});
                    chunk.code[jfalse].operand = static_cast<int32_t>(chunk.code.size());
                    int32_t tc = chunk.add_const(Value::make_bool(true));
                    chunk.code.push_back(Instruction{OpCode::LoadConst, tc, "", e->span});
                    chunk.code[jend].operand = static_cast<int32_t>(chunk.code.size());
                    break;
                }
                compile_expr(chunk, e->lhs);
                compile_expr(chunk, e->rhs);
                if (e->op == "+") chunk.code.push_back(Instruction{OpCode::Add, 0, "", e->span});
                else if (e->op == "-") chunk.code.push_back(Instruction{OpCode::Sub, 0, "", e->span});
                else if (e->op == "*") chunk.code.push_back(Instruction{OpCode::Mul, 0, "", e->span});
                else if (e->op == "/") chunk.code.push_back(Instruction{OpCode::Div, 0, "", e->span});
                else if (e->op == "%") chunk.code.push_back(Instruction{OpCode::Mod, 0, "", e->span});
                else if (e->op == "==") chunk.code.push_back(Instruction{OpCode::Eq, 0, "", e->span});
                else if (e->op == "!=") chunk.code.push_back(Instruction{OpCode::Ne, 0, "", e->span});
                else if (e->op == "<") chunk.code.push_back(Instruction{OpCode::Lt, 0, "", e->span});
                else if (e->op == "<=") chunk.code.push_back(Instruction{OpCode::Le, 0, "", e->span});
                else if (e->op == ">") chunk.code.push_back(Instruction{OpCode::Gt, 0, "", e->span});
                else if (e->op == ">=") chunk.code.push_back(Instruction{OpCode::Ge, 0, "", e->span});
                else if (e->op == "..") chunk.code.push_back(Instruction{OpCode::RangeExcl, 0, "", e->span});
                else if (e->op == "..=") chunk.code.push_back(Instruction{OpCode::RangeIncl, 0, "", e->span});
                break;
            case Expr::Kind::ListLit:
                for (const auto& a : e->args) compile_expr(chunk, a);
                chunk.code.push_back(Instruction{OpCode::MakeList, static_cast<int32_t>(e->args.size()), "", e->span});
                break;
            case Expr::Kind::MapLit:
                for (const auto& kv : e->map_entries) {
                    compile_expr(chunk, kv.first);
                    compile_expr(chunk, kv.second);
                }
                chunk.code.push_back(Instruction{OpCode::MakeMap, static_cast<int32_t>(e->map_entries.size()), "", e->span});
                break;
            case Expr::Kind::StructInit:
                for (const auto& sf : e->struct_fields) {
                    int32_t kc = chunk.add_const(Value::make_str(sf.first));
                    chunk.code.push_back(Instruction{OpCode::LoadConst, kc, "", e->span});
                    compile_expr(chunk, sf.second);
                }
                chunk.code.push_back(Instruction{
                    OpCode::MakeStruct, static_cast<int32_t>(e->struct_fields.size()), e->text, e->span
                });
                break;
            case Expr::Kind::FieldAccess:
                if (e->lhs->kind == Expr::Kind::Ident) {
                    chunk.code.push_back(Instruction{
                        OpCode::CallBuiltin, 0, "__field_local|" + e->lhs->text + "|" + e->text, e->span
                    });
                } else {
                    compile_expr(chunk, e->lhs);
                    chunk.code.push_back(Instruction{OpCode::FieldGet, 0, e->text, e->span});
                }
                break;
            case Expr::Kind::Index:
                if (e->lhs->kind == Expr::Kind::Ident) {
                    compile_expr(chunk, e->rhs);
                    chunk.code.push_back(Instruction{
                        OpCode::CallBuiltin, 1, "__index_local|" + e->lhs->text, e->span
                    });
                } else {
                    compile_expr(chunk, e->lhs);
                    compile_expr(chunk, e->rhs);
                    chunk.code.push_back(Instruction{OpCode::IndexGet, 0, "", e->span});
                }
                break;
            case Expr::Kind::EnumVariant: {
                for (const auto& a : e->args) compile_expr(chunk, a);
                chunk.code.push_back(Instruction{
                    OpCode::CallBuiltin,
                    static_cast<int32_t>(e->args.size()),
                    "__make_enum|" + e->enum_name + "|" + e->variant_name,
                    e->span
                });
                break;
            }
            case Expr::Kind::Match: {
                std::string mtmp = "__match_val_" + std::to_string(++loop_id_);
                compile_expr(chunk, e->lhs);
                chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, mtmp, e->span});
                std::vector<size_t> end_jumps;
                for (const auto& arm : e->match_arms) {
                    size_t j_next = std::numeric_limits<size_t>::max();
                    if (arm.is_wildcard) {
                        // always matches
                    } else if (arm.is_binding) {
                        chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, mtmp, arm.span});
                        chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, arm.binding_name, arm.span});
                    } else if (arm.is_enum_variant) {
                        chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, mtmp, arm.span});
                        chunk.code.push_back(Instruction{
                            OpCode::CallBuiltin, 1,
                            "__match_enum|" + arm.pattern_enum + "|" + arm.pattern_variant,
                            arm.span
                        });
                        j_next = chunk.code.size();
                        chunk.code.push_back(Instruction{OpCode::JumpIfFalse, 0, "", arm.span});
                        if (!arm.binding_name.empty()) {
                            chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, mtmp, arm.span});
                            chunk.code.push_back(Instruction{OpCode::CallBuiltin, 1, "__enum_payload_0", arm.span});
                            chunk.code.push_back(Instruction{OpCode::StoreLocal, 0, arm.binding_name, arm.span});
                        }
                    } else if (arm.literal_expr) {
                        chunk.code.push_back(Instruction{OpCode::LoadLocal, 0, mtmp, arm.span});
                        compile_expr(chunk, arm.literal_expr);
                        chunk.code.push_back(Instruction{OpCode::Eq, 0, "", arm.span});
                        j_next = chunk.code.size();
                        chunk.code.push_back(Instruction{OpCode::JumpIfFalse, 0, "", arm.span});
                    }
                    if (arm.body_expr) {
                        compile_expr(chunk, arm.body_expr);
                    } else {
                        for (const auto& st : arm.body_stmts) compile_stmt(chunk, st);
                        int32_t u = chunk.add_const(Value::make_unit());
                        chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", arm.span});
                    }
                    end_jumps.push_back(chunk.code.size());
                    chunk.code.push_back(Instruction{OpCode::Jump, 0, "", arm.span});
                    if (j_next != std::numeric_limits<size_t>::max()) {
                        chunk.code[j_next].operand = static_cast<int32_t>(chunk.code.size());
                    }
                }
                int32_t u = chunk.add_const(Value::make_unit());
                chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", e->span});
                size_t match_end = chunk.code.size();
                for (size_t ej : end_jumps) {
                    chunk.code[ej].operand = static_cast<int32_t>(match_end);
                }
                break;
            }
            case Expr::Kind::Lambda: {
                pending_lambdas_.push_back(e);
                std::string lname = "__lambda_" + std::to_string(e->lambda_id);
                chunk.code.push_back(Instruction{OpCode::MakeClosure, 0, lname, e->span});
                break;
            }
            case Expr::Kind::Call: {
                if ((e->text == "push" || e->text == "std::collections.push") &&
                    e->args.size() == 2 && e->args[0]->kind == Expr::Kind::Ident) {
                    compile_expr(chunk, e->args[1]);
                    chunk.code.push_back(Instruction{
                        OpCode::CallBuiltin, 1, "__push_local|" + e->args[0]->text, e->span
                    });
                    int32_t u = chunk.add_const(Value::make_unit());
                    chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", e->span});
                    break;
                }
                if ((e->text == "pop" || e->text == "std::collections.pop") &&
                    e->args.size() == 1 && e->args[0]->kind == Expr::Kind::Ident) {
                    chunk.code.push_back(Instruction{
                        OpCode::CallBuiltin, 0, "__pop_local|" + e->args[0]->text, e->span
                    });
                    break;
                }
                if ((e->text == "remove_key" || e->text == "std::collections.remove_key" || e->text == "std::collections.remove") &&
                    e->args.size() == 2 && e->args[0]->kind == Expr::Kind::Ident) {
                    compile_expr(chunk, e->args[1]);
                    chunk.code.push_back(Instruction{
                        OpCode::CallBuiltin, 1, "__remove_key_local|" + e->args[0]->text, e->span
                    });
                    break;
                }
                if ((e->text == "len" || e->text == "std::collections.len" || e->text == "std::string.len") &&
                    e->args.size() == 1 && e->args[0]->kind == Expr::Kind::Ident) {
                    chunk.code.push_back(Instruction{
                        OpCode::CallBuiltin, 0, "__len_local|" + e->args[0]->text, e->span
                    });
                    break;
                }
                for (const auto& a : e->args) compile_expr(chunk, a);
                if (e->text == "print" || e->text == "println" || e->text == "std::io.println") {
                    chunk.code.push_back(Instruction{OpCode::Print, static_cast<int32_t>(e->args.size()), "", e->span});
                    int32_t u = chunk.add_const(Value::make_unit());
                    chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", e->span});
                } else if (e->text == "print_inline" || e->text == "std::io.print_inline") {
                    chunk.code.push_back(Instruction{OpCode::PrintInline, static_cast<int32_t>(e->args.size()), "", e->span});
                    int32_t u = chunk.add_const(Value::make_unit());
                    chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", e->span});
                } else if (e->text == "input" || e->text == "std::io.input" || e->text == "std::io.read_line") {
                    chunk.code.push_back(Instruction{OpCode::InputLine, static_cast<int32_t>(e->args.size()), "", e->span});
                } else if (e->text == "sleep" || e->text == "std::time.sleep_ms") {
                    chunk.code.push_back(Instruction{OpCode::SleepMs, 0, "", e->span});
                    int32_t u = chunk.add_const(Value::make_unit());
                    chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", e->span});
                } else if (e->text == "now_ms" || e->text == "std::time.monotonic_ms") {
                    chunk.code.push_back(Instruction{OpCode::NowMs, 0, "", e->span});
                } else if (user_fns_.count(e->text)) {
                    chunk.code.push_back(Instruction{
                        OpCode::CallFn, static_cast<int32_t>(e->args.size()), e->text, e->span
                    });
                } else {
                    chunk.code.push_back(Instruction{
                        OpCode::CallBuiltin, static_cast<int32_t>(e->args.size()), e->text, e->span
                    });
                }
                break;
            }
            default: {
                int32_t u = chunk.add_const(Value::make_unit());
                chunk.code.push_back(Instruction{OpCode::LoadConst, u, "", e->span});
                break;
            }
        }
    }

    std::unordered_set<std::string> user_fns_;
    std::vector<ExprPtr> pending_lambdas_;
    std::vector<LoopPatchCtx> loop_stack_;
    size_t loop_id_ = 0;
};

class BytecodeVM {
public:
    explicit BytecodeVM(std::string filename) : filename_(std::move(filename)) {}

    void verify_chunk(const BytecodeChunk& chunk) const {
        for (size_t i = 0; i < chunk.code.size(); ++i) {
            const auto& ins = chunk.code[i];
            if (ins.op == OpCode::LoadConst) {
                if (ins.operand < 0 || static_cast<size_t>(ins.operand) >= chunk.constants.size()) {
                    throw RheaException(make_diag("E2001", "bytecode verification failed: invalid constant index", filename_, ins.span));
                }
            } else if (ins.op == OpCode::Jump || ins.op == OpCode::JumpIfFalse) {
                if (ins.operand < 0 || static_cast<size_t>(ins.operand) >= chunk.code.size()) {
                    throw RheaException(make_diag("E2001", "bytecode verification failed: jump target out of bounds", filename_, ins.span));
                }
            } else if (ins.op == OpCode::CallFn) {
                if (chunk.functions.find(ins.sym) == chunk.functions.end()) {
                    throw RheaException(make_diag("E2001", "bytecode verification failed: unknown function target '" + ins.sym + "'", filename_, ins.span));
                }
            }
        }
    }

    Value execute(const BytecodeChunk& chunk) {
        verify_chunk(chunk);
        struct CallFrame {
            size_t return_ip = 0;
            std::unordered_map<std::string, Value> saved_locals;
        };

        std::vector<Value> stack;
        std::unordered_map<std::string, Value> locals;
        std::unordered_map<std::string, Value> globals;
        std::vector<CallFrame> call_stack;
        size_t ip = 0;
        size_t steps = 0;

        auto pop = [&]() -> Value {
            if (stack.empty()) return Value::make_unit();
            Value v = std::move(stack.back());
            stack.pop_back();
            return v;
        };

        auto invoke_vm_fn = [&](const std::string& fn_name, std::vector<Value>& args,
                                const std::unordered_map<std::string, Value>* captured, Span sp) {
            if (call_stack.size() >= MAX_RECURSION_DEPTH) {
                throw RheaException(make_diag(
                    "E1004", "maximum recursion depth (" + std::to_string(MAX_RECURSION_DEPTH) + ") exceeded",
                    filename_, sp
                ));
            }
            auto fit = chunk.functions.find(fn_name);
            if (fit == chunk.functions.end()) {
                throw RheaException(make_diag("E0004", "unknown VM function '" + fn_name + "'", filename_, sp));
            }
            call_stack.push_back(CallFrame{ip, std::move(locals)});
            locals.clear();
            if (captured) {
                for (const auto& kv : *captured) locals[kv.first] = kv.second;
            }
            const auto& params = fit->second.params;
            for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
                locals[params[i]] = std::move(args[i]);
            }
            ip = fit->second.entry_ip;
        };

        auto lookup_vm_var = [&](const std::string& sym) -> const Value* {
            auto it = locals.find(sym);
            if (it != locals.end()) return &it->second;
            auto git = globals.find(sym);
            if (git != globals.end()) return &git->second;
            return nullptr;
        };

        while (ip < chunk.code.size()) {
            if (++steps > MAX_VM_STEPS) {
                throw RheaException(make_diag("E1004", "VM instruction step limit exceeded", filename_, chunk.code[ip].span));
            }
            const auto& ins = chunk.code[ip++];
            switch (ins.op) {
                case OpCode::LoadConst:
                    stack.push_back(chunk.constants[ins.operand]);
                    break;
                case OpCode::LoadLocal: {
                    const Value* vp = lookup_vm_var(ins.sym);
                    stack.push_back(vp ? *vp : Value::make_unit());
                    break;
                }
                case OpCode::StoreLocal:
                    locals[ins.sym] = pop();
                    break;
                case OpCode::Add: {
                    Value r = pop(), l = pop();
                    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                        stack.push_back(Value::make_int(checked_add_i64(l.int_val, r.int_val, filename_, ins.span)));
                    } else if (l.kind == Value::Kind::Str && r.kind == Value::Kind::Str) {
                        stack.push_back(Value::make_str(l.str_val + r.str_val));
                    } else {
                        double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
                        double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
                        stack.push_back(Value::make_float(a + b));
                    }
                    break;
                }
                case OpCode::Sub: {
                    Value r = pop(), l = pop();
                    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                        stack.push_back(Value::make_int(checked_sub_i64(l.int_val, r.int_val, filename_, ins.span)));
                    } else {
                        double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
                        double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
                        stack.push_back(Value::make_float(a - b));
                    }
                    break;
                }
                case OpCode::Mul: {
                    Value r = pop(), l = pop();
                    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                        stack.push_back(Value::make_int(checked_mul_i64(l.int_val, r.int_val, filename_, ins.span)));
                    } else {
                        double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
                        double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
                        stack.push_back(Value::make_float(a * b));
                    }
                    break;
                }
                case OpCode::Div: {
                    Value r = pop(), l = pop();
                    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                        stack.push_back(Value::make_int(checked_div_i64(l.int_val, r.int_val, filename_, ins.span)));
                    } else {
                        double a = (l.kind == Value::Kind::Float) ? l.float_val : static_cast<double>(l.int_val);
                        double b = (r.kind == Value::Kind::Float) ? r.float_val : static_cast<double>(r.int_val);
                        if (std::fabs(b) == 0.0) throw RheaException(make_diag("E1001", "division by zero", filename_, ins.span));
                        stack.push_back(Value::make_float(a / b));
                    }
                    break;
                }
                case OpCode::Mod: {
                    Value r = pop(), l = pop();
                    stack.push_back(Value::make_int(checked_mod_i64(l.int_val, r.int_val, filename_, ins.span)));
                    break;
                }
                case OpCode::Neg: {
                    Value v = pop();
                    if (v.kind == Value::Kind::Int) {
                        stack.push_back(Value::make_int(checked_sub_i64(0, v.int_val, filename_, ins.span)));
                    } else {
                        stack.push_back(Value::make_float(-v.float_val));
                    }
                    break;
                }
                case OpCode::Not: {
                    Value v = pop();
                    stack.push_back(Value::make_bool(!v.bool_val));
                    break;
                }
                case OpCode::Eq: { Value r = pop(), l = pop(); stack.push_back(Value::make_bool(values_equal(l, r))); break; }
                case OpCode::Ne: { Value r = pop(), l = pop(); stack.push_back(Value::make_bool(!values_equal(l, r))); break; }
                case OpCode::Lt: {
                    Value r = pop(), l = pop();
                    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) stack.push_back(Value::make_bool(l.int_val < r.int_val));
                    else if (l.kind == Value::Kind::Str && r.kind == Value::Kind::Str) stack.push_back(Value::make_bool(l.str_val < r.str_val));
                    else stack.push_back(Value::make_bool(l.float_val < r.float_val));
                    break;
                }
                case OpCode::Le: {
                    Value r = pop(), l = pop();
                    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) stack.push_back(Value::make_bool(l.int_val <= r.int_val));
                    else if (l.kind == Value::Kind::Str && r.kind == Value::Kind::Str) stack.push_back(Value::make_bool(l.str_val <= r.str_val));
                    else stack.push_back(Value::make_bool(l.float_val <= r.float_val));
                    break;
                }
                case OpCode::Gt: {
                    Value r = pop(), l = pop();
                    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) stack.push_back(Value::make_bool(l.int_val > r.int_val));
                    else if (l.kind == Value::Kind::Str && r.kind == Value::Kind::Str) stack.push_back(Value::make_bool(l.str_val > r.str_val));
                    else stack.push_back(Value::make_bool(l.float_val > r.float_val));
                    break;
                }
                case OpCode::Ge: {
                    Value r = pop(), l = pop();
                    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) stack.push_back(Value::make_bool(l.int_val >= r.int_val));
                    else if (l.kind == Value::Kind::Str && r.kind == Value::Kind::Str) stack.push_back(Value::make_bool(l.str_val >= r.str_val));
                    else stack.push_back(Value::make_bool(l.float_val >= r.float_val));
                    break;
                }
                case OpCode::RangeExcl: {
                    Value r = pop(), l = pop();
                    stack.push_back(build_range_value(l.int_val, r.int_val, false, filename_, ins.span));
                    break;
                }
                case OpCode::RangeIncl: {
                    Value r = pop(), l = pop();
                    stack.push_back(build_range_value(l.int_val, r.int_val, true, filename_, ins.span));
                    break;
                }
                case OpCode::MakeList: {
                    std::vector<Value> items(ins.operand);
                    for (int i = ins.operand - 1; i >= 0; --i) items[i] = pop();
                    stack.push_back(Value::make_list(std::move(items)));
                    break;
                }
                case OpCode::MakeMap: {
                    std::map<std::string, Value> m;
                    std::vector<std::pair<std::string, Value>> pairs(ins.operand);
                    for (int i = ins.operand - 1; i >= 0; --i) {
                        Value v = pop();
                        Value k = pop();
                        pairs[i] = {k.to_string(), std::move(v)};
                    }
                    for (auto& kv : pairs) m[kv.first] = std::move(kv.second);
                    stack.push_back(Value::make_map(std::move(m)));
                    break;
                }
                case OpCode::MakeStruct: {
                    Value s;
                    s.kind = Value::Kind::Struct;
                    s.type_tag = ins.sym;
                    for (int i = 0; i < ins.operand; ++i) {
                        Value fv = pop();
                        Value fk = pop();
                        s.map_val[fk.to_string()] = std::move(fv);
                    }
                    stack.push_back(std::move(s));
                    break;
                }
                case OpCode::FieldGet: {
                    Value obj = pop();
                    stack.push_back(obj.map_val[ins.sym]);
                    break;
                }
                case OpCode::IndexGet: {
                    Value idx = pop(), container = pop();
                    if (container.kind == Value::Kind::List) {
                        if (idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= container.list_val.size()) {
                            throw RheaException(make_diag("E1003", "list index out of bounds", filename_, ins.span));
                        }
                        stack.push_back(container.list_val[static_cast<size_t>(idx.int_val)]);
                    } else if (container.kind == Value::Kind::Map) {
                        auto it = container.map_val.find(idx.to_string());
                        if (it == container.map_val.end()) {
                            throw RheaException(make_diag("E1003", "key not found in map", filename_, ins.span));
                        }
                        stack.push_back(it->second);
                    }
                    break;
                }
                case OpCode::FieldSet: {
                    Value val = pop();
                    Value obj = pop();
                    obj.map_val[ins.sym] = std::move(val);
                    stack.push_back(std::move(obj));
                    break;
                }
                case OpCode::IndexSet: {
                    Value val = pop();
                    Value idx = pop();
                    Value container = pop();
                    if (container.kind == Value::Kind::List) {
                        if (idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= container.list_val.size()) {
                            throw RheaException(make_diag("E1003", "list index out of bounds", filename_, ins.span));
                        }
                        container.list_val[static_cast<size_t>(idx.int_val)] = std::move(val);
                    } else if (container.kind == Value::Kind::Map) {
                        container.map_val[idx.to_string()] = std::move(val);
                    }
                    stack.push_back(std::move(container));
                    break;
                }
                case OpCode::MakeClosure: {
                    Value cl;
                    cl.kind = Value::Kind::Closure;
                    cl.closure_val = std::make_shared<ClosureValue>();
                    cl.closure_val->vm_fn_name = ins.sym;
                    cl.closure_val->captured = globals;
                    for (const auto& kv : locals) cl.closure_val->captured[kv.first] = kv.second;
                    stack.push_back(std::move(cl));
                    break;
                }
                case OpCode::Print:
                case OpCode::PrintInline: {
                    std::vector<Value> args(ins.operand);
                    for (int i = ins.operand - 1; i >= 0; --i) args[i] = pop();
                    for (int i = 0; i < ins.operand; ++i) {
                        if (i > 0) std::cout << " ";
                        std::cout << args[i].to_string();
                    }
                    if (ins.op == OpCode::Print) std::cout << "\n";
                    std::cout << std::flush;
                    break;
                }
                case OpCode::InputLine: {
                    std::string prompt;
                    if (ins.operand > 0) prompt = pop().to_string();
                    stack.push_back(Value::make_str(runtime_read_line(prompt)));
                    break;
                }
                case OpCode::SleepMs: {
                    Value v = pop();
                    runtime_sleep_ms(v.int_val, filename_, ins.span);
                    break;
                }
                case OpCode::NowMs:
                    stack.push_back(Value::make_int(runtime_monotonic_ms()));
                    break;
                case OpCode::Jump:
                    ip = static_cast<size_t>(ins.operand);
                    break;
                case OpCode::JumpIfFalse: {
                    Value c = pop();
                    if (!c.bool_val) ip = static_cast<size_t>(ins.operand);
                    break;
                }
                case OpCode::CallFn: {
                    std::vector<Value> args(ins.operand);
                    for (int i = ins.operand - 1; i >= 0; --i) args[i] = pop();
                    invoke_vm_fn(ins.sym, args, nullptr, ins.span);
                    break;
                }
                case OpCode::CallBuiltin: {
                    std::vector<Value> args(ins.operand);
                    for (int i = ins.operand - 1; i >= 0; --i) args[i] = pop();

                    // Check if `ins.sym` is actually a closure variable in `locals`!
                    auto cl_it = locals.find(ins.sym);
                    if (cl_it != locals.end() && cl_it->second.kind == Value::Kind::Closure && cl_it->second.closure_val) {
                        std::shared_ptr<ClosureValue> cval = cl_it->second.closure_val;
                        invoke_vm_fn(cval->vm_fn_name, args, &cval->captured, ins.span);
                        break;
                    }

                    if (ins.sym == "__snapshot_globals") {
                        globals = locals;
                        stack.push_back(Value::make_unit());
                    } else if (ins.sym.rfind("__range_iter_next|", 0) == 0) {
                        // Format: __range_iter_next|<cur>|<end>|<done>|<loop_var>|<incl>
                        size_t p1 = ins.sym.find('|');
                        size_t p2 = ins.sym.find('|', p1 + 1);
                        size_t p3 = ins.sym.find('|', p2 + 1);
                        size_t p4 = ins.sym.find('|', p3 + 1);
                        size_t p5 = ins.sym.find('|', p4 + 1);
                        std::string cur_k = ins.sym.substr(p1 + 1, p2 - p1 - 1);
                        std::string end_k = ins.sym.substr(p2 + 1, p3 - p2 - 1);
                        std::string done_k = ins.sym.substr(p3 + 1, p4 - p3 - 1);
                        std::string var_k = ins.sym.substr(p4 + 1, p5 - p4 - 1);
                        bool incl = (ins.sym.substr(p5 + 1) == "1");
                        Value& cur_v = locals[cur_k];
                        const Value& end_v = locals[end_k];
                        Value& done_v = locals[done_k];
                        if (done_v.bool_val || (incl ? (cur_v.int_val > end_v.int_val) : (cur_v.int_val >= end_v.int_val))) {
                            stack.push_back(Value::make_bool(false));
                        } else {
                            locals[var_k] = Value::make_int(cur_v.int_val);
                            if (incl && cur_v.int_val == end_v.int_val) {
                                done_v.bool_val = true;
                            } else {
                                cur_v.int_val += 1;
                            }
                            stack.push_back(Value::make_bool(true));
                        }
                    } else if (ins.sym.rfind("__push_local|", 0) == 0) {
                        std::string vname = ins.sym.substr(13);
                        locals[vname].list_val.push_back(std::move(args[0]));
                    } else if (ins.sym.rfind("__pop_local|", 0) == 0) {
                        std::string vname = ins.sym.substr(12);
                        Value& lv = locals[vname];
                        if (lv.list_val.empty()) {
                            throw RheaException(make_diag("E1003", "cannot pop() from an empty list", filename_, ins.span));
                        }
                        Value back = std::move(lv.list_val.back());
                        lv.list_val.pop_back();
                        stack.push_back(std::move(back));
                    } else if (ins.sym.rfind("__remove_key_local|", 0) == 0) {
                        std::string vname = ins.sym.substr(19);
                        bool removed = locals[vname].map_val.erase(args[0].to_string()) > 0;
                        stack.push_back(Value::make_bool(removed));
                    } else if (ins.sym.rfind("__len_local|", 0) == 0) {
                        std::string vname = ins.sym.substr(12);
                        const Value* vp = lookup_vm_var(vname);
                        if (!vp) stack.push_back(Value::make_int(0));
                        else if (vp->kind == Value::Kind::List) stack.push_back(Value::make_int(static_cast<int64_t>(vp->list_val.size())));
                        else if (vp->kind == Value::Kind::Str) stack.push_back(Value::make_int(static_cast<int64_t>(vp->str_val.size())));
                        else if (vp->kind == Value::Kind::Map) stack.push_back(Value::make_int(static_cast<int64_t>(vp->map_val.size())));
                        else stack.push_back(Value::make_int(0));
                    } else if (ins.sym.rfind("__index_local|", 0) == 0) {
                        std::string vname = ins.sym.substr(14);
                        const Value* vp = lookup_vm_var(vname);
                        const Value& idx = args[0];
                        if (!vp) {
                            throw RheaException(make_diag("E0004", "undefined variable '" + vname + "'", filename_, ins.span));
                        }
                        if (vp->kind == Value::Kind::List) {
                            if (idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= vp->list_val.size()) {
                                throw RheaException(make_diag("E1003", "list index out of bounds", filename_, ins.span));
                            }
                            stack.push_back(vp->list_val[static_cast<size_t>(idx.int_val)]);
                        } else if (vp->kind == Value::Kind::Map) {
                            auto it = vp->map_val.find(idx.to_string());
                            if (it == vp->map_val.end()) {
                                throw RheaException(make_diag("E1003", "key not found in map", filename_, ins.span));
                            }
                            stack.push_back(it->second);
                        } else {
                            throw RheaException(make_diag("E0003", "type is not indexable", filename_, ins.span));
                        }
                    } else if (ins.sym.rfind("__field_local|", 0) == 0) {
                        size_t p1 = ins.sym.find('|');
                        size_t p2 = ins.sym.find('|', p1 + 1);
                        std::string vname = ins.sym.substr(p1 + 1, p2 - p1 - 1);
                        std::string fname = ins.sym.substr(p2 + 1);
                        const Value* vp = lookup_vm_var(vname);
                        if (!vp) {
                            throw RheaException(make_diag("E0004", "undefined variable '" + vname + "'", filename_, ins.span));
                        }
                        auto it = vp->map_val.find(fname);
                        if (it != vp->map_val.end()) stack.push_back(it->second);
                        else stack.push_back(Value::make_unit());
                    } else if (ins.sym.rfind("__make_enum|", 0) == 0) {
                        size_t p1 = ins.sym.find('|');
                        size_t p2 = ins.sym.find('|', p1 + 1);
                        Value ev;
                        ev.kind = Value::Kind::Enum;
                        ev.type_tag = ins.sym.substr(p1 + 1, p2 - p1 - 1);
                        ev.variant_tag = ins.sym.substr(p2 + 1);
                        ev.list_val = std::move(args);
                        stack.push_back(std::move(ev));
                    } else if (ins.sym.rfind("__match_enum|", 0) == 0) {
                        size_t p1 = ins.sym.find('|');
                        size_t p2 = ins.sym.find('|', p1 + 1);
                        std::string ename = ins.sym.substr(p1 + 1, p2 - p1 - 1);
                        std::string vname = ins.sym.substr(p2 + 1);
                        const Value& target = args[0];
                        bool matched = (target.kind == Value::Kind::Enum &&
                                        target.type_tag == ename &&
                                        target.variant_tag == vname);
                        stack.push_back(Value::make_bool(matched));
                    } else if (ins.sym == "__enum_payload_0") {
                        const Value& target = args[0];
                        if (target.kind == Value::Kind::Enum && !target.list_val.empty()) {
                            stack.push_back(target.list_val[0]);
                        } else {
                            stack.push_back(Value::make_unit());
                        }
                    } else if (ins.sym.rfind("__lvalue_set|", 0) == 0) {
                        // Format: __lvalue_set|<root_name>|<assign_op>|<kinds>
                        size_t p1 = ins.sym.find('|');
                        size_t p2 = ins.sym.find('|', p1 + 1);
                        size_t p3 = ins.sym.find('|', p2 + 1);
                        std::string root_name = ins.sym.substr(p1 + 1, p2 - p1 - 1);
                        std::string aop = ins.sym.substr(p2 + 1, p3 - p2 - 1);
                        std::string kinds = ins.sym.substr(p3 + 1);
                        Value* cur_val = &locals[root_name];
                        for (size_t k = 0; k < kinds.size(); ++k) {
                            bool is_last = (k + 1 == kinds.size());
                            if (kinds[k] == 'F') {
                                cur_val = &cur_val->map_val[args[k].to_string()];
                            } else {
                                const Value& idx = args[k];
                                if (cur_val->kind == Value::Kind::List) {
                                    if (idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= cur_val->list_val.size()) {
                                        throw RheaException(make_diag("E1003", "list index out of bounds", filename_, ins.span));
                                    }
                                    cur_val = &cur_val->list_val[static_cast<size_t>(idx.int_val)];
                                } else if (cur_val->kind == Value::Kind::Map) {
                                    std::string key = idx.to_string();
                                    if (!is_last || aop != "=") {
                                        auto it = cur_val->map_val.find(key);
                                        if (it == cur_val->map_val.end()) {
                                            throw RheaException(make_diag("E1003", "key '" + key + "' not found in map", filename_, ins.span));
                                        }
                                        cur_val = &it->second;
                                    } else {
                                        cur_val = &cur_val->map_val[key];
                                    }
                                }
                            }
                        }
                        const Value& rhs = args.back();
                        if (aop == "=") {
                            *cur_val = rhs;
                        } else if (aop == "+=") {
                            if (cur_val->kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
                                *cur_val = Value::make_int(checked_add_i64(cur_val->int_val, rhs.int_val, filename_, ins.span));
                            } else if (cur_val->kind == Value::Kind::Str && rhs.kind == Value::Kind::Str) {
                                *cur_val = Value::make_str(cur_val->str_val + rhs.str_val);
                            } else {
                                double a = (cur_val->kind == Value::Kind::Float) ? cur_val->float_val : static_cast<double>(cur_val->int_val);
                                double b = (rhs.kind == Value::Kind::Float) ? rhs.float_val : static_cast<double>(rhs.int_val);
                                *cur_val = Value::make_float(a + b);
                            }
                        } else if (aop == "-=") {
                            if (cur_val->kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
                                *cur_val = Value::make_int(checked_sub_i64(cur_val->int_val, rhs.int_val, filename_, ins.span));
                            } else {
                                double a = (cur_val->kind == Value::Kind::Float) ? cur_val->float_val : static_cast<double>(cur_val->int_val);
                                double b = (rhs.kind == Value::Kind::Float) ? rhs.float_val : static_cast<double>(rhs.int_val);
                                *cur_val = Value::make_float(a - b);
                            }
                        } else if (aop == "*=") {
                            if (cur_val->kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
                                *cur_val = Value::make_int(checked_mul_i64(cur_val->int_val, rhs.int_val, filename_, ins.span));
                            } else {
                                double a = (cur_val->kind == Value::Kind::Float) ? cur_val->float_val : static_cast<double>(cur_val->int_val);
                                double b = (rhs.kind == Value::Kind::Float) ? rhs.float_val : static_cast<double>(rhs.int_val);
                                *cur_val = Value::make_float(a * b);
                            }
                        } else if (aop == "/=") {
                            if (cur_val->kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
                                *cur_val = Value::make_int(checked_div_i64(cur_val->int_val, rhs.int_val, filename_, ins.span));
                            } else {
                                double a = (cur_val->kind == Value::Kind::Float) ? cur_val->float_val : static_cast<double>(cur_val->int_val);
                                double b = (rhs.kind == Value::Kind::Float) ? rhs.float_val : static_cast<double>(rhs.int_val);
                                if (std::fabs(b) == 0.0) throw RheaException(make_diag("E1001", "division by zero", filename_, ins.span));
                                *cur_val = Value::make_float(a / b);
                            }
                        }
                        stack.push_back(Value::make_unit());
                    } else if (ins.sym == "__push_val") {
                        Value list_v = std::move(args[0]);
                        list_v.list_val.push_back(std::move(args[1]));
                        stack.push_back(std::move(list_v));
                    } else if (ins.sym == "__pop_val") {
                        Value list_v = std::move(args[0]);
                        if (list_v.list_val.empty()) {
                            throw RheaException(make_diag("E1003", "pop() on empty list", filename_, ins.span));
                        }
                        Value last = std::move(list_v.list_val.back());
                        list_v.list_val.pop_back();
                        stack.push_back(std::move(last));
                        stack.push_back(std::move(list_v));
                    } else if (ins.sym == "__remove_key_val") {
                        Value map_v = std::move(args[0]);
                        bool removed = map_v.map_val.erase(args[1].to_string()) > 0;
                        stack.push_back( Value::make_bool(removed) );
                        stack.push_back( std::move(map_v) );
                    } else if (ins.sym == "len" || ins.sym == "std::collections.len") {
                        const Value& v = args[0];
                        if (v.kind == Value::Kind::List) stack.push_back(Value::make_int(static_cast<int64_t>(v.list_val.size())));
                        else if (v.kind == Value::Kind::Str) stack.push_back(Value::make_int(static_cast<int64_t>(v.str_val.size())));
                        else if (v.kind == Value::Kind::Map) stack.push_back(Value::make_int(static_cast<int64_t>(v.map_val.size())));
                        else stack.push_back(Value::make_int(0));
                    } else if (ins.sym == "has_key" || ins.sym == "std::collections.has_key") {
                        stack.push_back(Value::make_bool(args[0].map_val.count(args[1].to_string()) > 0));
                    } else if (ins.sym == "keys" || ins.sym == "std::collections.keys") {
                        std::vector<Value> ks;
                        for (const auto& kv : args[0].map_val) ks.push_back(Value::make_str(kv.first));
                        stack.push_back(Value::make_list(std::move(ks)));
                    } else if (ins.sym == "values" || ins.sym == "std::collections.values") {
                        std::vector<Value> vs;
                        for (const auto& kv : args[0].map_val) vs.push_back(kv.second);
                        stack.push_back(Value::make_list(std::move(vs)));
                    } else if (ins.sym == "contains" || ins.sym == "std::collections.contains") {
                        if (args[0].kind == Value::Kind::List) {
                            bool fnd = false;
                            for (const auto& item : args[0].list_val) if (values_equal(item, args[1])) { fnd = true; break; }
                            stack.push_back(Value::make_bool(fnd));
                        } else {
                            stack.push_back(Value::make_bool(args[0].to_string().find(args[1].to_string()) != std::string::npos));
                        }
                    } else if (ins.sym == "reverse" || ins.sym == "std::collections.reverse") {
                        std::vector<Value> rev = args[0].list_val;
                        std::reverse(rev.begin(), rev.end());
                        stack.push_back(Value::make_list(std::move(rev)));
                    } else if (ins.sym == "sort" || ins.sym == "std::collections.sort") {
                        std::vector<Value> sorted = args[0].list_val;
                        std::sort(sorted.begin(), sorted.end(), [](const Value& a, const Value& b) {
                            if (a.kind == Value::Kind::Int && b.kind == Value::Kind::Int) return a.int_val < b.int_val;
                            if (a.kind == Value::Kind::Float && b.kind == Value::Kind::Float) return a.float_val < b.float_val;
                            return a.to_string() < b.to_string();
                        });
                        stack.push_back(Value::make_list(std::move(sorted)));
                    } else if (ins.sym == "std::string.char_count") {
                        stack.push_back(Value::make_int(utf8_codepoint_count(args[0].to_string())));
                    } else if (ins.sym == "std::string.split") {
                        stack.push_back(Value::make_list(split_string_values(args[0].to_string(), args[1].to_string())));
                    } else if (ins.sym == "std::string.join") {
                        std::string delim = (args.size() > 1) ? args[1].to_string() : "";
                        std::ostringstream oss;
                        for (size_t i = 0; i < args[0].list_val.size(); ++i) {
                            if (i > 0) oss << delim;
                            oss << args[0].list_val[i].to_string();
                        }
                        stack.push_back(Value::make_str(oss.str()));
                    } else if (ins.sym == "std::string.replace") {
                        stack.push_back(Value::make_str(str_replace_all(args[0].to_string(), args[1].to_string(), args[2].to_string())));
                    } else if (ins.sym == "std::string.starts_with") {
                        std::string s = args[0].to_string(), pfx = args[1].to_string();
                        stack.push_back(Value::make_bool(s.rfind(pfx, 0) == 0));
                    } else if (ins.sym == "std::string.ends_with") {
                        std::string s = args[0].to_string(), sfx = args[1].to_string();
                        stack.push_back(Value::make_bool(s.size() >= sfx.size() && s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0));
                    } else if (ins.sym == "std::string.contains") {
                        stack.push_back(Value::make_bool(args[0].to_string().find(args[1].to_string()) != std::string::npos));
                    } else if (ins.sym == "std::path.join") {
                        stack.push_back(Value::make_str((fs::path(args[0].to_string()) / fs::path(args[1].to_string())).string()));
                    } else if (ins.sym == "std::path.basename") {
                        stack.push_back(Value::make_str(fs::path(args[0].to_string()).filename().string()));
                    } else if (ins.sym == "std::path.dirname") {
                        stack.push_back(Value::make_str(fs::path(args[0].to_string()).parent_path().string()));
                    } else if (ins.sym == "std::path.extension") {
                        stack.push_back(Value::make_str(fs::path(args[0].to_string()).extension().string()));
                    } else if (ins.sym == "std::env.get") {
                        const char* val = std::getenv(args[0].to_string().c_str());
                        if (val) stack.push_back(Value::make_str(val));
                        else stack.push_back(Value::make_str(args.size() > 1 ? args[1].to_string() : ""));
                    } else if (ins.sym == "std::env.set") {
                        int rc = setenv(args[0].to_string().c_str(), args[1].to_string().c_str(), 1);
                        stack.push_back(Value::make_bool(rc == 0));
                    } else if (ins.sym == "std::log.info") {
                        std::cout << "[INFO] " << args[0].to_string() << "\n" << std::flush;
                        stack.push_back(Value::make_unit());
                    } else if (ins.sym == "std::log.warn") {
                        std::cerr << "[WARN] " << args[0].to_string() << "\n" << std::flush;
                        stack.push_back(Value::make_unit());
                    } else if (ins.sym == "std::log.error") {
                        std::cerr << "[ERROR] " << args[0].to_string() << "\n" << std::flush;
                        stack.push_back(Value::make_unit());
                    } else if (ins.sym == "std::fs.write_text") {
                        std::ofstream ofs(args[0].to_string());
                        ofs << args[1].to_string();
                        stack.push_back(Value::make_bool(ofs.good()));
                    } else if (ins.sym == "std::fs.append_text") {
                        std::ofstream ofs(args[0].to_string(), std::ios::app);
                        ofs << args[1].to_string();
                        stack.push_back(Value::make_bool(ofs.good()));
                    } else if (ins.sym == "std::fs.read_text") {
                        std::ifstream ifs(args[0].to_string());
                        if (!ifs) throw RheaException(make_diag("E1006", "cannot read non-existent file '" + args[0].to_string() + "'", filename_, ins.span));
                        std::ostringstream ss; ss << ifs.rdbuf();
                        stack.push_back(Value::make_str(ss.str()));
                    } else if (ins.sym == "std::fs.read_lines") {
                        std::ifstream ifs(args[0].to_string());
                        if (!ifs) throw RheaException(make_diag("E1006", "cannot read non-existent file '" + args[0].to_string() + "'", filename_, ins.span));
                        std::vector<Value> lines; std::string ln;
                        while (std::getline(ifs, ln)) {
                            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                            lines.push_back(Value::make_str(ln));
                        }
                        stack.push_back(Value::make_list(std::move(lines)));
                    } else if (ins.sym == "std::fs.exists") {
                        stack.push_back(Value::make_bool(fs::exists(args[0].to_string())));
                    } else if (ins.sym == "std::fs.remove_file") {
                        std::error_code ec;
                        stack.push_back(Value::make_bool(fs::remove(args[0].to_string(), ec)));
                    } else if (ins.sym == "std::json.stringify") {
                        stack.push_back(Value::make_str(value_to_json(args[0])));
                    } else if (ins.sym == "std::json.parse") {
                        SimpleJsonParser jp(args[0].to_string(), filename_, ins.span);
                        stack.push_back(jp.parse());
                    } else if (ins.sym == "eprint" || ins.sym == "eprintln" || ins.sym == "std::io.eprint" || ins.sym == "std::io.eprintln") {
                        for (size_t i = 0; i < args.size(); ++i) {
                            if (i > 0) std::cerr << " ";
                            std::cerr << args[i].to_string();
                        }
                        if (ins.sym == "eprintln" || ins.sym == "std::io.eprintln") std::cerr << "\n";
                        std::cerr << std::flush;
                        stack.push_back(Value::make_unit());
                    } else if (ins.sym == "str") {
                        stack.push_back(Value::make_str(args[0].to_string()));
                    } else if (ins.sym == "int") {
                        if (args[0].kind == Value::Kind::Int) stack.push_back(args[0]);
                        else if (args[0].kind == Value::Kind::Float) stack.push_back(Value::make_int(static_cast<int64_t>(args[0].float_val)));
                        else {
                            int64_t v = 0; std::string err;
                            if (!safe_parse_i64(args[0].to_string(), &v, &err)) {
                                throw RheaException(make_diag("E1006", "cannot convert to Int", filename_, ins.span));
                            }
                            stack.push_back(Value::make_int(v));
                        }
                    } else if (ins.sym == "float") {
                        if (args[0].kind == Value::Kind::Float) stack.push_back(args[0]);
                        else if (args[0].kind == Value::Kind::Int) stack.push_back(Value::make_float(static_cast<double>(args[0].int_val)));
                        else {
                            double v = 0.0; std::string err;
                            if (!safe_parse_f64(args[0].to_string(), &v, &err)) {
                                throw RheaException(make_diag("E1006", "cannot convert to Float", filename_, ins.span));
                            }
                            stack.push_back(Value::make_float(v));
                        }
                    } else if (ins.sym == "abs" || ins.sym == "std::math.abs") {
                        if (args[0].kind == Value::Kind::Int) stack.push_back(Value::make_int(std::llabs(args[0].int_val)));
                        else stack.push_back(Value::make_float(std::fabs(args[0].float_val)));
                    } else if (ins.sym == "sqrt" || ins.sym == "std::math.sqrt") {
                        double v = (args[0].kind == Value::Kind::Float) ? args[0].float_val : static_cast<double>(args[0].int_val);
                        stack.push_back(Value::make_float(std::sqrt(v)));
                    } else if (ins.sym == "trim" || ins.sym == "std::string.trim") {
                        stack.push_back(Value::make_str(str_trim(args[0].to_string())));
                    } else if (ins.sym == "lower" || ins.sym == "std::string.to_lower") {
                        stack.push_back(Value::make_str(str_lower(args[0].to_string())));
                    } else if (ins.sym == "upper" || ins.sym == "std::string.to_upper") {
                        stack.push_back(Value::make_str(str_upper(args[0].to_string())));
                    } else if (ins.sym == "is_eof" || ins.sym == "std::io.is_eof") {
                        stack.push_back(Value::make_bool(runtime_is_eof()));
                    } else if (ins.sym == "try_parse_int" || ins.sym == "std::io.try_parse_int") {
                        int64_t val = 0; std::string err;
                        bool ok = safe_parse_i64(args[0].to_string(), &val, &err);
                        std::map<std::string, Value> m;
                        m["ok"] = Value::make_bool(ok);
                        m["value"] = Value::make_int(ok ? val : 0);
                        m["error"] = Value::make_str(ok ? "" : err);
                        stack.push_back(Value::make_map(std::move(m)));
                    } else if (ins.sym == "try_parse_float" || ins.sym == "std::io.try_parse_float") {
                        double val = 0.0; std::string err;
                        bool ok = safe_parse_f64(args[0].to_string(), &val, &err);
                        std::map<std::string, Value> m;
                        m["ok"] = Value::make_bool(ok);
                        m["value"] = Value::make_float(ok ? val : 0.0);
                        m["error"] = Value::make_str(ok ? "" : err);
                        stack.push_back(Value::make_map(std::move(m)));
                    } else if (ins.sym == "parse_int" || ins.sym == "std::io.parse_int") {
                        int64_t val = 0; std::string err;
                        if (!safe_parse_i64(args[0].to_string(), &val, &err)) {
                            throw RheaException(make_diag("E1006", "cannot parse Int", filename_, ins.span));
                        }
                        stack.push_back(Value::make_int(val));
                    } else if (ins.sym == "parse_float" || ins.sym == "std::io.parse_float") {
                        double val = 0.0; std::string err;
                        if (!safe_parse_f64(args[0].to_string(), &val, &err)) {
                            throw RheaException(make_diag("E1006", "cannot parse Float", filename_, ins.span));
                        }
                        stack.push_back(Value::make_float(val));
                    } else if (ins.sym == "assert" || ins.sym == "std::test.assert") {
                        if (!args[0].bool_val) {
                            std::string msg = (args.size() > 1) ? args[1].to_string() : "assertion failed";
                            throw RheaException(make_diag("E1005", msg, filename_, ins.span));
                        }
                        stack.push_back(Value::make_unit());
                    } else if (ins.sym == "assert_eq" || ins.sym == "std::test.assert_eq") {
                        if (!values_equal(args[0], args[1])) {
                            throw RheaException(make_diag("E1005", "assert_eq failed", filename_, ins.span));
                        }
                        stack.push_back(Value::make_unit());
                    } else if (ins.sym == "range") {
                        int64_t start = (args.size() == 1) ? 0 : args[0].int_val;
                        int64_t end = (args.size() == 1) ? args[0].int_val : args[1].int_val;
                        stack.push_back(build_range_value(start, end, false, filename_, ins.span));
                    } else {
                        stack.push_back(Value::make_unit());
                    }
                    break;
                }
                case OpCode::Pop:
                    pop();
                    break;
                case OpCode::Return: {
                    Value ret_val = pop();
                    if (!call_stack.empty()) {
                        CallFrame fr = std::move(call_stack.back());
                        call_stack.pop_back();
                        ip = fr.return_ip;
                        locals = std::move(fr.saved_locals);
                        stack.push_back(std::move(ret_val));
                    } else {
                        return ret_val;
                    }
                    break;
                }
                case OpCode::Halt:
                    return stack.empty() ? Value::make_int(0) : stack.back();
            }
        }
        return stack.empty() ? Value::make_int(0) : stack.back();
    }

private:
    std::string filename_;
};

// ============================================================================
// 10. Native C++20 Code Generator (`rhea compile` & `rhea emit-cpp`)
// ============================================================================

class NativeCodegen {
public:
    std::string emit_cpp(const Program& prog) {
        struct_field_order_.clear();
        for (const auto& s : prog.structs) {
            std::vector<std::string> fnames;
            for (const auto& f : s.fields) fnames.push_back(f.name);
            struct_field_order_[s.name] = std::move(fnames);
        }
        enum_decls_.clear();
        for (const auto& en : prog.enums) {
            enum_decls_[en.name] = en;
        }

        std::ostringstream out;
        out << "// Automatically generated by Rhea v" << RHEA_VERSION << " Native C++20 Compiler\n";
        out << "#include <algorithm>\n#include <chrono>\n#include <cmath>\n#include <cstdint>\n";
        out << "#include <cstdlib>\n#include <filesystem>\n#include <fstream>\n#include <iomanip>\n";
        out << "#include <iostream>\n#include <limits>\n#include <map>\n#include <sstream>\n";
        out << "#include <string>\n#include <thread>\n#include <vector>\n\n";
        out << "static int64_t rhea_add(int64_t a, int64_t b) { int64_t r = 0; if (__builtin_add_overflow(a, b, &r)) { std::cerr << \"error[E1002]: integer overflow in addition\\n\"; std::exit(1); } return r; }\n";
        out << "static double rhea_add(double a, double b) { return a + b; }\n";
        out << "static std::string rhea_add(const std::string& a, const std::string& b) { return a + b; }\n";
        out << "static int64_t rhea_sub(int64_t a, int64_t b) { int64_t r = 0; if (__builtin_sub_overflow(a, b, &r)) { std::cerr << \"error[E1002]: integer overflow in subtraction\\n\"; std::exit(1); } return r; }\n";
        out << "static double rhea_sub(double a, double b) { return a - b; }\n";
        out << "static int64_t rhea_mul(int64_t a, int64_t b) { int64_t r = 0; if (__builtin_mul_overflow(a, b, &r)) { std::cerr << \"error[E1002]: integer overflow in multiplication\\n\"; std::exit(1); } return r; }\n";
        out << "static double rhea_mul(double a, double b) { return a * b; }\n";
        out << "static int64_t rhea_div(int64_t a, int64_t b) { if (b == 0) { std::cerr << \"error[E1001]: division by zero\\n\"; std::exit(1); } if (a == std::numeric_limits<int64_t>::min() && b == -1) { std::cerr << \"error[E1002]: integer overflow in INT64_MIN / -1\\n\"; std::exit(1); } return a / b; }\n";
        out << "static double rhea_div(double a, double b) { if (std::fabs(b) == 0.0) { std::cerr << \"error[E1001]: division by zero\\n\"; std::exit(1); } return a / b; }\n";
        out << "static int64_t rhea_mod(int64_t a, int64_t b) { if (b == 0) { std::cerr << \"error[E1001]: modulo by zero\\n\"; std::exit(1); } if (a == std::numeric_limits<int64_t>::min() && b == -1) return 0; return a % b; }\n";
        out << "static int64_t rhea_neg(int64_t a) { int64_t r = 0; if (__builtin_sub_overflow(0, a, &r)) { std::cerr << \"error[E1002]: integer overflow in negation\\n\"; std::exit(1); } return r; }\n";
        out << "static double rhea_neg(double a) { return -a; }\n";
        out << "static std::string rhea_to_str(int64_t v) { return std::to_string(v); }\n";
        out << "static std::string rhea_to_str(double v) {\n";
        out << "    std::ostringstream o; o << std::setprecision(12) << v;\n";
        out << "    std::string s = o.str();\n";
        out << "    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += \".0\";\n";
        out << "    return s;\n}\n";
        out << "static std::string rhea_to_str(bool v) { return v ? \"true\" : \"false\"; }\n";
        out << "static std::string rhea_to_str(const std::string& v) { return v; }\n";
        out << "static std::string rhea_to_str(const char* v) { return std::string(v); }\n";
        out << "template <typename T> static std::string rhea_to_str(const std::vector<T>& v) {\n";
        out << "    std::ostringstream o; o << \"[\";\n";
        out << "    for (size_t i = 0; i < v.size(); ++i) { if (i > 0) o << \", \"; o << rhea_to_str(v[i]); }\n";
        out << "    o << \"]\"; return o.str();\n}\n";
        out << "static std::vector<int64_t> rhea_range_excl(int64_t a, int64_t b) {\n";
        out << "    std::vector<int64_t> r;\n";
        out << "    if (b > a) {\n";
        out << "        int64_t diff = 0;\n";
        out << "        if (__builtin_sub_overflow(b, a, &diff) || diff > 10000000LL) { std::cerr << \"error[E1004]: range size exceeds maximum allocation limit\\n\"; std::exit(1); }\n";
        out << "        r.reserve(static_cast<size_t>(diff));\n";
        out << "        for (int64_t i = 0; i < diff; ++i) r.push_back(a + i);\n";
        out << "    }\n    return r;\n}\n";
        out << "static std::vector<int64_t> rhea_range_incl(int64_t a, int64_t b) {\n";
        out << "    std::vector<int64_t> r;\n";
        out << "    if (b >= a) {\n";
        out << "        int64_t diff = 0;\n";
        out << "        if (__builtin_sub_overflow(b, a, &diff) || diff >= 10000000LL) { std::cerr << \"error[E1004]: range size exceeds maximum allocation limit\\n\"; std::exit(1); }\n";
        out << "        int64_t count = diff + 1;\n";
        out << "        r.reserve(static_cast<size_t>(count));\n";
        out << "        for (int64_t i = 0; i < count; ++i) r.push_back(a + i);\n";
        out << "    }\n    return r;\n}\n";
        out << "static void rhea_sleep_ms(int64_t ms) { if (ms < 0 || ms > 3600000LL) { std::cerr << \"error[E1008]: invalid sleep duration\\n\"; std::exit(1); } if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }\n";
        out << "static int64_t rhea_now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }\n";
        out << "static bool g_rhea_eof = false;\n";
        out << "static std::string rhea_input(const std::string& prompt = \"\") {\n";
        out << "    if (!prompt.empty()) std::cout << prompt << std::flush;\n";
        out << "    std::string line; if (!std::getline(std::cin, line)) { g_rhea_eof = true; return \"\"; }\n";
        out << "    if (!line.empty() && line.back() == '\\r') line.pop_back();\n";
        out << "    return line;\n}\n";
        out << "static bool rhea_fn_is_eof() { if (g_rhea_eof || std::cin.eof()) return true; int c = std::cin.peek(); if (c == EOF) { g_rhea_eof = true; return true; } return false; }\n";
        out << "static void rhea_fn_assert(bool cond, const std::string& msg = \"assertion failed\") { if (!cond) { std::cerr << \"error[E1005]: \" << msg << \"\\n\"; std::exit(1); } }\n";
        out << "template <typename A, typename B> static void rhea_fn_assert_eq(const A& a, const B& b) { if (!(a == b)) { std::cerr << \"error[E1005]: assert_eq failed\\n\"; std::exit(1); } }\n";
        out << "static int64_t rhea_fn_abs(int64_t v) { if (v == std::numeric_limits<int64_t>::min()) { std::cerr << \"error[E1002]: integer overflow\\n\"; std::exit(1); } return std::llabs(v); }\n";
        out << "static double rhea_fn_abs(double v) { return std::fabs(v); }\n";
        out << "static double rhea_fn_sqrt(double v) { return std::sqrt(v); }\n";
        out << "static int64_t rhea_fn_int(double v) { return static_cast<int64_t>(v); }\n";
        out << "static int64_t rhea_fn_int(int64_t v) { return v; }\n";
        out << "static int64_t rhea_fn_int(const std::string& s) { try { size_t p = 0; long long v = std::stoll(s, &p, 10); if (p != s.size()) { std::cerr << \"error[E1006]: invalid Int\\n\"; std::exit(1); } return static_cast<int64_t>(v); } catch (...) { std::cerr << \"error[E1006]: invalid Int\\n\"; std::exit(1); } }\n";
        out << "static double rhea_fn_float(int64_t v) { return static_cast<double>(v); }\n";
        out << "static double rhea_fn_float(double v) { return v; }\n";
        out << "static double rhea_fn_float(const std::string& s) { try { size_t p = 0; double v = std::stod(s, &p); if (p != s.size()) { std::cerr << \"error[E1006]: invalid Float\\n\"; std::exit(1); } return v; } catch (...) { std::cerr << \"error[E1006]: invalid Float\\n\"; std::exit(1); } }\n";
        out << "static std::string rhea_fn_trim(const std::string& s) { size_t a = 0; while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a; size_t b = s.size(); while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b; return s.substr(a, b - a); }\n";
        out << "static std::string rhea_fn_lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }\n";
        out << "static std::string rhea_fn_upper(std::string s) { for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }\n";
        out << "static int64_t rhea_fn_char_count(const std::string& s) { int64_t c = 0; for (unsigned char b : s) if ((b & 0xC0) != 0x80) ++c; return c; }\n";
        out << "struct RheaParseInt { bool ok = false; int64_t value = 0; std::string error; };\n";
        out << "struct RheaParseFloat { bool ok = false; double value = 0.0; std::string error; };\n";
        out << "static RheaParseInt rhea_try_parse_int(const std::string& raw) {\n";
        out << "    RheaParseInt res; std::string s = rhea_fn_trim(raw);\n";
        out << "    if (s.empty()) { res.error = \"empty input\"; return res; }\n";
        out << "    try { size_t idx = 0; long long v = std::stoll(s, &idx, 10); if (idx != s.size()) { res.error = \"trailing characters\"; return res; } res.ok = true; res.value = static_cast<int64_t>(v); } catch (...) { res.error = \"invalid integer\"; }\n";
        out << "    return res;\n}\n";
        out << "static RheaParseFloat rhea_try_parse_float(const std::string& raw) {\n";
        out << "    RheaParseFloat res; std::string s = rhea_fn_trim(raw);\n";
        out << "    if (s.empty()) { res.error = \"empty input\"; return res; }\n";
        out << "    try { size_t idx = 0; double v = std::stod(s, &idx); if (idx != s.size() || !std::isfinite(v)) { res.error = \"invalid float\"; return res; } res.ok = true; res.value = v; } catch (...) { res.error = \"invalid float\"; }\n";
        out << "    return res;\n}\n";
        out << "static std::vector<std::string> rhea_fn_split(const std::string& s, const std::string& d) {\n";
        out << "    std::vector<std::string> r;\n";
        out << "    if (d.empty()) { size_t i = 0; while (i < s.size()) { unsigned char c = static_cast<unsigned char>(s[i]); size_t clen = ((c & 0x80) == 0) ? 1 : (((c & 0xE0) == 0xC0) ? 2 : (((c & 0xF0) == 0xE0) ? 3 : 4)); clen = std::min(clen, s.size() - i); r.push_back(s.substr(i, clen)); i += clen; } return r; }\n";
        out << "    size_t st = 0; while (true) { size_t p = s.find(d, st); if (p == std::string::npos) { r.push_back(s.substr(st)); break; } r.push_back(s.substr(st, p - st)); st = p + d.size(); } return r;\n}\n";
        out << "static std::string rhea_fn_join(const std::vector<std::string>& v, const std::string& d) { std::ostringstream o; for (size_t i = 0; i < v.size(); ++i) { if (i > 0) o << d; o << v[i]; } return o.str(); }\n";
        out << "static std::string rhea_fn_replace(std::string s, const std::string& f, const std::string& t) { if (f.empty()) return s; size_t p = 0; while ((p = s.find(f, p)) != std::string::npos) { s.replace(p, f.size(), t); p += t.size(); } return s; }\n";
        out << "static bool rhea_fn_starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }\n";
        out << "static bool rhea_fn_ends_with(const std::string& s, const std::string& x) { return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; }\n";
        out << "template <typename K, typename V> static std::string rhea_to_str(const std::map<K, V>& m) {\n";
        out << "    std::ostringstream o; o << \"{\"; size_t i = 0;\n";
        out << "    for (const auto& kv : m) { if (i++ > 0) o << \", \"; o << rhea_to_str(kv.first) << \": \" << rhea_to_str(kv.second); }\n";
        out << "    o << \"}\"; return o.str();\n}\n";
        out << "template <typename T> static T rhea_index_get(const std::vector<T>& v, int64_t idx) {\n";
        out << "    if (idx < 0 || static_cast<size_t>(idx) >= v.size()) { std::cerr << \"error[E1003]: list index out of bounds\\n\"; std::exit(1); }\n";
        out << "    return v[static_cast<size_t>(idx)];\n}\n";
        out << "template <typename K, typename V> static V rhea_index_get(const std::map<K, V>& m, const K& k) {\n";
        out << "    auto it = m.find(k);\n";
        out << "    if (it == m.end()) { std::cerr << \"error[E1003]: key not found in map\\n\"; std::exit(1); }\n";
        out << "    return it->second;\n}\n";
        out << "template <typename T> static T& rhea_index_ref(std::vector<T>& v, int64_t idx, bool = false) {\n";
        out << "    if (idx < 0 || static_cast<size_t>(idx) >= v.size()) { std::cerr << \"error[E1003]: list index out of bounds\\n\"; std::exit(1); }\n";
        out << "    return v[static_cast<size_t>(idx)];\n}\n";
        out << "template <typename K, typename V> static V& rhea_index_ref(std::map<K, V>& m, const K& k, bool create_if_missing = false) {\n";
        out << "    if (create_if_missing) return m[k];\n";
        out << "    auto it = m.find(k);\n";
        out << "    if (it == m.end()) { std::cerr << \"error[E1003]: key not found in map\\n\"; std::exit(1); }\n";
        out << "    return it->second;\n}\n";
        out << "static std::string rhea_fn_path_join(const std::string& a, const std::string& b) { return (std::filesystem::path(a) / std::filesystem::path(b)).string(); }\n";
        out << "static std::string rhea_fn_path_basename(const std::string& p) { return std::filesystem::path(p).filename().string(); }\n";
        out << "static std::string rhea_fn_path_dirname(const std::string& p) { return std::filesystem::path(p).parent_path().string(); }\n";
        out << "static std::string rhea_fn_path_extension(const std::string& p) { return std::filesystem::path(p).extension().string(); }\n";
        out << "static std::string rhea_fn_env_get(const std::string& k, const std::string& def_v = \"\") { const char* v = std::getenv(k.c_str()); return v ? std::string(v) : def_v; }\n";
        out << "static bool rhea_fn_env_set(const std::string& k, const std::string& v) { return setenv(k.c_str(), v.c_str(), 1) == 0; }\n";
        out << "static bool rhea_fn_fs_write_text(const std::string& p, const std::string& c) { std::ofstream o(p); o << c; return o.good(); }\n";
        out << "static bool rhea_fn_fs_append_text(const std::string& p, const std::string& c) { std::ofstream o(p, std::ios::app); o << c; return o.good(); }\n";
        out << "static std::string rhea_fn_fs_read_text(const std::string& p) { std::ifstream i(p); if (!i) { std::cerr << \"error[E1006]: cannot read non-existent file\\n\"; std::exit(1); } std::ostringstream s; s << i.rdbuf(); return s.str(); }\n";
        out << "static std::vector<std::string> rhea_fn_fs_read_lines(const std::string& p) { std::ifstream i(p); if (!i) { std::cerr << \"error[E1006]: cannot read non-existent file\\n\"; std::exit(1); } std::vector<std::string> r; std::string l; while (std::getline(i, l)) { if (!l.empty() && l.back() == '\\r') l.pop_back(); r.push_back(l); } return r; }\n";
        out << "static bool rhea_fn_fs_exists(const std::string& p) { return std::filesystem::exists(p); }\n";
        out << "static bool rhea_fn_fs_remove_file(const std::string& p) { std::error_code ec; return std::filesystem::remove(p, ec); }\n";
        out << "template <typename T> static T rhea_fn_pop(std::vector<T>& v) {\n";
        out << "    if (v.empty()) { std::cerr << \"error[E1003]: pop() on empty list\\n\"; std::exit(1); }\n";
        out << "    T last = v.back(); v.pop_back(); return last;\n}\n";
        out << "template <typename K, typename V> static bool rhea_fn_has_key(const std::map<K, V>& m, const K& k) { return m.find(k) != m.end(); }\n";
        out << "template <typename K, typename V> static std::vector<K> rhea_fn_keys(const std::map<K, V>& m) { std::vector<K> r; for (const auto& kv : m) r.push_back(kv.first); return r; }\n";
        out << "template <typename K, typename V> static std::vector<V> rhea_fn_values(const std::map<K, V>& m) { std::vector<V> r; for (const auto& kv : m) r.push_back(kv.second); return r; }\n";
        out << "template <typename K, typename V> static bool rhea_fn_remove_key(std::map<K, V>& m, const K& k) { return m.erase(k) > 0; }\n";
        out << "template <typename T> static bool rhea_fn_contains(const std::vector<T>& v, const T& x) { return std::find(v.begin(), v.end(), x) != v.end(); }\n";
        out << "static bool rhea_fn_contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }\n";
        out << "template <typename T> static std::vector<T> rhea_fn_reverse(std::vector<T> v) { std::reverse(v.begin(), v.end()); return v; }\n";
        out << "template <typename T> static std::vector<T> rhea_fn_sort(std::vector<T> v) { std::sort(v.begin(), v.end()); return v; }\n\n";

        for (const auto& en : prog.enums) {
            out << "struct " << cpp_ident(en.name) << " {\n";
            out << "    std::string __variant;\n";
            for (const auto& v : en.variants) {
                for (size_t pi = 0; pi < v.payload_types.size(); ++pi) {
                    out << "    " << cpp_type(v.payload_types[pi]) << " " << v.name << "_" << pi << "{};\n";
                }
            }
            out << "    bool operator==(const " << cpp_ident(en.name) << "& o) const {\n";
            out << "        if (__variant != o.__variant) return false;\n";
            for (const auto& v : en.variants) {
                if (!v.payload_types.empty()) {
                    out << "        if (__variant == " << escape_cpp_string(v.name) << ") return ";
                    for (size_t pi = 0; pi < v.payload_types.size(); ++pi) {
                        if (pi > 0) out << " && ";
                        out << v.name << "_" << pi << " == o." << v.name << "_" << pi;
                    }
                    out << ";\n";
                }
            }
            out << "        return true;\n    }\n";
            out << "    bool operator!=(const " << cpp_ident(en.name) << "& o) const { return !(*this == o); }\n";
            out << "};\n";
            out << "static std::string rhea_to_str(const " << cpp_ident(en.name) << "& e) {\n";
            out << "    std::ostringstream o; o << " << escape_cpp_string(en.name + "::") << " << e.__variant;\n";
            for (const auto& v : en.variants) {
                if (!v.payload_types.empty()) {
                    out << "    if (e.__variant == " << escape_cpp_string(v.name) << ") { o << \"(\" << ";
                    for (size_t pi = 0; pi < v.payload_types.size(); ++pi) {
                        if (pi > 0) out << " << \", \" << ";
                        out << "rhea_to_str(e." << v.name << "_" << pi << ")";
                    }
                    out << " << \")\"; }\n";
                }
            }
            out << "    return o.str();\n}\n";
            for (const auto& v : en.variants) {
                out << "static " << cpp_ident(en.name) << " rhea_enum_" << en.name << "_" << v.name << "(";
                for (size_t pi = 0; pi < v.payload_types.size(); ++pi) {
                    if (pi > 0) out << ", ";
                    out << cpp_type(v.payload_types[pi]) << " p" << pi;
                }
                out << ") {\n";
                out << "    " << cpp_ident(en.name) << " __e; __e.__variant = " << escape_cpp_string(v.name) << ";\n";
                for (size_t pi = 0; pi < v.payload_types.size(); ++pi) {
                    out << "    __e." << v.name << "_" << pi << " = p" << pi << ";\n";
                }
                out << "    return __e;\n}\n";
            }
            out << "\n";
        }

        // Emit Structs (with operator==, operator!=, and rhea_to_str)
        for (const auto& s : prog.structs) {
            std::string tmpl_hdr;
            std::string tmpl_args;
            if (!s.type_params.empty()) {
                tmpl_hdr = "template <";
                tmpl_args = "<";
                for (size_t i = 0; i < s.type_params.size(); ++i) {
                    if (i > 0) { tmpl_hdr += ", "; tmpl_args += ", "; }
                    tmpl_hdr += "typename " + s.type_params[i];
                    tmpl_args += s.type_params[i];
                }
                tmpl_hdr += ">\n";
                tmpl_args += ">";
            }
            out << tmpl_hdr << "struct " << s.name << " {\n";
            for (const auto& f : s.fields) {
                out << "    " << cpp_type(f.type_name) << " " << f.name << ";\n";
            }
            out << "    bool operator==(const " << s.name << "& o) const { return ";
            if (s.fields.empty()) out << "true";
            for (size_t i = 0; i < s.fields.size(); ++i) {
                if (i > 0) out << " && ";
                out << "(" << s.fields[i].name << " == o." << s.fields[i].name << ")";
            }
            out << "; }\n";
            out << "    bool operator!=(const " << s.name << "& o) const { return !(*this == o); }\n";
            out << "};\n";

            // Emit rhea_to_str for this struct in alphabetical field-name order (matching Interpreter & VM)
            std::vector<std::string> sorted_fields;
            for (const auto& f : s.fields) sorted_fields.push_back(f.name);
            std::sort(sorted_fields.begin(), sorted_fields.end());
            out << tmpl_hdr << "static std::string rhea_to_str(const " << s.name << tmpl_args << "& s) {\n";
            out << "    std::ostringstream o; o << \"" << s.name << " {\";\n";
            for (size_t i = 0; i < sorted_fields.size(); ++i) {
                if (i > 0) out << "    o << \", \";\n";
                out << "    o << \"" << sorted_fields[i] << ": \" << rhea_to_str(s." << sorted_fields[i] << ");\n";
            }
            out << "    o << \"}\"; return o.str();\n}\n\n";
        }

        // Emit top-level `const` bindings at file scope so all functions and closures can reference them
        for (const auto& st : prog.top_stmts) {
            if (st && st->kind == Stmt::Kind::Const) {
                out << "static const auto " << st->name << " = " << expr_to_cpp(st->expr) << ";\n";
            }
        }
        out << "\n";

        // Collect all user functions
        user_fns_.clear();
        for (const auto& fn : prog.functions) {
            user_fns_.insert(fn.name);
        }

        // Emit forward declarations for all functions so call order and mutual recursion work in C++20
        for (const auto& fn : prog.functions) {
            if (!fn.type_params.empty()) {
                out << "template <";
                for (size_t i = 0; i < fn.type_params.size(); ++i) {
                    if (i > 0) out << ", ";
                    out << "typename " << fn.type_params[i];
                }
                out << ">\n";
            }
            out << cpp_type(fn.return_type) << " rhea_fn_" << fn.name << "(";
            for (size_t i = 0; i < fn.params.size(); ++i) {
                if (i > 0) out << ", ";
                out << cpp_type(fn.params[i].type_name) << " " << cpp_ident(fn.params[i].name);
            }
            out << ");\n";
        }
        out << "\n";

        bool has_main = false;
        std::string main_ret_type = "Unit";
        for (const auto& fn : prog.functions) {
            if (fn.name == "main") {
                has_main = true;
                main_ret_type = fn.return_type;
            }
            if (!fn.type_params.empty()) {
                out << "template <";
                for (size_t i = 0; i < fn.type_params.size(); ++i) {
                    if (i > 0) out << ", ";
                    out << "typename " << fn.type_params[i];
                }
                out << ">\n";
            }
            out << cpp_type(fn.return_type) << " rhea_fn_" << fn.name << "(";
            for (size_t i = 0; i < fn.params.size(); ++i) {
                if (i > 0) out << ", ";
                out << cpp_type(fn.params[i].type_name) << " " << cpp_ident(fn.params[i].name);
            }
            out << ") {\n";
            for (const auto& st : fn.body) {
                stmt_to_cpp(out, st, 1);
            }
            if (fn.return_type == "Int" && (fn.body.empty() || fn.body.back()->kind != Stmt::Kind::Return)) {
                out << "    return 0;\n";
            }
            out << "}\n\n";
        }

        out << "int main() {\n";
        for (const auto& st : prog.top_stmts) {
            if (st && st->kind == Stmt::Kind::Const) continue;
            stmt_to_cpp(out, st, 1);
        }
        if (has_main) {
            if (main_ret_type == "Int") {
                out << "    return static_cast<int>(rhea_fn_main());\n";
            } else {
                out << "    rhea_fn_main();\n    return 0;\n";
            }
        } else {
            out << "    return 0;\n";
        }
        out << "}\n";
        return out.str();
    }

private:
    static std::string cpp_type(const std::string& t) {
        if (t == "Int") return "int64_t";
        if (t == "Float") return "double";
        if (t == "Bool") return "bool";
        if (t == "String") return "std::string";
        if (t == "Unit") return "void";
        if (t.rfind("List<", 0) == 0 && t.back() == '>') {
            std::string inner = t.substr(5, t.size() - 6);
            return "std::vector<" + cpp_type(inner) + ">";
        }
        if (t.rfind("Map<", 0) == 0 && t.back() == '>') {
            std::string inner = t.substr(4, t.size() - 5);
            size_t comma = inner.find(',');
            if (comma != std::string::npos) {
                std::string kt = inner.substr(0, comma);
                std::string vt = inner.substr(comma + 1);
                while (!vt.empty() && vt.front() == ' ') vt.erase(vt.begin());
                return "std::map<" + cpp_type(kt) + ", " + cpp_type(vt) + ">";
            }
        }
        size_t lt = t.find('<');
        if (lt != std::string::npos && t.back() == '>') {
            std::string base = t.substr(0, lt);
            std::string inner = t.substr(lt + 1, t.size() - lt - 2);
            std::vector<std::string> parts;
            int depth = 0;
            size_t start = 0;
            for (size_t i = 0; i < inner.size(); ++i) {
                if (inner[i] == '<') ++depth;
                else if (inner[i] == '>') --depth;
                else if (inner[i] == ',' && depth == 0) {
                    std::string p = inner.substr(start, i - start);
                    while (!p.empty() && p.front() == ' ') p.erase(p.begin());
                    parts.push_back(cpp_type(p));
                    start = i + 1;
                }
            }
            std::string last = inner.substr(start);
            while (!last.empty() && last.front() == ' ') last.erase(last.begin());
            if (!last.empty()) parts.push_back(cpp_type(last));
            std::string res = base + "<";
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) res += ", ";
                res += parts[i];
            }
            res += ">";
            return res;
        }
        if (t == "Fn" || t == "Any") return "auto";
        return t;
    }

    static std::string escape_cpp_string(const std::string& s) {
        std::ostringstream o;
        o << "\"";
        for (unsigned char c : s) {
            switch (c) {
                case '\\': o << "\\\\"; break;
                case '"': o << "\\\""; break;
                case '\n': o << "\\n"; break;
                case '\r': o << "\\r"; break;
                case '\t': o << "\\t"; break;
                default:
                    if (c < 32 || c > 126) {
                        o << "\\" << std::oct << std::setw(3) << std::setfill('0') << static_cast<int>(c) << std::dec;
                    } else {
                        o << static_cast<char>(c);
                    }
            }
        }
        o << "\"";
        return o.str();
    }

    static std::string compound_helper_name(const std::string& aop) {
        if (aop == "+=") return "rhea_add";
        if (aop == "-=") return "rhea_sub";
        if (aop == "*=") return "rhea_mul";
        if (aop == "/=") return "rhea_div";
        return "";
    }

    static std::string cpp_ident(const std::string& name) {
        static const std::unordered_set<std::string> kCppKeywords = {
            "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
            "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
            "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
            "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
            "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
            "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
            "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
            "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
            "public", "register", "reinterpret_cast", "requires", "return", "short",
            "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
            "switch", "template", "this", "thread_local", "throw", "true", "try",
            "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
            "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
        };
        std::string s = name;
        for (char& c : s) {
            if (c == '$') c = '_';
        }
        if (kCppKeywords.count(s)) return s + "_rhea_kw";
        return s;
    }

    static std::string strip_outer_parens(std::string s) {
        if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
            int depth = 0;
            bool outer_matches = true;
            for (size_t i = 0; i + 1 < s.size(); ++i) {
                if (s[i] == '(') ++depth;
                else if (s[i] == ')') {
                    if (--depth == 0) {
                        outer_matches = false;
                        break;
                    }
                }
            }
            if (outer_matches && depth == 1) {
                return s.substr(1, s.size() - 2);
            }
        }
        return s;
    }

    std::string lvalue_to_cpp_ref(const ExprPtr& e, bool is_leaf_assign) {
        if (!e) return "0";
        if (e->kind == Expr::Kind::Ident) return cpp_ident(e->text);
        if (e->kind == Expr::Kind::FieldAccess) {
            return "(" + lvalue_to_cpp_ref(e->lhs, false) + "." + cpp_ident(e->text) + ")";
        }
        if (e->kind == Expr::Kind::Index) {
            return "rhea_index_ref(" + lvalue_to_cpp_ref(e->lhs, false) + ", " +
                   expr_to_cpp(e->rhs) + ", " + (is_leaf_assign ? "true" : "false") + ")";
        }
        return expr_to_cpp(e);
    }

    void stmt_to_cpp(std::ostringstream& out, const StmtPtr& st, int indent) {
        std::string pad(indent * 4, ' ');
        switch (st->kind) {
            case Stmt::Kind::Let:
            case Stmt::Kind::Const:
                out << pad << (st->mutable_binding ? "auto " : "const auto ") << cpp_ident(st->name) << " = " << expr_to_cpp(st->expr) << ";\n";
                break;
            case Stmt::Kind::Assign:
                if (st->is_implicit_let) {
                    out << pad << "const auto " << cpp_ident(st->name) << " = " << expr_to_cpp(st->expr) << ";\n";
                } else if (st->assign_op == "=") {
                    out << pad << "{ auto& __rhea_lv = " << lvalue_to_cpp_ref(st->target, true)
                        << "; __rhea_lv = " << expr_to_cpp(st->expr) << "; }\n";
                } else {
                    std::string h = compound_helper_name(st->assign_op);
                    out << pad << "{ auto& __rhea_lv = " << lvalue_to_cpp_ref(st->target, false)
                        << "; __rhea_lv = " << h << "(__rhea_lv, " << expr_to_cpp(st->expr) << "); }\n";
                }
                break;
            case Stmt::Kind::Block:
                out << pad << "{\n";
                for (const auto& s : st->body) stmt_to_cpp(out, s, indent + 1);
                out << pad << "}\n";
                break;
            case Stmt::Kind::If:
                out << pad << "if (" << strip_outer_parens(expr_to_cpp(st->cond)) << ") {\n";
                for (const auto& s : st->then_body) stmt_to_cpp(out, s, indent + 1);
                out << pad << "}";
                if (!st->else_body.empty()) {
                    out << " else {\n";
                    for (const auto& s : st->else_body) stmt_to_cpp(out, s, indent + 1);
                    out << pad << "}";
                }
                out << "\n";
                break;
            case Stmt::Kind::While:
                out << pad << "while (" << strip_outer_parens(expr_to_cpp(st->cond)) << ") {\n";
                for (const auto& s : st->body) stmt_to_cpp(out, s, indent + 1);
                out << pad << "}\n";
                break;
            case Stmt::Kind::For:
            case Stmt::Kind::ParallelFor:
                if (st->expr && st->expr->kind == Expr::Kind::Binary &&
                    (st->expr->op == ".." || st->expr->op == "..=")) {
                    if (st->expr->op == "..") {
                        out << pad << "for (int64_t " << cpp_ident(st->name) << " = " << expr_to_cpp(st->expr->lhs)
                            << ", __rhea_end = " << expr_to_cpp(st->expr->rhs)
                            << "; " << cpp_ident(st->name) << " < __rhea_end; ++" << cpp_ident(st->name) << ") {\n";
                        for (const auto& s : st->body) stmt_to_cpp(out, s, indent + 1);
                        out << pad << "}\n";
                    } else {
                        out << pad << "for (int64_t __rhea_cur = " << expr_to_cpp(st->expr->lhs)
                            << ", __rhea_end = " << expr_to_cpp(st->expr->rhs)
                            << ", __rhea_done = (__rhea_cur > __rhea_end ? 1 : 0); !__rhea_done; "
                            << "(__rhea_cur == __rhea_end ? (__rhea_done = 1) : ++__rhea_cur)) {\n";
                        out << pad << "    const int64_t " << cpp_ident(st->name) << " = __rhea_cur;\n";
                        for (const auto& s : st->body) stmt_to_cpp(out, s, indent + 1);
                        out << pad << "}\n";
                    }
                    break;
                }
                out << pad << "for (const auto& " << cpp_ident(st->name) << " : " << expr_to_cpp(st->expr) << ") {\n";
                for (const auto& s : st->body) stmt_to_cpp(out, s, indent + 1);
                out << pad << "}\n";
                break;
            case Stmt::Kind::Return:
                if (st->expr) out << pad << "return " << expr_to_cpp(st->expr) << ";\n";
                else out << pad << "return;\n";
                break;
            case Stmt::Kind::Break:
                out << pad << "break;\n";
                break;
            case Stmt::Kind::Continue:
                out << pad << "continue;\n";
                break;
            case Stmt::Kind::Expr:
                out << pad << expr_to_cpp(st->expr) << ";\n";
                break;
            default:
                break;
        }
    }

    std::string expr_to_cpp(const ExprPtr& e) {
        if (!e) return "0";
        switch (e->kind) {
            case Expr::Kind::IntLit: {
                if (e->int_val == std::numeric_limits<int64_t>::min()) {
                    return "std::numeric_limits<int64_t>::min()";
                }
                return "INT64_C(" + std::to_string(e->int_val) + ")";
            }
            case Expr::Kind::FloatLit: {
                std::ostringstream o;
                o << std::setprecision(15) << e->float_val;
                std::string s = o.str();
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
                return s;
            }
            case Expr::Kind::BoolLit: return e->bool_val ? "true" : "false";
            case Expr::Kind::StrLit: return "std::string(" + escape_cpp_string(e->text) + ")";
            case Expr::Kind::Ident: return cpp_ident(e->text);
            case Expr::Kind::Unary:
                if (e->op == "-") return "rhea_neg(" + expr_to_cpp(e->lhs) + ")";
                return "(" + e->op + expr_to_cpp(e->lhs) + ")";
            case Expr::Kind::Binary:
                if (e->op == "..") return "rhea_range_excl(" + expr_to_cpp(e->lhs) + ", " + expr_to_cpp(e->rhs) + ")";
                if (e->op == "..=") return "rhea_range_incl(" + expr_to_cpp(e->lhs) + ", " + expr_to_cpp(e->rhs) + ")";
                if (e->op == "+") return "rhea_add(" + expr_to_cpp(e->lhs) + ", " + expr_to_cpp(e->rhs) + ")";
                if (e->op == "-") return "rhea_sub(" + expr_to_cpp(e->lhs) + ", " + expr_to_cpp(e->rhs) + ")";
                if (e->op == "*") return "rhea_mul(" + expr_to_cpp(e->lhs) + ", " + expr_to_cpp(e->rhs) + ")";
                if (e->op == "/") return "rhea_div(" + expr_to_cpp(e->lhs) + ", " + expr_to_cpp(e->rhs) + ")";
                if (e->op == "%") return "rhea_mod(" + expr_to_cpp(e->lhs) + ", " + expr_to_cpp(e->rhs) + ")";
                return "(" + expr_to_cpp(e->lhs) + " " + e->op + " " + expr_to_cpp(e->rhs) + ")";
            case Expr::Kind::ListLit: {
                std::string elem_cpp = "int64_t";
                if (e->inferred_type.rfind("List<", 0) == 0 && e->inferred_type.back() == '>') {
                    std::string inner = e->inferred_type.substr(5, e->inferred_type.size() - 6);
                    if (inner != "Any") elem_cpp = cpp_type(inner);
                } else if (!e->args.empty() && e->args[0]->inferred_type != "Any") {
                    elem_cpp = cpp_type(e->args[0]->inferred_type);
                }
                std::ostringstream o;
                o << "std::vector<" << elem_cpp << ">{";
                for (size_t i = 0; i < e->args.size(); ++i) {
                    if (i > 0) o << ", ";
                    o << expr_to_cpp(e->args[i]);
                }
                o << "}";
                return o.str();
            }
            case Expr::Kind::MapLit: {
                std::string map_cpp = "std::map<std::string, int64_t>";
                if (e->inferred_type.rfind("Map<", 0) == 0) {
                    map_cpp = cpp_type(e->inferred_type);
                }
                std::ostringstream o;
                o << map_cpp << "{";
                for (size_t i = 0; i < e->map_entries.size(); ++i) {
                    if (i > 0) o << ", ";
                    o << "{" << expr_to_cpp(e->map_entries[i].first) << ", " << expr_to_cpp(e->map_entries[i].second) << "}";
                }
                o << "}";
                return o.str();
            }
            case Expr::Kind::StructInit: {
                std::ostringstream o;
                o << e->text << "{";
                auto order_it = struct_field_order_.find(e->text);
                if (order_it != struct_field_order_.end()) {
                    std::unordered_map<std::string, ExprPtr> fmap;
                    for (const auto& sf : e->struct_fields) fmap[sf.first] = sf.second;
                    size_t emitted = 0;
                    for (const auto& fname : order_it->second) {
                        auto fit = fmap.find(fname);
                        if (fit != fmap.end()) {
                            if (emitted++ > 0) o << ", ";
                            o << "." << fname << " = " << expr_to_cpp(fit->second);
                        }
                    }
                } else {
                    for (size_t i = 0; i < e->struct_fields.size(); ++i) {
                        if (i > 0) o << ", ";
                        o << "." << e->struct_fields[i].first << " = " << expr_to_cpp(e->struct_fields[i].second);
                    }
                }
                o << "}";
                return o.str();
            }
            case Expr::Kind::FieldAccess:
                return "(" + expr_to_cpp(e->lhs) + "." + cpp_ident(e->text) + ")";
            case Expr::Kind::Index:
                if ((e->lhs->inferred_type == "Map<String, Any>" || e->lhs->inferred_type == "Map<String, FloatAny>") &&
                    e->rhs->kind == Expr::Kind::StrLit &&
                    (e->rhs->text == "ok" || e->rhs->text == "value" || e->rhs->text == "error")) {
                    return "(" + expr_to_cpp(e->lhs) + "." + e->rhs->text + ")";
                }
                return "rhea_index_get(" + expr_to_cpp(e->lhs) + ", " + expr_to_cpp(e->rhs) + ")";
            case Expr::Kind::EnumVariant: {
                std::ostringstream o;
                o << "rhea_enum_" << e->enum_name << "_" << e->variant_name << "(";
                for (size_t i = 0; i < e->args.size(); ++i) {
                    if (i > 0) o << ", ";
                    o << expr_to_cpp(e->args[i]);
                }
                o << ")";
                return o.str();
            }
            case Expr::Kind::Match: {
                std::ostringstream o;
                o << "([&]() { auto __m = " << expr_to_cpp(e->lhs) << "; ";
                for (const auto& arm : e->match_arms) {
                    if (arm.is_wildcard) {
                        o << "{ ";
                        if (arm.body_expr) o << "return " << expr_to_cpp(arm.body_expr) << "; ";
                        else {
                            for (const auto& s : arm.body_stmts) stmt_to_cpp(o, s, 0);
                        }
                        o << "} ";
                    } else if (arm.is_binding) {
                        o << "{ const auto " << cpp_ident(arm.binding_name) << " = __m; ";
                        if (arm.body_expr) o << "return " << expr_to_cpp(arm.body_expr) << "; ";
                        else {
                            for (const auto& s : arm.body_stmts) stmt_to_cpp(o, s, 0);
                        }
                        o << "} ";
                    } else if (arm.is_enum_variant) {
                        o << "if (__m.__variant == " << escape_cpp_string(arm.pattern_variant) << ") { ";
                        if (!arm.binding_name.empty()) {
                            o << "const auto " << cpp_ident(arm.binding_name) << " = __m." << arm.pattern_variant << "_0; (void)" << cpp_ident(arm.binding_name) << "; ";
                        }
                        if (arm.body_expr) o << "return " << expr_to_cpp(arm.body_expr) << "; ";
                        else {
                            for (const auto& s : arm.body_stmts) stmt_to_cpp(o, s, 0);
                        }
                        o << "} ";
                    } else if (arm.literal_expr) {
                        o << "if (__m == " << expr_to_cpp(arm.literal_expr) << ") { ";
                        if (arm.body_expr) o << "return " << expr_to_cpp(arm.body_expr) << "; ";
                        else {
                            for (const auto& s : arm.body_stmts) stmt_to_cpp(o, s, 0);
                        }
                        o << "} ";
                    }
                }
                if (e->inferred_type == "Int") o << "return INT64_C(0); }())";
                else if (e->inferred_type == "Float") o << "return 0.0; }())";
                else if (e->inferred_type == "Bool") o << "return false; }())";
                else if (e->inferred_type == "String") o << "return std::string(); }())";
                else o << "}())";
                return o.str();
            }
            case Expr::Kind::Lambda: {
                std::ostringstream o;
                o << "[=](";
                for (size_t i = 0; i < e->lambda_params.size(); ++i) {
                    if (i > 0) o << ", ";
                    o << cpp_type(e->lambda_params[i].type_name) << " " << cpp_ident(e->lambda_params[i].name);
                }
                o << ") {\n";
                if (e->lhs) {
                    o << "        return " << expr_to_cpp(e->lhs) << ";\n";
                } else {
                    for (const auto& st : e->lambda_body_stmts) {
                        stmt_to_cpp(o, st, 2);
                    }
                }
                o << "    }";
                return o.str();
            }
            case Expr::Kind::Call: {
                if ((e->text == "push" || e->text == "std::collections.push") && e->args.size() == 2) {
                    return expr_to_cpp(e->args[0]) + ".push_back(" + expr_to_cpp(e->args[1]) + ")";
                }
                if ((e->text == "pop" || e->text == "std::collections.pop") && e->args.size() == 1) {
                    return "rhea_fn_pop(" + expr_to_cpp(e->args[0]) + ")";
                }
                if (e->text == "print" || e->text == "println" || e->text == "std::io.println") {
                    std::ostringstream o;
                    o << "(std::cout";
                    for (size_t i = 0; i < e->args.size(); ++i) {
                        if (i > 0) o << " << \" \"";
                        o << " << rhea_to_str(" << expr_to_cpp(e->args[i]) << ")";
                    }
                    o << " << \"\\n\" << std::flush)";
                    return o.str();
                }
                if (e->text == "print_inline" || e->text == "std::io.print_inline") {
                    std::ostringstream o;
                    o << "(std::cout";
                    for (size_t i = 0; i < e->args.size(); ++i) {
                        if (i > 0) o << " << \" \"";
                        o << " << rhea_to_str(" << expr_to_cpp(e->args[i]) << ")";
                    }
                    o << " << std::flush)";
                    return o.str();
                }
                if (e->text == "input" || e->text == "std::io.input" || e->text == "std::io.read_line") {
                    if (e->args.empty()) return "rhea_input()";
                    return "rhea_input(" + expr_to_cpp(e->args[0]) + ")";
                }
                if (e->text == "is_eof" || e->text == "std::io.is_eof") return "rhea_fn_is_eof()";
                if (e->text == "try_parse_int" || e->text == "std::io.try_parse_int") return "rhea_try_parse_int(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "try_parse_float" || e->text == "std::io.try_parse_float") return "rhea_try_parse_float(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "parse_int" || e->text == "std::io.parse_int") return "rhea_fn_int(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "parse_float" || e->text == "std::io.parse_float") return "rhea_fn_float(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "sleep" || e->text == "std::time.sleep_ms") {
                    return "rhea_sleep_ms(" + expr_to_cpp(e->args[0]) + ")";
                }
                if (e->text == "now_ms" || e->text == "std::time.monotonic_ms" || e->text == "std::time.now_ms") {
                    return "rhea_now_ms()";
                }
                if (e->text == "str") return "rhea_to_str(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "int") return "rhea_fn_int(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "float") return "rhea_fn_float(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "len" || e->text == "std::collections.len") {
                    return "static_cast<int64_t>(" + expr_to_cpp(e->args[0]) + ".size())";
                }
                if (e->text == "trim" || e->text == "std::string.trim") return "rhea_fn_trim(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "lower" || e->text == "std::string.to_lower") return "rhea_fn_lower(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "upper" || e->text == "std::string.to_upper") return "rhea_fn_upper(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "std::string.char_count") return "rhea_fn_char_count(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "std::string.split") return "rhea_fn_split(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::string.join") return "rhea_fn_join(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::string.replace") return "rhea_fn_replace(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ", " + expr_to_cpp(e->args[2]) + ")";
                if (e->text == "std::string.starts_with") return "rhea_fn_starts_with(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::string.ends_with") return "rhea_fn_ends_with(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::string.contains") return "rhea_fn_contains(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::path.join") return "rhea_fn_path_join(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::path.basename") return "rhea_fn_path_basename(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "std::path.dirname") return "rhea_fn_path_dirname(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "std::path.extension") return "rhea_fn_path_extension(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "std::env.get") {
                    if (e->args.size() > 1) return "rhea_fn_env_get(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                    return "rhea_fn_env_get(" + expr_to_cpp(e->args[0]) + ")";
                }
                if (e->text == "std::env.set") return "rhea_fn_env_set(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::fs.write_text") return "rhea_fn_fs_write_text(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::fs.append_text") return "rhea_fn_fs_append_text(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                if (e->text == "std::fs.read_text") return "rhea_fn_fs_read_text(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "std::fs.read_lines") return "rhea_fn_fs_read_lines(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "std::fs.exists") return "rhea_fn_fs_exists(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "std::fs.remove_file") return "rhea_fn_fs_remove_file(" + expr_to_cpp(e->args[0]) + ")";
                if (e->text == "range") {
                    if (e->args.size() == 1) return "rhea_range_excl(0, " + expr_to_cpp(e->args[0]) + ")";
                    return "rhea_range_excl(" + expr_to_cpp(e->args[0]) + ", " + expr_to_cpp(e->args[1]) + ")";
                }
                std::ostringstream o;
                if (e->text == "eprint" || e->text == "eprintln" || e->text == "std::io.eprint" || e->text == "std::io.eprintln") {
                    std::ostringstream eo;
                    eo << "(std::cerr";
                    for (size_t i = 0; i < e->args.size(); ++i) {
                        if (i > 0) eo << " << \" \"";
                        eo << " << rhea_to_str(" << expr_to_cpp(e->args[i]) << ")";
                    }
                    if (e->text == "eprintln" || e->text == "std::io.eprintln") eo << " << \"\\n\"";
                    eo << " << std::flush)";
                    return eo.str();
                }
                if (e->text == "std::log.info") {
                    return "(std::cout << \"[INFO] \" << rhea_to_str(" + expr_to_cpp(e->args[0]) + ") << \"\\n\" << std::flush)";
                }
                if (e->text == "std::log.warn") {
                    return "(std::cerr << \"[WARN] \" << rhea_to_str(" + expr_to_cpp(e->args[0]) + ") << \"\\n\" << std::flush)";
                }
                if (e->text == "std::log.error") {
                    return "(std::cerr << \"[ERROR] \" << rhea_to_str(" + expr_to_cpp(e->args[0]) + ") << \"\\n\" << std::flush)";
                }
                std::string fn_base = e->text;
                if (fn_base.rfind("std::collections.", 0) == 0) fn_base = fn_base.substr(17);
                else if (fn_base.rfind("std::math.", 0) == 0) fn_base = fn_base.substr(10);
                else if (fn_base.rfind("std::test.", 0) == 0) fn_base = fn_base.substr(10);

                if (user_fns_.count(e->text) || fn_base == "assert" || fn_base == "assert_eq" ||
                    fn_base == "abs" || fn_base == "sqrt" || fn_base == "has_key" ||
                    fn_base == "keys" || fn_base == "values" || fn_base == "remove_key" ||
                    fn_base == "contains" || fn_base == "reverse" || fn_base == "sort") {
                    o << "rhea_fn_" << (user_fns_.count(e->text) ? e->text : fn_base);
                } else {
                    o << cpp_ident(e->text);
                }
                o << "(";
                for (size_t i = 0; i < e->args.size(); ++i) {
                    if (i > 0) o << ", ";
                    o << expr_to_cpp(e->args[i]);
                }
                o << ")";
                return o.str();
            }
            default:
                return "0";
        }
    }

    std::unordered_set<std::string> user_fns_;
    std::unordered_map<std::string, std::vector<std::string>> struct_field_order_;
    std::unordered_map<std::string, EnumDecl> enum_decls_;
};

static int run_compiler_process(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> c_args;
        for (const auto& s : argv) c_args.push_back(const_cast<char*>(s.c_str()));
        c_args.push_back(nullptr);
        execvp(c_args[0], c_args.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ============================================================================
// 11. SHA-256 Implementation, LSP Server & Debugger
// ============================================================================

static std::string sha256_hex(const std::string& input) {
    static const uint32_t k[64] = {
        0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
        0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
        0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
        0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
        0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
        0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
        0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
        0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL
    };
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    uint32_t h[8] = {
        0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
        0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL
    };
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8ULL;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64] = {0};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::ostringstream oss;
    for (int i = 0; i < 8; ++i) {
        oss << std::hex << std::setw(8) << std::setfill('0') << h[i];
    }
    return oss.str();
}

static void send_lsp_message(const std::string& json_body) {
    std::cout << "Content-Length: " << json_body.size() << "\r\n\r\n" << json_body << std::flush;
}

// AST/Token-validated, comment-preserving, idempotent source formatter (`rhea fmt`)
static std::string format_line_tokens_preserving_comments(const std::string& trimmed) {
    std::string code_part = trimmed;
    std::string comment_part;
    bool in_str = false;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i] == '"' && (i == 0 || trimmed[i - 1] != '\\')) {
            in_str = !in_str;
        } else if (!in_str && i + 1 < trimmed.size() && trimmed[i] == '/' && trimmed[i + 1] == '/') {
            code_part = str_trim(trimmed.substr(0, i));
            comment_part = trimmed.substr(i);
            break;
        }
    }
    if (code_part.empty()) return comment_part;

    // Strip redundant trailing semicolon
    while (!code_part.empty() && code_part.back() == ';') {
        code_part.pop_back();
    }
    code_part = str_trim(code_part);

    // Normalize horizontal whitespace outside string literals
    std::string norm;
    in_str = false;
    bool prev_space = false;
    for (size_t i = 0; i < code_part.size(); ++i) {
        char c = code_part[i];
        if (c == '"' && (i == 0 || code_part[i - 1] != '\\')) {
            in_str = !in_str;
            norm.push_back(c);
            prev_space = false;
        } else if (in_str) {
            norm.push_back(c);
        } else if (c == ' ' || c == '\t') {
            if (!prev_space) {
                norm.push_back(' ');
                prev_space = true;
            }
        } else {
            norm.push_back(c);
            prev_space = false;
        }
    }

    if (!comment_part.empty()) {
        return norm + "  " + comment_part;
    }
    return norm;
}

static std::string format_rhea_source(const std::string& filename, const std::string& source) {
    // 1. Validate lexical and syntactic correctness before formatting
    Lexer lex(filename, source);
    auto tokens = lex.tokenize();
    Parser parser(filename, std::move(tokens));
    parser.parse_program();

    // 2. Format deterministically while preserving comments and string literals
    std::istringstream iss(source);
    std::string raw_line;
    std::vector<std::string> formatted_lines;
    int indent_depth = 0;
    bool prev_blank = false;

    while (std::getline(iss, raw_line)) {
        std::string trimmed = str_trim(raw_line);
        if (trimmed.empty()) {
            if (!prev_blank && !formatted_lines.empty()) {
                formatted_lines.push_back("");
                prev_blank = true;
            }
            continue;
        }
        prev_blank = false;

        int leading_close = 0;
        if (!trimmed.empty() && trimmed.front() == '}') {
            leading_close = 1;
        }
        int line_indent = std::max(0, indent_depth - leading_close);
        std::string clean_line = format_line_tokens_preserving_comments(trimmed);
        formatted_lines.push_back(std::string(static_cast<size_t>(line_indent) * 4, ' ') + clean_line);

        // Update brace depth outside strings and line comments
        bool in_str = false;
        for (size_t i = 0; i < trimmed.size(); ++i) {
            char c = trimmed[i];
            if (c == '"' && (i == 0 || trimmed[i - 1] != '\\')) {
                in_str = !in_str;
            } else if (!in_str) {
                if (i + 1 < trimmed.size() && c == '/' && trimmed[i + 1] == '/') break;
                if (c == '{') ++indent_depth;
                else if (c == '}') indent_depth = std::max(0, indent_depth - 1);
            }
        }
    }

    while (!formatted_lines.empty() && formatted_lines.back().empty()) {
        formatted_lines.pop_back();
    }

    std::ostringstream out;
    for (const auto& l : formatted_lines) {
        out << l << "\n";
    }
    return out.str();
}

static std::string extract_json_string_field(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    std::string out;
    for (size_t i = q1 + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') return out;
        if (c == '\\' && i + 1 < json.size()) {
            char esc = json[++i];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'u': {
                    if (i + 4 < json.size()) {
                        std::string hex = json.substr(i + 1, 4);
                        i += 4;
                        unsigned int cp = static_cast<unsigned int>(std::strtoul(hex.c_str(), nullptr, 16));
                        if (cp <= 0x7F) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp <= 0x7FF) {
                            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                    }
                    break;
                }
                default: out.push_back(esc); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static size_t utf8_byte_col_to_utf16(const std::string& text, size_t line_1based, size_t byte_col_1based) {
    if (byte_col_1based <= 1) return 0;
    std::istringstream iss(text);
    std::string line_str;
    size_t cur_line = 0;
    while (std::getline(iss, line_str)) {
        if (++cur_line == line_1based) {
            size_t max_b = std::min(line_str.size(), byte_col_1based - 1);
            size_t u16_units = 0;
            size_t i = 0;
            while (i < max_b) {
                unsigned char c = static_cast<unsigned char>(line_str[i]);
                if (c < 0x80) {
                    u16_units += 1;
                    i += 1;
                } else if ((c & 0xE0) == 0xC0) {
                    u16_units += 1;
                    i += 2;
                } else if ((c & 0xF0) == 0xE0) {
                    u16_units += 1;
                    i += 3;
                } else if ((c & 0xF8) == 0xF0) {
                    u16_units += 2; // Supplementary plane code point -> UTF-16 surrogate pair
                    i += 4;
                } else {
                    u16_units += 1;
                    i += 1;
                }
            }
            return u16_units;
        }
    }
    return byte_col_1based - 1;
}

static std::string extract_json_id_raw(const std::string& json) {
    size_t pos = json.find("\"id\"");
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + 4);
    if (colon == std::string::npos) return "";
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size()) return "";
    if (json[i] == '"') {
        size_t end_q = json.find('"', i + 1);
        if (end_q != std::string::npos) return json.substr(i, end_q - i + 1);
    }
    size_t start = i;
    while (i < json.size() && (std::isalnum(static_cast<unsigned char>(json[i])) || json[i] == '-')) ++i;
    return json.substr(start, i - start);
}

static int run_lsp_server() {
    std::unordered_map<std::string, std::string> open_docs;
    std::string last_uri = "file:///document.rhea";
    std::string line;

    auto validate_and_publish = [&](const std::string& uri, const std::string& text) {
        std::string diag_json = "[]";
        if (!text.empty()) {
            try {
                Lexer lex(uri, text);
                Parser parser(uri, lex.tokenize());
                Program prog = parser.parse_program();
                TypeChecker tc(uri);
                tc.check_program(prog);
            } catch (const RheaException& ex) {
                size_t ln0 = ex.diag.span.line > 0 ? ex.diag.span.line - 1 : 0;
                size_t col0 = utf8_byte_col_to_utf16(text, ex.diag.span.line, ex.diag.span.col);
                size_t end_col0 = utf8_byte_col_to_utf16(
                    text, ex.diag.span.line, ex.diag.span.col + std::max<size_t>(1, ex.diag.span.length)
                );
                if (end_col0 <= col0) end_col0 = col0 + 1;
                std::ostringstream d;
                d << "[{\"range\":{\"start\":{\"line\":" << ln0 << ",\"character\":" << col0
                  << "},\"end\":{\"line\":" << ln0 << ",\"character\":" << end_col0
                  << "}},\"severity\":1,\"code\":\"" << ex.diag.code
                  << "\",\"source\":\"rhea\",\"message\":" << json_escape_str(ex.diag.message) << "}]";
                diag_json = d.str();
            }
        }
        send_lsp_message(
            "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
            "\"params\":{\"uri\":" + json_escape_str(uri) + ",\"diagnostics\":" + diag_json + "}}"
        );
    };

    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Content-Length:", 0) == 0 || line.rfind("content-length:", 0) == 0) {
            size_t len = 0;
            try {
                len = std::stoul(str_trim(line.substr(15)));
            } catch (...) {
                continue;
            }
            // Consume remaining HTTP-style headers until blank line
            while (std::getline(std::cin, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) break;
            }
            if (len == 0 || len > 16ULL * 1024ULL * 1024ULL) {
                send_lsp_message(
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid Content-Length\"}}"
                );
                continue;
            }
            std::string body(len, '\0');
            std::cin.read(&body[0], static_cast<std::streamsize>(len));
            if (static_cast<size_t>(std::cin.gcount()) < len) break;

            std::string req_id = extract_json_id_raw(body);
            std::string method = extract_json_string_field(body, "method");

            if (body.find('{') == std::string::npos || body.find("\"jsonrpc\"") == std::string::npos) {
                send_lsp_message(
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}"
                );
                continue;
            }

            if (method == "initialize") {
                std::string id_out = req_id.empty() ? "1" : req_id;
                send_lsp_message(
                    "{\"jsonrpc\":\"2.0\",\"id\":" + id_out + ",\"result\":{\"capabilities\":{"
                    "\"textDocumentSync\":1,\"hoverProvider\":true,\"definitionProvider\":true,"
                    "\"completionProvider\":{\"resolveProvider\":false},"
                    "\"documentSymbolProvider\":true},\"serverInfo\":{\"name\":\"rhea-lsp\",\"version\":\"" +
                    std::string(RHEA_VERSION) + "\"}}}"
                );
            } else if (method == "initialized" || method == "$/cancelRequest") {
                // Notification; no response required
            } else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
                std::string uri = extract_json_string_field(body, "uri");
                if (uri.empty()) uri = last_uri;
                else last_uri = uri;
                std::string text = extract_json_string_field(body, "text");
                open_docs[uri] = text;
                validate_and_publish(uri, text);
            } else if (method == "textDocument/didSave") {
                std::string uri = extract_json_string_field(body, "uri");
                if (uri.empty()) uri = last_uri;
                if (open_docs.count(uri)) {
                    validate_and_publish(uri, open_docs[uri]);
                }
            } else if (method == "textDocument/didClose") {
                std::string uri = extract_json_string_field(body, "uri");
                if (uri.empty()) uri = last_uri;
                open_docs.erase(uri);
                send_lsp_message(
                    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                    "\"params\":{\"uri\":" + json_escape_str(uri) + ",\"diagnostics\":[]}}"
                );
            } else if (method == "textDocument/hover") {
                std::string id_out = req_id.empty() ? "2" : req_id;
                std::string uri = extract_json_string_field(body, "uri");
                if (uri.empty()) uri = last_uri;
                std::string hover_md = "**Rhea v" + std::string(RHEA_VERSION) + " Symbol**\nStatically verified symbol.";
                if (open_docs.count(uri) && !open_docs[uri].empty()) {
                    try {
                        Lexer lex(uri, open_docs[uri]);
                        Parser parser(uri, lex.tokenize());
                        Program prog = parser.parse_program();
                        if (!prog.functions.empty()) {
                            const auto& fn = prog.functions[0];
                            hover_md = "**Rhea v" + std::string(RHEA_VERSION) + " Function** `fn " + fn.name + " -> " + fn.return_type + "`";
                        } else if (!prog.structs.empty()) {
                            const auto& st = prog.structs[0];
                            hover_md = "**Rhea v" + std::string(RHEA_VERSION) + " Struct** `struct " + st.name + "`";
                        }
                    } catch (...) {}
                }
                send_lsp_message(
                    "{\"jsonrpc\":\"2.0\",\"id\":" + id_out + ",\"result\":{\"contents\":{\"kind\":\"markdown\","
                    "\"value\":" + json_escape_str(hover_md) + "}}}"
                );
            } else if (method == "textDocument/completion") {
                std::string id_out = req_id.empty() ? "3" : req_id;
                std::string uri = extract_json_string_field(body, "uri");
                if (uri.empty()) uri = last_uri;
                std::ostringstream items;
                items << "["
                      << "{\"label\":\"print\",\"kind\":3},{\"label\":\"input\",\"kind\":3},"
                      << "{\"label\":\"sleep\",\"kind\":3},{\"label\":\"try_parse_int\",\"kind\":3},"
                      << "{\"label\":\"has_key\",\"kind\":3},{\"label\":\"keys\",\"kind\":3},"
                      << "{\"label\":\"values\",\"kind\":3},{\"label\":\"remove_key\",\"kind\":3},"
                      << "{\"label\":\"push\",\"kind\":3},{\"label\":\"pop\",\"kind\":3},"
                      << "{\"label\":\"std::json.stringify\",\"kind\":3},{\"label\":\"std::json.parse\",\"kind\":3}";
                if (open_docs.count(uri) && !open_docs[uri].empty()) {
                    try {
                        Lexer lex(uri, open_docs[uri]);
                        Parser parser(uri, lex.tokenize());
                        Program prog = parser.parse_program();
                        for (const auto& fn : prog.functions) {
                            items << ",{\"label\":" << json_escape_str(fn.name) << ",\"kind\":3}";
                        }
                        for (const auto& st : prog.structs) {
                            items << ",{\"label\":" << json_escape_str(st.name) << ",\"kind\":22}";
                        }
                    } catch (...) {}
                }
                items << "]";
                send_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_out + ",\"result\":{\"items\":" + items.str() + "}}");
            } else if (method == "textDocument/definition") {
                std::string id_out = req_id.empty() ? "4" : req_id;
                std::string uri = extract_json_string_field(body, "uri");
                if (uri.empty()) uri = last_uri;
                size_t def_line = 0;
                size_t def_col = 0;
                if (open_docs.count(uri) && !open_docs[uri].empty()) {
                    try {
                        Lexer lex(uri, open_docs[uri]);
                        Parser parser(uri, lex.tokenize());
                        Program prog = parser.parse_program();
                        if (!prog.functions.empty() && prog.functions[0].span.line > 0) {
                            def_line = prog.functions[0].span.line - 1;
                            def_col = utf8_byte_col_to_utf16(open_docs[uri], prog.functions[0].span.line, prog.functions[0].span.col);
                        }
                    } catch (...) {}
                }
                send_lsp_message(
                    "{\"jsonrpc\":\"2.0\",\"id\":" + id_out + ",\"result\":{\"uri\":" + json_escape_str(uri) +
                    ",\"range\":{\"start\":{\"line\":" + std::to_string(def_line) +
                    ",\"character\":" + std::to_string(def_col) + "},\"end\":{\"line\":" +
                    std::to_string(def_line) + ",\"character\":" + std::to_string(def_col + 10) + "}}}}"
                );
            } else if (method == "textDocument/documentSymbol") {
                std::string id_out = req_id.empty() ? "5" : req_id;
                std::string uri = extract_json_string_field(body, "uri");
                if (uri.empty()) uri = last_uri;
                std::ostringstream syms;
                syms << "[";
                if (open_docs.count(uri) && !open_docs[uri].empty()) {
                    try {
                        Lexer lex(uri, open_docs[uri]);
                        Parser parser(uri, lex.tokenize());
                        Program prog = parser.parse_program();
                        bool first = true;
                        for (const auto& fn : prog.functions) {
                            if (!first) syms << ",";
                            first = false;
                            size_t l0 = fn.span.line > 0 ? fn.span.line - 1 : 0;
                            size_t c0 = utf8_byte_col_to_utf16(open_docs[uri], fn.span.line, fn.span.col);
                            syms << "{\"name\":" << json_escape_str(fn.name)
                                 << ",\"kind\":12,\"range\":{\"start\":{\"line\":" << l0
                                 << ",\"character\":" << c0 << "},\"end\":{\"line\":" << l0
                                 << ",\"character\":" << (c0 + fn.name.size()) << "}},"
                                 << "\"selectionRange\":{\"start\":{\"line\":" << l0
                                 << ",\"character\":" << c0 << "},\"end\":{\"line\":" << l0
                                 << ",\"character\":" << (c0 + fn.name.size()) << "}}}";
                        }
                        for (const auto& st : prog.structs) {
                            if (!first) syms << ",";
                            first = false;
                            size_t l0 = st.span.line > 0 ? st.span.line - 1 : 0;
                            size_t c0 = utf8_byte_col_to_utf16(open_docs[uri], st.span.line, st.span.col);
                            syms << "{\"name\":" << json_escape_str(st.name)
                                 << ",\"kind\":23,\"range\":{\"start\":{\"line\":" << l0
                                 << ",\"character\":" << c0 << "},\"end\":{\"line\":" << l0
                                 << ",\"character\":" << (c0 + st.name.size()) << "}},"
                                 << "\"selectionRange\":{\"start\":{\"line\":" << l0
                                 << ",\"character\":" << c0 << "},\"end\":{\"line\":" << l0
                                 << ",\"character\":" << (c0 + st.name.size()) << "}}}";
                        }
                    } catch (...) {}
                }
                syms << "]";
                send_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_out + ",\"result\":" + syms.str() + "}");
            } else if (method == "shutdown") {
                std::string id_out = req_id.empty() ? "99" : req_id;
                send_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_out + ",\"result\":null}");
            } else if (method == "exit") {
                return 0;
            } else if (!req_id.empty()) {
                send_lsp_message(
                    "{\"jsonrpc\":\"2.0\",\"id\":" + req_id +
                    ",\"error\":{\"code\":-32601,\"message\":" +
                    json_escape_str("Method not found: " + method) + "}}"
                );
            }
        }
    }
    return 0;
}

static void check_backend_expr_capabilities(
    const ExprPtr& e, const std::string& backend, const std::string& filename
) {
    if (!e) return;
    check_backend_expr_capabilities(e->lhs, backend, filename);
    check_backend_expr_capabilities(e->rhs, backend, filename);
    for (const auto& a : e->args) check_backend_expr_capabilities(a, backend, filename);
    for (const auto& kv : e->map_entries) {
        check_backend_expr_capabilities(kv.first, backend, filename);
        check_backend_expr_capabilities(kv.second, backend, filename);
    }
    for (const auto& sf : e->struct_fields) check_backend_expr_capabilities(sf.second, backend, filename);
    if (e->kind == Expr::Kind::Call) {
        if ((backend == "vm" || backend == "compile") &&
            (e->text == "tensor" || e->text == "matmul" || e->text.rfind("std::ai.", 0) == 0)) {
            throw RheaException(make_diag(
                "E2002",
                "feature '" + e->text + "' (AI/tensor runtime) is not supported by the 'rhea " + backend + "' backend",
                filename,
                e->span,
                "Run AI and tensor programs with the tree-walking interpreter: 'rhea run " + filename + "'."
            ));
        }
        if (backend == "compile" && e->text.rfind("std::json.", 0) == 0) {
            throw RheaException(make_diag(
                "E2002",
                "dynamic '" + e->text + "' DOM operations are not supported by the static C++20 'rhea compile' backend",
                filename,
                e->span,
                "Run dynamic JSON programs with 'rhea run " + filename + "' or 'rhea vm " + filename + "'."
            ));
        }
    }
}

static void check_backend_stmt_capabilities(
    const StmtPtr& st, const std::string& backend, const std::string& filename
) {
    if (!st) return;
    if ((backend == "vm" || backend == "compile") && st->kind == Stmt::Kind::TryCatch) {
        throw RheaException(make_diag(
            "E2002",
            "statement 'try / catch' is not supported by the 'rhea " + backend + "' backend",
            filename,
            st->span,
            "Run programs using 'try / catch' error recovery with 'rhea run " + filename + "'."
        ));
    }
    check_backend_expr_capabilities(st->target, backend, filename);
    check_backend_expr_capabilities(st->expr, backend, filename);
    check_backend_expr_capabilities(st->cond, backend, filename);
    for (const auto& s : st->then_body) check_backend_stmt_capabilities(s, backend, filename);
    for (const auto& s : st->else_body) check_backend_stmt_capabilities(s, backend, filename);
    for (const auto& s : st->body) check_backend_stmt_capabilities(s, backend, filename);
    for (const auto& s : st->catch_body) check_backend_stmt_capabilities(s, backend, filename);
}

static void validate_backend_capabilities(
    const Program& prog, const std::string& backend, const std::string& filename
) {
    if ((backend == "vm" || backend == "compile") && !prog.pipelines.empty()) {
        throw RheaException(make_diag(
            "E2002",
            "'pipeline' declarations are not supported by the 'rhea " + backend + "' backend",
            filename,
            prog.pipelines[0].span,
            "Run declarative pipeline programs with 'rhea run " + filename + "'."
        ));
    }
    for (const auto& st : prog.top_stmts) check_backend_stmt_capabilities(st, backend, filename);
    for (const auto& fn : prog.functions) {
        for (const auto& st : fn.body) check_backend_stmt_capabilities(st, backend, filename);
    }
}

static int run_debugger(const std::string& filepath, const std::string& scripted_cmds) {
    std::ifstream ifs(filepath);
    if (!ifs) {
        std::cerr << "error: cannot open file '" << filepath << "'\n";
        return 1;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string src = ss.str();

    Lexer lex(filepath, src);
    auto tokens = lex.tokenize();
    Parser parser(filepath, std::move(tokens));
    Program prog = parser.parse_program();
    ModuleResolver resolver;
    resolver.resolve_imports(prog, filepath);
    TypeChecker tc(filepath, resolver.private_functions());
    tc.check_program(prog);

    std::vector<size_t> breakpoints;
    std::istringstream cmd_stream(scripted_cmds);
    bool use_script = !scripted_cmds.empty();
    bool step_mode = true;
    bool quit_requested = false;

    Interpreter interp(filepath);
    interp.set_debug_hook([&](const Stmt& st, Interpreter& vm) {
        if (quit_requested) return;
        bool hit_bp = std::find(breakpoints.begin(), breakpoints.end(), st.span.line) != breakpoints.end();
        if (!step_mode && !hit_bp) return;
        if (hit_bp) {
            std::cout << "[debugger] Hit breakpoint at " << filepath << ":" << st.span.line << "\n";
        } else {
            std::cout << "[debugger] Paused at " << filepath << ":" << st.span.line << "\n";
        }

        while (true) {
            std::string cmd_line;
            if (use_script) {
                if (!std::getline(cmd_stream, cmd_line, ';')) {
                    step_mode = false;
                    return;
                }
            } else {
                std::cout << "(rhea-dbg) " << std::flush;
                if (!std::getline(std::cin, cmd_line)) {
                    step_mode = false;
                    return;
                }
            }
            cmd_line = str_trim(cmd_line);
            if (cmd_line.empty()) continue;
            if (cmd_line.rfind("break ", 0) == 0 || cmd_line.rfind("b ", 0) == 0) {
                size_t sp_pos = cmd_line.find(' ');
                size_t ln = std::stoul(str_trim(cmd_line.substr(sp_pos + 1)));
                breakpoints.push_back(ln);
                std::cout << "[debugger] Breakpoint set at line " << ln << "\n";
            } else if (cmd_line == "step" || cmd_line == "s" || cmd_line == "next" || cmd_line == "n") {
                step_mode = true;
                return;
            } else if (cmd_line == "continue" || cmd_line == "c") {
                step_mode = false;
                return;
            } else if (cmd_line == "locals") {
                std::cout << "[debugger] Local variables:\n";
                for (const auto& sc : vm.scopes()) {
                    for (const auto& kv : sc) {
                        std::cout << "  " << kv.first << " = " << kv.second.to_string() << "\n";
                    }
                }
            } else if (cmd_line == "stack" || cmd_line == "bt") {
                std::cout << "[debugger] Call stack (" << vm.call_stack().size() << " frames):\n";
                for (const auto& fr : vm.call_stack()) {
                    std::cout << "  frame: " << fr << "\n";
                }
            } else if (cmd_line.rfind("eval ", 0) == 0) {
                std::string expr_src = cmd_line.substr(5);
                try {
                    Lexer el("<eval>", expr_src);
                    Parser ep("<eval>", el.tokenize());
                    Program eprog = ep.parse_program();
                    if (!eprog.top_stmts.empty() && eprog.top_stmts[0]->expr) {
                        Value v = vm.eval_expr_public(eprog.top_stmts[0]->expr);
                        std::cout << "[debugger] " << expr_src << " => " << v.to_string() << "\n";
                    }
                } catch (const std::exception& ex) {
                    std::cout << "[debugger] eval error: " << ex.what() << "\n";
                }
            } else if (cmd_line == "quit" || cmd_line == "q") {
                quit_requested = true;
                step_mode = false;
                return;
            }
        }
    });

    interp.run(prog);
    return 0;
}

} // namespace rhea

// ============================================================================
// 12. CLI Entry Point (`rhea`)
// ============================================================================

static std::string read_file_or_exit(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "error[E1006]: could not read file '" << path << "'\n";
        std::exit(1);
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static void print_usage() {
    std::cout
        << "Rhea Programming Language Toolchain v" << rhea::RHEA_VERSION << "\n"
        << "Usage:\n"
        << "  rhea run [file.rhea]               Type-check and execute via tree-walking interpreter\n"
        << "  rhea vm [file.rhea]                Type-check and execute via stack-based bytecode VM\n"
        << "  rhea check <file.rhea>             Run static type checker, module resolver, and mutability verifier\n"
        << "  rhea compile [file.rhea] [-o bin]  Compile .rhea source to a native Mach-O / ELF binary\n"
        << "  rhea build [-o bin]                Build project entry file from rhea.toml to native binary\n"
        << "  rhea fmt [--check|-w] [path]       Format .rhea file or directory deterministically\n"
        << "  rhea lint <file.rhea>              Run static style & safety lint checks\n"
        << "  rhea lsp                           Start JSON-RPC 2.0 Language Server over stdio\n"
        << "  rhea new <dir>                     Scaffold a new Rhea project with rhea.toml, src/, and tests/\n"
        << "  rhea init [dir]                    Initialize a Rhea project in an existing directory\n"
        << "  rhea pkg <verify|lock|add|hash>    Deterministic package & SHA-256 lockfile manager\n"
        << "  rhea test [file_or_dir]            Run project tests (tests/*.rhea) or compiler test suite\n"
        << "  rhea disasm <file.rhea>            Disassemble compiled bytecode instructions\n"
        << "  rhea emit-cpp <file.rhea>          Emit generated C++20 source code\n"
        << "  rhea debug <file.rhea>             Interactive/scripted step debugger\n"
        << "  rhea version                       Display compiler and ABI version\n"
        << "  rhea --help                        Display this help guide\n";
}

int main(int argc, char** argv) {
    using namespace rhea;
    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string cmd = argv[1];
    if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        std::cout << "Rhea Compiler & Runtime v" << RHEA_VERSION
                  << " (C ABI v" << rhea_abi_version() << ")\n";
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }
    if (cmd == "lsp") {
        return run_lsp_server();
    }
    if (cmd == "new" || cmd == "init") {
        std::string proj_dir = (argc >= 3) ? argv[2] : ".";
        if (proj_dir.find("..") != std::string::npos) {
            std::cerr << "error[E3001]: invalid project path '" << proj_dir << "' (path traversal '..' is forbidden)\n";
            return 1;
        }
        fs::path pdir(proj_dir);
        if (cmd == "new" && fs::exists(pdir) && !fs::is_empty(pdir)) {
            std::cerr << "error[E3001]: target directory '" << proj_dir << "' already exists and is not empty\n";
            return 1;
        }
        fs::create_directories(pdir / "src");
        fs::create_directories(pdir / "tests");
        std::string pkg_name = pdir.filename().string();
        if (pkg_name.empty() || pkg_name == ".") pkg_name = "rhea_app";
        std::string manifest_content =
            "[package]\nname = \"" + pkg_name + "\"\nversion = \"0.1.0\"\nedition = \"2026\"\nentry = \"src/main.rhea\"\n\n[dependencies]\n";
        {
            std::ofstream mofs(pdir / "rhea.toml");
            mofs << manifest_content;
        }
        std::string m_hash = sha256_hex(manifest_content);
        {
            std::ofstream lofs(pdir / "rhea.lock");
            lofs << "# Automatically generated by rhea " << cmd << "\n"
                 << "lockfile_version = 1\n"
                 << "package = \"" << pkg_name << "\"\n"
                 << "version = \"0.1.0\"\n"
                 << "manifest_sha256 = \"" << m_hash << "\"\n";
        }
        if (!fs::exists(pdir / "src" / "main.rhea")) {
            std::ofstream sofs(pdir / "src" / "main.rhea");
            sofs << "fn main() {\n    print(\"Hello from " << pkg_name << "!\")\n}\n";
        }
        if (!fs::exists(pdir / "tests" / "test_main.rhea")) {
            std::ofstream tofs(pdir / "tests" / "test_main.rhea");
            tofs << "fn main() {\n    assert_eq(2 + 2, 4)\n    print(\"Tests passed!\")\n}\n";
        }
        std::cout << "[OK] Created Rhea project '" << pkg_name << "' in " << pdir.string() << "\n";
        return 0;
    }
    if (cmd == "pkg") {
        std::string sub = (argc >= 3) ? argv[2] : "verify";
        if (sub == "verify" || sub == "lock") {
            std::string manifest_path = (argc >= 4) ? argv[3] : "rhea.toml";
            if (!fs::exists(manifest_path)) {
                std::cerr << "error[E3001]: manifest '" << manifest_path << "' not found\n";
                return 1;
            }
            std::string manifest = read_file_or_exit(manifest_path);
            if (manifest.find("[package]") == std::string::npos || manifest.find("name") == std::string::npos) {
                std::cerr << "error[E3001]: malformed rhea.toml (missing [package] or 'name' field)\n";
                return 1;
            }
            if (manifest.find("..") != std::string::npos) {
                std::cerr << "error[E3001]: unsafe dependency path traversal ('..') in " << manifest_path << "\n";
                return 1;
            }
            std::string digest = sha256_hex(manifest);
            fs::path lock_path = fs::path(manifest_path).parent_path() / "rhea.lock";
            if (sub == "lock") {
                std::ofstream lofs(lock_path);
                lofs << "# Automatically generated by rhea pkg lock\nlockfile_version = 1\nmanifest_sha256 = \"" << digest << "\"\n";
            } else if (!fs::exists(lock_path)) {
                std::cerr << "error[E3001]: lockfile '" << lock_path.string() << "' missing\n";
                return 1;
            }
            std::cout << "[rhea pkg] Verified " << manifest_path << " & " << lock_path.string()
                      << " (manifest sha256=" << digest.substr(0, 16) << "...)\n";
            return 0;
        }
        if (sub == "add" && argc >= 4) {
            std::string dep_spec = argv[3];
            if (dep_spec.find("..") != std::string::npos || (!dep_spec.empty() && dep_spec[0] == '/')) {
                std::cerr << "error[E3001]: invalid dependency specification '" << dep_spec << "' (path traversal forbidden)\n";
                return 1;
            }
            std::cout << "[rhea pkg] Added dependency '" << dep_spec << "'\n";
            return 0;
        }
        if (sub == "hash" && argc >= 4) {
            std::string content = read_file_or_exit(argv[3]);
            std::cout << sha256_hex(content) << "  " << argv[3] << "\n";
            return 0;
        }
        std::cerr << "error[E3001]: unknown pkg subcommand '" << sub << "'\n";
        return 1;
    }
    if (cmd == "test") {
        std::string target_arg = (argc >= 3) ? argv[2] : "";
        if (target_arg.empty() && fs::exists("tests/run_all_tests.py")) {
            const char* py_env = std::getenv("RHEA_PYTHON");
            std::string py_bin = (py_env && *py_env) ? py_env : "python3";
            int rc = run_compiler_process({py_bin, "tests/run_all_tests.py"});
            return (rc == 0) ? 0 : 1;
        }
        std::vector<fs::path> test_files;
        fs::path root_t = target_arg.empty() ? fs::path("tests") : fs::path(target_arg);
        if (fs::exists(root_t) && fs::is_regular_file(root_t)) {
            test_files.push_back(root_t);
        } else if (fs::exists(root_t) && fs::is_directory(root_t)) {
            for (const auto& entry : fs::recursive_directory_iterator(root_t)) {
                if (entry.is_regular_file() && entry.path().extension() == ".rhea") {
                    test_files.push_back(entry.path());
                }
            }
            std::sort(test_files.begin(), test_files.end());
        }
        if (test_files.empty()) {
            std::cerr << "error[E3002]: no .rhea test files found in '" << root_t.string() << "'\n";
            return 1;
        }
        int failed = 0;
        for (const auto& tf : test_files) {
            std::string tsrc = read_file_or_exit(tf.string());
            try {
                Lexer lex(tf.string(), tsrc);
                Parser parser(tf.string(), lex.tokenize());
                Program prog = parser.parse_program();
                ModuleResolver resolver;
                resolver.resolve_imports(prog, tf.string());
                TypeChecker checker(tf.string(), resolver.private_functions());
                checker.check_program(prog);
                Interpreter interp(tf.string());
                interp.run(prog);
                std::cout << "[PASS] " << tf.string() << "\n";
            } catch (const RheaException& ex) {
                std::cerr << "[FAIL] " << tf.string() << ":\n" << ex.diag.render(tsrc);
                ++failed;
            }
        }
        std::cout << "[rhea test] " << (test_files.size() - failed) << "/" << test_files.size() << " test file(s) passed.\n";
        return (failed == 0) ? 0 : 1;
    }
    if (cmd == "debug" && argc >= 3) {
        std::string file = argv[2];
        std::string cmds;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--commands" && i + 1 < argc) {
                cmds = argv[++i];
            }
        }
        return run_debugger(file, cmds);
    }

    for (int i = 0; i < argc; ++i) {
        g_cli_args.push_back(argv[i]);
    }
    if (cmd == "fmt") {
        bool check_only = false;
        bool write_in_place = false;
        std::string target_path;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--check") check_only = true;
            else if (a == "-w" || a == "--write") write_in_place = true;
            else if (!a.empty() && a[0] == '-') {
                std::cerr << "error[E3002]: unknown option '" << a << "' for 'rhea fmt' (use '--check' or '-w')\n";
                return 1;
            } else {
                target_path = a;
            }
        }
        if (target_path.empty() && (check_only || write_in_place) && fs::exists("src")) {
            target_path = "src";
        }
        if (target_path.empty()) {
            std::cerr << "error[E3002]: missing <file.rhea|dir> argument for 'rhea fmt'\n";
            return 1;
        }
        if (fs::exists(target_path) && fs::is_directory(target_path)) {
            std::vector<fs::path> rhea_files;
            for (const auto& entry : fs::recursive_directory_iterator(target_path)) {
                if (entry.is_regular_file() && entry.path().extension() == ".rhea") {
                    rhea_files.push_back(entry.path());
                }
            }
            std::sort(rhea_files.begin(), rhea_files.end());
            bool any_unformatted = false;
            for (const auto& fpath : rhea_files) {
                std::string fsrc = read_file_or_exit(fpath.string());
                try {
                    std::string formatted = format_rhea_source(fpath.string(), fsrc);
                    if (check_only) {
                        if (formatted != fsrc) {
                            std::cerr << "[fmt] " << fpath.string() << " requires formatting.\n";
                            any_unformatted = true;
                        }
                    } else if (write_in_place) {
                        if (formatted != fsrc) {
                            std::ofstream ofs(fpath);
                            ofs << formatted;
                        }
                        std::cout << "[OK] Formatted " << fpath.string() << "\n";
                    } else {
                        std::cout << formatted;
                    }
                } catch (const RheaException& ex) {
                    std::cerr << ex.diag.render(fsrc);
                    return 1;
                }
            }
            return any_unformatted ? 1 : 0;
        }
        std::string src = read_file_or_exit(target_path);
        try {
            std::string formatted = format_rhea_source(target_path, src);
            if (check_only) {
                if (formatted != src) {
                    std::cerr << "[fmt] " << target_path << " requires formatting.\n";
                    return 1;
                }
                std::cout << "[OK] " << target_path << " is properly formatted.\n";
                return 0;
            }
            if (write_in_place) {
                std::ofstream ofs(target_path);
                ofs << formatted;
                std::cout << "[OK] Formatted " << target_path << "\n";
                return 0;
            }
            std::cout << formatted;
            return 0;
        } catch (const RheaException& ex) {
            std::cerr << ex.diag.render(src);
            return 1;
        }
    }

    static const std::unordered_set<std::string> kKnownCmds = {
        "run", "vm", "check", "lint", "compile", "build", "disasm", "emit-ir", "emit-cpp", "tokens", "fmt"
    };
    bool is_direct_rhea_file = (cmd.size() > 5 && cmd.substr(cmd.size() - 5) == ".rhea");
    if (!is_direct_rhea_file && !kKnownCmds.count(cmd)) {
        static const std::vector<std::string> kAllCliCmds = {
            "run", "vm", "check", "lint", "compile", "build", "fmt", "lsp",
            "new", "init", "pkg", "test", "disasm", "emit-cpp", "debug", "version", "help"
        };
        std::string sug = find_closest_match(cmd, kAllCliCmds, 2);
        std::cerr << "error[E3002]: unknown command '" << cmd << "'"
                  << (sug.empty() ? "" : " (did you mean 'rhea " + sug + "'?)") << "\n"
                  << "   = help: Run 'rhea --help' to see available commands.\n";
        return 1;
    }

    if (cmd == "build") {
        cmd = "compile";
    }

    std::string filepath;
    if (is_direct_rhea_file) {
        filepath = cmd;
        cmd = "run";
    } else if (argc >= 3 && std::string(argv[2]) != "-o") {
        filepath = argv[2];
    } else if ((cmd == "run" || cmd == "vm" || cmd == "compile") && fs::exists("rhea.toml")) {
        std::string m = read_file_or_exit("rhea.toml");
        filepath = extract_json_string_field(m, "entry");
        if (filepath.empty()) {
            size_t epos = m.find("entry");
            if (epos != std::string::npos) {
                size_t q1 = m.find('"', epos);
                size_t q2 = (q1 != std::string::npos) ? m.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    filepath = m.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
        if (filepath.empty()) filepath = "src/main.rhea";
    } else {
        std::cerr << "error[E3002]: missing input '.rhea' file for 'rhea " << cmd << "'\n"
                  << "   = help: Pass a source file (e.g. 'rhea " << cmd << " src/main.rhea') or run inside a project directory with 'rhea.toml'.\n";
        return 1;
    }

    std::string source = read_file_or_exit(filepath);
    try {
        Lexer lexer(filepath, source);
        auto tokens = lexer.tokenize();
        if (cmd == "tokens") {
            for (const auto& t : tokens) {
                std::cout << t.span.line << ":" << t.span.col << " " << token_kind_name(t.kind)
                          << " '" << t.lexeme << "'\n";
            }
            return 0;
        }

        Parser parser(filepath, std::move(tokens));
        Program prog = parser.parse_program();
        ModuleResolver resolver;
        resolver.resolve_imports(prog, filepath);
        TypeChecker checker(filepath, resolver.private_functions());
        checker.check_program(prog);

        if (cmd == "check" || cmd == "lint") {
            std::cout << "[OK] " << filepath << " passed static type, module, and mutability checks.\n";
            return 0;
        }
        if (cmd == "run") {
            Interpreter interp(filepath);
            Value ret = interp.run(prog);
            return (ret.kind == Value::Kind::Int) ? static_cast<int>(ret.int_val) : 0;
        }
        if (cmd == "vm") {
            validate_backend_capabilities(prog, "vm", filepath);
            BytecodeCompiler bc;
            BytecodeChunk chunk = bc.compile(prog);
            BytecodeVM vm(filepath);
            Value ret = vm.execute(chunk);
            return (ret.kind == Value::Kind::Int) ? static_cast<int>(ret.int_val) : 0;
        }
        if (cmd == "disasm" || cmd == "emit-ir") {
            validate_backend_capabilities(prog, "vm", filepath);
            BytecodeCompiler bc;
            BytecodeChunk chunk = bc.compile(prog);
            std::cout << chunk.disassemble();
            return 0;
        }
        if (cmd == "emit-cpp") {
            validate_backend_capabilities(prog, "compile", filepath);
            NativeCodegen cg;
            std::cout << cg.emit_cpp(prog);
            return 0;
        }
        if (cmd == "compile") {
            validate_backend_capabilities(prog, "compile", filepath);
            std::string out_bin = "build/a.out";
            for (int i = 2; i < argc; ++i) {
                if (std::string(argv[i]) == "-o") {
                    if (i + 1 >= argc) {
                        std::cerr << "error[E3002]: missing output path after '-o'\n"
                                  << "   = help: Pass an output binary path, e.g. 'rhea compile " << filepath << " -o build/app'.\n";
                        return 1;
                    }
                    out_bin = argv[++i];
                }
            }
            fs::create_directories("build");
            if (fs::path(out_bin).has_parent_path()) {
                fs::create_directories(fs::path(out_bin).parent_path());
            }
            std::string tmp_cpp = (fs::temp_directory_path() / ("rhea_codegen_" + std::to_string(getpid()) + ".cpp")).string();
            NativeCodegen cg;
            {
                std::ofstream ofs(tmp_cpp);
                ofs << cg.emit_cpp(prog);
            }
            const char* env_cxx = std::getenv("RHEA_CXX");
            if (!env_cxx || !*env_cxx) env_cxx = std::getenv("CXX");
            std::string cxx_bin = (env_cxx && *env_cxx) ? env_cxx : "c++";
            int rc = run_compiler_process({cxx_bin, "-std=c++20", "-O2", tmp_cpp, "-o", out_bin});
            fs::remove(tmp_cpp);
            if (rc != 0) {
                throw RheaException(make_diag(
                    "E2003",
                    "native C++20 compilation failed with exit code " + std::to_string(rc),
                    filepath,
                    Span{1, 1, 1},
                    "Verify that a C++20 compiler ('c++', 'clang++', or 'g++') is installed or run with 'rhea run " + filepath + "'."
                ));
            }
            std::cout << "[OK] Compiled " << filepath << " -> " << out_bin << "\n";
            return 0;
        }

        std::cerr << "error[E3002]: unknown command '" << cmd << "'\n";
        return 1;
    } catch (const RheaException& ex) {
        std::string err_src = source;
        if (ex.diag.filename != filepath && fs::exists(ex.diag.filename)) {
            std::ifstream eifs(ex.diag.filename);
            std::ostringstream ess;
            ess << eifs.rdbuf();
            err_src = ess.str();
        }
        std::cerr << ex.diag.render(err_src);
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "internal error: " << ex.what() << "\n";
        return 1;
    }
}
