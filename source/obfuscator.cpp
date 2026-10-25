#include "luau_vm/obfuscator.hpp"

#include <cctype>
#include <string>

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

} // namespace luau_vm
