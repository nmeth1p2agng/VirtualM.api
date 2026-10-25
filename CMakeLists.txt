cmake_minimum_required(VERSION 3.20)
project(VirtualM_api LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(luau_vm STATIC source/vm.cpp source/bytecode.cpp source/bytecode_compiler.cpp)
target_include_directories(luau_vm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(luau_vm PRIVATE -Wall -Wextra -Wpedantic)

add_executable(virtualm_api source/server.cpp)
target_link_libraries(virtualm_api PRIVATE luau_vm)

include(CTest)
if(BUILD_TESTING)
  add_executable(test_vm tests/test_vm.cpp)
  target_link_libraries(test_vm PRIVATE luau_vm)
  add_test(NAME vm_tests COMMAND test_vm)
  add_executable(test_bytecode_compiler tests/test_bytecode_compiler.cpp)
  target_link_libraries(test_bytecode_compiler PRIVATE luau_vm)
  add_test(NAME bytecode_compiler_tests COMMAND test_bytecode_compiler)
endif()
