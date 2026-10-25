#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "luau_vm/result.hpp"
#include "luau_vm/value.hpp"

namespace luau_vm {

enum class OpCode : std::uint8_t {
    LoadConstant,
    LoadNil,
    LoadBoolean,
    LoadLocal,
    StoreLocal,
    Add,
    Subtract,
    Multiply,
    Divide,
    FloorDivide,
    Modulo,
    Power,
    Negate,
    LogicalNot,
    Equal,
    LessThan,
    LessEqual,
    Concatenate,
    GetTable,
    SetTable,
    Pop,
    Jump,
    JumpIfFalse,
    Return
};

struct Instruction {
    OpCode opcode{OpCode::LoadNil};
    // Constant/local index or absolute instruction index for jumps.
    std::int32_t operand{0};
};

struct BytecodeChunk {
    std::vector<Instruction> instructions;
    std::size_t constant_count{0};
    // LoadBoolean reads operand 0 as false and 1 as true.
    std::size_t local_count{0};
};

struct BytecodeVerification {
    bool valid{false};
    std::string error;
    std::size_t max_stack{0};
};

// Validates operand indexes, jump targets, and stack balance on every reachable path.
[[nodiscard]] BytecodeVerification verify_bytecode(const BytecodeChunk& chunk);

// Executes verified scalar bytecode. Tables are intentionally rejected until the
// bytecode engine is connected to the VM's managed table store.
[[nodiscard]] Result<Value> execute_bytecode(
    const BytecodeChunk& chunk,
    const std::vector<Value>& constants,
    std::size_t max_steps = 1'000'000);

} // namespace luau_vm
