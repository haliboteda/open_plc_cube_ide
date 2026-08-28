// can_test.h
//
// Standalone bring-up test for the CAN interface (U8 on the Upper Deck), for
// the v0.1.3-testcase branch only. Nothing here belongs to the bootloader: no
// CANopen, no J1939, no CAN-based IAP. The single question it answers is
// whether this board can put a CAN frame on the wire and read one back.
//
// ---------------------------------------------------------------------------
// What the hardware actually is
// ---------------------------------------------------------------------------
// Sources: Hardware/STM32H743IIK6_GPIO_ASSIGNMENT_Schaeffer_Bridge_20260822.xlsx
// sheet GPIO_ASSIGNMENT rows 95-97; Hardware/Production/UpperDeck/netlist.ipc
// (pin-level IPC-356 netlist); Production/UpperDeck/Schematics/
// OpenPLC_UpperDeck_R3.pdf pages 1 and 5; the Upper Deck BOM.
//
//   FDCAN1_TX    PB9    AF9    BGA ball B4    net CAN_TXD_PB9
//   FDCAN1_RX    PI9    AF9    BGA ball D3    net CAN_RXD_PI9
//
// FDCAN2 is not usable on this board - every one of its pins is taken (PB12 =
// RMII_TXD0, PB5/PB6 = DIN2/DIN3, PB13 = HSFET_1). The other FDCAN1 alternates
// are taken too (PD0/PD1 = FMC, PA11/PA12 = USB FS, PH13/PH14 = UART4). PB9 +
// PI9 is the only CAN pin pair this board has.
//
// Transceiver: U8 = TI ISO1044BDR, an ISOLATED CAN FD transceiver, SOIC-8.
//
//   U8 pin 1  VCC1   +3V3            (logic side)
//   U8 pin 2  TXD    CAN_TXD_PB9
//   U8 pin 3  RXD    CAN_RXD_PI9
//   U8 pin 4  GND1   GNDD            (digital ground)
//   U8 pin 5  CANL   CAN_L
//   U8 pin 6  CANH   CAN_H
//   U8 pin 7  GND2   CAN_GND         (isolated, NOT common with GNDD)
//   U8 pin 8  VCC2   isolated 5 V from U7
//
// The bus side is powered by U7 = PDS1-S5-S5-M / MPB1205, an isolated 5V->5V
// 1 W DC/DC fed from +5V through L1 (6.8 uH). U7 IS MOUNTED ON THE BOTTOM SIDE
// of the Upper Deck. If U7 is not delivering, the transceiver cannot drive the
// bus at all and every frame fails as a bit error - see the diagnosis block.
//
// There is NO standby / enable / silent-mode pin. The ISO1044 has eight pins
// and all eight are accounted for above, so listen-only has to come from the
// FDCAN peripheral (FDCAN_MODE_BUS_MONITORING), not from hardware.
//
// There is NO LED anywhere on the CAN circuit, and NO test point on CAN_H or
// CAN_L. The only two test points are TP_CAN_VDD1 (isolated 5 V) and
// TP_CAN_GND1. A scope has to clip onto the J10 screw terminals.
//
// ---------------------------------------------------------------------------
// Terminals - and a documentation trap
// ---------------------------------------------------------------------------
//   CAN H       J10 pin 1     terminal C08 / A08
//   CAN L       J10 pin 2     terminal C07 / A07
//   CAN_GND     J11 pin 4     terminal      A09
//
// Two naming schemes are in circulation for the same terminals: the KiCad
// schematic page 1 calls the block A01..A12, Klemmblockzuordnung.pdf calls it
// C01..C12.
//
// TRAP: Klemmblockzuordnung.pdf page 4 (and UpperDeck_overview.txt:87-90) are
// WRONG from terminal 09 onwards - they list C09 as RS485 A and omit CAN_GND
// entirely, shifting RS485 up by one position. The schematic and the netlist
// agree with each other: A09 = CAN_GND, A10 = RS485 A, A11 = RS485 B. Wire
// CAN_GND from the netlist, not from that table.
//
// ---------------------------------------------------------------------------
// Termination
// ---------------------------------------------------------------------------
// R69 (120 R 1%) IS fitted, but it sits in series with JP7, a 2-pad
// SolderJumper_2_Open that ships OPEN - bare copper, nothing placed. So the
// board is UNTERMINATED as delivered. Terminating means bridging JP7 with
// solder, which is not reversible in any convenient way.
//
// The loopback phases do not need termination, and neither does a short
// (< 1 m) link to a USB-CAN analyser at 500 kbit/s. Do not solder JP7 until
// something actually points at reflections.
//
// R83 and R84 are 0R DNP parts that would defeat the isolation (R83 bridges
// the isolated 5 V to +3V3, R84 bridges CAN_GND to GNDD). Leave them unfitted.
//
// ---------------------------------------------------------------------------
// Clock and bit timing
// ---------------------------------------------------------------------------
// Nothing in this project calls HAL_RCCEx_PeriphCLKConfig for
// RCC_PERIPHCLK_FDCAN, so the kernel clock sits at its reset default. That
// default is FDCANSEL = 00 = hse_ck, verified on the target: D2CCIP1R reads
// 0x00000000 and RCC_FDCANCLKSOURCE_HSE is 0 in stm32h7xx_hal_rcc_ex.h:1442.
// This test selects HSE explicitly anyway, so the timing table below cannot be
// invalidated by someone changing that elsewhere.
//
// HSE is 25 MHz and already running, so
// no PLL work is needed and there is no collision with the PLL2 settings
// sd_test.c installs for SDMMC. And 25 MHz divides exactly for every standard
// bit rate with prescaler 1, so there is no rounding error to argue about:
//
//   rate     tq/bit   presc   Seg1   Seg2   SJW   sample point
//   125 k      200      1      174     25     4      87.5%
//   250 k      100      1       87     12     4      88%
//   500 k       50      1       43      6     4      88%     <- default
//   1 M         25      1       21      3     3      88%
//
// where 1 + Seg1 + Seg2 = tq/bit. All four fit the HAL limits (prescaler
// <= 512, Seg1 <= 256, Seg2 <= 128).
//
// DO NOT copy the numbers out of ref/Hello_World_OpenPLC/Core/Src/fdcan.c.
// That project runs FDCAN off PLL1Q at 80 MHz with prescaler 10 / Seg1 13 /
// Seg2 2; the same values here would be five times the intended bit rate. Its
// structure is worth borrowing, its constants are not.
//
// ---------------------------------------------------------------------------
// Test phases
// ---------------------------------------------------------------------------
// Output goes to printf (RS232 UART4 115200 and SWO), same as the other
// TestCase modules. Received frames are drained from RX FIFO0 into a ring and
// printed outside the drain loop: a frame at 500 kbit/s is about 110 us and a
// 60-character console line costs 5 ms at 115200 baud, so printing inside the
// loop would lose frames.
//
//   P0  Peripheral and configuration report. Reads the FDCAN core release and
//       endianness registers to prove the peripheral answers at all, prints
//       the kernel clock actually selected and the four bit-timing numbers,
//       and prints the wiring and JP7 reminders. Nothing is started.
//   P1  FDCAN_MODE_INTERNAL_LOOPBACK. Frames never reach the pins. This
//       proves the MCU peripheral, the message RAM layout and the bit-timing
//       arithmetic. If P1 fails the problem is firmware, and no amount of
//       probing the board will help - stop here.
//   P2  FDCAN_MODE_EXTERNAL_LOOPBACK. Frames are driven on the TX pin.
//       ⚠️ P2 DOES NOT PROVE THE PHYSICAL BUS. An earlier version of this
//       comment claimed the frame leaves PB9, crosses U8, goes out on
//       CAN_H/CAN_L and comes back in on PI9. That is wrong. In external loop
//       back mode the M_CAN feeds its own transmit output back to its receive
//       input internally and DISREGARDS the value of the RX pin, and it
//       ignores acknowledge errors so that it does not need a second node.
//       P2 passing therefore proves the peripheral, the message RAM and the
//       bit timing - the same things P1 proves - plus that frames are driven
//       on the TX pin. It says nothing about U8, U7, J8 or the wiring.
//       To prove the transmit path physically, put a scope on PB9: that is
//       what CAN_Test_Scope_Run() below is for.
//   P3  FDCAN_MODE_BUS_MONITORING. Listen only; TX is held recessive and
//       nothing is put on the bus. Shows real traffic from a USB-CAN analyser
//       or any other node without disturbing it.
//   P4  FDCAN_MODE_NORMAL. Sends and receives against a real second node.
//
// P4 has a single-node trap: in normal mode a frame needs another node to
// acknowledge it, so with nothing else on the bus the transmit error counter
// climbs until the peripheral goes bus-off. AutoRetransmission is therefore
// DISABLE, and P4 is only meaningful with the analyser connected. The TEC/REC
// readout after P4 says exactly which case you are in.
//
// The filter is a global accept-everything (HAL_FDCAN_ConfigGlobalFilter with
// FDCAN_ACCEPT_IN_RX_FIFO0), not an ID match. During bring-up the question is
// "did anything arrive", not "did the expected ID arrive".
//
// After each phase the protocol status register's last-error-code and the
// error counters are read and turned into "measure this point, here is what
// the reading means" - the same shape as the KNX diagnosis block.
//
// To run it, main.c calls CAN_Test_Run() right after MX_UART4_Init(), guarded
// by CAN_TEST_ENABLE. Runs forever (does not return).
//
// ---------------------------------------------------------------------------
// The soak mode - CAN_Test_Soak_Run(), guarded by CAN_SOAK_TEST_ENABLE
// ---------------------------------------------------------------------------
// One FDCAN_MODE_NORMAL session that never changes phase: print every frame
// that arrives, forever. It exists because the phase cycle above is hard to
// watch against a second node - the bus spends two thirds of each round in a
// mode that cannot answer, and P3 in particular makes an unacknowledged
// analyser retransmit one frame endlessly.
//
// It receives only. CAN_SOAK_TX_ENABLE in can_test.c turns transmitting on.
// The mode is normal rather than bus monitoring even while receive-only,
// because normal mode acknowledges: bus monitoring would leave the sender
// retransmitting the same frame forever, which is what floods the console.
// A status line every 3 s carries TEC/REC and the receive FIFO level, so the
// board is visibly alive even when the bus is silent.
//
// It recovers from bus-off by itself. With no second node acknowledging, the
// transmit error counter gains 8 per frame and reaches 256 after 32 of them,
// so at one frame per 3 s the peripheral would go bus-off after about 96 s and
// then stay silent forever. The loop reads the protocol status after each
// transmit and re-initialises FDCAN1 when the bus-off flag is set, counting
// how often that happened - a climbing recovery count is itself the readout
// that nobody is acknowledging.
//
// Each transmit line carries TEC/REC, so the acknowledge state is visible
// without a separate status dump: TEC that stays at 0 means a second node is
// acknowledging, TEC that climbs in steps of 8 means nobody is.

// ---------------------------------------------------------------------------
// The scope run - CAN_Test_Scope_Run(), guarded by CAN_SCOPE_TEST_ENABLE
// ---------------------------------------------------------------------------
// For answering "is anything at all coming out of PB9" with an oscilloscope.
// Three steps repeat forever, each printing what to expect before it runs:
//
//   [1] PB9 as a plain GPIO output, 500 Hz square wave, 5 s. No CAN peripheral
//       involved. Nothing on the scope here means the fault is between the MCU
//       ball and the probe point.
//   [2] EXTERNAL_LOOPBACK, frames queued back to back, 10 s. Loop back ignores
//       acknowledge errors, so this transmits continuously with no second node
//       and never reaches bus-off.
//   [3] NORMAL with AutoRetransmission ENABLED, back to back, 10 s. On the real
//       bus. Traffic in [2] but not in [3] means the peripheral is fine and
//       something on the bus is holding the line.
//
// Why back to back rather than the soak mode's one frame per second: a frame
// is about 110 us, so one per second leaves the pin busy 0.011% of the time.
// On a normal timebase that reads as a flat line no matter how healthy the
// hardware is.

// ---------------------------------------------------------------------------
// The echo run - CAN_Test_Echo_Run(), guarded by CAN_ECHO_TEST_ENABLE
// ---------------------------------------------------------------------------
// Transmits nothing on its own. Every frame that arrives is printed, its
// payload is incremented as ONE big-endian number, and the result is sent back
// on the SAME identifier with the same length. So 0x05 comes back as 0x06, and
// 00 FF comes back as 01 00 - the carry crosses byte boundaries rather than
// each byte wrapping on its own.
//
// Remote frames and zero-length frames are printed but not answered: there is
// no payload to increment.
//
// The mode is FDCAN_MODE_NORMAL, so arriving frames are acknowledged. A status
// line every 5 s carries the receive and reply counts plus TEC/REC, which is
// what tells you the board is alive while the bus is quiet.
//
// The host side is $TOOL/TestCase/tools/can_send.py.

#ifndef INC_CAN_TEST_H_
#define INC_CAN_TEST_H_

void CAN_Test_Run(void);
void CAN_Test_Soak_Run(void);
void CAN_Test_Scope_Run(void);
void CAN_Test_Echo_Run(void);

#endif /* INC_CAN_TEST_H_ */
