# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The commit of SRC's tree, for the window titles: its short hash, with "-dirty" when the work tree differs, else
# "unknown" outside a git checkout. Writes OUT only when that changed, so a commit relinks the two programs that show
# it and nothing else. With CHECK set, compares OUT with `git rev-parse` instead, as the build_info_commit test
execute_process(COMMAND git -C "${SRC}" describe --always --dirty --abbrev=7 --match=snx-no-tag-matches
    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
if(failed OR NOT commit)
    set(commit "unknown")
endif()
if(CHECK)
    file(READ "${OUT}" header)
    execute_process(COMMAND git -C "${SRC}" rev-parse --short=7 HEAD OUTPUT_VARIABLE head
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
    if(failed OR NOT head)
        set(head "unknown")
    endif()
    if(NOT header MATCHES "#define SNX_COMMIT \"${head}(-dirty)?\"")
        message(FATAL_ERROR "${OUT} says ${header}, the tree is at ${head}")
    endif()
    return()
endif()
file(CONFIGURE OUTPUT "${OUT}" CONTENT "#define SNX_COMMIT \"@commit@\"\n" @ONLY)
