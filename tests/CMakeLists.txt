# SPDX-License-Identifier: MIT
function(th_unit_test name)
    add_executable(${name} unit/${name}.c)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME unit.${name} COMMAND ${name})
    set_tests_properties(unit.${name} PROPERTIES LABELS unit TIMEOUT 120)
endfunction()

th_unit_test(test_core th_base)
th_unit_test(test_ipc th_base Threads::Threads)
th_unit_test(test_db th_engine)
th_unit_test(test_indexer th_engine)
th_unit_test(test_search th_engine)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME integration.daemon COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/daemon.py $<TARGET_FILE:treehoundd>)
set_tests_properties(integration.daemon PROPERTIES LABELS integration TIMEOUT 60)
