# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# ScarletNX's own C is written to ISO C's rules, and two flags would hide code that breaks them:
# - -fno-strict-aliasing: an object is accessed through its own type, a character type, or memcpy
#   (common/utilities/endian.h for guest and wire bytes), and builds with Clang's default strict aliasing;
# - -fwrapv (and -fno-strict-overflow, which implies it): signed arithmetic doesn't overflow. Arithmetic that wraps is
#   done in unsigned types and converted back, a product that can overflow gets a wider intermediate.
# The configure stops on either anywhere it reaches that code: the global flags, and every target, source file and
# directory defined from this project's source tree. Third-party packages (CPM's, under .cache/cpm or the build tree)
# keep their own flags. The LLVM backend's runtime library, which is compiled by a custom command, checks its own flags
# (src/common/cpu/recompiler/llvm/).
#
# SNX_TRAPV builds that same code, and the runtime library, with -ftrapv: a signed overflow left in it stops the program
# there (an illegal instruction) instead of giving whatever the optimizer made of it. A build to find them with
# (the tests, a game run), never one to ship. -fsanitize=signed-integer-overflow (SNX_TEST_SANITIZERS) says where.
include_guard(GLOBAL)

option(SNX_TRAPV "Build ScarletNX's own code and the game modules' runtime library with -ftrapv, to find signed \
overflows (cmake/banned_flags.cmake)" OFF)

function(snx_check_banned_flags what flags)
    if("${flags}" MATCHES "fno-strict-aliasing")
        message(FATAL_ERROR "${what} has -fno-strict-aliasing, which ScarletNX doesn't build with: access an "
                            "object through its own type, a character type or memcpy instead (cmake/banned_flags.cmake)")
    endif()
    if("${flags}" MATCHES "fwrapv|fno-strict-overflow")
        message(FATAL_ERROR "${what} has -fwrapv or -fno-strict-overflow, which ScarletNX doesn't build with: do "
                            "arithmetic that wraps in unsigned types, and give a product that can overflow a wider "
                            "intermediate (cmake/banned_flags.cmake)")
    endif()
endfunction()

function(snx_check_banned_flags_dir dir)
    get_directory_property(options DIRECTORY "${dir}" COMPILE_OPTIONS)
    snx_check_banned_flags("The directory ${dir}" "${options}")
    get_directory_property(targets DIRECTORY "${dir}" BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        set(props INTERFACE_COMPILE_OPTIONS)
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            list(APPEND props COMPILE_OPTIONS COMPILE_FLAGS)
        endif()
        foreach(prop IN LISTS props)
            get_target_property(value ${target} ${prop})
            if(value)
                snx_check_banned_flags("The target ${target} (${prop})" "${value}")
            endif()
        endforeach()
        if(SNX_TRAPV AND type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-ftrapv>)
        endif()
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(sources ${target} SOURCES)
            get_target_property(target_dir ${target} SOURCE_DIR)
            foreach(source IN LISTS sources)
                if(source MATCHES "^\\$<") # a generator expression's sources have no properties of their own here
                    continue()
                endif()
                cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${target_dir}" NORMALIZE)
                get_source_file_property(value "${source}" TARGET_DIRECTORY ${target} COMPILE_OPTIONS)
                get_source_file_property(flags "${source}" TARGET_DIRECTORY ${target} COMPILE_FLAGS)
                snx_check_banned_flags("The source ${source} of ${target}" "${value} ${flags}")
            endforeach()
        endif()
    endforeach()
    get_directory_property(subdirs DIRECTORY "${dir}" SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${subdir}" NORMALIZE ours)
        cmake_path(IS_PREFIX PROJECT_BINARY_DIR "${subdir}" NORMALIZE generated)
        if(ours AND NOT generated AND NOT subdir MATCHES "/\\.cache/")
            snx_check_banned_flags_dir("${subdir}")
        endif()
    endforeach()
endfunction()

# Called at the end of the top-level CMakeLists.txt, once every target is defined
function(snx_check_project_flags)
    set(flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}")
    foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_BUILD_TYPE})
        string(TOUPPER "${config}" config)
        string(APPEND flags " ${CMAKE_C_FLAGS_${config}} ${CMAKE_CXX_FLAGS_${config}}")
    endforeach()
    snx_check_banned_flags("CMAKE_C_FLAGS or CMAKE_CXX_FLAGS" "${flags}")
    snx_check_banned_flags_dir("${PROJECT_SOURCE_DIR}")
endfunction()
