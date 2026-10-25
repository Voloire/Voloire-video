add_executable(vv_unit_tests unit_tests.cpp)
target_include_directories(vv_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(vv_unit_tests PRIVATE vv_core)
add_test(NAME unit COMMAND vv_unit_tests)

# No capture, encoding, device access or UI: exercise the real controller over JSON lines.
add_executable(vv_functional_driver functional_driver.cpp)
target_link_libraries(vv_functional_driver PRIVATE vv_core)
add_test(NAME unit_functional COMMAND vv_unit_tests
  "--test-suite=functional")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME functional COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/functional_tests.py --driver $<TARGET_FILE:vv_functional_driver>)
set_tests_properties(unit_functional functional PROPERTIES LABELS functional TIMEOUT 90)
