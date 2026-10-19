# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# tools/settingsgen on a spec of its own, with LLVM headers written here: every setter of a bool or an integer is
# found whichever way its declaration is broken over lines (LLVM 20's headers put the name on the line after
# LLVM_C_ABI void, LLVM 19's after void on the same line), the others are left out, the passes of a pipeline get
# their toggles, and an @ID that LLVM lacks is said on stderr. Run by ctest as settingsgen, with SETTINGSGEN the
# generator and WORK a folder of its own
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out" "${WORK}/include/llvm-c/Transforms")
file(WRITE "${WORK}/include/llvm-c/Transforms/PassBuilder.h" "
LLVM_C_ABI LLVMErrorRef LLVMRunPassesOnFunction(
    LLVMValueRef F, const char *Passes, LLVMTargetMachineRef TM,
    LLVMPassBuilderOptionsRef Options);
LLVM_C_ABI void
LLVMPassBuilderOptionsSetAAPipeline(LLVMPassBuilderOptionsRef Options,
                                    const char *AAPipeline);
LLVM_C_ABI void
LLVMPassBuilderOptionsSetLoopVectorization(LLVMPassBuilderOptionsRef Options,
                                           LLVMBool LoopVectorization);
void LLVMPassBuilderOptionsSetSLPVectorization(LLVMPassBuilderOptionsRef Options, LLVMBool SLPVectorization);
LLVM_C_ABI void LLVMPassBuilderOptionsSetForgetAllSCEVInLoopUnroll(
    LLVMPassBuilderOptionsRef Options, LLVMBool ForgetAllSCEVInLoopUnroll);
LLVM_C_ABI void
LLVMPassBuilderOptionsSetLicmMssaOptCap(LLVMPassBuilderOptionsRef Options,
                                        unsigned LicmMssaOptCap);
LLVM_C_ABI void
LLVMPassBuilderOptionsSetInlinerThreshold(LLVMPassBuilderOptionsRef Options,
                                          int Threshold);
/* avoid LLVMPassBuilderOptionsSetDebugLogging: a mention of a setter is not its declaration */
")
file(WRITE "${WORK}/include/llvm-c/TargetMachine.h" "
LLVM_C_ABI void LLVMSetTargetMachineAsmVerbosity(LLVMTargetMachineRef T,
                                                 LLVMBool VerboseAsm);
LLVM_C_ABI void
LLVMSetTargetMachineGlobalISelAbort(LLVMTargetMachineRef T,
                                    LLVMGlobalISelAbortMode Mode);
LLVM_C_ABI void LLVMSetTargetMachineFastISel(LLVMTargetMachineRef T,
                                             LLVMBool Enable);
")
file(WRITE "${WORK}/pipelines.txt"
     "0 function(ee-instrument<>),always-inline,verify\n2 function<eager-inv>(sroa<modify-cfg>,instcombine<max-iterations=1;no-verify-fixpoint>),require<globals-aa>,verify\n")
file(WRITE "${WORK}/settings.txt" "
table g_test_ini
setting Plain
    key plain
    int 0 1
    default 0
table g_test_llvm_ini llvm
setting @instcombine
    name Instruction combining
    help Combines instructions (instcombine)
setting @LoopVectorization
    name Loop vectorization
    help Whether the pipeline vectorizes loops
setting @InlinerThreshold
    name Inliner threshold
    help How large a function the inliner inlines
setting @NoSuchOption
    name Not in this LLVM
    help An ID this LLVM lacks, said and left out
section Test: g_test_ini
section Launch: g_test_llvm_ini/LLVM Internals    launcher
")
execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" --pipelines "${WORK}/pipelines.txt"
                        --llvm-include "${WORK}/include" "${WORK}/settings.txt"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result)
    message(FATAL_ERROR "settingsgen ended with ${result}: ${errors}")
endif()
file(READ "${WORK}/out/settings_llvm.c" llvm)
file(READ "${WORK}/out/snx_settings.h" header)
# each setter found, with its argument cast to the type the header declares, and the setting its @ID names
foreach(line "LLVMPassBuilderOptionsSetLoopVectorization(options, (LLVMBool)s->loop_vectorization)"
             "LLVMPassBuilderOptionsSetSLPVectorization(options, (LLVMBool)s->llvm_slp_vectorization)"
             "LLVMPassBuilderOptionsSetForgetAllSCEVInLoopUnroll(options, (LLVMBool)s->llvm_forget_all_scev_in_loop_unroll)"
             "LLVMPassBuilderOptionsSetLicmMssaOptCap(options, (unsigned)s->llvm_licm_mssa_opt_cap)"
             "LLVMPassBuilderOptionsSetInlinerThreshold(options, (int)s->inliner_threshold)"
             "LLVMSetTargetMachineAsmVerbosity(machine, (LLVMBool)s->llvm_asm_verbosity)"
             "LLVMSetTargetMachineFastISel(machine, (LLVMBool)s->llvm_fast_i_sel)")
    string(FIND "${llvm}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_llvm.c lacks \"${line}\":\n${llvm}")
    endif()
endforeach()
# a setter of a string or an enum, a function that is no setter, and a setter only mentioned: none
foreach(word AAPipeline GlobalISelAbort RunPassesOnFunction DebugLogging)
    if(llvm MATCHES "${word}")
        message(FATAL_ERROR "settings_llvm.c sets ${word}, which takes no bool or integer:\n${llvm}")
    endif()
endforeach()
# the pipelines' passes, named as they are, a toggle each: not a nested pipeline (function), nor require<>
foreach(pass instcombine sroa verify always-inline ee-instrument)
    string(FIND "${llvm}" "{\"${pass}\", offsetof(Settings, " at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_llvm.c has no toggle for the pass ${pass}:\n${llvm}")
    endif()
endforeach()
if(llvm MATCHES "\"function\", offsetof|\"require\", offsetof")
    message(FATAL_ERROR "settings_llvm.c toggles a pipeline or a require<>:\n${llvm}")
endif()
# the members: the @ID's name in snake_case, else LLVM's; an int option ranges to INT_MAX, a bool to 1
foreach(member "int instruction_combining; /* llvm_pass_instcombine */" "int llvm_pass_sroa; /* llvm_pass_sroa */"
               "int inliner_threshold; /* llvm_inliner_threshold */" "int llvm_slp_vectorization; /* llvm_slp_vectorization */")
    string(FIND "${header}" "${member}" at)
    if(at LESS 0)
        message(FATAL_ERROR "snx_settings.h lacks \"${member}\":\n${header}")
    endif()
endforeach()
file(READ "${WORK}/out/settings_launch.c" launch)
foreach(row "{\"llvm_inliner_threshold\", NULL, \"How large a function the inliner inlines\", INI_INT, 0, &g_settings.inliner_threshold, INT_MIN + 1, INT_MAX, NULL, \"Inliner threshold\", NULL, 0, 0, 0}"
            "{\"llvm_slp_vectorization\", NULL, \"LLVMPassBuilderOptionsSetSLPVectorization, LLVM's own value until it is set\", INI_INT, 0, &g_settings.llvm_slp_vectorization, 0, 1, NULL, \"LLVM SLP vectorization\", \"Off|On\", 0, 0, 0}")
    string(FIND "${launch}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_launch.c lacks the row ${row}:\n${launch}")
    endif()
endforeach()
if(header MATCHES "no_such_option|not_in_this_llvm")
    message(FATAL_ERROR "snx_settings.h has a member for @NoSuchOption, which this LLVM lacks:\n${header}")
endif()
