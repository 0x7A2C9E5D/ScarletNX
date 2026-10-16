# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# `scarletnx --settings`: every setting of the game's .ini, by section, a line each of seven fields, the help on one
# line (runtime/options.c's ini_list), as the menu's Settings read them. Run by ctest as runtime_settings_list
execute_process(COMMAND "${RUNTIME}" --settings RESULT_VARIABLE result OUTPUT_VARIABLE list ERROR_VARIABLE errors)
if(result OR NOT errors STREQUAL "")
    message(FATAL_ERROR "scarletnx --settings ended with ${result}: ${errors}")
endif()
set(field "[^\t\r\n]*")
string(REGEX REPLACE "${field}\t${field}\t${field}\t(int|float|text)\t${field}\t${field}\t${field}\r?\n" "" rest
       "${list}")
if(NOT rest STREQUAL "" OR list STREQUAL "")
    message(FATAL_ERROR "lines of scarletnx --settings that are not seven fields:\n${rest}")
endif()
foreach(row "Display\tdocked\t0\tint\t0\t1\t1 = TV mode" "Display\tfullscreen\t0\tint\t0\t1\t"
            "Graphics\tresolution\t1\tfloat\t0.25\t4\t" "Performance\tfps_limit\tgame\ttext\t"
            "Input\tcontrols\t" "Input\tcontrols_a\t\ttext\t0\t0\tA control's place" "Debug\tlog\t1\tint\t0\t2\t"
            ", 2 French, 3 German, 4 Italian, 5 Spanish, 6 Chinese, 7 Korean,")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
string(FIND "${list}" "\ttouch\t" at) # the game's menu sets it
if(NOT at LESS 0)
    message(FATAL_ERROR "scarletnx --settings lists touch, which the game's menu sets")
endif()
if(NOT list MATCHES "^Display\t.*\nGraphics\t.*\nPerformance\t.*\nInput\t.*\nNetwork\t.*\nDebug\t")
    message(FATAL_ERROR "scarletnx --settings lists the sections out of the .ini's order")
endif()
