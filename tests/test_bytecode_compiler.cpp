#include "luau_vm/bytecode_compiler.hpp"

#include <cassert>
#include <cmath>
#include <string>
#include <variant>

using namespace luau_vm;

int main() {
    std::vector<Value> constants;

    auto arithmetic = compile_bytecode_expression("-2 ^ 2 + 10 / 2", constants);
    assert(arithmetic);
    auto arithmetic_result = execute_bytecode(arithmetic.value(), constants);
    assert(arithmetic_result);
    assert(std::abs(std::get<double>(arithmetic_result.value()) - 1.0) < 1e-12);

    auto concatenation = compile_bytecode_expression("'lu' .. 'au'", constants);
    assert(concatenation);
    auto string_result = execute_bytecode(concatenation.value(), constants);
    assert(string_result);
    assert(std::get<std::string>(string_result.value()) == "luau");

    auto boolean = compile_bytecode_expression("not false", constants);
    assert(boolean);
    auto boolean_result = execute_bytecode(boolean.value(), constants);
    assert(boolean_result && std::get<bool>(boolean_result.value()));

    auto invalid = compile_bytecode_expression("missing + 1", constants);
    assert(!invalid);

    auto malformed = compile_bytecode_expression("(1 + 2", constants);
    assert(!malformed);
}
