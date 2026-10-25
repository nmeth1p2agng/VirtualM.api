#include "luau_vm/obfuscator.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
        std::size_t j = i + 1;
        std::size_t n = 0;
        while (j < s.size() && s[j] == '=' && n < eq_count) { ++j; ++n; }
        if (n == eq_count && j < s.size() && s[j] == ']') return j + 1;
    }
    return s.size();
}

bool needs_separator(char left, char right) {
    const auto a = static_cast<unsigned char>(left);
    const auto b = static_cast<unsigned char>(right);
    if (is_word(a) && is_word(b)) return true;
    // Prevent forming a comment, concatenation, comparison, floor division,
    // or another multi-character operator across removed whitespace.
    const std::string pair{left, right};
    if (pair == "--" || pair == ".." || pair == "//" || pair == "==" ||
        pair == "~=" || pair == "<=" || pair == ">=" || pair == "::" ||
        pair == "<<" || pair == ">>") return true;
    return false;
}

struct Token {
    std::string text;
    std::size_t start{};
    std::size_t end{};
    bool identifier{};
};

bool is_ident_start(unsigned char c) {
    return std::isalpha(c) || c == '_' || c >= 0x80;
}

bool is_ident_continue(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

std::vector<Token> lex_tokens(std::string_view s) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isspace(c)) { ++i; continue; }
        if (s[i] == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            i += 2;
            std::size_t eq = 0, content = 0;
            if (long_bracket_open(s, i, eq, content)) i = find_long_close(s, content, eq);
            else while (i < s.size() && s[i] != '\n' && s[i] != '\r') ++i;
            continue;
        }
        const std::size_t start = i;
        if (s[i] == '\'' || s[i] == '"') {
            const char quote = s[i++];
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) { i += 2; continue; }
                if (s[i++] == quote) break;
            }
            tokens.push_back({std::string(s.substr(start, i - start)), start, i, false});
            continue;
        }
        std::size_t eq = 0, content = 0;
        if (long_bracket_open(s, i, eq, content)) {
            i = find_long_close(s, content, eq);
            tokens.push_back({std::string(s.substr(start, i - start)), start, i, false});
            continue;
        }
        if (is_ident_start(c)) {
            ++i;
            while (i < s.size() && is_ident_continue(static_cast<unsigned char>(s[i]))) ++i;
            tokens.push_back({std::string(s.substr(start, i - start)), start, i, true});
            continue;
        }
        if (i + 1 < s.size()) {
            const std::string_view two = s.substr(i, 2);
            if (two == "==" || two == "~=" || two == "<=" || two == ">=" ||
                two == ".." || two == "//" || two == "::" || two == "<<" || two == ">>") i += 2;
            else ++i;
        } else ++i;
        tokens.push_back({std::string(s.substr(start, i - start)), start, i, false});
    }
    return tokens;
}

bool is_keyword(const std::string& s) {
    static const std::unordered_set<std::string> keywords = {
        "and", "break", "continue", "do", "else", "elseif", "end", "false",
        "for", "function", "if", "in", "local", "nil", "not", "or", "repeat",
        "return", "then", "true", "until", "while"
    };
    return keywords.contains(s);
}

} // namespace

std::string minify_lua_source(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    std::size_t i = 0;
    bool pending_space = false;

    auto emit = [&](char c) {
        if (pending_space && !out.empty() && needs_separator(out.back(), c)) out.push_back(' ');
        pending_space = false;
        out.push_back(c);
    };

    while (i < source.size()) {
        const unsigned char c = static_cast<unsigned char>(source[i]);

        if (std::isspace(c)) {
            pending_space = true;
            ++i;
            continue;
        }

        // Line or long-bracket comment.
        if (source[i] == '-' && i + 1 < source.size() && source[i + 1] == '-') {
            i += 2;
            std::size_t eq = 0, content = 0;
            if (long_bracket_open(source, i, eq, content)) {
                i = find_long_close(source, content, eq);
            } else {
                while (i < source.size() && source[i] != '\n' && source[i] != '\r') ++i;
            }
            pending_space = true;
            continue;
        }

        // Quoted string; preserve every byte, including escaped quotes.
        if (source[i] == '\'' || source[i] == '"') {
            const char quote = source[i];
            emit(source[i++]);
            while (i < source.size()) {
                const char ch = source[i++];
                emit(ch);
                if (ch == '\\' && i < source.size()) {
                    emit(source[i++]);
                    continue;
                }
                if (ch == quote) break;
            }
            continue;
        }

        // Lua long-bracket string.
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
    const std::string minified = minify_lua_source(source);
    const auto tokens = lex_tokens(minified);

    // Deliberately conservative: this first renaming pass supports straight-line
    // code only. Complex scopes and table/member syntax fall back to minification.
    for (const auto& token : tokens) {
        if (token.text == "{" || token.text == "}" || token.text == "." ||
            token.text == ":" || token.text == "[" || token.text == "]") return minified;
        if (!token.identifier) continue;
        if (token.text == "function" || token.text == "if" || token.text == "then" ||
            token.text == "elseif" || token.text == "else" || token.text == "end" ||
            token.text == "for" || token.text == "while" || token.text == "repeat" ||
            token.text == "until" || token.text == "do" || token.text == "continue" ||
            token.text == "break") return minified;
    }

    std::unordered_map<std::string, std::size_t> declarations;
    for (std::size_t i = 0; i + 2 < tokens.size(); ++i) {
        if (tokens[i].text == "local" && tokens[i + 1].identifier &&
            !is_keyword(tokens[i + 1].text) && tokens[i + 2].text == "=") {
            ++declarations[tokens[i + 1].text];
        }
    }
    if (declarations.empty()) return minified;
    for (const auto& [name, count] : declarations) {
        (void)name;
        if (count != 1) return minified;
    }

    // Avoid ambiguous declarations (multiple locals in one declaration) and
    // only rename identifiers that occur as ordinary name tokens.
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].text == "local" && tokens[i + 1].identifier) {
            std::size_t j = i + 2;
            while (j < tokens.size() && tokens[j].text != "=" && tokens[j].text != ";" &&
                   tokens[j].text != "return") {
                if (tokens[j].text == ",") return minified;
                ++j;
            }
        }
    }

    std::unordered_map<std::string, std::size_t> declaration_positions;
    for (std::size_t i = 0; i + 2 < tokens.size(); ++i) {
        if (tokens[i].text == "local" && tokens[i + 1].identifier &&
            declarations.contains(tokens[i + 1].text) && tokens[i + 2].text == "=") {
            declaration_positions[tokens[i + 1].text] = tokens[i + 1].start;
        }
    }
    for (const auto& token : tokens) {
        if (token.identifier && declaration_positions.contains(token.text) &&
            token.start < declaration_positions.at(token.text)) return minified;
    }

    std::unordered_set<std::string> used;
    for (const auto& token : tokens) if (token.identifier) used.insert(token.text);
    std::unordered_map<std::string, std::string> renames;
    std::size_t serial = 0;
    for (const auto& token : tokens) {
        if (!token.identifier || !declarations.contains(token.text) || renames.contains(token.text)) continue;
        std::string candidate;
        do { candidate = "_v" + std::to_string(serial++); } while (used.contains(candidate));
        used.insert(candidate);
        renames.emplace(token.text, std::move(candidate));
    }

    std::string out;
    out.reserve(minified.size());
    std::size_t cursor = 0;
    for (const auto& token : tokens) {
        out.append(minified.substr(cursor, token.start - cursor));
        if (token.identifier && renames.contains(token.text)) out += renames.at(token.text);
        else out += token.text;
        cursor = token.end;
    }
    out.append(minified.substr(cursor));
    return out;
}

} // namespace luau_vm
