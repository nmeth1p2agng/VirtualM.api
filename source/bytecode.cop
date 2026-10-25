#include "luau_vm/bytecode.hpp"

#include <deque>
#include <limits>
#include <optional>
#include <sstream>

namespace luau_vm {
namespace {

struct Effect { int pops; int pushes; };

Effect stack_effect(OpCode op) {
    switch (op) {
        case OpCode::LoadConstant:
        case OpCode::LoadNil:
        case OpCode::LoadBoolean:
        case OpCode::LoadLocal: return {0, 1};
        case OpCode::StoreLocal: return {1, 0};
        case OpCode::Negate:
        case OpCode::LogicalNot: return {1, 1};
        case OpCode::Add: case OpCode::Subtract: case OpCode::Multiply:
        case OpCode::Divide: case OpCode::FloorDivide: case OpCode::Modulo:
        case OpCode::Power: case OpCode::Equal: case OpCode::LessThan:
        case OpCode::LessEqual: case OpCode::Concatenate: case OpCode::GetTable:
            return {2, 1};
        case OpCode::SetTable: return {3, 0};
        case OpCode::Pop: return {1, 0};
        case OpCode::Jump: return {0, 0};
        case OpCode::JumpIfFalse: return {1, 0};
        case OpCode::Return: return {1, 0};
    }
    return {-1, -1};
}

std::string at_instruction(std::size_t i, const std::string& message) {
    return "Bytecode instruction " + std::to_string(i) + ": " + message;
}
} // namespace

BytecodeVerification verify_bytecode(const BytecodeChunk& chunk) {
    BytecodeVerification result;
    const auto count = chunk.instructions.size();
    if (count == 0) { result.error = "Bytecode chunk is empty"; return result; }

    for (std::size_t i = 0; i < count; ++i) {
        const auto& ins = chunk.instructions[i];
        if (ins.opcode == OpCode::LoadConstant &&
            (ins.operand < 0 || static_cast<std::size_t>(ins.operand) >= chunk.constant_count)) {
            result.error = at_instruction(i, "constant index out of range"); return result;
        }
        if ((ins.opcode == OpCode::LoadLocal || ins.opcode == OpCode::StoreLocal) &&
            (ins.operand < 0 || static_cast<std::size_t>(ins.operand) >= chunk.local_count)) {
            result.error = at_instruction(i, "local index out of range"); return result;
        }
        if (ins.opcode == OpCode::Jump || ins.opcode == OpCode::JumpIfFalse) {
            if (ins.operand < 0 || static_cast<std::size_t>(ins.operand) >= count) {
                result.error = at_instruction(i, "jump target out of range"); return result;
            }
        }
        if (stack_effect(ins.opcode).pops < 0) {
            result.error = at_instruction(i, "unknown opcode"); return result;
        }
    }

    // Data-flow analysis: a program point must have the same stack height on every path.
    std::vector<std::optional<std::size_t>> depths(count);
    std::deque<std::size_t> work;
    depths[0] = 0;
    work.push_back(0);
    bool saw_return = false;
    while (!work.empty()) {
        const auto pc = work.front(); work.pop_front();
        const auto depth = *depths[pc];
        const auto& ins = chunk.instructions[pc];
        const auto effect = stack_effect(ins.opcode);
        if (depth < static_cast<std::size_t>(effect.pops)) {
            result.error = at_instruction(pc, "stack underflow"); return result;
        }
        const auto next_depth = depth - static_cast<std::size_t>(effect.pops) + static_cast<std::size_t>(effect.pushes);
        if (next_depth > 1000000) { result.error = at_instruction(pc, "stack limit exceeded"); return result; }
        if (next_depth > result.max_stack) result.max_stack = next_depth;

        if (ins.opcode == OpCode::Return) { saw_return = true; continue; }
        auto add_successor = [&](std::size_t target) -> bool {
            if (target >= count) {
                result.error = at_instruction(pc, "execution falls off end without return");
                return false;
            }
            if (!depths[target]) { depths[target] = next_depth; work.push_back(target); }
            else if (*depths[target] != next_depth) {
                result.error = at_instruction(target, "inconsistent stack height across control-flow paths");
                return false;
            }
            return true;
        };
        if (ins.opcode == OpCode::Jump) {
            if (!add_successor(static_cast<std::size_t>(ins.operand))) return result;
        } else if (ins.opcode == OpCode::JumpIfFalse) {
            if (!add_successor(static_cast<std::size_t>(ins.operand))) return result;
            if (!add_successor(pc + 1)) return result;
        } else if (!add_successor(pc + 1)) return result;
    }
    if (!saw_return) { result.error = "Bytecode has no reachable return instruction"; return result; }
    result.valid = true;
    return result;
}

} // namespace luau_vm
