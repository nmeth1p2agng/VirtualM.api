#include "luau_vm/obfuscator.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace luau_vm {
namespace {

bool is_word(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

bool long_bracket_open(std::string_view s, std::size_t pos, std::size_t& eq_count,
                       std::size_t& content_start) {
    if (pos >= s.size() || s[pos] != '[') return false;
    std::size_t i = pos + 1;
    while (i < s.size() && s[i] == '=') ++i;
    if (i >= s.size() || s[i] != '[') return false;
    eq_count = i - pos - 1;
    content_start = i + 1;
    return true;
}

std::size_t find_long_close(std::string_view s, std::size_t from, std::size_t eq_count) {
    for (std::size_t i = from; i < s.size(); ++i) {
        if (s[i] != ']') continue;
        std::size_t j = i + 1, n = 0;
        while (j < s.size() && s[j] == '=' && n < eq_count) { ++j; ++n; }
        if (n == eq_count && j < s.size() && s[j] == ']') return j + 1;
    }
    return s.size();
}

bool needs_separator(std::string_view output, char right) {
    if (output.empty()) return false;
    const char left = output.back();
    const auto a = static_cast<unsigned char>(left);
    const auto b = static_cast<unsigned char>(right);
    if (is_word(a) && is_word(b)) return true;

    // Keep whitespace whenever removing it could create a different Luau token.
    // This includes comparison/shift/concat operators, comments, type annotations,
    // and compound assignments. Check three-character tokens before two-character
    // tokens because Luau's lexer uses longest-match tokenization.
    if (output.size() >= 2) {
        const std::string tri{output[output.size() - 2], left, right};
        if (tri == "..." || tri == "..=" || tri == "//=" || tri == "<<=" || tri == ">>=")
            return true;
    }
    const std::string pair{left, right};
    static constexpr std::string_view operators[] = {
        "--", "..", "//", "==", "~=", "<=", ">=", "::", "<<", ">>",
        "+=", "-=", "*=", "/=", "%=", "^=", "->"
    };
    for (const auto op : operators) if (pair == op) return true;
    // A digit followed by a dot (or vice versa) can alter numeric tokenization;
    // preserve the original boundary rather than guessing at the literal grammar.
    if ((std::isdigit(a) && right == '.') || (left == '.' && std::isdigit(b))) return true;
    return false;
}

struct Token { std::size_t begin; std::size_t end; bool identifier; };

std::vector<Token> tokenize(std::string_view s) {
    std::vector<Token> out;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isspace(c)) { ++i; continue; }
        if (s[i] == '\'' || s[i] == '"') {
            const std::size_t start = i;
            const char q = s[i++];
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) { i += 2; continue; }
                if (s[i++] == q) break;
            }
            out.push_back({start, i, false});
            continue;
        }
        std::size_t eq = 0, content = 0;
        if (s[i] == '[' && long_bracket_open(s, i, eq, content)) {
            const std::size_t start = i;
            i = find_long_close(s, content, eq);
            out.push_back({start, i, false});
            continue;
        }
        if (std::isalpha(c) || s[i] == '_' || c >= 0x80) {
            const std::size_t start = i++;
            while (i < s.size()) {
                unsigned char d = static_cast<unsigned char>(s[i]);
                if (!is_word(d)) break;
                ++i;
            }
            out.push_back({start, i, true});
            continue;
        }
        if (std::isdigit(c)) {
            const std::size_t start = i++;
            while (i < s.size()) {
                unsigned char d = static_cast<unsigned char>(s[i]);
                if (!(std::isalnum(d) || s[i] == '.' || s[i] == '_')) break;
                ++i;
            }
            out.push_back({start, i, false});
            continue;
        }
        out.push_back({i, i + 1, false});
        ++i;
    }
    return out;
}

bool is_identifier(std::string_view s) {
    if (s.empty()) return false;
    unsigned char c = static_cast<unsigned char>(s[0]);
    if (!(std::isalpha(c) || s[0] == '_' || c >= 0x80)) return false;
    for (unsigned char d : s) if (!is_word(d)) return false;
    return true;
}

} // namespace

std::string minify_lua_source(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    std::size_t i = 0;
    bool pending_space = false;
    auto emit = [&](char c) {
        if (pending_space && !out.empty() && needs_separator(out, c)) out.push_back(' ');
        pending_space = false;
        out.push_back(c);
    };
    while (i < source.size()) {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        if (std::isspace(c)) { pending_space = true; ++i; continue; }
        if (source[i] == '-' && i + 1 < source.size() && source[i + 1] == '-') {
            i += 2;
            std::size_t eq = 0, content = 0;
            if (long_bracket_open(source, i, eq, content)) i = find_long_close(source, content, eq);
            else while (i < source.size() && source[i] != '\n' && source[i] != '\r') ++i;
            pending_space = true;
            continue;
        }
        if (source[i] == '\'' || source[i] == '"') {
            const char quote = source[i];
            emit(source[i++]);
            while (i < source.size()) {
                const char ch = source[i++]; emit(ch);
                if (ch == '\\' && i < source.size()) { emit(source[i++]); continue; }
                if (ch == quote) break;
            }
            continue;
        }
        std::size_t eq = 0, content = 0;
        if (long_bracket_open(source, i, eq, content)) {
            const std::size_t end = find_long_close(source, content, eq);
            while (i < end) emit(source[i++]);
            continue;
        }
        emit(source[i++]);
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    return out;
}

std::string obfuscate_lua_source(std::string_view source) {
    std::string minified = minify_lua_source(source);
    // Deliberately conservative: rename only a single local bound to a numeric
    // literal and returned in a simple expression. Complex scopes, closures,
    // globals, strings, and Roblox-heavy scripts are left minified rather than
    // risk changing semantics without a full Luau AST/scope resolver.
    const auto tokens = tokenize(minified);
    if (tokens.size() < 7) return minified;
    auto text_at = [&](std::size_t i) -> std::string_view {
        return std::string_view(minified).substr(tokens[i].begin, tokens[i].end - tokens[i].begin);
    };
    if (text_at(0) != "local" || !tokens[1].identifier || text_at(2) != "=" ||
        text_at(4) != "return") return minified;
    const std::string_view name = text_at(1);
    const std::string_view value = text_at(3);
    if (!is_identifier(name) || name == "self") return minified;
    // Numeric literal subset only; this avoids strings, calls, tables, and side effects.
    std::size_t n = 0;
    if (value.empty()) return minified;
    if (value[n] == '+' || value[n] == '-') ++n;
    bool digit = false;
    for (; n < value.size(); ++n) {
        unsigned char c = static_cast<unsigned char>(value[n]);
        if (std::isdigit(c) || c == '.' || c == '_' || c == 'x' || c == 'X' ||
            (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) { if (std::isdigit(c)) digit = true; continue; }
        return minified;
    }
    if (!digit) return minified;
    if (tokens[4].begin <= tokens[3].end) return minified;
    // Refuse shadowing/repeated declarations and only rename references that are
    // identifier tokens, never occurrences inside strings or property names.
    std::size_t declarations = 0;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
        if (text_at(i) == "local" && text_at(i + 1) == name) ++declarations;
    if (declarations != 1) return minified;
    std::string result;
    result.reserve(minified.size() + 8);
    std::size_t cursor = 0;
    for (const auto& t : tokens) {
        if (t.identifier && std::string_view(minified).substr(t.begin, t.end - t.begin) == name) {
            // Do not rename a table/member key following '.' or ':'; this case
            // is outside the supported tiny subset, so abandon instead.
            const std::size_t prev = t.begin == 0 ? 0 : t.begin - 1;
            if (t.begin > 0 && (minified[prev] == '.' || minified[prev] == ':')) return minified;
        }
    }
    for (const auto& t : tokens) {
        result.append(minified, cursor, t.begin - cursor);
        if (t.identifier && std::string_view(minified).substr(t.begin, t.end - t.begin) == name)
            result += "_v0";
        else result.append(minified, t.begin, t.end - t.begin);
        cursor = t.end;
    }
    result.append(minified, cursor, std::string::npos);
    return result;
}

} // namespace luau_vm
