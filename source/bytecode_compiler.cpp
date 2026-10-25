#include "luau_vm/bytecode_compiler.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <string>
#include <utility>

namespace luau_vm {
namespace {
enum class Kind { End, Number, String, Identifier, Op, LParen, RParen };
struct Token { Kind kind{Kind::End}; std::string text; double number{}; };

class Compiler {
public:
    explicit Compiler(std::string_view source) : source_(source) { next(); }

    Result<BytecodeChunk> run(std::vector<Value>& constants) {
        constants_ = &constants;
        constants.clear();
        if (current_.kind == Kind::End) return Result<BytecodeChunk>::failure("Expression is empty");
        if (!expression(0)) return Result<BytecodeChunk>::failure(error_);
        if (current_.kind != Kind::End) return fail("unexpected token '" + current_.text + "'");
        chunk_.constant_count = constants.size();
        chunk_.local_count = 0;
        chunk_.instructions.push_back({OpCode::Return, 0});
        const auto verification = verify_bytecode(chunk_);
        if (!verification.valid) return Result<BytecodeChunk>::failure("Compiler produced invalid bytecode: " + verification.error);
        return Result<BytecodeChunk>::success(std::move(chunk_));
    }

private:
    std::string_view source_;
    std::size_t pos_{0};
    Token current_;
    std::string error_;
    BytecodeChunk chunk_;
    std::vector<Value>* constants_{nullptr};

    Result<BytecodeChunk> fail(std::string message) {
        return Result<BytecodeChunk>::failure("Bytecode compiler: " + std::move(message));
    }
    bool set_error(std::string message) { if (error_.empty()) error_ = "Bytecode compiler: " + std::move(message); return false; }
    void skip_space() { while (pos_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[pos_]))) ++pos_; }
    void next() {
        skip_space(); current_ = {};
        if (pos_ >= source_.size()) return;
        const char c = source_[pos_];
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && pos_ + 1 < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_ + 1])))) {
            const std::size_t start = pos_++;
            while (pos_ < source_.size()) {
                const char d = source_[pos_];
                if (std::isalnum(static_cast<unsigned char>(d)) || d == '.' || d == '+' || d == '-') {
                    if ((d == '+' || d == '-') && pos_ > start && source_[pos_ - 1] != 'e' && source_[pos_ - 1] != 'E') break;
                    ++pos_;
                } else break;
            }
            current_.kind = Kind::Number; current_.text = std::string(source_.substr(start, pos_ - start));
            char* end = nullptr; current_.number = std::strtod(current_.text.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(current_.number)) { current_.kind = Kind::Op; current_.text = "<invalid-number>"; }
            return;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const auto start = pos_++;
            while (pos_ < source_.size() && (std::isalnum(static_cast<unsigned char>(source_[pos_])) || source_[pos_] == '_')) ++pos_;
            current_.kind = Kind::Identifier; current_.text = std::string(source_.substr(start, pos_ - start)); return;
        }
        if (c == '\'' || c == '"') {
            const char quote = c; ++pos_; std::string value; bool closed = false;
            while (pos_ < source_.size()) {
                char d = source_[pos_++];
                if (d == quote) { closed = true; break; }
                if (d == '\\') {
                    if (pos_ >= source_.size()) break;
                    d = source_[pos_++];
                    switch (d) { case 'n': value.push_back('\n'); break; case 'r': value.push_back('\r'); break; case 't': value.push_back('\t'); break; case '\\': value.push_back('\\'); break; case '\'': value.push_back('\''); break; case '"': value.push_back('"'); break; default: value.push_back(d); break; }
                } else value.push_back(d);
            }
            current_.kind = closed ? Kind::String : Kind::Op; current_.text = closed ? std::move(value) : "<unterminated-string>"; return;
        }
        if (c == '(') { ++pos_; current_ = {Kind::LParen, "("}; return; }
        if (c == ')') { ++pos_; current_ = {Kind::RParen, ")"}; return; }
        for (const char* op : {"//", "..", "==", "<=", ">=", "~=", "+", "-", "*", "/", "%", "^", "<", ">"}) {
            const std::string_view candidate(op);
            if (source_.substr(pos_, candidate.size()) == candidate) { current_ = {Kind::Op, std::string(candidate)}; pos_ += candidate.size(); return; }
        }
        current_ = {Kind::Op, std::string(1, c)}; ++pos_;
    }
    int precedence(const Token& t) const {
        if (t.kind == Kind::Identifier && t.text == "or") return 1;
        if (t.kind == Kind::Identifier && t.text == "and") return 2;
        if (t.kind != Kind::Op) return -1;
        if (t.text == "==" || t.text == "~=" || t.text == "<" || t.text == "<=" || t.text == ">" || t.text == ">=") return 3;
        if (t.text == "..") return 4;
        if (t.text == "+" || t.text == "-") return 5;
        if (t.text == "*" || t.text == "/" || t.text == "//" || t.text == "%") return 6;
        if (t.text == "^") return 8;
        return -1;
    }
    bool emit_constant(Value v) {
        if (constants_->size() >= static_cast<std::size_t>(INT32_MAX)) return set_error("too many constants");
        const auto index = static_cast<std::int32_t>(constants_->size()); constants_->push_back(std::move(v));
        chunk_.instructions.push_back({OpCode::LoadConstant, index}); return true;
    }
    bool primary() {
        if (current_.kind == Kind::Op && current_.text == "<invalid-number>") return set_error("invalid numeric literal");
        if (current_.kind == Kind::Number) { const double n = current_.number; next(); return emit_constant(n); }
        if (current_.kind == Kind::String) { auto s = current_.text; next(); return emit_constant(std::move(s)); }
        if (current_.kind == Kind::Identifier) {
            auto name = current_.text; next();
            if (name == "true" || name == "false") { chunk_.instructions.push_back({OpCode::LoadBoolean, name == "true" ? 1 : 0}); return true; }
            if (name == "nil") { chunk_.instructions.push_back({OpCode::LoadNil, 0}); return true; }
            if (name == "not") { if (!expression(7)) return false; chunk_.instructions.push_back({OpCode::LogicalNot, 0}); return true; }
            return set_error("unsupported identifier '" + name + "'");
        }
        if (current_.kind == Kind::Op && (current_.text == "-")) { next(); if (!expression(7)) return false; chunk_.instructions.push_back({OpCode::Negate, 0}); return true; }
        if (current_.kind == Kind::LParen) { next(); if (!expression(0)) return false; if (current_.kind != Kind::RParen) return set_error("expected ')' "); next(); return true; }
        return set_error("expected a literal or parenthesized expression");
    }
    bool expression(int min_precedence) {
        if (!primary()) return false;
        while (true) {
            const int p = precedence(current_);
            if (p < min_precedence) break;
            const auto op = current_; next();
            const bool right_associative = op.text == "^" || op.text == "..";
            if (!expression(p + (right_associative ? 0 : 1))) return false;
            OpCode code;
            if (op.text == "+") code = OpCode::Add;
            else if (op.text == "-") code = OpCode::Subtract;
            else if (op.text == "*") code = OpCode::Multiply;
            else if (op.text == "/") code = OpCode::Divide;
            else if (op.text == "//") code = OpCode::FloorDivide;
            else if (op.text == "%") code = OpCode::Modulo;
            else if (op.text == "^") code = OpCode::Power;
            else if (op.text == "..") code = OpCode::Concatenate;
            else if (op.text == "==") code = OpCode::Equal;
            else if (op.text == "<") code = OpCode::LessThan;
            else if (op.text == "<=") code = OpCode::LessEqual;
            else if (op.text == ">") code = OpCode::GreaterThan;
            else if (op.text == ">=") code = OpCode::GreaterEqual;
            else if (op.text == "~=") { chunk_.instructions.push_back({OpCode::Equal, 0}); chunk_.instructions.push_back({OpCode::LogicalNot, 0}); continue; }
            else return set_error("operator '" + op.text + "' is not supported by this bytecode subset yet");
            chunk_.instructions.push_back({code, 0});
        }
        return true;
    }
};
} // namespace

Result<BytecodeChunk> compile_bytecode_expression(std::string_view source, std::vector<Value>& constants) {
    Compiler compiler(source);
    return compiler.run(constants);
}

} // namespace luau_vm
