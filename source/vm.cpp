#include "luau_vm/vm.hpp"

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luau_vm {
namespace {

enum class TokenKind {
    End, Number, String, Identifier,
    Plus, Minus, Star, Slash, Percent, Caret, Concat,
    LeftParen, RightParen, Semicolon, Comma,
    Equal, EqualEqual, NotEqual, Less, LessEqual, Greater, GreaterEqual
};

struct Token {
    TokenKind kind;
    std::string text;
    std::size_t offset;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    std::vector<Token> scan() {
        std::vector<Token> tokens;
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++pos_; continue; }
            if (c == '-' && peek(1) == '-') {
                pos_ += 2;
                while (pos_ < source_.size() && source_[pos_] != '\n') ++pos_;
                continue;
            }
            const auto start = pos_;
            if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
                scan_number(tokens, start);
                continue;
            }
            if (is_alpha(c) || c == '_') {
                ++pos_;
                while (pos_ < source_.size() && (is_alpha(source_[pos_]) || is_digit(source_[pos_]) || source_[pos_] == '_')) ++pos_;
                tokens.push_back({TokenKind::Identifier, std::string(source_.substr(start, pos_ - start)), start});
                continue;
            }
            if (c == '\'' || c == '"') {
                const char quote = c; ++pos_; std::string value; bool closed = false;
                while (pos_ < source_.size()) {
                    char ch = source_[pos_++];
                    if (ch == quote) { closed = true; break; }
                    if (ch == '\\') {
                        if (pos_ >= source_.size()) break;
                        const char escaped = source_[pos_++];
                        switch (escaped) {
                            case 'n': value.push_back('\n'); break;
                            case 'r': value.push_back('\r'); break;
                            case 't': value.push_back('\t'); break;
                            case '\\': value.push_back('\\'); break;
                            case '\'': value.push_back('\''); break;
                            case '"': value.push_back('"'); break;
                            default: value.push_back(escaped); break;
                        }
                    } else value.push_back(ch);
                }
                if (!closed) throw std::runtime_error("Unterminated string at offset " + std::to_string(start));
                tokens.push_back({TokenKind::String, std::move(value), start});
                continue;
            }
            switch (c) {
                case '+': simple(tokens, TokenKind::Plus, start); break;
                case '-': simple(tokens, TokenKind::Minus, start); break;
                case '*': simple(tokens, TokenKind::Star, start); break;
                case '/': simple(tokens, TokenKind::Slash, start); break;
                case '%': simple(tokens, TokenKind::Percent, start); break;
                case '^': simple(tokens, TokenKind::Caret, start); break;
                case '(': simple(tokens, TokenKind::LeftParen, start); break;
                case ')': simple(tokens, TokenKind::RightParen, start); break;
                case ';': simple(tokens, TokenKind::Semicolon, start); break;
                case ',': simple(tokens, TokenKind::Comma, start); break;
                case '.':
                    if (peek(1) == '.') { pos_ += 2; tokens.push_back({TokenKind::Concat, "..", start}); }
                    else throw std::runtime_error("Unexpected '.' at offset " + std::to_string(start));
                    break;
                case '=': two(tokens, '=', TokenKind::EqualEqual, TokenKind::Equal, start); break;
                case '~':
                    if (peek(1) == '=') { pos_ += 2; tokens.push_back({TokenKind::NotEqual, "~=", start}); }
                    else throw std::runtime_error("Unexpected '~' at offset " + std::to_string(start));
                    break;
                case '<': two(tokens, '=', TokenKind::LessEqual, TokenKind::Less, start); break;
                case '>': two(tokens, '=', TokenKind::GreaterEqual, TokenKind::Greater, start); break;
                default: throw std::runtime_error("Unexpected character at offset " + std::to_string(start));
            }
        }
        tokens.push_back({TokenKind::End, "", source_.size()});
        return tokens;
    }
private:
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }
    static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    char peek(std::size_t ahead) const { return pos_ + ahead < source_.size() ? source_[pos_ + ahead] : '\0'; }
    void simple(std::vector<Token>& out, TokenKind kind, std::size_t start) {
        out.push_back({kind, std::string(1, source_[pos_]), start}); ++pos_;
    }
    void two(std::vector<Token>& out, char expected, TokenKind yes, TokenKind no, std::size_t start) {
        const char first = source_[pos_++];
        if (pos_ < source_.size() && source_[pos_] == expected) {
            ++pos_; out.push_back({yes, std::string(source_.substr(start, 2)), start});
        } else out.push_back({no, std::string(1, first), start});
    }
    void scan_number(std::vector<Token>& out, std::size_t start) {
        bool saw_dot = false;
        if (source_[pos_] == '.') { saw_dot = true; ++pos_; }
        while (is_digit(peek(0))) ++pos_;
        if (!saw_dot && peek(0) == '.' && peek(1) != '.') {
            saw_dot = true; ++pos_;
            while (is_digit(peek(0))) ++pos_;
        }
        if (peek(0) == 'e' || peek(0) == 'E') {
            const auto exponent_start = pos_++;
            if (peek(0) == '+' || peek(0) == '-') ++pos_;
            if (!is_digit(peek(0))) throw std::runtime_error("Invalid exponent at offset " + std::to_string(exponent_start));
            while (is_digit(peek(0))) ++pos_;
        }
        out.push_back({TokenKind::Number, std::string(source_.substr(start, pos_ - start)), start});
    }
    std::string_view source_;
    std::size_t pos_{};
};

bool truthy(const Value& value) {
    if (std::holds_alternative<Nil>(value)) return false;
    if (const auto* b = std::get_if<bool>(&value)) return *b;
    return true;
}

std::string type_name(const Value& v) {
    if (std::holds_alternative<Nil>(v)) return "nil";
    if (std::holds_alternative<bool>(v)) return "boolean";
    if (std::holds_alternative<double>(v)) return "number";
    if (std::holds_alternative<std::string>(v)) return "string";
    if (std::holds_alternative<TableRef>(v)) return "table";
    return "function";
}

double number(const Value& v, const Token& token) {
    if (const auto* n = std::get_if<double>(&v)) return *n;
    throw std::runtime_error("Expected number at offset " + std::to_string(token.offset) + ", got " + type_name(v));
}

std::string stringify_for_concat(const Value& value, const Token& token) {
    if (const auto* s = std::get_if<std::string>(&value)) return *s;
    if (const auto* n = std::get_if<double>(&value)) {
        std::string result = std::to_string(*n);
        while (result.size() > 1 && result.back() == '0') result.pop_back();
        if (!result.empty() && result.back() == '.') result.pop_back();
        return result;
    }
    throw std::runtime_error("Concatenation expects strings or numbers at offset " + std::to_string(token.offset));
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Value run() {
        while (!check(TokenKind::End)) {
            if (match_identifier("local")) {
                const auto name = consume(TokenKind::Identifier, "Expected variable name after 'local'").text;
                Value value = Nil{};
                if (match(TokenKind::Equal)) value = expression();
                variables_[name] = std::move(value);
            } else if (match_identifier("return")) {
                if (check(TokenKind::End) || check(TokenKind::Semicolon)) return Nil{};
                Value result = expression();
                match(TokenKind::Semicolon);
                if (!check(TokenKind::End)) fail("Only one return value is supported in this milestone");
                return result;
            } else if (check(TokenKind::Identifier) && check_next(TokenKind::Equal)) {
                const auto name = advance().text; advance();
                auto it = variables_.find(name);
                if (it == variables_.end()) fail("Assignment to unknown variable '" + name + "'");
                it->second = expression();
            } else {
                (void)expression();
            }
            match(TokenKind::Semicolon);
        }
        return Nil{};
    }
private:
    Value expression() { return equality(); }
    Value equality() {
        Value left = comparison();
        while (match(TokenKind::EqualEqual) || match(TokenKind::NotEqual)) {
            const auto op = previous().kind; Value right = comparison(); bool equal = false;
            if (left.index() == right.index()) {
                if (std::holds_alternative<Nil>(left)) equal = true;
                else if (const auto* a = std::get_if<bool>(&left)) equal = *a == std::get<bool>(right);
                else if (const auto* a = std::get_if<double>(&left)) equal = *a == std::get<double>(right);
                else if (const auto* a = std::get_if<std::string>(&left)) equal = *a == std::get<std::string>(right);
                else equal = left == right;
            }
            left = (op == TokenKind::EqualEqual) ? equal : !equal;
        }
        return left;
    }
    Value comparison() {
        Value left = concat();
        while (match(TokenKind::Less) || match(TokenKind::LessEqual) || match(TokenKind::Greater) || match(TokenKind::GreaterEqual)) {
            const auto op = previous(); Value right = concat();
            if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
                const auto& a = std::get<std::string>(left); const auto& b = std::get<std::string>(right);
                if (op.kind == TokenKind::Less) left = a < b;
                else if (op.kind == TokenKind::LessEqual) left = a <= b;
                else if (op.kind == TokenKind::Greater) left = a > b;
                else left = a >= b;
            } else {
                const double a = number(left, op), b = number(right, op);
                if (op.kind == TokenKind::Less) left = a < b;
                else if (op.kind == TokenKind::LessEqual) left = a <= b;
                else if (op.kind == TokenKind::Greater) left = a > b;
                else left = a >= b;
            }
        }
        return left;
    }
    Value concat() {
        Value left = term();
        if (match(TokenKind::Concat)) {
            const auto op = previous();
            Value right = concat(); // right-associative, as in Lua/Luau
            left = stringify_for_concat(left, op) + stringify_for_concat(right, op);
        }
        return left;
    }
    Value term() {
        Value left = factor();
        while (match(TokenKind::Plus) || match(TokenKind::Minus)) {
            const auto op = previous(); Value right = factor();
            if (op.kind == TokenKind::Plus && std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right))
                throw std::runtime_error("Use '..' to concatenate strings at offset " + std::to_string(op.offset));
            const double a = number(left, op), b = number(right, op);
            left = op.kind == TokenKind::Plus ? a + b : a - b;
        }
        return left;
    }
    Value factor() {
        Value left = unary();
        while (match(TokenKind::Star) || match(TokenKind::Slash) || match(TokenKind::Percent)) {
            const auto op = previous(); Value right = unary();
            const double a = number(left, op), b = number(right, op);
            if (op.kind == TokenKind::Star) left = a * b;
            else if (op.kind == TokenKind::Slash) left = a / b;
            else left = std::fmod(a, b);
        }
        return left;
    }
    Value unary() {
        if (match(TokenKind::Minus)) { const auto op = previous(); return -number(unary(), op); }
        if (match_identifier("not")) return !truthy(unary());
        return power();
    }
    Value power() {
        Value left = primary();
        if (match(TokenKind::Caret)) { const auto op = previous(); left = std::pow(number(left, op), number(unary(), op)); }
        return left;
    }
    Value primary() {
        if (match(TokenKind::Number)) {
            const auto& t = previous(); char* end = nullptr;
            const double n = std::strtod(t.text.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(n)) fail_at(t, "Invalid numeric literal");
            return n;
        }
        if (match(TokenKind::String)) return previous().text;
        if (match(TokenKind::LeftParen)) { Value v = expression(); consume(TokenKind::RightParen, "Expected ')' after expression"); return v; }
        if (match(TokenKind::Identifier)) {
            const auto token = previous();
            if (token.text == "nil") return Nil{};
            if (token.text == "true") return true;
            if (token.text == "false") return false;
            const auto it = variables_.find(token.text);
            if (it == variables_.end()) fail_at(token, "Unknown variable '" + token.text + "'");
            return it->second;
        }
        fail("Expected expression");
    }
    bool check(TokenKind kind) const { return peek().kind == kind; }
    bool check_next(TokenKind kind) const { return current_ + 1 < tokens_.size() && tokens_[current_ + 1].kind == kind; }
    bool match(TokenKind kind) { if (!check(kind)) return false; advance(); return true; }
    bool match_identifier(std::string_view text) { if (check(TokenKind::Identifier) && peek().text == text) { advance(); return true; } return false; }
    const Token& advance() { if (!check(TokenKind::End)) ++current_; return previous(); }
    const Token& peek() const { return tokens_[current_]; }
    const Token& previous() const { return tokens_[current_ - 1]; }
    Token consume(TokenKind kind, const std::string& message) { if (check(kind)) return advance(); fail(message); }
    [[noreturn]] void fail(const std::string& message) const { fail_at(peek(), message); }
    [[noreturn]] void fail_at(const Token& token, const std::string& message) const { throw std::runtime_error(message + " at offset " + std::to_string(token.offset)); }

    std::vector<Token> tokens_;
    std::size_t current_{};
    std::unordered_map<std::string, Value> variables_;
};
} // namespace

struct VM::Impl {};

VM::VM() : impl_(std::make_unique<Impl>()) {}
VM::~VM() = default;
VM::VM(VM&&) noexcept = default;
VM& VM::operator=(VM&&) noexcept = default;

Result<Value> VM::execute_source(std::string_view source) {
    if (!impl_) return Result<Value>::failure("VM is not initialized");
    if (source.empty()) return Result<Value>::failure("Source code is empty");
    try {
        Lexer lexer(source);
        Parser parser(lexer.scan());
        return Result<Value>::success(parser.run());
    } catch (const std::exception& e) {
        return Result<Value>::failure(e.what());
    }
}

} // namespace luau_vm
