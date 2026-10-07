cmake_minimum_required(VERSION 3.20)
project(vb_bitstring_contract C)
enable_testing()
get_filename_component(VB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(bitstring_floor OBJECT "${VB_ROOT}/src/bitstring_lle.c")
target_include_directories(bitstring_floor PRIVATE "${VB_ROOT}/include")
target_compile_definitions(bitstring_floor PRIVATE
    vb_bitstring_execute=vb_bitstring_lle_reference
    vb_bitstring_implementation=vb_bitstring_lle_identity)
add_executable(bitstring_test "${VB_ROOT}/tests/bitstring_test.c"
    "${VB_ROOT}/src/bitstring_hle.c" $<TARGET_OBJECTS:bitstring_floor>)
target_include_directories(bitstring_test PRIVATE "${VB_ROOT}/include")
if(MSVC)
    target_compile_options(bitstring_test PRIVATE /UNDEBUG)
else()
    target_compile_options(bitstring_test PRIVATE -UNDEBUG)
endif()
add_test(NAME bitstring_completed_contract COMMAND bitstring_test)
