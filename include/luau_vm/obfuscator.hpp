#pragma once

#include <string>
#include <string_view>

namespace luau_vm {

// Removes comments and unnecessary whitespace without rewriting identifiers or literals.
// This is a source minifier, not a semantic obfuscator.
std::string minify_lua_source(std::string_view source);

} // namespace luau_vm
