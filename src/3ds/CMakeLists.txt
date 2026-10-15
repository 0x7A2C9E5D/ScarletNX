# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Stage 1 deliberately needs no NX runtime, LLVM, SDL or Azahar build tree.
add_library(snx_ctr_board STATIC board/board.c)
target_include_directories(snx_ctr_board PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(snx_ctr_board PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_ctr_board PRIVATE -Wall -Wextra -Werror=missing-prototypes)
add_executable(scarletnx-3ds runtime/main.c)
target_link_libraries(scarletnx-3ds PRIVATE snx_ctr_board scarletnx-common)
set_target_properties(scarletnx-3ds PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_options(scarletnx-3ds PRIVATE -Wall -Wextra -Werror=missing-prototypes)
include(GNUInstallDirs)
install(TARGETS scarletnx-3ds RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    target_compile_definitions(scarletnx-3ds PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
