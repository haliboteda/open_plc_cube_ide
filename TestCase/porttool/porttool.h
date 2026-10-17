// porttool.h
//
// Port test tool: an instrument panel for the terminals, driven from a PC over
// the RS232 log port. Pick a port, set its parameters, start it, and it keeps
// reporting until stopped - the hardware engineer watches while probing.
//
// The board only samples; the PC makes every verdict. main.c calls
// PortTool_Run() behind PORTTOOL_ENABLE and it never returns, so the
// bootloader's normal path never runs.
//
// One serial line carries pushed sample frames ("!din t=48213 v=0x16 ...", a
// leading "!") and the printf log for a person.
//
// Commands (one per line, echoed nowhere - the reply is the answer):
//
//   pt.caps                    what this firmware exposes
//   pt.id                      board identity
//   pt.cal                     the calibration area's bytes, read-only
//   pt.calw  <112 hex digits>  write the calibration area into sector 15
//   pt.list                    which sessions are running
//   pt.start <port> [k=v ...]  start one
//   pt.set   <port> k=v ...    change parameters while it runs
//   pt.stop  <port> | all      stop
//   pt.echo  <port> <n>        close the loopback count for one frame
//   pt.hold  [<ms>]            renew the deadman, 0 disarms, bare word queries
//   pt.led   fault=0|1         drive the indicator from the PC
//   pt.run   [<target>]        list one-shot actions, or perform one
//   pt.aodiag exp=E0..E6 ...   TEMPORARY AO noise experiment, not in pt.caps
//
// pt.hold is the one thing the board decides on its own: whether the PC is
// still there, so a dead PC does not leave outputs driven (DECISIONS.md 37).

#ifndef TESTCASE_PORTTOOL_PORTTOOL_H_
#define TESTCASE_PORTTOOL_PORTTOOL_H_

#include <stdint.h>

/* Every porttool_*.c wraps its whole body in this, so an image built without it
 * carries none of this code: ports[] in porttool.c references every session,
 * so --gc-sections alone could not drop them. Set PORTTOOL_ENABLE=1 to build
 * the fixture image (tools/build_fixture.py in $PORTTOOL). */
#ifndef PORTTOOL_ENABLE
#define PORTTOOL_ENABLE 0
#endif

/* 115200 8N1 is about 11.5 KB/s and printf blocks, so a session that samples
 * too fast stalls the loop it lives in. */
#define PORTTOOL_PERIOD_MIN_MS   50U
#define PORTTOOL_PERIOD_DEF_MS   200U
/* LINE_MAX is one line from the PC; 256 fits six different relay values
 * (decision 123). REPLY_MAX is every line the board sends. PORTTOOL-FLOW.md A.3. */
#define PORTTOOL_LINE_MAX        256U
#define PORTTOOL_REPLY_MAX       192U

/* Channel sets are a uint32_t bitmask. */
#define PORTTOOL_MAX_CHANNELS    32U

/* The product's four boards plus "whole" (OPLC-SPEC-HW-001 section 1). Protocol
 * values: English lowercase; the panel translates them and shows an unknown
 * one verbatim. */
#define PORTTOOL_BOARD_BRIDGE   "bridge"    /* CPU, SDRAM, Ethernet, USB, SD, RTC */
#define PORTTOOL_BOARD_UPPER    "upper"     /* Upper Deck: DI, AI, AO, CAN, RS485, RS232, KNX */
#define PORTTOOL_BOARD_LOWER    "lower"     /* Lower Deck: high-side out, relays, temperature */
#define PORTTOOL_BOARD_JUNCTION "junction"  /* Junction Link: 24 V in, rails, DIN rail connector */
#define PORTTOOL_BOARD_WHOLE    "whole"     /* spans boards - bringup does */

/* noreturn lets --gc-sections drop everything main() calls after this (BOOT0
 * gesture, IAP server, USB stack, mbedTLS). lwIP stays: porttool_eth.c calls
 * MX_LWIP_Init() itself (DECISIONS.md 28). */
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
    PORTTOOL_LOOP_SELF,      /* the port under test IS the control port (rs232
                                only), so the round trip is its verdict
                                (DECISIONS.md 16) */
} porttool_loop_t;

/* The echo counter one session keeps: the board sends a number, the PC sends
 * it back, the board counts on from what it got. */
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
 * A matching reply advances seq and clears miss; anything else holds seq and
 * counts a miss, so a dead loop shows as a stalled counter. */
void PortTool_EchoTick(porttool_echo_t *e);

/* Writes "seq=<n> rx=<n> miss=<n>" and returns how many characters. No leading
 * space: these come first in a frame body, and a caller appending anything
 * else adds its own separator. */
uint32_t PortTool_EchoFields(const porttool_echo_t *e, char *out, uint32_t out_len);

/* Where a decimal scan of a link's bytes has got to; kept by the caller across
 * calls, so a number split over two reads still comes out whole. */
typedef struct {
    uint32_t acc;
    int      in_number;
} porttool_numscan_t;

/* Reads back the numbers the peer echoed on a loop=link port: decimal runs,
 * any other byte ends one, each finished number goes to PortTool_EchoGot.
 * Counters only, no printf: the USB session calls it in interrupt context. */
void PortTool_EchoScan(porttool_echo_t *e, porttool_numscan_t *s,
                       const uint8_t *data, uint32_t len);

/* CRC-32 (IEEE, zlib-compatible) over n more bytes. Start from 0xFFFFFFFF and
 * invert the result. */
uint32_t PortTool_Crc32Update(uint32_t crc, const uint8_t *p, uint32_t n);

/* One testable port. Each porttool_<name>.c defines one and the table in
 * porttool.c lists it. The const fields are what pt.caps reports, so the panel
 * is built from the firmware, not from a copy. */
typedef struct {
    const char *name;
    const char *board;   /* PORTTOOL_BOARD_*: where the port physically lives */

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
     * Only loop=ctrl ports have one: a loop=link port must take its reply off
     * the link under test, or its counter would climb while that link is dead. */
    void (*echo)(uint32_t value);

    /* The port's current parameter values for pt.caps, as "k=v k=v". */
    void (*caps)(char *out, uint32_t out_len);

    /* What each parameter accepts, as "name:spec name:spec" (lo..hi, lo..,
     * a|b|c, 1..n; PORTTOOL-FLOW.md A.4). Optional. Build it with snprintf from
     * the same constants apply() checks, so the two cannot drift. Its own caps
     * line because the port= line would overflow PORTTOOL_REPLY_MAX. */
    void (*limits)(char *out, uint32_t out_len);

    /* Read-only hardware facts for pt.caps, as "k=v k=v" on an "OK info="
     * line - not parameters, pt.set cannot change them. Optional.
     * See PORTTOOL-FLOW.md A.4. */
    void (*info)(char *out, uint32_t out_len);

    int running;
} porttool_port_t;

/* Pushes one sample frame: "!<port> t=<ms> " then the formatted body. */
void PortTool_Frame(const char *port, const char *fmt, ...);

/* "ctrl" / "link" / "self" / "none", as pt.caps spells them. */
const char *PortTool_LoopName(porttool_loop_t loop);

/* Complete lines the command port has taken in since reset: the rs232
 * session's evidence that the receive direction carries traffic. */
uint32_t PortTool_LinesReceived(void);

/* Receive errors (overrun, framing, noise) cleared on the command port since
 * reset, reported by pt.id. Nonzero means a command was lost, which would
 * otherwise look like a fault in the port under test. */
uint32_t PortTool_RxErrors(void);

/* Clamps a requested sampling period to something the serial line can carry. */
uint32_t PortTool_ClampPeriod(uint32_t requested_ms);

/* The control line's own rate (decision 104, PORTTOOL-FLOW.md B.7).
 *
 * The rs232 session asks for a rate; the reply to the command that asked ends
 * in " line=<baud>" and the switch happens only after that reply has gone out,
 * so the PC reads it at the rate it is listening on. Frames are held until a
 * byte arrives at the new rate, and 3 s without any byte switches back. */
#define PORTTOOL_LINE_DEFAULT_BAUD 115200U
#define PORTTOOL_LINE_SILENCE_MS   3000U

/* Asks for a rate, applied after the current command's reply. */
void PortTool_LineRequest(uint32_t baud);

/* The rate the line runs at now. */
uint32_t PortTool_LineBaud(void);

/* Whether a port name belongs to a session, so porttool_run.c can put that
 * port's pt.run targets on the session's caps row (DECISIONS.md 17). */
int PortTool_IsSessionPort(const char *name);

/* Whether that port's session is running; 0 for a name that is not a session
 * port. A pt.run target asks before re-muxing pins a live session owns. */
int PortTool_PortRunning(const char *name);

/* Services lwIP once the ethernet session has brought it up; called every
 * superloop pass. Lives in porttool_eth.c. */
void PortEth_Poll(void);

/* Hands the usb session bytes from the CDC pipe. Runs in USB interrupt context
 * (CDC_Receive_FS), so it only counts and scans; frames go out from the tick.
 * usbd_cdc_if.c calls this instead of IAP_data_recv under PORTTOOL_ENABLE, so
 * the IAP server is not linked in (DECISIONS.md 31). Lives in
 * porttool_usb.c. */
void PortUsb_Received(const uint8_t *data, uint32_t len);


/* ---- SD file transfer (porttool_sdfile.c, PORTTOOL-FLOW.md B.11) -------
 *
 * The eth and usb sessions in mode=file hand received bytes to
 * PortSdFile_Rx and call PortSdFile_Pump from their tick with a sender for
 * their own link. A sender returns how many bytes it accepted: all or none. */
typedef uint32_t (*sdf_send_t)(const uint8_t *data, uint32_t len);

void PortSdFile_Rx(const uint8_t *data, uint32_t len);
void PortSdFile_Pump(sdf_send_t send, uint32_t now_ms);
void PortSdFile_Abort(const char *why);
void PortSdFile_RunPut(const char *args, char *out, uint32_t len);
void PortSdFile_RunGet(const char *args, char *out, uint32_t len);
void PortSdFile_RunList(const char *args, char *out, uint32_t len);
void PortSdFile_RunRemove(const char *args, char *out, uint32_t len);
void PortSdFile_RunEject(const char *args, char *out, uint32_t len);

#endif /* TESTCASE_PORTTOOL_PORTTOOL_H_ */
