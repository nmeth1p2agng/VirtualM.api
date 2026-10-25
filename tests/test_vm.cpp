#include "luau_vm/vm.hpp"

#include <cassert>

int main() {
    luau_vm::VM vm;

    const auto empty = vm.execute_source("");

    assert(!empty);
    assert(empty.error() == "Source code is empty");

    const auto unsupported =
        vm.execute_source("return 1 + 1");

    assert(!unsupported);
    assert(
        unsupported.error() ==
        "Source execution is not implemented yet"
    );

    return 0;
}
