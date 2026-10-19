/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the MCP server's files share (mcp.c, mcp_edenprobe.c): a growing text, and the edenprobe tools */
#ifndef SNX_MCP_INT_H
#define SNX_MCP_INT_H
#include <stddef.h>

/* text grown as it is written, p NULL until then; lines counts what a tool chose to count */
typedef struct {
    char* p;
    size_t n, cap;
    int lines;
} Text;
void mcp_put_n(Text* t, const char* p, size_t n);
void mcp_put(Text* t, const char* p);
void mcp_put_num(Text* t, unsigned long v);

/* The edenprobe tools (mcp_edenprobe.c): each returns its answer, malloc'd, or NULL with why in err */

/* A service case's mechanical parts written into the edenprobe checkout, every file or none: the case's enum entry
   and row, its judge and the judge's reference check (both to be filled in), the device side asking nothing yet, and
   its suite's row in ui.c when the suite is new. The answer lists what was written and what is left to write */
char* mcp_edenprobe_scaffold(const char* checkout, const char* suite, const char* id, const char* name,
                             const char* tier, long issue, Text* err);
/* make -f host.mk host-run in the checkout: the judges and their reference checks, on the host compiler. The
   answer: the lines about suite, the failures and the end */
char* mcp_edenprobe_check(const char* checkout, const char* suite, Text* err);
/* The NRO recompiled by self (this program) into cache (once while the NRO is unchanged) and run headless with the
   mode words until its summary, relaunched after a crash: the report's lines for filter ("suite" or "suite: name"),
   and its summary */
char* mcp_edenprobe_run(const char* self, const char* cache, const char* nro, const char* mode, const char* filter,
                        Text* err);
/* The case's lines in ScarletNX's report and the reference's (another emulator's, a console's), and the edenprobe pull
   request's text: from which issue, what each machine answered, the tests run, and what a case may not hold */
char* mcp_edenprobe_finding(long issue, const char* suite, const char* name, const char* ours, const char* theirs,
                            const char* source, Text* err);

#endif
