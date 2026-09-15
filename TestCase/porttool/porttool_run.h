// porttool_run.h
//
// One-shot actions: pt.run <target> performs something that finishes, reports
// what it measured, and comes back to the command loop.
//
// This is the second shape the tool has, next to the one in porttool.h:
//
//   session    pt.start / pt.stop, keeps pushing "!" frames until stopped
//   run        pt.run, finishes and answers with one OK line of k=v
//
// A production sequence needs a shape that finishes and reports, so every
// check a plan makes can read its result off one line.
//
// *** The reply carries measurements, never a verdict. *** No target here
// prints PASS or FAIL. The limit lives in the PC's plan file, which is what
// lets production change a limit without reflashing every board -
// ../../docs/design/DECISIONS.md 22.

#ifndef TESTCASE_PORTTOOL_PORTTOOL_RUN_H_
#define TESTCASE_PORTTOOL_PORTTOOL_RUN_H_

#include <stdint.h>

/* Runs one target and prints its OK line. Returns 0 when the name is unknown,
 * in which case nothing was printed and the caller reports the error.
 *
 * `args` is what followed the target name on the pt.run line ("bytes=1048576"
 * and the like), or "" - so a one-shot can be parameterised from a plan file
 * the same way a session is. Pass NULL and it is treated as empty. */
int PortTool_RunTarget(const char *name, const char *args);

/* Prints one OK line per target, for pt.run with no argument. */
void PortTool_RunList(void);

/* Contributes the one-shot hardware to pt.caps: one "OK port=... kind=run"
 * line per piece of hardware, its targets in runs=. Returns how many lines
 * that is; with `emit` 0 it only counts, which is how pt.caps knows its own
 * length before printing the header.
 *
 * Without this the PC cannot check a plan file's pt.run targets offline - a
 * typo would only surface on a board, mid-station. */
uint32_t PortTool_RunCaps(int emit);

/* Writes the run targets that belong to `port` as "a,b,c" and returns how many
 * there were. */
uint32_t PortTool_RunTargetsFor(const char *port, char *out, uint32_t out_len);

#endif /* TESTCASE_PORTTOOL_PORTTOOL_RUN_H_ */
