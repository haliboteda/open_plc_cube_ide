// porttool_run.h
//
// One-shot actions: pt.run <target> finishes and answers with one OK line of
// k=v measurements, never a verdict - limits live in the PC's plan file
// (DECISIONS.md 22). Sessions (porttool.h) are the other shape.

#ifndef TESTCASE_PORTTOOL_PORTTOOL_RUN_H_
#define TESTCASE_PORTTOOL_PORTTOOL_RUN_H_

#include <stdint.h>

/* Runs one target and prints its OK line. Returns 0, printing nothing, when the
 * name is unknown. `args` is what followed the name ("bytes=1048576"); NULL is
 * empty. */
int PortTool_RunTarget(const char *name, const char *args);

/* Prints one OK line per target, for pt.run with no argument. */
void PortTool_RunList(void);

/* Contributes the one-shot hardware to pt.caps: one "OK port=... kind=run" line
 * per piece of hardware, its targets in runs=, so the PC can check a plan's
 * targets offline. Returns the line count; with `emit` 0 it only counts. */
uint32_t PortTool_RunCaps(int emit);

/* Writes the run targets that belong to `port` as "a,b,c" and returns how many
 * there were. */
uint32_t PortTool_RunTargetsFor(const char *port, char *out, uint32_t out_len);

#endif /* TESTCASE_PORTTOOL_PORTTOOL_RUN_H_ */
