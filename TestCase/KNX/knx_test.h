// knx_test.h
//
// Standalone bring-up test for the STKNX transceiver (U9 on the Upper Deck),
// for the v0.1.3-testcase branch only. Nothing here belongs to the bootloader:
// no signature check, no IAP state, no KNX protocol stack.
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
// TX -> bus -> RX loopback the core test here.
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
//   PB14 must therefore never be left floating: with no LED current the
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
// MEASURED 2026-08-26: PA10 sits HIGH while the bus is idle, i.e. the opposite
// of the first line. That is a board fault, not a firmware one, and it is with
// the hardware engineer. Nothing in this file works until PA10 idles LOW, so
// KNX bring-up is PARKED - leave the code as it is and do not re-test.
//
// ---------------------------------------------------------------------------
// Known deviations from the STKNX / TLP2362 datasheets
// ---------------------------------------------------------------------------
// The STKNX external network matches datasheet Figure 3 (buck disabled) point
// for point, and CGATE 10 uF + CVDDHV 220 uF is exactly the Table 8 row for
// 30 mA fan-in. Four things do not match, and the test is built to expose the
// two that software can reach:
//
//   a) R88/R89/R90/R91 = 332 R from 3.3 V gives the TLP2362 LEDs ~5.4 mA
//      typical and ~4.3 mA worst case. TLP2362 recommends IF(ON) >= 7.5 mA
//      and only guarantees switching at IFHL <= 5.0 mA. Worst case is below
//      the guaranteed threshold. P2's lost-pulse count is the symptom.
//      Note R89/R90/R91 are fed from VCCCORE, whose IREG limit is 20 mA, and
//      three of those LEDs are on simultaneously when idle - so this cannot
//      simply be fixed by lowering the resistors on the isolated side.
//   b) /KNX_RX has no board pull-up, unlike R92/R93/R94 on the other three
//      couplers. P5 measures whether that matters.
//   c) C27 (CVDDHV) is 220 uF / 10 V where Table 2 requires >= 35 V.
//   d) C20 (CREF) is 470 nF / 10 V, same 35 V requirement. VREF's own
//      KNX_OK thresholds sit at 9.7 V .. 13.5 V.
//
//   (c) and (d) are voltage ratings on the isolated side. No amount of
//   firmware can see them; the diagnosis block prints a standing reminder to
//   measure both by hand.
//
// ---------------------------------------------------------------------------
// Test phases
// ---------------------------------------------------------------------------
// Output goes to printf (RS232 UART4 115200 and SWO). P0 runs once, then
// P1..P5 plus the diagnosis loop forever.
//
// ---------------------------------------------------------------------------
// Receive-only bring-up (KNX_TX_ENABLE = 0, the current default)
// ---------------------------------------------------------------------------
// While the hardware is being debugged the slim loop transmits nothing at all,
// so the bus carries only foreign traffic. The bit engine still runs with CCR1
// held at 0, which keeps PB14 driven low - the one thing that must not change,
// see the TX chain note above.
//
// Printing is deliberately not gated on anything being valid. Octets collect
// in a burst buffer until the line has been quiet for KNX_RX_FLUSH_MS, then
// the whole burst prints raw, bit-inverted, and with both readings matched
// against the KNX services; whatever does not match reads "null":
//
//   [   12345 ms]  BUS  9 octets
//      raw: BC FF FA 08 01 E1 00 81 2F
//        -> L_Data_Standard prio=low  15.15.250 -> 1/0/1  hop=6 len=1
//           T_Data_Group  GroupValueWrite = 1
//      inv: 43 00 05 F7 FE 1E FF 7E D0
//        -> null
//
// A '!' after an octet means its start, parity or stop bit was wrong; the
// octet is printed anyway. Lone pulses that never became a character are
// counted and reported too, so "nothing on the wire" and "something on the
// wire that will not decode" never look alike on the console.
//
// Expect the inverted line to read "null" almost always: ~0xBC is 0x43, which
// matches no control field. The comparison earns its place the other way
// round - a raw line reading null while the inverted one decodes cleanly means
// the whole RX chain is inverted.
//
// Collecting before printing is not cosmetic. A 60-character console line
// costs 5 ms at 115200 baud, which is 48 bit periods, so printing per
// character would starve the decoder and destroy the data being looked for.
//
// ---------------------------------------------------------------------------
// What goes on the bus (KNX_TX_ENABLE = 1)
// ---------------------------------------------------------------------------
// A lone TP1 character is invisible to ETS; a bus or group monitor only shows
// complete L_Data frames. So the test sends a real GroupValueWrite, toggling
// between OFF and ON on KNX_GROUP_ADDR every KNX_TX_EVERY_MS:
//
//   OFF   BC FF FA 08 01 E1 00 80 2E
//   ON    BC FF FA 08 01 E1 00 81 2F
//         |  |     |     |  |  |  `- check octet: XOR of all previous, inverted
//         |  |     |     |  |  `---- GroupValueWrite | 1-bit value
//         |  |     |     |  `------- TPCI: T_Data_Group
//         |  |     |     `---------- group address | hop count 6 | length 1
//         |  |     `---------------- destination 1/0/1
//         |  `---------------------- source 15.15.250, an address no real
//         |                          device is programmed with
//         `------------------------- standard frame | not repeated |
//                                    broadcast | low priority
//
// Field values and the check-octet rule were taken from the OpenPLC_KNX
// reference stack, not from memory: knx/knx_types.h for the flag constants,
// knx/tp_frame.h for the octet order, knx/cemi_frame.cpp:205 for the check
// octet. The whole frame is handed to the bit engine in one piece so the
// inter-character gap stays at the two idle bits the encoding carries.
//
// Before transmitting, the test waits for the line to be quiet for
// KNX_ARB_IDLE_BITS bit periods; if the bus stays busy it says so and sends
// anyway rather than stalling.
//
// Received characters are reassembled the same way - characters exactly
// KNX_CHAR_BITS apart belong to one frame, octet 5's low nibble gives the
// length - and each complete frame prints as raw octets plus the decoded
// source, destination and service. Nothing else prints.
//
// ---------------------------------------------------------------------------
//
// KNX_SLIM_MODE = 1 (the default) skips P1..P5 entirely and runs one loop.
// With KNX_TX_ENABLE = 1 it also sends one frame every KNX_TX_EVERY_MS.
// The cadence never breaks and listening never stops, which is what you want
// while operating real devices on the bus and watching what arrives. Each
// decoded character is tagged (own loopback) or (FROM BUS) by whether it
// landed within KNX_LOOPBACK_MS of a transmit. A compact status line prints
// every KNX_SLIM_REPORT_S, and the full diagnosis fires once, on the first
// report that shows no loopback at all - not on a timer, because its text
// costs seconds of console time during which traffic would be missed.
//
// Set KNX_SLIM_MODE to 0 for the full P1..P5 round below, which adds per-pulse
// loss statistics, histograms and the pull-up A/B test at the cost of a 20 s
// silent window per round.
//
// In the full round, everything the test transmits happens once every
// KNX_TX_EVERY_MS (5 s) and prints one line, so a live bus stays usable and
// the console stays readable. Receive printing is real time, but bounded -
// see P4.
//
//   P0  Bus and pin self-check against the signature above. Waits, reporting
//       every change, until the bus comes up. While the bus is down the round
//       is skipped entirely and the test holds in a one-pulse-a-second poll.
//   P1  KNX_P1_SHOTS single 35 us pulses, one every 5 s. TIM1_CH3 captures the
//       KNX_RX response and every edge is printed with its offset in
//       microseconds from the TX rising edge.
//   P2  KNX_P2_BURSTS bursts of KNX_P2_BURST_PULSES back-to-back pulses, one
//       burst every 5 s. Reports the per-pulse loss count (the update ISR
//       judges each pulse against the capture ISR), plus histograms of
//       KNX_RX pulse width and TX->RX delay. A 200-pulse burst is 21 ms of
//       dominant state, short enough not to jam a working bus.
//   P3  Sends a real TP1 character (start + 8 data LSB first + even parity +
//       stop) and decodes the loopback. Prints PASS/FAIL per test byte.
//   P4  Passive listen with TX parked low. The decode loop runs flat out and
//       never prints - a 60-character console line costs 5 ms at 115200 baud,
//       which is 48 bit periods, and the pulse ring only holds 64 entries, so
//       printing inside the loop starves the decoder and turns real traffic
//       into a flood of lone start bits. Finished frames go through a result
//       ring instead: well-formed characters print immediately (capped at
//       KNX_P4_CHARS_PER_S), lone pulses and malformed frames are counted and
//       summarised once a second, raw edges appear only while nothing
//       decodes. Any pulse lost to console back-pressure is reported as such,
//       so it is never mistaken for a bus fault.
//   P5  Pull-up A/B: one burst with PA10's internal pull-up on and one with it
//       off, comparing loss and pulse width.
//
//   Then a diagnosis block classifies the round and prints, for whatever it
//   found, the exact points to measure and what each reading means. The two
//   phases feeding the most important distinction are P3 and P4: P3 passing
//   while P4 hears nothing means the bus is simply quiet, not broken.
//
// KNX_OK and KNX_VCC_OK are sampled every bit period throughout, so any
// transition during transmission is logged with a microsecond timestamp.
//
// To run it, main.c calls KNX_Test_Run() right after MX_UART4_Init(), guarded
// by KNX_TEST_ENABLE. Runs forever (does not return).

#ifndef TESTCASE_KNX_KNX_TEST_H_
#define TESTCASE_KNX_KNX_TEST_H_

void KNX_Test_Run(void);

#endif /* TESTCASE_KNX_KNX_TEST_H_ */
