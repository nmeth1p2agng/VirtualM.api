#include "luau_vm/vm.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luau_vm {
namespace {

enum class TokenKind {
    End, Number, String, Identifier,
    Plus, Minus, Star, Slash, Percent, Caret,
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
            if (is_digit(c)) {
                ++pos_;
                while (pos_ < source_.size() && (is_digit(source_[pos_]) || source_[pos_] == '.')) ++pos_;
                tokens.push_back({TokenKind::Number, std::string(source_.substr(start, pos_ - start)), start});
                continue;
            }
            if (is_alpha(c) || c == '_') {
                ++pos_;
                while (pos_ < source_.size() && (is_alpha(source_[pos_]) || is_digit(source_[pos_]) || source_[pos_] == '_')) ++pos_;
                tokens.push_back({TokenKind::Identifier, std::string(source_.substr(start, pos_ - start)), start});
                continue;
            }
            if (c == '\'' || c == '"') {
                const char quote = c; ++pos_; std::string value;
                bool closed = false;
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
            auto two = [&](char expected, TokenKind yes, TokenKind no) {
                ++pos_;
                if (pos_ < source_.size() && source_[pos_] == expected) {
                    ++pos_; tokens.push_back({yes, std::string(source_.substr(start, 2)), start});
                } else tokens.push_back({no, std::string(1, c), start});
            };
            switch (c) {
                case '+': ++pos_; tokens.push_back({TokenKind::Plus, "+", start}); break;
                case '-': ++pos_; tokens.push_back({TokenKind::Minus, "-", start}); break;
                case '*': ++pos_; tokens.push_back({TokenKind::Star, "*", start}); break;
                case '/': ++pos_; tokens.push_back({TokenKind::Slash, "/", start}); break;
                case '%': ++pos_; tokens.push_back({TokenKind::Percent, "%", start}); break;
                case '^': ++pos_; tokens.push_back({TokenKind::Caret, "^", start}); break;
                case '(': ++pos_; tokens.push_back({TokenKind::LeftParen, "(", start}); break;
                case ')': ++pos_; tokens.push_back({TokenKind::RightParen, ")", start}); break;
                case ';': ++pos_; tokens.push_back({TokenKind::Semicolon, ";", start}); break;
                case ',': ++pos_; tokens.push_back({TokenKind::Comma, ",", start}); break;
                case '=': two('=', TokenKind::EqualEqual, TokenKind::Equal); break;
                case '~':
                    ++pos_;
                    if (pos_ < source_.size() && source_[pos_] == '=') { ++pos_; tokens.push_back({TokenKind::NotEqual, "~=", start}); }
                    else throw std::runtime_error("Unexpected '~' at offset " + std::to_string(start));
                    break;
                case '<': two('=', TokenKind::LessEqual, TokenKind::Less); break;
                case '>': two('=', TokenKind::GreaterEqual, TokenKind::Greater); break;
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
                if (match(TokenKind::Semicolon)) {}
                if (!check(TokenKind::End)) fail("Only one return value is supported in this initial milestone");
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
            const auto op = previous().kind; Value right = comparison();
            bool equal = false;
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
        Value left = term();
        while (match(TokenKind::Less) || match(TokenKind::LessEqual) || match(TokenKind::Greater) || match(TokenKind::GreaterEqual)) {
            const auto op = previous(); Value right = term();
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
    Value term() {
        Value left = factor();
        while (match(TokenKind::Plus) || match(TokenKind::Minus)) {
            const auto op = previous(); Value right = factor();
            if (op.kind == TokenKind::Plus && std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right))
                left = std::get<std::string>(left) + std::get<std::string>(right);
            else {
                const double a = number(left, op), b = number(right, op);
                left = op.kind == TokenKind::Plus ? a + b : a - b;
            }
        }
        return left;
    }
    Value factor() {
        Value left = power();
        while (match(TokenKind::Star) || match(TokenKind::Slash) || match(TokenKind::Percent)) {
            const auto op = previous(); Value right = power();
            const double a = number(left, op), b = number(right, op);
            if (op.kind == TokenKind::Star) left = a * b;
            else if (op.kind == TokenKind::Slash) left = a / b;
            else left = std::fmod(a, b);
        }
        return left;
    }
    Value power() {
        Value left = unary();
        if (match(TokenKind::Caret)) { const auto op = previous(); left = std::pow(number(left, op), number(power(), op)); }
        return left;
    }
    Value unary() {
        if (match(TokenKind::Minus)) { const auto op = previous(); return -number(unary(), op); }
        if (match_identifier("not")) return !truthy(unary());
        return primary();
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
