#include "luau_vm/vm.hpp"

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


    return 0;
}
