#pragma once

#include <string>
#include <string_view>

namespace luau_vm {

// Removes comments and unnecessary whitespace without rewriting identifiers.
std::string minify_lua_source(std::string_view source);

// Renames simple top-level local variables when the source is within the
// conservative supported subset; otherwise returns the minified source.
std::string obfuscate_lua_source(std::string_view source);

} // namespace luau_vm
