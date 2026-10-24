/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A module's relocatable objects in one file, for the host that loads them with the JIT loader (code_loader.h) instead
   of linking them: "snx-objs", their count, then each one's size and bytes, sizes and count as 8-byte little-endian
   numbers. The recompile of a game for iOS writes one (docs/APPLE.md), and the app loads it */
#ifndef SNX_RECOMPILER_LLVM_OBJECTS_PACK_H
#define SNX_RECOMPILER_LLVM_OBJECTS_PACK_H
#include "common/cpu/recompiler/llvm/link.h"

/* The pack of count inputs (their data and size), malloc'd, or NULL when out of memory */
unsigned char* snx_objects_pack(const SnxLinkInput* inputs, size_t count, size_t* size);
/* The same pack written to path a record at a time, with no second copy of the objects in memory. released(arg, i), when
   given, is called as input i is written so that the caller can free it. 0 with the file's size in *size, or 1 when the
   file can't be written. An input whose data is NULL is read when its turn comes by load(arg, i, &size), which returns
   it malloc'd with its size (the input's) and is freed once written, or NULL, which fails the write: a caller that keeps
   an object on disk need not hold it in memory until then */
int snx_objects_pack_write(const SnxLinkInput* inputs, size_t count, const char* path, size_t* size,
                           void (*released)(void* arg, size_t index),
                           unsigned char* (*load)(void* arg, size_t index, size_t* size), void* arg);
/* The inputs of the pack at data (size bytes), whose data point into it, malloc'd with their count in *count; each
   name is name. NULL when it is not a pack, is cut short or is out of memory */
SnxLinkInput* snx_objects_unpack(const unsigned char* data, size_t size, const char* name, size_t* count);

#endif
