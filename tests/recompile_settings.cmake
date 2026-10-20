# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# scarletnx -r --KEY=VALUE, a setting of the recompile (settings.txt's recompile and llvm tables): a value the setting
# does not take is refused before anything runs, saying what it takes; one it takes reaches the input's check. Run by
# ctest as runtime_recompile_settings
set(dir "${CMAKE_CURRENT_BINARY_DIR}/recompile-settings")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
foreach(case "--aot_threads=abc|aot_threads=abc is not a whole number from 0 to 1024"
             "--aot_threads=1.5|aot_threads=1.5 is not a whole number from 0 to 1024"
             "--aot_threads=2000|aot_threads=2000 is not a whole number from 0 to 1024"
             "--aot_threads=|aot_threads= is not a whole number from 0 to 1024"
             "--llvm_inliner_threshold=50x|llvm_inliner_threshold=50x is not a whole number from -2147483647 to 2147483647"
             "--opt=5|opt expects 0,1,2,3,s,z"
             "--aot_threads=4|holds no exefs/main" "--llvm_inliner_threshold=50|holds no exefs/main"
             "--opt=z|holds no exefs/main")
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 option)
    list(GET case 1 expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                            "${RUNTIME}" -r missing.nro ${option}
                    WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out
                    ERROR_VARIABLE out)
    string(FIND "${out}" "${expected}" at)
    if(result EQUAL 0 OR at LESS 0)
        file(REMOVE_RECURSE "${dir}")
        message(FATAL_ERROR "scarletnx -r missing.nro ${option} ended with ${result}, not \"${expected}\":\n${out}")
    endif()
endforeach()
file(REMOVE_RECURSE "${dir}")
