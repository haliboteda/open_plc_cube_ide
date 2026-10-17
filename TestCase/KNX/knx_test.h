// knx_test.h
//
// The STKNX transceiver (U9 on the Upper Deck) as the port tool drives it: the
// knx session and the pt.run knx.* diagnostics. Nothing here belongs to the
// bootloader: no signature check, no IAP state, no KNX protocol stack.
//
// ---------------------------------------------------------------------------
// What the hardware actually is
// ---------------------------------------------------------------------------
// STKNX is a raw analogue KNX TP1 transceiver, not a TP-UART. It has no byte
// level host protocol at all; the MCU has to produce the TP1 bit timing
// itself. From the datasheet (DocID031327 Rev 1, section 5.1, page 17):
//
//   Bit period                    104 us  (9600 bit/s)
//   Logic 1 / idle                KNX_TX low for the whole 104 us
//   Logic 0 / active pulse        KNX_TX high 35 us, then low 69 us
//   KNX_RX                        high only while an active pulse is on the
//                                 bus, low during equalization and idle
//
// The receiver senses the bus, so it also sees the pulses this device sends
// itself (datasheet pin 2, KNX_AC: "Rx input and Tx feedback"). That makes a
// TX -> bus -> RX loopback the core measurement here.
//
// ---------------------------------------------------------------------------
// Pins and signal polarity, traced through the boards
// ---------------------------------------------------------------------------
// Sources: Production/Bridge/1436_01_SCHAE-BR.pdf page 5 (net label next to
// the MCU pin label), Production/UpperDeck/netlist.ipc (pin-level IPC-356
// netlist) and Production/UpperDeck/Schematics/OpenPLC_UpperDeck_R3.pdf p4.
//
//   KNX_TX       PB14   TIM12_CH1 / AF2
//   KNX_RX       PA10   TIM1_CH3  / AF1
//   KNX_OK       PD7    STKNX pin 21, bus power good
//   KNX_VCC_OK   PH12   STKNX pin 19, VCCCORE power good
//   KNX_Prog_LED PG11   heartbeat only
//   KNX_Prog_KEY PG9    SHARED WITH BOOT0 - this test never drives it
//
// TX chain, non-inverting end to end:
//   +3V3 -> U12 LED anode; LED cathode -> R88 332R -> PB14.  PB14 low lights
//   the LED, PB14 high leaves it dark.  U12 is a TLP2362: inverting, open
//   collector, LED on -> output low.  R94 10k to VCCCORE pulls the output up.
//   So PB14 high -> STKNX pin 24 high -> active pulse.  PB14 low -> idle.
//
//   *** PB14 must never be left floating: with no LED current the
//   output releases and R94 holds STKNX pin 24 high, i.e. the transceiver
//   sinks bus current continuously.  STKNX's own 6 uA pull-down on pin 24
//   cannot fight a 10k pull-up.  Park PB14 driven low.
//
// RX chain, non-inverting but with an undriven high level:
//   VCCCORE -> R89 332R -> U13 LED anode; LED cathode -> STKNX pin 23.  STKNX
//   pin 23 low lights the LED, so U13's open-collector output pulls PA10 low.
//   STKNX pin 23 high leaves the LED dark and the output released - and the
//   board carries NO pull-up on /KNX_RX, unlike the other three optocouplers
//   (R92/R93/R94).  The high level has to come from the MCU's internal
//   pull-up, so PA10 is always configured GPIO_PULLUP here.
//
// KNX_OK / KNX_VCC_OK, non-inverting end to end: STKNX output high -> Q6/Q7
// (PDTC114YU, NPN) conducts -> U10/U11 LED on -> open-collector output low ->
// U14/U15 (NL17SZ04, inverter) output high -> PD7 / PH12 high.
//
// ---------------------------------------------------------------------------
// Bus power
// ---------------------------------------------------------------------------
// STKNX draws all its power from the KNX bus (pin 11 KNX_A). With no bus
// supply the whole isolated side is dead and the MCU sees a clean signature:
//
//   bus powered, idle   PH12 HIGH   PD7 HIGH   PA10 LOW
//   no bus power        PH12 LOW    PD7 LOW    PA10 HIGH
//
// Read those three pins first: they tell bus-power problems apart from anything
// this file could be doing wrong.
//
// ---------------------------------------------------------------------------
// Known deviations from the STKNX / TLP2362 datasheets
// ---------------------------------------------------------------------------
// The STKNX external network matches datasheet Figure 3 (buck disabled) point
// for point, and CGATE 10 uF + CVDDHV 220 uF is exactly the Table 8 row for
// 30 mA fan-in. Four things do not match; software can reach two of them:
//
//   a) R88/R89/R90/R91 = 332 R from 3.3 V gives the TLP2362 LEDs ~5.4 mA
//      typical and ~4.3 mA worst case. TLP2362 recommends IF(ON) >= 7.5 mA
//      and only guarantees switching at IFHL <= 5.0 mA. Worst case is below
//      the guaranteed threshold. knx.hist's miss count is the symptom.
//      Note R89/R90/R91 are fed from VCCCORE, whose IREG limit is 20 mA, and
//      three of those LEDs are on simultaneously when idle - so this cannot
//      simply be fixed by lowering the resistors on the isolated side.
//   b) /KNX_RX has no board pull-up, unlike R92/R93/R94 on the other three
//      couplers. knx.pullup measures whether that matters.
//   c) C27 (CVDDHV) is 220 uF / 10 V where Table 2 requires >= 35 V.
//   d) C20 (CREF) is 470 nF / 10 V, same 35 V requirement. VREF's own
//      KNX_OK thresholds sit at 9.7 V .. 13.5 V.
//
//   (c) and (d) are voltage ratings on the isolated side. No amount of
//   firmware can see them; measure both by hand.
//
// ---------------------------------------------------------------------------
// What goes on the bus
// ---------------------------------------------------------------------------
// A lone TP1 character is invisible to ETS; a bus or group monitor only shows
// complete L_Data frames. KNX_Test_SessionSendGroupWrite() therefore sends a
// real GroupValueWrite:
//
//   ON    BC FF FA 08 01 E1 00 81 2F
//         |  |     |     |  |  |  `- check octet: XOR of all previous, inverted
//         |  |     |     |  |  `---- GroupValueWrite | 1-bit value
//         |  |     |     |  `------- TPCI: T_Data_Group
//         |  |     |     `---------- group address | hop count 6 | length 1
//         |  |     `---------------- destination 1/0/1
//         |  `---------------------- source 15.15.250
//         `------------------------- standard frame | not repeated |
//                                    broadcast | low priority
//
// Field values and the check-octet rule were taken from the OpenPLC_KNX
// reference stack, not from memory: knx/knx_types.h for the flag constants,
// knx/tp_frame.h for the octet order, knx/cemi_frame.cpp:205 for the check
// octet. The whole frame is handed to the bit engine in one piece so the
// inter-character gap stays at the two idle bits the encoding carries.
//
// Before transmitting it waits for the line to be quiet for KNX_ARB_IDLE_BITS
// bit periods; if the bus stays busy it says so and sends anyway rather than
// stalling.
//
// Received characters are reassembled the same way - characters exactly
// KNX_CHAR_BITS apart belong to one frame, octet 5's low nibble gives the
// length.
//
// KNX_OK and KNX_VCC_OK are sampled every bit period throughout.

// Every call below performs one step and returns, so the port tool can drive
// KNX from its command loop. The capture and bit-engine ISRs in knx_test.c are
// what actually keep the timing; a session only starts them and drains what
// they queued.

#ifndef TESTCASE_KNX_KNX_TEST_H_
#define TESTCASE_KNX_KNX_TEST_H_

#include <stdint.h>

typedef struct {
    uint8_t  bus;            /* 0 OK, 1 dead, 2 odd - knx_bus_t */
    uint8_t  vcc_ok;
    uint8_t  bus_ok;
    uint8_t  rx_idle;

    uint32_t pulses;         /* rising edges the capture ISR has seen */
    uint32_t dropped;        /* pulses the decoder was too slow to take */

    /* Active pulse width, microseconds. min/max keep their reset sentinels
     * until something is measured, so "nothing arrived" is distinguishable
     * from "measured zero". count says which. */
    uint32_t w_min, w_max, w_avg, w_count;

    /* Delay from our own transmit edge to the pulse that came back. */
    uint32_t d_min, d_max, d_avg, d_count;

    uint32_t ms_since_edge;
} knx_session_stats_t;

/* Longest frame the board assembles or sends, octets. A KNX standard frame can
 * carry up to 23; this is the buffer the bring-up path has always used, and a
 * longer frame is reported truncated rather than dropped. */
#define KNX_FRAME_MAX      16u

/* Which reading of a received frame's octets passed the check octet. Both are
 * reported because the RX chain's polarity is not settled on this board: the
 * octets as received may be the frame, or the frame may be their bit-inverse.
 * This is the machine's answer to "which reading is the real KNX frame". */
#define KNX_FRAME_CRC_BAD   0u   /* neither reading passes */
#define KNX_FRAME_CRC_RAW   1u   /* the octets as received are the frame */
#define KNX_FRAME_CRC_INV   2u   /* the frame is the octets bit-inverted */

/* A lone octet is an acknowledge, not a frame, and has no check octet. Judging
 * one as a bad frame would make every answered transmission look like a
 * failure - a real installation answers every frame it accepts. */
#define KNX_ACK_NONE        0u   /* not a valid acknowledge octet */
#define KNX_ACK_ACK         1u   /* L_Ack ACK - the frame was accepted */
#define KNX_ACK_NAK         2u
#define KNX_ACK_BUSY        3u

uint8_t KNX_Test_AckKind(uint8_t octet);

int  KNX_Test_SessionInit(void);
void KNX_Test_SessionStatsReset(void);

/* ⚠️ Spins for about 1.35 ms while the bit engine walks the character. */
int  KNX_Test_SessionSendChar(uint8_t b);

/* 1 when a character closed. Never blocks. */
int  KNX_Test_SessionPollChar(uint8_t *out, uint8_t *framing_ok);

/* Frame layer. PollFrame drains every queued character, assembles frames and
 * returns 1 once per completed frame; bad_chars counts the characters that
 * failed framing/parity during this call whether or not a frame closed.
 * Never blocks. Call FrameReset when a session starts. */
void     KNX_Test_SessionFrameReset(void);
int      KNX_Test_SessionPollFrame(uint8_t *out, uint8_t *out_len, uint8_t cap,
                                   uint8_t *which, uint8_t *bad_chars);
uint32_t KNX_Test_SessionPartialFrames(void);

/* Builds an L_Data_Standard GroupValueWrite (DPT 1.001) and puts it on the bus
 * as one frame, so the inter-character gap stays the two idle bits the
 * encoding carries. ⚠️ Spins for about 12 ms. Returns the octet count. */
uint8_t  KNX_Test_SessionSendGroupWrite(uint16_t src, uint16_t ga, uint8_t value,
                                        uint8_t *out, uint8_t cap,
                                        uint8_t *bus_was_idle);

void KNX_Test_SessionStats(knx_session_stats_t *out);

/* ---- Diagnostics, for pt.run knx.edges / knx.hist / knx.pullup ----------
 *
 * Per-edge timing, pulse-width / delay histograms and the pull-up comparison
 * (DECISIONS.md 87, PORTTOOL-FLOW.md A.3). Each brings the transceiver up on
 * first use, prints its detail as prose, and returns 0 without transmitting
 * when the bus has no power (bus_ok = 0). Not to be called while a knx
 * session is running: they share its timers and statistics. */

#define KNX_DIAG_BINS 8u    /* histogram cells; edges in knx_test.c */

typedef struct {
    uint8_t  bus_ok;
    uint32_t shots, moved, edges, dropped;
    int32_t  first_us;      /* mean time of the first KNX_RX edge after TX */
    int32_t  width_us;      /* mean time between the first two edges */
} knx_diag_edges_t;

typedef struct {
    uint8_t  bus_ok;
    uint32_t tx, rx, miss;
    uint32_t w_min, w_avg, w_max;     /* KNX_RX pulse width, us */
    uint32_t d_min, d_avg, d_max;     /* TX -> RX delay, us */
    uint32_t w_hist[KNX_DIAG_BINS];
    uint32_t d_hist[KNX_DIAG_BINS];
} knx_diag_hist_t;

typedef struct {
    uint8_t  bus_ok;
    uint32_t tx[2], rx[2], miss[2], w_avg[2];   /* [0] pull-up on, [1] off */
} knx_diag_pullup_t;

int KNX_Test_DiagEdges(uint32_t shots, knx_diag_edges_t *out);
int KNX_Test_DiagHist(uint32_t bursts, knx_diag_hist_t *out);
int KNX_Test_DiagPullup(knx_diag_pullup_t *out);

#endif /* TESTCASE_KNX_KNX_TEST_H_ */
