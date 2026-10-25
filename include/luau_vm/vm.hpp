#pragma once

#include <memory>
#include <string_view>

#include "luau_vm/result.hpp"
#include "luau_vm/value.hpp"

namespace luau_vm {

class VM {
public:
    VM();
    ~VM();

    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    VM(VM&&) noexcept;
    VM& operator=(VM&&) noexcept;

    // Executes the currently supported Luau subset and returns the first
    // value from a return statement (nil when no value is returned).
    [[nodiscard]] Result<Value> execute_source(std::string_view source);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace luau_vm
