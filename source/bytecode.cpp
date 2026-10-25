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

#include <cmath>
#include <limits>
#include <variant>
#include "luau_vm/value.hpp"

namespace luau_vm {
namespace {
bool truthy(const Value& v) {
    if (std::holds_alternative<Nil>(v)) return false;
    if (const auto* b = std::get_if<bool>(&v)) return *b;
    return true;
}
Result<Value> runtime_error(std::size_t pc, const std::string& message) {
    return Result<Value>::failure("Bytecode instruction " + std::to_string(pc) + ": " + message);
}
}

Result<Value> execute_bytecode(const BytecodeChunk& chunk,
                               const std::vector<Value>& constants,
                               std::size_t max_steps) {
    const auto verification = verify_bytecode(chunk);
    if (!verification.valid) return Result<Value>::failure("Invalid bytecode: " + verification.error);
    if (constants.size() != chunk.constant_count)
        return Result<Value>::failure("Bytecode constant count does not match chunk metadata");

    std::vector<Value> stack;
    stack.reserve(verification.max_stack);
    std::vector<Value> locals(chunk.local_count, Nil{});
    std::size_t pc = 0;
    std::size_t steps = 0;
    auto pop = [&]() -> Value { Value v = std::move(stack.back()); stack.pop_back(); return v; };
    auto number = [&](const Value& v, std::size_t at) -> Result<double> {
        if (const auto* n = std::get_if<double>(&v)) return Result<double>::success(*n);
        return Result<double>::failure("Bytecode instruction " + std::to_string(at) + ": expected number");
    };
    while (pc < chunk.instructions.size()) {
        if (++steps > max_steps) return Result<Value>::failure("Bytecode execution step limit exceeded");
        const auto at = pc;
        const auto ins = chunk.instructions[pc++];
        switch (ins.opcode) {
            case OpCode::LoadConstant: stack.push_back(constants[static_cast<std::size_t>(ins.operand)]); break;
            case OpCode::LoadNil: stack.emplace_back(Nil{}); break;
            case OpCode::LoadBoolean: stack.emplace_back(ins.operand != 0); break;
            case OpCode::LoadLocal: stack.push_back(locals[static_cast<std::size_t>(ins.operand)]); break;
            case OpCode::StoreLocal: locals[static_cast<std::size_t>(ins.operand)] = pop(); break;
            case OpCode::Pop: (void)pop(); break;
            case OpCode::Jump: pc = static_cast<std::size_t>(ins.operand); break;
            case OpCode::JumpIfFalse: if (!truthy(pop())) pc = static_cast<std::size_t>(ins.operand); break;
            case OpCode::Return: return Result<Value>::success(pop());
            case OpCode::Negate: {
                auto v = number(stack.back(), at); if (!v) return Result<Value>::failure(v.error());
                stack.back() = -v.value(); break;
            }
            case OpCode::LogicalNot: { auto v = truthy(stack.back()); stack.back() = !v; break; }
            case OpCode::Add: case OpCode::Subtract: case OpCode::Multiply:
            case OpCode::Divide: case OpCode::FloorDivide: case OpCode::Modulo:
            case OpCode::Power: case OpCode::LessThan: case OpCode::LessEqual: {
                Value rhs = pop(), lhs = pop();
                auto a = number(lhs, at); if (!a) return Result<Value>::failure(a.error());
                auto b = number(rhs, at); if (!b) return Result<Value>::failure(b.error());
                const double x = a.value(), y = b.value();
                if ((ins.opcode == OpCode::Divide || ins.opcode == OpCode::FloorDivide || ins.opcode == OpCode::Modulo) && y == 0.0)
                    return runtime_error(at, "division by zero");
                switch (ins.opcode) {
                    case OpCode::Add: stack.emplace_back(x + y); break;
                    case OpCode::Subtract: stack.emplace_back(x - y); break;
                    case OpCode::Multiply: stack.emplace_back(x * y); break;
                    case OpCode::Divide: stack.emplace_back(x / y); break;
                    case OpCode::FloorDivide: stack.emplace_back(std::floor(x / y)); break;
                    case OpCode::Modulo: stack.emplace_back(x - std::floor(x / y) * y); break;
                    case OpCode::Power: stack.emplace_back(std::pow(x, y)); break;
                    case OpCode::LessThan: stack.emplace_back(x < y); break;
                    case OpCode::LessEqual: stack.emplace_back(x <= y); break;
                    default: break;
                }
                break;
            }
            case OpCode::Equal: {
                Value rhs = pop(), lhs = pop();
                bool equal = false;
                if (lhs.index() == rhs.index()) {
                    if (std::holds_alternative<Nil>(lhs)) equal = true;
                    else if (auto* a = std::get_if<bool>(&lhs)) equal = *a == std::get<bool>(rhs);
                    else if (auto* a = std::get_if<double>(&lhs)) equal = *a == std::get<double>(rhs);
                    else if (auto* a = std::get_if<std::string>(&lhs)) equal = *a == std::get<std::string>(rhs);
                    else if (auto* a = std::get_if<TableRef>(&lhs)) equal = *a == std::get<TableRef>(rhs);
                    else if (auto* a = std::get_if<FunctionRef>(&lhs)) equal = *a == std::get<FunctionRef>(rhs);
                }
                stack.emplace_back(equal); break;
            }
            case OpCode::Concatenate: {
                Value rhs = pop(), lhs = pop();
                auto stringify = [](const Value& v) -> Result<std::string> {
                    if (auto* s = std::get_if<std::string>(&v)) return Result<std::string>::success(*s);
                    if (auto* n = std::get_if<double>(&v)) return Result<std::string>::success(std::to_string(*n));
                    return Result<std::string>::failure("concatenation expects strings or numbers");
                };
                auto a = stringify(lhs); if (!a) return runtime_error(at, a.error());
                auto b = stringify(rhs); if (!b) return runtime_error(at, b.error());
                stack.emplace_back(a.value() + b.value()); break;
            }
            case OpCode::GetTable: case OpCode::SetTable:
                return runtime_error(at, "table opcodes are not connected to the managed table engine yet");
        }
    }
    return Result<Value>::success(Nil{});
}
} // namespace luau_vm
