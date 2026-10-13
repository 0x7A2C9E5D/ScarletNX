# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Decrypted dumps keep the PFS0/HFS0 container layouts, mark gamecards DXCI and content archives DNCA, and set section
# encryption to None. Indirect/BKTR tables remain in decrypted patches. These changes use the pinned hactool's own
# parsers and preserve its encrypted paths; no game dump or downloaded source is modified.
# Header and indirect-table layouts follow the pinned hactool's nca.h, xci.h and bktr.h.

function(snx_hactool_replace variable before after)
    set(content "${${variable}}")
    string(FIND "${content}" "${before}" first)
    if(first LESS 0)
        message(FATAL_ERROR "The pinned hactool source no longer matches its decrypted-container patch (${variable})")
    endif()
    string(LENGTH "${before}" length)
    math(EXPR next "${first} + ${length}")
    string(SUBSTRING "${content}" ${next} -1 tail)
    string(FIND "${tail}" "${before}" second)
    if(NOT second LESS 0)
        message(FATAL_ERROR "The hactool decrypted-container patch has an ambiguous match (${variable})")
    endif()
    string(REPLACE "${before}" "${after}" content "${content}")
    set(${variable} "${content}" PARENT_SCOPE)
endfunction()

file(READ "${hactool_src}/xci.c" xci_text)
file(READ "${hactool_src}/nca.c" nca_text)
string(REPLACE "\r\n" "\n" xci_text "${xci_text}")
string(REPLACE "\r\n" "\n" nca_text "${nca_text}")

snx_hactool_replace(xci_text [=[#include "xci.h"]=] [=[#include "xci.h"

#define SNX_MAGIC_DXCI 0x49435844 /* "DXCI", decrypted gamecard. */]=])
snx_hactool_replace(xci_text [=[if (ctx->header.magic != MAGIC_HEAD) {]=]
    [=[if (ctx->header.magic != MAGIC_HEAD && ctx->header.magic != SNX_MAGIC_DXCI) {]=])
# DXCI retains the encrypted gamecard-info bytes, but its rebuilt HFS0 hashes use no compatibility suffix.
snx_hactool_replace(xci_text [=[if (memcmp(ctx->tool_ctx->settings.keyset.xci_header_key, null_key, 0x10) != 0) {]=]
    [=[if (ctx->header.magic != SNX_MAGIC_DXCI &&
        memcmp(ctx->tool_ctx->settings.keyset.xci_header_key, null_key, 0x10) != 0) {]=])

snx_hactool_replace(nca_text [=[#include "filepath.h"]=] [=[#include "filepath.h"

#define SNX_MAGIC_DNCA 0x41434E44 /* "DNCA", NCA3 layout with plaintext sections. */]=])
snx_hactool_replace(nca_text [=[    /* Try to support decrypted NCA headers. */]=]
    [=[    /* DNCA explicitly marks a complete decrypted NCA3 header and all section payloads. */
    if (ctx->header.magic == SNX_MAGIC_DNCA) {
        if (read_size != 0xC00) {
            fprintf(stderr, "Failed to read DNCA header!\n");
            return 0;
        }
        ctx->is_decrypted = 1;
        ctx->format_version = NCAVERSION_NCA3;
        return 1;
    }

    /* Try to support decrypted NCA headers. */]=])
# DNCA may retain its rights ID and decrypted key area; neither is needed for plaintext section reads.
snx_hactool_replace(nca_text [=[    /* Decrypt key area if required. */
    if (!ctx->has_rights_id) {
        nca_decrypt_key_area(ctx);
    } else {
        /* Decrypt title key. */]=]
    [=[    /* Decrypt key area if required. */
    if (!ctx->is_decrypted && !ctx->has_rights_id) {
        nca_decrypt_key_area(ctx);
    } else if (!ctx->is_decrypted) {
        /* Decrypt title key. */]=])
snx_hactool_replace(nca_text [=[                if (ctx->section_contexts[i].crypt_type == CRYPT_BKTR) {]=]
    [=[                if (ctx->section_contexts[i].crypt_type == CRYPT_BKTR ||
                    (ctx->section_contexts[i].is_decrypted &&
                     ctx->section_contexts[i].header->bktr_superblock.relocation_header.magic == MAGIC_BKTR &&
                     ctx->section_contexts[i].header->bktr_superblock.subsection_header.magic == MAGIC_BKTR)) {
                    /* Decrypted encryption flags do not remove the indirect tables needed for virtual reads. */
                    ctx->section_contexts[i].crypt_type = CRYPT_BKTR;]=])
snx_hactool_replace(nca_text [=[            if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_contentkey) {
                ctx->section_contexts[i].aes = new_aes_ctx]=]
    [=[            if (ctx->section_contexts[i].is_decrypted) {
                ctx->section_contexts[i].aes = NULL;
            } else if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_contentkey) {
                ctx->section_contexts[i].aes = new_aes_ctx]=])
# Read raw tables before they are initialized, then let plaintext BKTR follow the same virtual mapping as encrypted
# BKTR. Physical saves must bypass the base/patch relocation map even when a base file is present.
snx_hactool_replace(nca_text [=[    if (ctx->is_decrypted) {
        fseeko64(ctx->file, (ctx->offset + offset), SEEK_SET);]=]
    [=[    if (ctx->is_decrypted && (ctx->type != BKTR || ctx->bktr_ctx.subsection_block == NULL)) {
        fseeko64(ctx->file, (ctx->offset + offset), SEEK_SET);]=])
snx_hactool_replace(nca_text [=[if (ctx->tool_ctx->base_file == NULL && ctx->physical_reads == 0) {]=]
    [=[if (ctx->tool_ctx->base_file == NULL || ctx->physical_reads != 0) {]=])
snx_hactool_replace(nca_text [=[    if (ctx->is_decrypted && ctx->type != BKTR) {]=]
    [=[    if (ctx->is_decrypted && (ctx->type != BKTR || ctx->bktr_ctx.subsection_block == NULL)) {]=])
# Plaintext patch sections may place integrity metadata after CTREx, or leave a gap between the two tables. Keep
# encrypted layout checks unchanged; plaintext tables still have to fit the section without overflow or overlap.
snx_hactool_replace(nca_text [=[        if (sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||
            sb->subsection_header.offset + sb->subsection_header.size != ctx->size) {]=]
    [=[        int invalid_layout;
        if (ctx->is_decrypted) {
            invalid_layout = sb->relocation_header.offset > ctx->size ||
                sb->relocation_header.size > ctx->size - sb->relocation_header.offset ||
                sb->subsection_header.offset > ctx->size ||
                sb->subsection_header.size > ctx->size - sb->subsection_header.offset ||
                sb->relocation_header.offset + sb->relocation_header.size > sb->subsection_header.offset;
        } else {
            invalid_layout = sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||
                sb->subsection_header.offset + sb->subsection_header.size != ctx->size;
        }
        if (invalid_layout) {]=])
# Modern CTREx roots stop at the patch-data end before indirect metadata; older roots include the indirect table.
snx_hactool_replace(nca_text [=[        if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset) {]=]
    [=[        if (ctx->is_decrypted ?
            (ctx->bktr_ctx.subsection_block->total_size < sb->relocation_header.offset ||
             ctx->bktr_ctx.subsection_block->total_size > sb->subsection_header.offset) :
            ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset) {]=])

set(hactool_patched_dir "${CMAKE_CURRENT_BINARY_DIR}/hactool-decrypted")
file(MAKE_DIRECTORY "${hactool_patched_dir}")
file(WRITE "${hactool_patched_dir}/xci.c" "${xci_text}")
file(WRITE "${hactool_patched_dir}/nca.c" "${nca_text}")
