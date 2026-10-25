#include "luau_vm/vm.hpp"

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luau_vm {
namespace {

enum class TokenKind {
    End, Number, String, Identifier,
    Plus, Minus, Star, Slash, FloorSlash, Percent, Caret, Concat,
    LeftParen, RightParen, LeftBrace, RightBrace, LeftBracket, RightBracket,
    Dot, Semicolon, Comma, Hash,
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
                case '/':
                    if (peek(1) == '/') { pos_ += 2; tokens.push_back({TokenKind::FloorSlash, "//", start}); }
                    else simple(tokens, TokenKind::Slash, start);
                    break;
                case '%': simple(tokens, TokenKind::Percent, start); break;
                case '^': simple(tokens, TokenKind::Caret, start); break;
                case '#': simple(tokens, TokenKind::Hash, start); break;
                case '(': simple(tokens, TokenKind::LeftParen, start); break;
                case ')': simple(tokens, TokenKind::RightParen, start); break;
                case '{': simple(tokens, TokenKind::LeftBrace, start); break;
                case '}': simple(tokens, TokenKind::RightBrace, start); break;
                case '[': simple(tokens, TokenKind::LeftBracket, start); break;
                case ']': simple(tokens, TokenKind::RightBracket, start); break;
                case ';': simple(tokens, TokenKind::Semicolon, start); break;
                case ',': simple(tokens, TokenKind::Comma, start); break;
                case '.':
                    if (peek(1) == '.') { pos_ += 2; tokens.push_back({TokenKind::Concat, "..", start}); }
                    else simple(tokens, TokenKind::Dot, start);
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
        if (!saw_dot && peek(0) == '.' && is_digit(peek(1))) {
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

using Table = std::unordered_map<std::string, Value>;

std::string table_key(const Value& value, const Token& token) {
    if (const auto* s = std::get_if<std::string>(&value)) return "s:" + *s;
    if (const auto* n = std::get_if<double>(&value)) {
        if (!std::isfinite(*n)) throw std::runtime_error("Table index cannot be NaN or infinity at offset " + std::to_string(token.offset));
        std::ostringstream out;
        out << "n:" << std::setprecision(17) << *n;
        return out.str();
    }
    if (const auto* b = std::get_if<bool>(&value)) return *b ? "b:true" : "b:false";
    if (std::holds_alternative<Nil>(value)) throw std::runtime_error("Table index cannot be nil at offset " + std::to_string(token.offset));
    if (const auto* t = std::get_if<TableRef>(&value)) return "t:" + std::to_string(t->index);
    if (const auto* f = std::get_if<FunctionRef>(&value)) return "f:" + std::to_string(f->index);
    throw std::runtime_error("Unsupported table index at offset " + std::to_string(token.offset));
}

struct FunctionDef {
    std::vector<std::string> parameters;
    std::vector<Token> body;
};

struct ExecutionState {
    VMOptions limits;
    std::size_t steps{};
    std::size_t table_entries{};
    std::size_t call_depth{};

    void charge(std::size_t amount = 1) {
        if (amount > limits.max_execution_steps - std::min(steps, limits.max_execution_steps))
            throw std::runtime_error("Execution step limit exceeded");
        steps += amount;
    }
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, std::vector<Table>& tables,
                    std::vector<FunctionDef>& functions, ExecutionState& state,
                    std::unordered_map<std::string, Value> initial = {})
        : tokens_(std::move(tokens)), tables_(tables), functions_(functions), state_(state) {
        scopes_.emplace_back(std::move(initial));
    }

    Value run() {
        while (!check(TokenKind::End) && !returned_) {
            if (match_identifier("if")) {
                execute_if();
            } else if (match_identifier("while")) {
                execute_while();
            } else if (match_identifier("function")) {
                function_statement(false);
            } else {
                statement();
            }
            match(TokenKind::Semicolon);
        }
        return return_value_;
    }
private:
    void statement() {
        if (match_identifier("local")) {
            if (match_identifier("function")) { function_statement(true); return; }
            const auto name = consume(TokenKind::Identifier, "Expected variable name after 'local'").text;
            Value value = Nil{};
            if (match(TokenKind::Equal)) value = expression();
            scopes_.back()[name] = std::move(value);
        } else if (match_identifier("return")) {
            if (check(TokenKind::End) || check(TokenKind::Semicolon) || is_block_terminator()) return_value_ = Nil{};
            else return_value_ = expression();
            returned_ = true;
        } else if (check(TokenKind::Identifier) && check_next(TokenKind::Equal)) {
            const auto name = advance().text; advance();
            auto* slot = find_variable(name);
            if (!slot) fail("Assignment to unknown variable '" + name + "'");
            *slot = expression();
        } else if (is_simple_table_assignment()) {
            const auto name = advance().text;
            const auto* variable = find_variable_value(name);
            if (!variable) fail("Assignment to unknown variable '" + name + "'");
            Token key_token = peek();
            Value key = Nil{};
            if (match(TokenKind::Dot)) {
                key_token = consume(TokenKind::Identifier, "Expected field name after '.'");
                key = key_token.text;
            } else {
                consume(TokenKind::LeftBracket, "Expected '['");
                key = expression();
                key_token = previous();
                consume(TokenKind::RightBracket, "Expected ']' after index");
            }
            consume(TokenKind::Equal, "Expected '=' in table assignment");
            Value assigned = expression();
            const auto* ref = std::get_if<TableRef>(variable);
            if (!ref) fail_at(key_token, "Attempt to index a " + type_name(*variable) + " value");
            if (ref->index >= tables_.size()) fail_at(key_token, "Invalid table reference");
            const auto encoded = table_key(key, key_token);
            if (std::holds_alternative<Nil>(assigned)) tables_[ref->index].erase(encoded);
            else tables_[ref->index][encoded] = std::move(assigned);
        } else {
            (void)expression();
        }
    }

    bool is_block_terminator() const {
        return check(TokenKind::Identifier) && (peek().text == "end" || peek().text == "else" || peek().text == "elseif");
    }

    std::size_t find_block_end(std::size_t from) const {
        int depth = 1;
        for (std::size_t i = from; i < tokens_.size(); ++i) {
            if (tokens_[i].kind != TokenKind::Identifier) continue;
            const auto& word = tokens_[i].text;
            if (word == "if" || word == "while" || word == "function") ++depth;
            else if (word == "end" && --depth == 0) return i;
        }
        throw std::runtime_error("Expected 'end' for control-flow block");
    }

    void execute_range(std::size_t begin, std::size_t end) {
        const auto saved = current_;
        scopes_.emplace_back();
        current_ = begin;
        try {
            while (current_ < end && !returned_) {
                if (match_identifier("if")) execute_if();
                else if (match_identifier("while")) execute_while();
                else statement();
                match(TokenKind::Semicolon);
            }
        } catch (...) {
            scopes_.pop_back();
            current_ = saved;
            throw;
        }
        scopes_.pop_back();
        current_ = saved;
    }

    void execute_if() {
        const auto first_condition = current_;
        Value condition = expression();
        consume_identifier("then", "Expected 'then' after if condition");
        const auto first_body = current_;
        const auto block_end = find_block_end(first_body);
        bool taken = false;
        if (truthy(condition)) {
            execute_range(first_body, find_branch_boundary(first_body, block_end));
            taken = true;
        }
        current_ = find_branch_boundary(first_body, block_end);
        while (current_ < block_end && check(TokenKind::Identifier) && peek().text == "elseif") {
            advance();
            const auto cond_start = current_;
            Value branch_condition = expression();
            consume_identifier("then", "Expected 'then' after elseif condition");
            const auto body_start = current_;
            const auto boundary = find_branch_boundary(body_start, block_end);
            if (!taken && truthy(branch_condition)) { execute_range(body_start, boundary); taken = true; }
            current_ = boundary;
            (void)cond_start;
        }
        if (current_ < block_end && match_identifier("else")) {
            if (!taken) execute_range(current_, block_end);
            current_ = block_end;
        }
        consume_identifier("end", "Expected 'end' after if block");
        (void)first_condition;
    }

    std::size_t find_branch_boundary(std::size_t from, std::size_t block_end) const {
        int depth = 0;
        for (std::size_t i = from; i < block_end; ++i) {
            if (tokens_[i].kind != TokenKind::Identifier) continue;
            const auto& word = tokens_[i].text;
            if (word == "if" || word == "while" || word == "function") ++depth;
            else if (word == "end" && depth > 0) --depth;
            else if (depth == 0 && (word == "elseif" || word == "else")) return i;
        }
        return block_end;
    }

    void execute_while() {
        const auto condition_start = current_;
        (void)expression();
        const auto condition_end = current_;
        consume_identifier("do", "Expected 'do' after while condition");
        const auto body_start = current_;
        const auto block_end = find_block_end(body_start);
        std::size_t iterations = 0;
        while (!returned_) {
            const auto saved = current_;
            current_ = condition_start;
            Value condition = expression();
            current_ = saved;
            if (!truthy(condition)) break;
            if (++iterations > state_.limits.max_execution_steps)
                fail_at(tokens_[condition_start], "Loop iteration limit exceeded");
            state_.charge();
            execute_range(body_start, block_end);
        }
        current_ = block_end;
        consume_identifier("end", "Expected 'end' after while block");
        (void)condition_end;
    }

    void function_statement(bool is_local) {
        const Token name_token = consume(TokenKind::Identifier, "Expected function name");
        consume(TokenKind::LeftParen, "Expected '(' after function name");
        std::vector<std::string> parameters;
        if (!check(TokenKind::RightParen)) {
            do {
                parameters.push_back(consume(TokenKind::Identifier, "Expected parameter name").text);
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RightParen, "Expected ')' after parameters");
        const std::size_t body_start = current_;
        const std::size_t end = find_block_end(body_start);
        FunctionDef def;
        def.parameters = std::move(parameters);
        def.body.assign(tokens_.begin() + static_cast<std::ptrdiff_t>(body_start),
                        tokens_.begin() + static_cast<std::ptrdiff_t>(end));
        def.body.push_back({TokenKind::End, "", tokens_[end].offset});
        if (functions_.size() >= state_.limits.max_functions ||
            functions_.size() >= static_cast<std::size_t>(UINT32_MAX))
            fail_at(name_token, "Function limit exceeded");
        const auto index = static_cast<std::uint32_t>(functions_.size());
        functions_.push_back(std::move(def));
        scopes_.back()[name_token.text] = FunctionRef{index};
        current_ = end + 1;
        (void)is_local;
    }

    void consume_identifier(std::string_view word, const std::string& message) {
        if (!match_identifier(word)) fail(message);
    }
private:
    Value expression() { return logical_or(); }
    Value logical_or() {
        Value left = logical_and();
        while (match_identifier("or")) {
            Value right = logical_and();
            left = truthy(left) ? left : right;
        }
        return left;
    }
    Value logical_and() {
        Value left = equality();
        while (match_identifier("and")) {
            Value right = equality();
            left = truthy(left) ? right : left;
        }
        return left;
    }
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
        while (match(TokenKind::Star) || match(TokenKind::Slash) || match(TokenKind::FloorSlash) || match(TokenKind::Percent)) {
            const auto op = previous(); Value right = unary();
            const double a = number(left, op), b = number(right, op);
            if (op.kind == TokenKind::Star) left = a * b;
            else if (op.kind == TokenKind::Slash) left = a / b;
            else if (op.kind == TokenKind::FloorSlash) left = std::floor(a / b);
            else left = std::fmod(a, b);
        }
        return left;
    }
    Value unary() {
        if (match(TokenKind::Minus)) { const auto op = previous(); return -number(unary(), op); }
        if (match_identifier("not")) return !truthy(unary());
        if (match(TokenKind::Hash)) {
            const auto op = previous();
            Value operand = unary();
            if (const auto* text = std::get_if<std::string>(&operand))
                return static_cast<double>(text->size());
            if (const auto* ref = std::get_if<TableRef>(&operand)) {
                if (ref->index >= tables_.size()) fail_at(op, "Invalid table reference");
                const auto& table = tables_[ref->index];
                std::size_t length = 0;
                for (;;) {
                    const double next = static_cast<double>(length + 1);
                    std::ostringstream encoded;
                    encoded << "n:" << std::setprecision(17) << next;
                    const auto it = table.find(encoded.str());
                    if (it == table.end() || std::holds_alternative<Nil>(it->second)) break;
                    ++length;
                }
                return static_cast<double>(length);
            }
            fail_at(op, "Length operator expects a string or table, got " + type_name(operand));
        }
        return power();
    }
    Value power() {
        Value left = primary();
        if (match(TokenKind::Caret)) { const auto op = previous(); left = std::pow(number(left, op), number(unary(), op)); }
        return left;
    }
    Value primary() {
        Value value = Nil{};
        if (match(TokenKind::Number)) {
            const auto& t = previous(); char* end = nullptr;
            const double n = std::strtod(t.text.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(n)) fail_at(t, "Invalid numeric literal");
            value = n;
        } else if (match(TokenKind::String)) {
            value = previous().text;
        } else if (match(TokenKind::LeftParen)) {
            value = expression();
            consume(TokenKind::RightParen, "Expected ')' after expression");
        } else if (match(TokenKind::LeftBrace)) {
            const auto open = previous();
            if (tables_.size() >= state_.limits.max_tables || tables_.size() >= static_cast<std::size_t>(UINT32_MAX))
                fail_at(open, "Table limit exceeded");
            const auto table_index = static_cast<std::uint32_t>(tables_.size());
            tables_.emplace_back();
            value = TableRef{table_index};
            double array_index = 1.0;
            while (!check(TokenKind::RightBrace) && !check(TokenKind::End)) {
                Value key = Nil{};
                Value item = Nil{};
                Token key_token = peek();
                if (match(TokenKind::LeftBracket)) {
                    key = expression();
                    key_token = previous();
                    consume(TokenKind::RightBracket, "Expected ']' after table key");
                    consume(TokenKind::Equal, "Expected '=' after table key");
                    item = expression();
                } else if (check(TokenKind::Identifier) && check_next(TokenKind::Equal)) {
                    key = advance().text;
                    key_token = previous();
                    advance();
                    item = expression();
                } else {
                    key = array_index++;
                    item = expression();
                }
                auto& table = tables_[table_index];
                const auto encoded = table_key(key, key_token);
                const auto existing = table.find(encoded);
                if (std::holds_alternative<Nil>(item)) {
                    if (existing != table.end()) { table.erase(existing); --state_.table_entries; }
                } else {
                    if (existing == table.end()) {
                        if (state_.table_entries >= state_.limits.max_table_entries) fail_at(key_token, "Table entry limit exceeded");
                        ++state_.table_entries;
                    }
                    table[encoded] = std::move(item);
                }
                if (!match(TokenKind::Comma) && !match(TokenKind::Semicolon)) break;
            }
            consume(TokenKind::RightBrace, "Expected '}' after table constructor");
        } else if (match(TokenKind::Identifier)) {
            const auto token = previous();
            if (token.text == "nil") value = Nil{};
            else if (token.text == "true") value = true;
            else if (token.text == "false") value = false;
            else {
                const auto* found = find_variable_value(token.text);
                if (!found) fail_at(token, "Unknown variable '" + token.text + "'");
                value = *found;
            }
        } else {
            fail("Expected expression");
        }

        while (check(TokenKind::LeftParen)) {
            const Token call_token = advance();
            std::vector<Value> arguments;
            if (!check(TokenKind::RightParen)) {
                do { arguments.push_back(expression()); } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RightParen, "Expected ')' after function arguments");
            const auto* function_ref = std::get_if<FunctionRef>(&value);
            if (!function_ref) fail_at(call_token, "Attempt to call a " + type_name(value) + " value");
            if (function_ref->index >= functions_.size()) fail_at(call_token, "Invalid function reference");
            const FunctionDef function = functions_[function_ref->index];
            if (arguments.size() != function.parameters.size())
                fail_at(call_token, "Function expected " + std::to_string(function.parameters.size()) + " arguments, got " + std::to_string(arguments.size()));
            std::unordered_map<std::string, Value> locals;
            for (std::size_t i = 0; i < arguments.size(); ++i) locals[function.parameters[i]] = std::move(arguments[i]);
            if (state_.call_depth >= state_.limits.max_call_depth) fail_at(call_token, "Function call depth limit exceeded");
            ++state_.call_depth;
            try {
                Parser callee(function.body, tables_, functions_, state_, std::move(locals));
                value = callee.run();
            } catch (...) {
                --state_.call_depth;
                throw;
            }
            --state_.call_depth;
        }

        while (true) {
            Token key_token = peek();
            Value key = Nil{};
            if (match(TokenKind::Dot)) {
                key_token = consume(TokenKind::Identifier, "Expected field name after '.'");
                key = key_token.text;
            } else if (match(TokenKind::LeftBracket)) {
                key = expression();
                key_token = previous();
                consume(TokenKind::RightBracket, "Expected ']' after index");
            } else {
                break;
            }
            const auto* table_ref = std::get_if<TableRef>(&value);
            if (!table_ref) fail_at(key_token, "Attempt to index a " + type_name(value) + " value");
            if (table_ref->index >= tables_.size()) fail_at(key_token, "Invalid table reference");
            const auto it = tables_[table_ref->index].find(table_key(key, key_token));
            value = it == tables_[table_ref->index].end() ? Value{Nil{}} : it->second;
        }
        return value;
    }
    bool is_simple_table_assignment() const {
        if (!check(TokenKind::Identifier) || (!check_next(TokenKind::Dot) && !check_next(TokenKind::LeftBracket))) return false;
        std::size_t i = current_ + 1;
        if (tokens_[i].kind == TokenKind::Dot) {
            i += 2;
            return i < tokens_.size() && tokens_[i].kind == TokenKind::Equal;
        }
        ++i;
        int brackets = 1;
        int parens = 0;
        for (; i < tokens_.size(); ++i) {
            const auto kind = tokens_[i].kind;
            if (kind == TokenKind::LeftBracket) ++brackets;
            else if (kind == TokenKind::RightBracket && --brackets == 0) {
                ++i;
                return i < tokens_.size() && tokens_[i].kind == TokenKind::Equal;
            } else if (kind == TokenKind::LeftParen) ++parens;
            else if (kind == TokenKind::RightParen) --parens;
            if (kind == TokenKind::End || (parens < 0)) return false;
        }
        return false;
    }

    bool check(TokenKind kind) const { return peek().kind == kind; }
    bool check_next(TokenKind kind) const { return current_ + 1 < tokens_.size() && tokens_[current_ + 1].kind == kind; }
    bool match(TokenKind kind) { if (!check(kind)) return false; advance(); return true; }
    bool match_identifier(std::string_view text) { if (check(TokenKind::Identifier) && peek().text == text) { advance(); return true; } return false; }
    const Token& advance() {
        if (!check(TokenKind::End)) { state_.charge(); ++current_; }
        return previous();
    }
    const Token& peek() const { return tokens_[current_]; }
    const Token& previous() const { return tokens_[current_ - 1]; }
    Token consume(TokenKind kind, const std::string& message) { if (check(kind)) return advance(); fail(message); }
    [[noreturn]] void fail(const std::string& message) const { fail_at(peek(), message); }
    [[noreturn]] void fail_at(const Token& token, const std::string& message) const { throw std::runtime_error(message + " at offset " + std::to_string(token.offset)); }

    Value* find_variable(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }
    const Value* find_variable_value(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    std::vector<Token> tokens_;
    std::size_t current_{};
    std::vector<std::unordered_map<std::string, Value>> scopes_;
    std::vector<Table>& tables_;
    std::vector<FunctionDef>& functions_;
    ExecutionState& state_;
    Value return_value_{Nil{}};
    bool returned_{};
};
} // namespace

struct VM::Impl {
    explicit Impl(VMOptions configured) : options(configured) {}
    VMOptions options;
    std::vector<Table> tables;
    std::vector<FunctionDef> functions;
};

VM::VM(VMOptions options) : impl_(std::make_unique<Impl>(options)) {}
VM::~VM() = default;
VM::VM(VM&&) noexcept = default;
VM& VM::operator=(VM&&) noexcept = default;

Result<Value> VM::execute_source(std::string_view source) {
    if (!impl_) return Result<Value>::failure("VM is not initialized");
    if (source.empty()) return Result<Value>::failure("Source code is empty");
    if (source.size() > impl_->options.max_source_bytes)
        return Result<Value>::failure("Source size limit exceeded");
    try {
        ExecutionState state{impl_->options};
        // Recount persistent objects in case the VM has executed earlier snippets.
        for (const auto& table : impl_->tables) state.table_entries += table.size();
        Lexer lexer(source);
        Parser parser(lexer.scan(), impl_->tables, impl_->functions, state);
        return Result<Value>::success(parser.run());
    } catch (const std::exception& e) {
        return Result<Value>::failure(e.what());
    }
}

} // namespace luau_vm
