# SPDX-License-Identifier: MIT
function(th_unit_test name)
    add_executable(${name} unit/${name}.c)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME unit.${name} COMMAND ${name})
    set_tests_properties(unit.${name} PROPERTIES LABELS unit TIMEOUT 120)
endfunction()

th_unit_test(test_core th_base)
th_unit_test(test_treemap th_base m)
th_unit_test(test_ipc th_base Threads::Threads)
th_unit_test(test_db th_engine)
th_unit_test(test_indexer th_engine)
th_unit_test(test_search th_engine)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME integration.daemon COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/daemon.py $<TARGET_FILE:treehoundd> $<TARGET_FILE:treehound>)
set_tests_properties(integration.daemon PROPERTIES LABELS integration TIMEOUT 60)

if(TREEHOUND_BUILD_GUI)
    find_program(XVFB_RUN xvfb-run)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    if(XVFB_RUN AND DBUS_RUN_SESSION)
        add_test(NAME integration.gui COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/integration/gui.py $<TARGET_FILE:treehoundd> $<TARGET_FILE:treehound>)
        set_tests_properties(integration.gui PROPERTIES LABELS integration TIMEOUT 30)
    endif()
endif()

# A test-only linker wrapper injects one ENOSPC without changing host limits.
add_executable(treehoundd_watchfault ../src/daemon/main.c ../src/daemon/handlers.c
    ../src/daemon/scanner_runtime.c ../src/daemon/monitor.c integration/watch_fault.c)
target_link_libraries(treehoundd_watchfault PRIVATE th_engine)
target_link_options(treehoundd_watchfault PRIVATE -Wl,--wrap=inotify_add_watch -Wl,--wrap=fstatat64)
add_test(NAME integration.watch_fault COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/watch_fault.py $<TARGET_FILE:treehoundd_watchfault>)
set_tests_properties(integration.watch_fault PROPERTIES LABELS integration TIMEOUT 20)

add_test(NAME integration.scan_fault COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/scan_fault.py $<TARGET_FILE:treehoundd_watchfault>)
set_tests_properties(integration.scan_fault PROPERTIES LABELS integration TIMEOUT 20)
