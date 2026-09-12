// porttool.h
//
// Port test tool: an instrument panel for the terminals, driven from a PC over
// the RS232 log port. Pick a port, set its parameters, start it, and it keeps
// reporting until stopped - the hardware engineer watches while probing.
//
// This is not a pass/fail production test. Nothing here judges anything; it
// samples, and the PC side decides what to show.
//
// *** Entry point only. main.c calls PortTool_Run() behind PORTTOOL_ENABLE and
// *** it never returns, so the IAP server never starts and nothing in the
// *** bootloader's normal path is touched.
//
// One serial line carries two kinds of output:
//
//   !din t=48213 v=0x16 ...      pushed samples, for the PC side
//   [T1 ] inputs: PB5=0 ...      the existing bring-up printf log, for a person
//
// A "!" first character is the whole protocol difference. A person watching a
// plain terminal sees both and can read either.
//
// Commands (one per line, echoed nowhere - the reply is the answer):
//
//   pt.caps                    what this firmware exposes
//   pt.id                      board identity
//   pt.list                    which sessions are running
//   pt.start <port> [k=v ...]  start one
//   pt.set   <port> k=v ...    change parameters while it runs
//   pt.stop  <port> | all      stop
//   pt.echo  <port> <n>        close the loopback count for one frame
//   pt.hold  [<ms>]            renew the deadman, 0 disarms, bare word queries
//   pt.led   fault=0|1         drive the indicator from the PC
//   pt.run   [<target>]        list one-shot actions, or perform one
//   pt.handover [<target>]     list exclusive tests, or hand control to one
//
// A timed run is timed by the PC, which also makes every verdict: the board
// only samples. pt.hold is the one thing the board decides on its own, and it
// decides whether anyone is still listening - not whether the board is good.
// Without it a PC that dies leaves the outputs driven until somebody presses
// reset. See docs/design/DECISIONS.md 37.

#ifndef TESTCASE_PORTTOOL_PORTTOOL_H_
#define TESTCASE_PORTTOOL_PORTTOOL_H_

#include <stdint.h>

/* Every porttool_*.c wraps its whole body in this, so an image built without it
 * carries none of this code. Turn it on with a project symbol:
 * PORTTOOL_ENABLE=1
 *
 * The gate is about the port table, not about flash: the ports[] array in
 * porttool.c references every session, so nothing here is "merely uncalled"
 * and the linker could not drop it on its own.
 *
 * (An earlier version of this comment said the project links without
 * --gc-sections. It does not - the build has -ffunction-sections
 * -fdata-sections -Wl,--gc-sections and -Os. Checked 2026-09-08 against the
 * real build log.) */
#ifndef PORTTOOL_ENABLE
#define PORTTOOL_ENABLE 0
#endif

/* 115200 8N1 is about 11.5 KB/s and printf blocks, so a session that samples
 * too fast stalls the loop it lives in. */
#define PORTTOOL_PERIOD_MIN_MS   50U
#define PORTTOOL_PERIOD_DEF_MS   200U
#define PORTTOOL_LINE_MAX        192U
#define PORTTOOL_REPLY_MAX       192U

/* Channel sets are carried in a uint32_t bitmask, so no port may have more
 * channels than that holds. The widest today is eight. */
#define PORTTOOL_MAX_CHANNELS    32U

/* The product's four boards, plus one for a test that spans them.
 *
 * These are protocol values, so they stay English and lowercase; the panel
 * turns them into whatever it shows a person, and displays an unrecognised one
 * verbatim rather than dropping the port. The set is the product's structure
 * (OPLC-SPEC-HW-001 section 1), not a list of ports, which is why it can live
 * as a fixed set here and in the panel at the same time without the staleness
 * problem a port list would have. */
#define PORTTOOL_BOARD_BRIDGE   "bridge"    /* CPU, SDRAM, Ethernet, USB, SD, RTC */
#define PORTTOOL_BOARD_UPPER    "upper"     /* Upper Deck: DI, AI, AO, CAN, RS485, RS232, KNX */
#define PORTTOOL_BOARD_LOWER    "lower"     /* Lower Deck: high-side out, relays, temperature */
#define PORTTOOL_BOARD_JUNCTION "junction"  /* Junction Link: 24 V in, rails, DIN rail connector */
#define PORTTOOL_BOARD_WHOLE    "whole"     /* spans boards - bringup does */

/* noreturn is not documentation here, it is what makes the image fit.
 *
 * main.c calls this from Phase 1 and everything after the call - the BOOT0
 * gesture, the start decision, the IAP server, the USB stack, mbedTLS - is
 * unreachable in a PORTTOOL_ENABLE image. Without noreturn the compiler cannot
 * know that, so all of it stays live: the project already builds with
 * -ffunction-sections -fdata-sections -Wl,--gc-sections, and the linker still
 * had to keep every one of those functions because main() still called them.
 *
 * Measured 2026-09-08: the tool image overflowed the 120K bootloader region by
 * 47,608 bytes for exactly this reason.
 *
 * lwIP is the one exception, as of the ethernet session (DECISIONS.md 28): it
 * is reachable again because porttool_eth.c calls MX_LWIP_Init() itself. That
 * is why the tool image is now 153,956 bytes rather than about 100K - which
 * costs nothing, since it is flashed whole by ST-Link and has no size gate. */
__attribute__((noreturn)) void PortTool_Run(void);

/* Where a port's echo counter travels. The PC replies to every sample frame
 * and the board counts on from the value it gets back; which wire that reply
 * rides on decides what the counter is worth. */
typedef enum {
    PORTTOOL_LOOP_NONE = 0,  /* not a link at all - no peer to answer */
    PORTTOOL_LOOP_CTRL,      /* rides the RS232 control port: proves only that
                                the control port and the loop are alive, never
                                that the port under test works */
    PORTTOOL_LOOP_LINK,      /* rides the port under test: proves that link */
    PORTTOOL_LOOP_SELF,      /* the port under test IS the control port, so the
                                control-port round trip does prove the link.
                                Only rs232: its bytes physically cross the
                                terminal, the MAX3221 and PC10/PC11 on their way
                                to and from the PC. Distinct from ctrl because
                                for this one port the counter really is the
                                verdict, and a panel that said otherwise would
                                be lying about the only test it has. */
} porttool_loop_t;

/* Whether a port is a session that can be started and stopped, or a one-way
 * handover into a standalone bring-up entry. */
typedef enum {
    PORTTOOL_KIND_SESSION = 0,
    PORTTOOL_KIND_HANDOVER,
} porttool_kind_t;

/* The echo counter one session keeps.
 *
 * The board sends a number, the PC sends it back, and the board counts on from
 * what it got. Whether that round trip proves anything about the port depends
 * on which wire the reply came back over - see porttool_loop_t. */
typedef struct {
    uint32_t seq;      /* the number the last frame carried */
    uint32_t rx;       /* the last number that actually came back */
    uint32_t miss;     /* consecutive periods with no usable reply */
    uint32_t got;      /* what arrived since the last tick */
    int      have;     /* whether anything arrived at all */
    int      started;  /* whether a frame has gone out yet */
} porttool_echo_t;

/* Starts the counter over. A session calls this from its start(). */
void PortTool_EchoReset(porttool_echo_t *e);

/* Hands the counter a number that came back. For loop=ctrl ports that is
 * pt.echo; for loop=link ports the session reads it off its own link. */
void PortTool_EchoGot(porttool_echo_t *e, uint32_t value);

/* Advances the counter one period. Call once per frame, before formatting.
 *
 * A reply that matches advances seq and clears miss. Anything else - nothing
 * back, or a number that does not match what was sent - leaves seq where it is
 * and counts a miss, so a stalled counter is visible rather than a counter
 * that keeps climbing on its own and looks healthy. */
void PortTool_EchoTick(porttool_echo_t *e);

/* Writes "seq=<n> rx=<n> miss=<n>" and returns how many characters. No leading
 * space: these come first in a frame body, and a caller appending anything
 * else adds its own separator. */
uint32_t PortTool_EchoFields(const porttool_echo_t *e, char *out, uint32_t out_len);

/* One testable port. Each porttool_<name>.c defines one and the table in
 * porttool.c lists it.
 *
 * The const fields are what pt.caps reports so the PC can build its panel from
 * the firmware rather than from a hard-coded copy: adding a port here is the
 * only edit needed for it to appear on screen. */
typedef struct {
    const char *name;

    /* Which of the product's boards this port physically lives on, so the
     * panel can group by board without keeping its own table of which port is
     * where - a table that would go stale the moment a port is added here.
     * One of PORTTOOL_BOARD_*. */
    const char *board;

    const char *blk;     /* Klemmblock letter, "-" when the port has no terminal */
    const char *term;    /* terminal range, e.g. "D02-D09", or "-" */
    const char *terms;   /* one label per channel when they are not a plain run
                            of term, e.g. relay's "B01+B02,B03+B04,...".
                            NULL means the panel can derive them from term. */
    const char *params;  /* parameter names pt.start and pt.set accept */

    porttool_loop_t loop;
    uint8_t         channels;

    /* Applies `args` and starts sampling. On refusal writes a reason into
     * `err` and returns 0 - a session that cannot produce trustworthy data
     * must not start and quietly emit something that looks like data. */
    int  (*start)(const char *args, char *err, uint32_t err_len);

    /* Applies `args` to a running session. Same refusal contract. */
    int  (*set)(const char *args, char *err, uint32_t err_len);

    void (*stop)(void);

    /* Called every superloop pass. The driver decides whether its period has
     * elapsed and pushes its own frames. */
    void (*tick)(uint32_t now_ms);

    /* Hands the session a number the PC echoed back over the control port.
     * Only loop=ctrl ports have one: a loop=link port takes its reply off the
     * link under test, and accepting one here instead would let its counter
     * climb while that link is dead - a pass the hardware never earned. */
    void (*echo)(uint32_t value);

    /* The port's *current parameter values* for pt.caps, as "k=v k=v".
     * Everything fixed about the port is in the const fields above, so this
     * reports only what pt.start and pt.set can change. */
    void (*caps)(char *out, uint32_t out_len);

    /* What each parameter will accept, as "name:spec name:spec". Optional; a
     * port with nothing worth stating leaves it NULL.
     *
     * Four spec shapes, and no others:
     *   lo..hi   a closed range          duty:0..100
     *   lo..     open at the top         period:50..
     *   a|b|c    an enumeration          mode:hold|blink
     *   1..n     channel numbers         ch:1..8
     *
     * *** Build the string from the same constants the apply() checks use. ***
     * Written out by hand it would be a second copy of every limit, and the
     * two would drift - which is the exact failure this line exists to stop on
     * the PC side. snprintf from the macros costs nothing and cannot disagree.
     *
     * Why it is a separate caps line and not part of params=: the port= line
     * is already 138 bytes for can, and PORTTOOL_LINE_MAX is 192. A truncated
     * caps line is a limit the panel silently never enforces. */
    void (*limits)(char *out, uint32_t out_len);

    int running;
} porttool_port_t;

/* Pushes one sample frame: "!<port> t=<ms> " then the formatted body. */
void PortTool_Frame(const char *port, const char *fmt, ...);

/* "ctrl" / "link" / "self" / "none", as pt.caps spells them. */
const char *PortTool_LoopName(porttool_loop_t loop);

/* How many complete lines the command port has taken in since reset.
 *
 * This is the RS232 session's evidence that the receive direction is carrying
 * real traffic and not just sitting idle: a number that climbs means bytes are
 * crossing the terminal, the MAX3221 and PC11 the whole time. */
uint32_t PortTool_LinesReceived(void);

/* Receive errors cleared on the command port since reset - overrun, framing or
 * noise. Reported by pt.id.
 *
 * Nonzero means at least one command was lost. It has to be visible: the board
 * keeps pushing frames either way, so a station with a deaf control channel
 * looks alive, and a plan step that timed out looks like a fault in the port it
 * was testing rather than in the cable carrying the request. */
uint32_t PortTool_RxErrors(void);

/* Clamps a requested sampling period to something the serial line can carry. */
uint32_t PortTool_ClampPeriod(uint32_t requested_ms);

/* Whether a port name belongs to a session. porttool_run.c asks so it can keep
 * a port's pt.run targets on the session's caps row instead of printing a
 * second row under the same name - see DECISIONS.md 17. */
int PortTool_IsSessionPort(const char *name);

/* Services lwIP, once the ethernet session has brought it up. Called every
 * superloop pass; does nothing in an image whose operator never started that
 * session. Lives in porttool_eth.c. */
void PortEth_Poll(void);

/* Hands the usb session bytes that arrived on the CDC pipe.
 *
 * Called from CDC_Receive_FS, which runs in USB interrupt context - so it
 * counts and scans and does nothing else. Frames are pushed from the tick.
 *
 * *** usbd_cdc_if.c calls this INSTEAD of IAP_data_recv under PORTTOOL_ENABLE.
 * *** That is not only about behaviour: the IAP call is what would link the
 * *** whole bootloader server into the tool image. Lives in porttool_usb.c. */
void PortUsb_Received(const uint8_t *data, uint32_t len);

#endif /* TESTCASE_PORTTOOL_PORTTOOL_H_ */
