# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME runtime_architecture
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/architecture.py"
            --repo "${PROJECT_SOURCE_DIR}" --build "${PROJECT_BINARY_DIR}" --cmake "${CMAKE_COMMAND}")
    # Graph generation reconfigures the existing build tree, so it cannot overlap
    # a native regression test that invokes the project's compiler or driver.
    set_tests_properties(runtime_architecture PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
    add_test(NAME architecture_checker COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/architecture_test.py")
    # TODO: fix windows 
    # Traceback (most recent call last):
    #   File "D:\a\ScarletNX\ScarletNX\tests\gnuabi_diff_test.py", line 62, in test_mismatch_saves_elf_and_both_streams
    #     self.assertIn(str(saved / 'program.elf'), metadata['results']['qemu']['replay_argv'])
    #     ~~~~~~~~~~~~~^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    # AssertionError: 'C:\\Users\\RUNNER~1\\AppData\\Local\\Temp\\snx-diff-test-grwp7ao9\\artifacts\\test-1791863865831241200\\program.elf' not found in ['C:\\hostedtoolcache\\windows\\Python\\3.14.7\\x64\\python.exe', 'C:\\Users\\RUNNER~1\\AppData\\Local\\Temp\\snx-diff-test-grwp7ao9\\right.py', 'C:\\Users\\runneradmin\\AppData\\Local\\Temp\\snx-diff-test-grwp7ao9\\artifacts\\test-1791863865831241200\\program.elf']
    if (NOT WIN32)
        add_test(NAME gnuabi_differential_harness COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/gnuabi_diff_test.py")
    endif()
    if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND SNX_BACKEND STREQUAL "headless" AND NOT SNX_VULKAN)
        add_test(NAME nx_runtime_homebrew
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/nx_runtime_smoke.py"
                --runtime $<TARGET_FILE:scarletnx> --cc "${CMAKE_C_COMPILER}" --repo "${PROJECT_SOURCE_DIR}"
                --artifact-dir "${PROJECT_BINARY_DIR}/nx-smoke-failures")
        set_tests_properties(nx_runtime_homebrew PROPERTIES TIMEOUT 90)
    endif()
endif()
