#include "luau_vm/vm.hpp"

#include <memory>

namespace luau_vm {

struct VM::Impl {
    // Runtime state akan ditambahkan di sini.
};

VM::VM()
    : impl_(std::make_unique<Impl>()) {}

VM::~VM() = default;

VM::VM(VM&&) noexcept = default;

VM& VM::operator=(VM&&) noexcept = default;

Result<bool> VM::execute_source(std::string_view source) {
    if (!impl_) {
        return Result<bool>::failure(
            "VM is not initialized"
        );
    }

    if (source.empty()) {
        return Result<bool>::failure(
            "Source code is empty"
        );
    }

    return Result<bool>::failure(
        "Source execution is not implemented yet"
    );
}

} // namespace luau_vm
