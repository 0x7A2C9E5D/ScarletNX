/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_FS_H
#define SNX_CTR_FS_H
#include "3ds/ipc/ctr_ipc.h"
void snx_ctr_fs_dispatch(SnxCtrIpc* c);
/* A new file or directory object answered as the request's moved handle (word 3): kind CTR_FILE_*, a FILE* host and
   an FsNode* node it retains. False when the reply already says why not */
bool snx_ctr_fs_file_object(SnxCtrIpc* c, uint32_t kind, uint32_t writable, uint32_t fixed, void* host, void* node);
#endif
