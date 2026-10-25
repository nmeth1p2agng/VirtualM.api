#include "luau_vm/vm.hpp"
#include "luau_vm/bytecode.hpp"

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
    assert(strings);
    assert(std::get<std::string>(strings.value()) == "luau");

    const auto numeric_concat = vm.execute_source("return 12 .. 'px'");
    assert(numeric_concat);
    assert(std::get<std::string>(numeric_concat.value()) == "12px");

    const auto concat_associativity = vm.execute_source("return 'a' .. 'b' .. 'c'");
    assert(concat_associativity);
    assert(std::get<std::string>(concat_associativity.value()) == "abc");

    const auto comparison = vm.execute_source("return (4 >= 4) == true");
    assert(comparison);
    assert(std::get<bool>(comparison.value()));

    const auto nil_result = vm.execute_source("return nil");
    assert(nil_result);
    assert(std::holds_alternative<Nil>(nil_result.value()));

    const auto exponent = vm.execute_source("return 1.5e2");
    assert(exponent);
    assert(std::get<double>(exponent.value()) == 150.0);

    const auto precedence = vm.execute_source("return -2 ^ 2");
    assert(precedence);
    assert(std::get<double>(precedence.value()) == -4.0);

    const auto negative_exponent = vm.execute_source("return 2 ^ -2");
    assert(negative_exponent);
    assert(std::abs(std::get<double>(negative_exponent.value()) - 0.25) < 1e-12);

    const auto unknown = vm.execute_source("return missing + 1");
    assert(!unknown);
    assert(unknown.error().find("Unknown variable") != std::string::npos);

    const auto malformed = vm.execute_source("return (1 + 2");
    assert(!malformed);

    const auto invalid_exponent = vm.execute_source("return 1e+");
    assert(!invalid_exponent);

    const auto table_field = vm.execute_source("local t = {x = 7}; return t.x");
    assert(table_field);
    assert(std::get<double>(table_field.value()) == 7.0);

    const auto table_index = vm.execute_source(R"(local t = {["name"] = 'Luau'}; return t["name"])");
    assert(table_index);
    assert(std::get<std::string>(table_index.value()) == "Luau");

    const auto table_array = vm.execute_source("return {10, 20, 30}[2]");
    assert(table_array);
    assert(std::get<double>(table_array.value()) == 20.0);

    const auto nested_table = vm.execute_source("return {nested = {value = 42}}.nested.value");
    assert(nested_table);
    assert(std::get<double>(nested_table.value()) == 42.0);

    const auto table_write = vm.execute_source("local t = {x = 1}; t.x = 9; t['y'] = 5; return t.x + t.y");
    assert(table_write);
    assert(std::get<double>(table_write.value()) == 14.0);

    const auto missing_table_key = vm.execute_source("return {x = 1}.missing");
    assert(missing_table_key);
    assert(std::holds_alternative<Nil>(missing_table_key.value()));

    const auto table_length = vm.execute_source("local items = {10, 20, 30}; return #items");
    assert(table_length);
    assert(std::get<double>(table_length.value()) == 3.0);

    const auto string_length = vm.execute_source("return #'VirtualM'");
    assert(string_length);
    assert(std::get<double>(string_length.value()) == 8.0);

    const auto sparse_table_length = vm.execute_source("local items = {[1] = 'a', [2] = 'b', [4] = 'd'}; return #items");
    assert(sparse_table_length);
    assert(std::get<double>(sparse_table_length.value()) == 2.0);

    const auto invalid_length = vm.execute_source("return #42");
    assert(!invalid_length);
    assert(invalid_length.error().find("Length operator") != std::string::npos);

    const auto invalid_table_index = vm.execute_source("return 5.x");
    assert(!invalid_table_index);

    const auto if_true = vm.execute_source("local x = 0; if 2 > 1 then x = 10 end; return x");
    assert(if_true);
    assert(std::get<double>(if_true.value()) == 10.0);

    const auto if_else = vm.execute_source("local x = 0; if false then x = 1 else x = 2 end; return x");
    assert(if_else);
    assert(std::get<double>(if_else.value()) == 2.0);

    const auto if_elseif = vm.execute_source("local x = 0; if false then x = 1 elseif 3 > 2 then x = 7 else x = 9 end; return x");
    assert(if_elseif);
    assert(std::get<double>(if_elseif.value()) == 7.0);

    const auto while_loop = vm.execute_source("local i = 0; local total = 0; while i < 5 do i = i + 1; total = total + i end; return total");
    assert(while_loop);
    assert(std::get<double>(while_loop.value()) == 15.0);

    const auto nested_control = vm.execute_source("local x = 0; while x < 3 do if x < 2 then x = x + 1 else x = x + 1 end end; return x");
    assert(nested_control);
    assert(std::get<double>(nested_control.value()) == 3.0);

    const auto function_call = vm.execute_source("local function add(a, b) return a + b end; return add(7, 8)");
    assert(function_call);
    assert(std::get<double>(function_call.value()) == 15.0);

    const auto function_without_return = vm.execute_source("local function noop() local x = 1 end; return noop()");
    assert(function_without_return);
    assert(std::holds_alternative<Nil>(function_without_return.value()));

    const auto function_arity = vm.execute_source("local function one(a) return a end; return one()");
    assert(!function_arity);
    assert(function_arity.error().find("arguments") != std::string::npos);

    const auto block_local_does_not_escape = vm.execute_source(
        "if true then local hidden = 12 end; return hidden");
    assert(!block_local_does_not_escape);
    assert(block_local_does_not_escape.error().find("Unknown variable") != std::string::npos);

    const auto block_scope_shadowing = vm.execute_source(
        "local value = 3; if true then local value = 9 end; return value");
    assert(block_scope_shadowing);
    assert(std::get<double>(block_scope_shadowing.value()) == 3.0);

    const auto outer_assignment_survives_block = vm.execute_source(
        "local value = 3; if true then value = 11 end; return value");
    assert(outer_assignment_survives_block);
    assert(std::get<double>(outer_assignment_survives_block.value()) == 11.0);

    const auto function_local_isolated = vm.execute_source(
        "local function calc() local secret = 44; return secret end; calc(); return secret");
    assert(!function_local_isolated);
    assert(function_local_isolated.error().find("Unknown variable") != std::string::npos);

    const auto while_local_does_not_escape = vm.execute_source(
        "local i = 0; while i < 1 do local transient = 5; i = i + 1 end; return transient");
    assert(!while_local_does_not_escape);
    assert(while_local_does_not_escape.error().find("Unknown variable") != std::string::npos);


    const auto floor_division = vm.execute_source("return 7 // 2");
    assert(floor_division);
    assert(std::get<double>(floor_division.value()) == 3.0);

    const auto floor_division_negative = vm.execute_source("return -7 // 2");
    assert(floor_division_negative);
    assert(std::get<double>(floor_division_negative.value()) == -4.0);

    const auto logical_and = vm.execute_source("return true and 12");
    assert(logical_and);
    assert(std::get<double>(logical_and.value()) == 12.0);

    const auto logical_or = vm.execute_source("return false or 'fallback'");
    assert(logical_or);
    assert(std::get<std::string>(logical_or.value()) == "fallback");

    const auto logical_precedence = vm.execute_source("return false or true and 9");
    assert(logical_precedence);
    assert(std::get<double>(logical_precedence.value()) == 9.0);

    const auto logical_truthiness = vm.execute_source("return 0 or 5");
    assert(logical_truthiness);
    assert(std::get<double>(logical_truthiness.value()) == 0.0);

    // Bytecode verifier: valid linear program and invalid operands/control flow.
    using luau_vm::BytecodeChunk;
    using luau_vm::Instruction;
    using luau_vm::OpCode;
    BytecodeChunk valid_chunk;
    valid_chunk.constant_count = 1;
    valid_chunk.instructions = {{OpCode::LoadConstant, 0}, {OpCode::Return, 0}};
    const auto verified = luau_vm::verify_bytecode(valid_chunk);
    assert(verified.valid);
    assert(verified.max_stack == 1);

    BytecodeChunk bad_constant;
    bad_constant.constant_count = 1;
    bad_constant.instructions = {{OpCode::LoadConstant, 2}, {OpCode::Return, 0}};
    assert(!luau_vm::verify_bytecode(bad_constant).valid);

    BytecodeChunk underflow;
    underflow.instructions = {{OpCode::Add, 0}, {OpCode::Return, 0}};
    assert(!luau_vm::verify_bytecode(underflow).valid);

    BytecodeChunk bad_jump;
    bad_jump.instructions = {{OpCode::Jump, 8}, {OpCode::Return, 0}};
    assert(!luau_vm::verify_bytecode(bad_jump).valid);

    BytecodeChunk branch;
    branch.instructions = {{OpCode::LoadBoolean, 1}, {OpCode::JumpIfFalse, 4},
                           {OpCode::LoadNil, 0}, {OpCode::Jump, 5},
                           {OpCode::LoadNil, 0}, {OpCode::Return, 0}};
    assert(luau_vm::verify_bytecode(branch).valid);

    // Runtime protection limits are configurable and fail with explicit errors.
    luau_vm::VMOptions step_limits;
    step_limits.max_execution_steps = 12;
    luau_vm::VM bounded_vm(step_limits);
    const auto too_many_steps = bounded_vm.execute_source("local i = 0; while true do i = i + 1 end");
    assert(!too_many_steps);
    assert(too_many_steps.error().find("Execution step limit") != std::string::npos ||
           too_many_steps.error().find("Loop iteration limit") != std::string::npos);

    // Expression-heavy code must consume the same execution budget even without loops.
    luau_vm::VMOptions expression_limits;
    expression_limits.max_execution_steps = 2;
    luau_vm::VM expression_bounded_vm(expression_limits);
    const auto too_many_expressions = expression_bounded_vm.execute_source("return 1 + 2 + 3 + 4");
    assert(!too_many_expressions);
    assert(too_many_expressions.error().find("Execution step limit exceeded") != std::string::npos);

    luau_vm::VMOptions table_limits;
    table_limits.max_tables = 1;
    luau_vm::VM one_table_vm(table_limits);
    const auto too_many_tables = one_table_vm.execute_source("local a = {}; local b = {}; return 1");
    assert(!too_many_tables);
    assert(too_many_tables.error().find("Table limit") != std::string::npos);

    luau_vm::VMOptions entry_limits;
    entry_limits.max_table_entries = 1;
    luau_vm::VM one_entry_vm(entry_limits);
    const auto too_many_entries = one_entry_vm.execute_source("local t = {a = 1, b = 2}; return 1");
    assert(!too_many_entries);
    assert(too_many_entries.error().find("Table entry limit") != std::string::npos);


    // Parser complexity is bounded independently of source byte length.
    luau_vm::VMOptions token_limits;
    token_limits.max_tokens = 5;
    luau_vm::VM token_bounded_vm(token_limits);
    const auto too_many_tokens = token_bounded_vm.execute_source("local a = 1; return a + 2");
    assert(!too_many_tokens);
    assert(too_many_tokens.error().find("Token limit exceeded") != std::string::npos);

    luau_vm::VMOptions enough_tokens;
    enough_tokens.max_tokens = 20;
    luau_vm::VM token_ok_vm(enough_tokens);
    const auto within_token_budget = token_ok_vm.execute_source("return 2 + 3");
    assert(within_token_budget);
    assert(std::get<double>(within_token_budget.value()) == 5.0);

    // Failed snippets roll back VM-managed objects so they cannot poison later runs.
    luau_vm::VMOptions rollback_limits;
    rollback_limits.max_tables = 1;
    VM rollback_vm(rollback_limits);
    const auto failed_after_allocation = rollback_vm.execute_source("local t = {}; return missing + 1");
    assert(!failed_after_allocation);
    const auto succeeds_after_rollback = rollback_vm.execute_source("local t = {}; return 42");
    assert(succeeds_after_rollback);
    assert(std::get<double>(succeeds_after_rollback.value()) == 42.0);

    return 0;
}
