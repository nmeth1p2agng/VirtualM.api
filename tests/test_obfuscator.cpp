#include "luau_vm/obfuscator.hpp"

#include <cassert>
#include <string>

int main() {
    using luau_vm::minify_lua_source;
    using luau_vm::obfuscate_lua_source;

    assert(minify_lua_source("local x = 1 -- remove me\nreturn x") ==
           "local x=1 return x");
    assert(minify_lua_source("local s = 'a  -- b'; return s") ==
           "local s='a  -- b';return s");
    assert(minify_lua_source("return a - -b") == "return a- -b");
    assert(minify_lua_source("local s = [=[a -- b\n c]=]\nreturn s") ==
           "local s=[=[a -- b\n c]=]return s");
    assert(minify_lua_source("return foo --[=[comment]=]\n + 1") ==
           "return foo+1");
    // Whitespace must not be removed when it would merge separate operators.
    assert(minify_lua_source("local x = a + = b") == "local x=a+ =b");
    assert(minify_lua_source("local x = a .. . b") == "local x=a.. .b");
    assert(minify_lua_source("local x = a / / b") == "local x=a/ /b");
    assert(minify_lua_source("local x = 1 . . 2") == "local x=1 . . 2");

    const auto renamed = obfuscate_lua_source("local angka = 10\nreturn angka + 20");
    assert(renamed.find("local _v0=10") != std::string::npos);
    assert(renamed.find("return _v0+20") != std::string::npos);
    assert(obfuscate_lua_source("local x=1; local x=2; return x").find("local x=1") != std::string::npos);
    assert(obfuscate_lua_source("local x=1; if x > 0 then return x end").find("local x=1") != std::string::npos);
    assert(obfuscate_lua_source("local s='x'; return s").find("'x'") != std::string::npos);
    return 0;
}
