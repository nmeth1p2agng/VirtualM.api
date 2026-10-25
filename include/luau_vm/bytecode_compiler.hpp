#pragma once

#include <string_view>

#include "luau_vm/bytecode.hpp"

namespace luau_vm {

// Compiles a single scalar expression into bytecode ending in Return.
// Supported literals: numbers, quoted strings, true, false, nil; supported
// operators: + - * / // % ^ .. == < <=, unary - and not, and parentheses.
// Variables, calls, tables, and statements are intentionally not supported yet.
[[nodiscard]] Result<BytecodeChunk> compile_bytecode_expression(
    std::string_view source,
    std::vector<Value>& constants);

} // namespace luau_vm
