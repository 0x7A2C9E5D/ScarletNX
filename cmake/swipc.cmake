# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# swipc (tools/swipc/swipc.c) generates the IPC dispatchers from the interface definitions (.swipc, docs/IPC.md). It
# runs during the build, so a cross build builds it a second time, for the machine doing the build, with that
# machine's C compiler (SNX_HOST_C_COMPILER, clang by default)
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    set(SNX_HOST_C_COMPILER "clang" CACHE STRING "C compiler of the machine doing a cross build, for build-time tools")
    include(ExternalProject)
    set(snx_swipc_exe "${CMAKE_BINARY_DIR}/swipc-host/swipc")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_swipc_exe ".exe")
    endif()
    ExternalProject_Add(snx_swipc_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/swipc"
        BINARY_DIR "${CMAKE_BINARY_DIR}/swipc-host"
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_C_COMPILER}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra -Werror"
        # a local source has no download step to tell the build that swipc.c changed: its own build decides
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_swipc_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-swipc IMPORTED)
    set_target_properties(snx-swipc PROPERTIES IMPORTED_LOCATION "${snx_swipc_exe}")
    add_dependencies(snx-swipc snx_swipc_host)
else()
    add_executable(snx-swipc "${PROJECT_SOURCE_DIR}/tools/swipc/swipc.c")
    set_target_properties(snx-swipc PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-swipc PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_swipc(<target> ROOT <dir> FILES <file.swipc>...): generates <file.swipc>.h and .c in <target>-swipc in the
# build folder for each file (relative to ROOT, types before their first use), compiles the .c files into <target> and
# gives it the headers' include root, so they are included as "<file.swipc>.h". A test that compiles a service's C
# file generates that service's files for itself the same way
function(snx_swipc target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "ROOT" "FILES")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${target}-swipc")
    set(inputs)
    set(outputs)
    foreach(f IN LISTS arg_FILES)
        get_filename_component(dir "${out}/${f}" DIRECTORY)
        file(MAKE_DIRECTORY "${dir}")
        list(APPEND inputs "${arg_ROOT}/${f}")
        list(APPEND outputs "${out}/${f}.h" "${out}/${f}.c")
        target_sources(${target} PRIVATE "${out}/${f}.c")
    endforeach()
    add_custom_command(OUTPUT ${outputs}
        COMMAND snx-swipc --root "${arg_ROOT}" --out "${out}" ${arg_FILES}
        DEPENDS snx-swipc ${inputs}
        COMMENT "Generating the IPC dispatchers of ${target}" VERBATIM)
    target_include_directories(${target} PUBLIC "${out}")
endfunction()
