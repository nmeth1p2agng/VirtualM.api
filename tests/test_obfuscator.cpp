#include "luau_vm/obfuscator.hpp"

#include <cassert>
#include <string>

int main() {
    using luau_vm::minify_lua_source;

    assert(minify_lua_source("local x = 1 -- remove me\nreturn x") ==
           "local x=1 return x");
    assert(minify_lua_source("local s = 'a  -- b'; return s") ==
           "local s='a  -- b';return s");
    assert(minify_lua_source("return a - -b") == "return a- -b");
    assert(minify_lua_source("local s = [=[a -- b\n c]=]\nreturn s") ==
           "local s=[=[a -- b\n c]=]return s");
    assert(minify_lua_source("return foo --[=[comment]=]\n + 1") ==
           "return foo+1");
    return 0;
}
