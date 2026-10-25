#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace luau_vm {

struct Nil {
    friend bool operator==(Nil, Nil) = default;
};

struct TableRef {
    std::uint32_t index{};

    friend bool operator==(
        const TableRef&,
        const TableRef&
    ) = default;
};

struct FunctionRef {
    std::uint32_t index{};

    friend bool operator==(
        const FunctionRef&,
        const FunctionRef&
    ) = default;
};

using Value = std::variant<
    Nil,
    bool,
    double,
    std::string,
    TableRef,
    FunctionRef
>;

} // namespace luau_vm
