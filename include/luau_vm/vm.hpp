#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "luau_vm/result.hpp"
#include "luau_vm/value.hpp"

namespace luau_vm {

// Resource limits for untrusted source. These limit interpreter work and
// managed VM objects; they are not an operating-system security sandbox.
struct VMOptions {
    std::size_t max_execution_steps{1'000'000};
    std::size_t max_tables{10'000};
    std::size_t max_table_entries{100'000};
    std::size_t max_functions{10'000};
    std::size_t max_call_depth{128};
};

class VM {
public:
    explicit VM(VMOptions options = {});
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
