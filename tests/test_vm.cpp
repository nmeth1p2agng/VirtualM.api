#include "luau_vm/vm.hpp"

#include <cassert>
#include <cmath>
#include <string>

using luau_vm::Nil;
using luau_vm::VM;

int main() {
    VM vm;

    const auto empty = vm.execute_source("");
    assert(!empty);
    assert(empty.error() == "Source code is empty");

    const auto arithmetic = vm.execute_source("return 1 + 2 * 3");
    assert(arithmetic);
    assert(std::get<double>(arithmetic.value()) == 7.0);

    const auto locals = vm.execute_source("local x = 10; x = x + 5; return x");
    assert(locals);
    assert(std::get<double>(locals.value()) == 15.0);

    const auto strings = vm.execute_source("return 'lu' .. 'au'");
    // Concatenation is not in this milestone; unsupported syntax must fail safely.
    assert(!strings);

    const auto comparison = vm.execute_source("return (4 >= 4) == true");
    assert(comparison);
    assert(std::get<bool>(comparison.value()));

    const auto nil_result = vm.execute_source("return nil");
    assert(nil_result);
    assert(std::holds_alternative<Nil>(nil_result.value()));

    const auto unknown = vm.execute_source("return missing + 1");
    assert(!unknown);
    assert(unknown.error().find("Unknown variable") != std::string::npos);

    const auto malformed = vm.execute_source("return (1 + 2");
    assert(!malformed);

    return 0;
}
