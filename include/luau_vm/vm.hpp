#pragma once

#include <memory>
#include <string_view>

#include "luau_vm/result.hpp"

namespace luau_vm {

class VM {
public:
    VM();
    ~VM();

    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    VM(VM&&) noexcept;
    VM& operator=(VM&&) noexcept;

    [[nodiscard]]
    Result<bool> execute_source(std::string_view source);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace luau_vm
