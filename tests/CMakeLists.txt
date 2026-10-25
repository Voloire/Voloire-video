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

if(VV_BUILD_UI)
  add_executable(vv_ui_functional_tests ui_functional_tests.cpp)
  set_target_properties(vv_ui_functional_tests PROPERTIES AUTOMOC ON)
  target_include_directories(vv_ui_functional_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
  target_link_libraries(vv_ui_functional_tests PRIVATE vv_ui)
  get_target_property(_test_qmake Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(_test_qtbin ${_test_qmake} DIRECTORY)
  target_compile_definitions(vv_ui_functional_tests PRIVATE VV_QT_PLUGIN_PATH="${_test_qtbin}/../plugins/platforms")
  add_test(NAME ui_functional COMMAND vv_ui_functional_tests)
  set_tests_properties(ui_functional PROPERTIES LABELS functional TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
