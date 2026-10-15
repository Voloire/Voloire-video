add_executable(vv_unit_tests unit_tests.cpp)
target_include_directories(vv_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(vv_unit_tests PRIVATE vv_core)
add_test(NAME unit COMMAND vv_unit_tests)
