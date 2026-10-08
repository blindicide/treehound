# SPDX-License-Identifier: MIT
function(th_unit_test name)
    add_executable(${name} unit/${name}.c)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME unit.${name} COMMAND ${name})
    set_tests_properties(unit.${name} PROPERTIES LABELS unit TIMEOUT 120)
endfunction()

th_unit_test(test_core th_base)
th_unit_test(test_db th_engine)
th_unit_test(test_indexer th_engine)
