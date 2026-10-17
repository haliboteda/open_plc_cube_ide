// porttool_aodiag.h
//
// *** TEMPORARY diagnostic: remove once the AO noise source is found. ***
// pt.aodiag runs one AO noise experiment (E0..E6). Experiments, probes and how
// to read them: $PROD/maps/fixture-test-stages/issues/STG-41-where-does-the-ao-noise-come-from.md
// Not listed in pt.caps.

#ifndef TESTCASE_PORTTOOL_PORTTOOL_AODIAG_H_
#define TESTCASE_PORTTOOL_PORTTOOL_AODIAG_H_

/* pt.aodiag exp=E0..E6 [ch=1|2] [mv=<dac mV>] [secs=N] [scale=0..3]
 * Validates every parameter first; on success calls stop_sessions, prints
 * "OK aodiag ... starting", blocks for the experiment, restores, then prints
 * "OK aodiag done ...". */
void PortTool_AoDiag(const char *args, void (*stop_sessions)(void));

#endif /* TESTCASE_PORTTOOL_PORTTOOL_AODIAG_H_ */
