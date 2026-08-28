// can_test.c - see can_test.h for the hardware facts this is built on.

/* The FDCAN HAL driver is a private copy in TestCase/common, not a project-wide
 * module - see testcase_hal_guard.h. This define must precede main.h, which is
 * what pulls in stm32h7xx_hal.h and conditionally declares FDCAN_HandleTypeDef
 * and the HAL_FDCAN_* prototypes. */
#include "testcase_hal_guard.h"   /* fires if this peripheral becomes real -- read it */
#define HAL_FDCAN_MODULE_ENABLED

#include "can_test.h"
#include "main.h"
#include <stdio.h>
#include <inttypes.h>

/* --- Pins, Hardware/Production/UpperDeck/netlist.ipc:260-261 -------------- */

#define CAN_TX_PORT            GPIOB
#define CAN_TX_PIN             GPIO_PIN_9    /* FDCAN1_TX -> U8 pin 2 */
#define CAN_RX_PORT            GPIOI
#define CAN_RX_PIN             GPIO_PIN_9    /* FDCAN1_RX <- U8 pin 3 */
#define CAN_PIN_AF             GPIO_AF9_FDCAN1

/* --- Bit timing --------------------------------------------------------- */

/* Valid only while the kernel clock is HSE. 1 + Seg1 + Seg2 = tq per bit, and
 * HSE / (prescaler * tq) is the bit rate exactly, with no rounding. */
typedef struct {
	uint32_t bps;
	uint16_t prescaler;
	uint16_t seg1;
	uint16_t seg2;
	uint16_t sjw;
} can_timing_t;

static const can_timing_t CAN_TIMINGS[] = {
	{  125000u, 1u, 174u, 25u, 4u },
	{  250000u, 1u,  87u, 12u, 4u },
	{  500000u, 1u,  43u,  6u, 4u },
	{ 1000000u, 1u,  21u,  3u, 3u },
};

#define CAN_RATE_INDEX          2u   /* 500 kbit/s */

/* --- Phase sizing ------------------------------------------------------- */

#define CAN_FRAMES_PER_LOOPBACK  8u
#define CAN_RX_WAIT_MS          20u   /* a loopback frame is back in ~1 ms */
#define CAN_LISTEN_MS        20000u   /* P3 */
#define CAN_NORMAL_MS        20000u   /* P4 */
#define CAN_NORMAL_EVERY_MS   1000u   /* P4 transmit cadence */
#define CAN_ROUND_GAP_MS      2000u

/* --- Message RAM -------------------------------------------------------- */

/* 32 frames of slack between the bus and the console: a frame at 500 kbit/s is
 * about 110 us and one console line costs 5 ms at 115200 baud, so the FIFO is
 * what keeps a burst from being lost while printing. HAL caps FIFO0 at 64
 * elements and TxBuffers + TxFifoQueue at 32. */
#define CAN_RX_FIFO_ELMTS       32u
#define CAN_TX_FIFO_ELMTS        8u

#define CAN_TEST_ID          0x123u
#define CAN_ENDN_EXPECT   0x87654321u   /* FDCAN_ENDN reads this when alive */

static FDCAN_HandleTypeDef s_h;
static uint8_t             s_rate = CAN_RATE_INDEX;

/* can_open() reads this. DISABLE is what every phase of CAN_Test_Run() wants:
 * a lone node gets no acknowledge, and retrying would bury the symptom. The
 * scope run turns it on to keep the TX pin busy. */
static FunctionalState     s_auto_retx = DISABLE;

/* --- Small helpers ------------------------------------------------------ */

static uint32_t can_dlc(uint8_t len)
{
	static const uint32_t DLC[9] = {
		FDCAN_DLC_BYTES_0, FDCAN_DLC_BYTES_1, FDCAN_DLC_BYTES_2,
		FDCAN_DLC_BYTES_3, FDCAN_DLC_BYTES_4, FDCAN_DLC_BYTES_5,
		FDCAN_DLC_BYTES_6, FDCAN_DLC_BYTES_7, FDCAN_DLC_BYTES_8
	};
	return DLC[(len > 8u) ? 8u : len];
}

static uint8_t can_dlc_to_len(uint32_t dlc)
{
	return (uint8_t) ((dlc > FDCAN_DLC_BYTES_8) ? 8u : dlc);
}

static const char *can_clk_name(void)
{
	switch (__HAL_RCC_GET_FDCAN_SOURCE()) {
	case RCC_FDCANCLKSOURCE_HSE:  return "HSE";
	case RCC_FDCANCLKSOURCE_PLL:  return "PLL1Q";
	case RCC_FDCANCLKSOURCE_PLL2: return "PLL2Q";
	default:                      return "unknown";
	}
}

/* Only HSE is ever selected here. The other two report 0 on purpose, so a
 * wrong selection shows up as an obviously bogus rate instead of a plausible
 * one. */
static uint32_t can_clk_hz(void)
{
	return (__HAL_RCC_GET_FDCAN_SOURCE() == RCC_FDCANCLKSOURCE_HSE)
	     ? (uint32_t) HSE_VALUE : 0u;
}

static const char *can_mode_name(uint32_t mode)
{
	switch (mode) {
	case FDCAN_MODE_NORMAL:            return "NORMAL";
	case FDCAN_MODE_BUS_MONITORING:    return "BUS_MONITORING (listen only)";
	case FDCAN_MODE_INTERNAL_LOOPBACK: return "INTERNAL_LOOPBACK";
	case FDCAN_MODE_EXTERNAL_LOOPBACK: return "EXTERNAL_LOOPBACK";
	default:                           return "?";
	}
}

static const char *can_lec_name(uint32_t lec)
{
	switch (lec) {
	case FDCAN_PROTOCOL_ERROR_NONE:      return "none";
	case FDCAN_PROTOCOL_ERROR_STUFF:     return "STUFF";
	case FDCAN_PROTOCOL_ERROR_FORM:      return "FORM";
	case FDCAN_PROTOCOL_ERROR_ACK:       return "ACK";
	case FDCAN_PROTOCOL_ERROR_BIT1:      return "BIT1 (sent recessive, read dominant)";
	case FDCAN_PROTOCOL_ERROR_BIT0:      return "BIT0 (sent dominant, read recessive)";
	case FDCAN_PROTOCOL_ERROR_CRC:       return "CRC";
	case FDCAN_PROTOCOL_ERROR_NO_CHANGE: return "no change since last read";
	default:                             return "?";
	}
}

static const char *can_activity_name(uint32_t act)
{
	switch (act) {
	case FDCAN_COM_STATE_SYNC: return "synchronizing";
	case FDCAN_COM_STATE_IDLE: return "idle";
	case FDCAN_COM_STATE_RX:   return "receiving";
	case FDCAN_COM_STATE_TX:   return "transmitting";
	default:                   return "?";
	}
}

/* --- Clock and pins ----------------------------------------------------- */

/* Nothing in this project configures the FDCAN kernel clock, so it sits at its
 * reset default (FDCANSEL = 00 = PLL1Q, which SystemClock_Config leaves at
 * 400 MHz). Select HSE before touching the peripheral. */
static uint8_t can_clock_init(void)
{
	RCC_PeriphCLKInitTypeDef p = { 0 };

	p.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
	p.FdcanClockSelection  = RCC_FDCANCLKSOURCE_HSE;
	if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK) {
		printf("** CAN: FDCAN kernel clock select FAILED\r\n");
		return 0u;
	}
	__HAL_RCC_FDCAN_CLK_ENABLE();
	return 1u;
}

static void can_gpio_init(void)
{
	GPIO_InitTypeDef g = { 0 };

	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();

	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_NOPULL;
	g.Speed     = GPIO_SPEED_FREQ_LOW;
	g.Alternate = CAN_PIN_AF;

	g.Pin = CAN_TX_PIN;
	HAL_GPIO_Init(CAN_TX_PORT, &g);
	g.Pin = CAN_RX_PIN;
	HAL_GPIO_Init(CAN_RX_PORT, &g);
}

/* --- Peripheral open / close -------------------------------------------- */

static HAL_StatusTypeDef can_open(uint32_t mode)
{
	const can_timing_t *t = &CAN_TIMINGS[s_rate];

	s_h.Instance                  = FDCAN1;
	s_h.Init.FrameFormat          = FDCAN_FRAME_CLASSIC;
	s_h.Init.Mode                 = mode;
	s_h.Init.AutoRetransmission   = s_auto_retx;
	s_h.Init.TransmitPause        = DISABLE;
	s_h.Init.ProtocolException    = DISABLE;
	s_h.Init.NominalPrescaler     = t->prescaler;
	s_h.Init.NominalSyncJumpWidth = t->sjw;
	s_h.Init.NominalTimeSeg1      = t->seg1;
	s_h.Init.NominalTimeSeg2      = t->seg2;
	s_h.Init.DataPrescaler        = 1u;   /* classic CAN: data phase unused */
	s_h.Init.DataSyncJumpWidth    = 1u;
	s_h.Init.DataTimeSeg1         = 1u;
	s_h.Init.DataTimeSeg2         = 1u;
	s_h.Init.MessageRAMOffset     = 0u;
	s_h.Init.StdFiltersNbr        = 0u;   /* the global filter takes everything */
	s_h.Init.ExtFiltersNbr        = 0u;
	s_h.Init.RxFifo0ElmtsNbr      = CAN_RX_FIFO_ELMTS;
	s_h.Init.RxFifo0ElmtSize      = FDCAN_DATA_BYTES_8;
	s_h.Init.RxFifo1ElmtsNbr      = 0u;
	s_h.Init.RxFifo1ElmtSize      = FDCAN_DATA_BYTES_8;
	s_h.Init.RxBuffersNbr         = 0u;
	s_h.Init.RxBufferSize         = FDCAN_DATA_BYTES_8;
	s_h.Init.TxEventsNbr          = 0u;
	s_h.Init.TxBuffersNbr         = 0u;
	s_h.Init.TxFifoQueueElmtsNbr  = CAN_TX_FIFO_ELMTS;
	s_h.Init.TxFifoQueueMode      = FDCAN_TX_FIFO_OPERATION;
	s_h.Init.TxElmtSize           = FDCAN_DATA_BYTES_8;

	if (HAL_FDCAN_Init(&s_h) != HAL_OK) {
		printf("   HAL_FDCAN_Init FAILED\r\n");
		return HAL_ERROR;
	}

	/* No ID filter at all: during bring-up the question is whether anything
	 * arrived, not whether the expected ID arrived. */
	if (HAL_FDCAN_ConfigGlobalFilter(&s_h, FDCAN_ACCEPT_IN_RX_FIFO0,
	                                 FDCAN_ACCEPT_IN_RX_FIFO0,
	                                 FDCAN_FILTER_REMOTE,
	                                 FDCAN_FILTER_REMOTE) != HAL_OK) {
		printf("   HAL_FDCAN_ConfigGlobalFilter FAILED\r\n");
		return HAL_ERROR;
	}

	if (HAL_FDCAN_Start(&s_h) != HAL_OK) {
		printf("   HAL_FDCAN_Start FAILED\r\n");
		return HAL_ERROR;
	}
	return HAL_OK;
}

static void can_close(void)
{
	(void) HAL_FDCAN_Stop(&s_h);
	(void) HAL_FDCAN_DeInit(&s_h);
}

/* --- Transmit / receive ------------------------------------------------- */

static HAL_StatusTypeDef can_send(uint32_t id, const uint8_t *d, uint8_t len)
{
	FDCAN_TxHeaderTypeDef tx = { 0 };

	tx.Identifier          = id;
	tx.IdType              = FDCAN_STANDARD_ID;
	tx.TxFrameType         = FDCAN_DATA_FRAME;
	tx.DataLength          = can_dlc(len);
	tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
	tx.BitRateSwitch       = FDCAN_BRS_OFF;
	tx.FDFormat            = FDCAN_CLASSIC_CAN;
	tx.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
	tx.MessageMarker       = 0u;

	return HAL_FDCAN_AddMessageToTxFifoQ(&s_h, &tx, d);
}

static void can_print_frame(const FDCAN_RxHeaderTypeDef *h, const uint8_t *d)
{
	uint8_t len = can_dlc_to_len(h->DataLength);

	printf("[%8" PRIu32 " ms]  RX  id=0x%03" PRIX32 "  %s  %s  len=%u  data=",
	       HAL_GetTick(), h->Identifier,
	       (h->IdType == FDCAN_STANDARD_ID) ? "std" : "EXT",
	       (h->RxFrameType == FDCAN_DATA_FRAME) ? "data" : "RTR ",
	       (unsigned) len);
	for (uint8_t i = 0u; i < len; i++) {
		printf(" %02X", (unsigned) d[i]);
	}
	printf("\r\n");
}

/* Drains FIFO0 and returns how many frames came out. Printing happens here,
 * outside any tight loop, which is what the 32-element FIFO is buying. */
static uint32_t can_drain(uint8_t print)
{
	FDCAN_RxHeaderTypeDef h;
	uint8_t  d[8];
	uint32_t n = 0u;

	while (HAL_FDCAN_GetRxFifoFillLevel(&s_h, FDCAN_RX_FIFO0) != 0u) {
		if (HAL_FDCAN_GetRxMessage(&s_h, FDCAN_RX_FIFO0, &h, d) != HAL_OK) {
			break;
		}
		n++;
		if (print != 0u) {
			can_print_frame(&h, d);
		}
	}
	return n;
}

/* --- Status readout ----------------------------------------------------- */

/* PSR.LEC is reset-on-read, so this runs exactly once per phase and hands the
 * value on to the diagnosis rather than reading it again. */
static uint32_t can_status_report(void)
{
	FDCAN_ProtocolStatusTypeDef ps;
	FDCAN_ErrorCountersTypeDef  ec;

	(void) HAL_FDCAN_GetProtocolStatus(&s_h, &ps);
	(void) HAL_FDCAN_GetErrorCounters(&s_h, &ec);

	printf("   status: last error = %s   activity = %s\r\n",
	       can_lec_name(ps.LastErrorCode), can_activity_name(ps.Activity));
	printf("   counters: TEC=%" PRIu32 "  REC=%" PRIu32 "  logged=%" PRIu32
	       "   flags:%s%s%s\r\n",
	       ec.TxErrorCnt, ec.RxErrorCnt, ec.ErrorLogging,
	       (ps.Warning != 0u)      ? " WARNING"       : "",
	       (ps.ErrorPassive != 0u) ? " ERROR_PASSIVE" : "",
	       (ps.BusOff != 0u)       ? " BUS_OFF"       : "");

	return ps.LastErrorCode;
}

/* --- Diagnosis: symptom -> what to measure ------------------------------ */

static void can_diagnose(const char *phase, uint32_t lec, uint32_t sent,
                         uint32_t got)
{
	printf("\r\n   --- %s diagnosis: sent %" PRIu32 ", received %" PRIu32
	       " ---\r\n", phase, sent, got);

	if ((sent != 0u) && (got == sent) && (lec == FDCAN_PROTOCOL_ERROR_NONE)) {
		printf("   clean.\r\n");
		return;
	}

	switch (lec) {
	case FDCAN_PROTOCOL_ERROR_ACK:
		printf("   ACK ERROR - the frame went out but nobody acknowledged it.\r\n"
		       "   In NORMAL mode that means there is no second node:\r\n"
		       "     1. is the USB-CAN analyser plugged in and its window open?\r\n"
		       "     2. is it set to %" PRIu32 " bit/s AND to Classic CAN, not "
		       "CAN FD?\r\n"
		       "   This error on its own does NOT mean the board is broken - if "
		       "P2 passed,\r\n"
		       "   the transmit and receive chain is already proven.\r\n",
		       CAN_TIMINGS[s_rate].bps);
		break;

	case FDCAN_PROTOCOL_ERROR_BIT1:
		printf("   BIT1 ERROR - a recessive bit was sent and read back as "
		       "dominant,\r\n"
		       "   i.e. something is holding the bus down.\r\n"
		       "   MEASURE, black lead on TP_CAN_GND1, NOT on digital ground:\r\n"
		       "     a) J10 pin 1 (CAN_H) and pin 2 (CAN_L) with nothing "
		       "transmitting -\r\n"
		       "        both should sit near 2.5 V\r\n"
		       "     b) CAN_H to CAN_L resistance with the power off - a few "
		       "kohm or more\r\n"
		       "        is normal; near 0 means the pair is shorted, and 120 "
		       "ohm means\r\n"
		       "        JP7 got bridged\r\n"
		       "     c) check CAN_H and CAN_L are not swapped at the analyser\r\n");
		break;

	case FDCAN_PROTOCOL_ERROR_BIT0:
		printf("   BIT0 ERROR - a dominant bit was sent and read back as "
		       "recessive:\r\n"
		       "   the transceiver is not driving the bus.\r\n"
		       "   MEASURE THIS FIRST - it is the most likely single fault:\r\n"
		       "     TP_CAN_VDD1 to TP_CAN_GND1, expect 5 V.\r\n"
		       "   That rail comes from U7 (PDS1-S5-S5-M), an isolated 5V->5V "
		       "DC/DC\r\n"
		       "   ON THE BOTTOM SIDE of the Upper Deck, fed from +5V through "
		       "L1 (6.8 uH).\r\n"
		       "   No 5 V there and the ISO1044 bus side is unpowered, which is "
		       "exactly\r\n"
		       "   this symptom. If the 5 V is present, scope U8 pin 2 (TXD) "
		       "while this\r\n"
		       "   runs - no activity there points back at PB9 or the J8 link.\r\n");
		break;

	case FDCAN_PROTOCOL_ERROR_STUFF:
	case FDCAN_PROTOCOL_ERROR_FORM:
	case FDCAN_PROTOCOL_ERROR_CRC:
		printf("   %s ERROR - the bits arrived but did not parse. That is a bit "
		       "rate\r\n"
		       "   mismatch or noise, not a dead link.\r\n"
		       "     1. confirm the analyser is at %" PRIu32 " bit/s\r\n"
		       "     2. re-run at 125 kbit/s, the most tolerant setting, by "
		       "changing\r\n"
		       "        CAN_RATE_INDEX to 0\r\n"
		       "     3. if only long cable runs fail, this is where termination "
		       "starts\r\n"
		       "        to matter - see the JP7 note in can_test.h\r\n",
		       can_lec_name(lec), CAN_TIMINGS[s_rate].bps);
		break;

	case FDCAN_PROTOCOL_ERROR_NONE:
	case FDCAN_PROTOCOL_ERROR_NO_CHANGE:
	default:
		if (sent == 0u) {
			printf("   nothing was transmitted in this phase, so there is no "
			       "error to\r\n"
			       "   report. Silence here means nobody else is talking, which "
			       "is not a\r\n"
			       "   fault.\r\n");
		} else if (got == 0u) {
			printf("   frames were accepted by the peripheral but none came "
			       "back, and the\r\n"
			       "   protocol layer reports no error at all. That combination "
			       "points at\r\n"
			       "   the receive path only:\r\n"
			       "     a) scope U8 pin 3 (RXD) - does it move while sending? "
			       "moving means\r\n"
			       "        the fault is between U8 and PI9\r\n"
			       "     b) not moving while U8 pin 2 (TXD) does move -> the "
			       "transceiver\r\n"
			       "        or its isolated 5 V (TP_CAN_VDD1)\r\n");
		} else {
			printf("   partial: %" PRIu32 " of %" PRIu32 " came back with no "
			       "protocol error\r\n"
			       "   logged. Intermittent, so suspect the physical layer "
			       "rather than the\r\n"
			       "   configuration.\r\n", got, sent);
		}
		break;
	}
}

/* --- P0: peripheral and configuration report ---------------------------- */

static void can_p0_report(void)
{
	const can_timing_t *t = &CAN_TIMINGS[s_rate];
	uint32_t hz   = can_clk_hz();
	uint32_t tq   = 1u + t->seg1 + t->seg2;
	uint32_t endn = FDCAN1->ENDN;
	uint32_t crel = FDCAN1->CREL;

	printf("\r\n--- P0: peripheral and configuration ---\r\n");

	printf("   FDCAN1 ENDN = 0x%08" PRIX32 " (%s)   CREL = 0x%08" PRIX32 "\r\n",
	       endn, (endn == CAN_ENDN_EXPECT) ? "alive" : "WRONG - not clocked?",
	       crel);

	printf("   kernel clock: %s = %" PRIu32 " Hz\r\n", can_clk_name(), hz);
	printf("   bit timing:   %" PRIu32 " bit/s = prescaler %u, Seg1 %u, Seg2 %u,"
	       " SJW %u  ->  %" PRIu32 " tq/bit\r\n",
	       t->bps, (unsigned) t->prescaler, (unsigned) t->seg1,
	       (unsigned) t->seg2, (unsigned) t->sjw, tq);

	if (hz != 0u) {
		uint32_t actual = hz / ((uint32_t) t->prescaler * tq);
		printf("   -> actual %" PRIu32 " bit/s, %s\r\n", actual,
		       (actual == t->bps) ? "exact" : "** MISMATCH **");
	}

	printf("   pins: FDCAN1_TX = PB9 -> U8 pin 2,  FDCAN1_RX = PI9 <- U8 pin 3\r\n");
	printf("   terminals: CAN_H = J10 pin 1 (C08/A08), CAN_L = J10 pin 2 "
	       "(C07/A07),\r\n"
	       "              CAN_GND = J11 pin 4 (A09)\r\n");
	printf("   ** Klemmblockzuordnung.pdf is WRONG from terminal 09 on - it "
	       "omits\r\n"
	       "      CAN_GND and shifts RS485 up by one. Use the numbers above.\r\n");
	printf("   ** R69 (120R) is fitted but JP7 ships OPEN, so this board is "
	       "UNTERMINATED.\r\n"
	       "      Switch the terminator on at the analyser instead of soldering "
	       "JP7.\r\n");
}

/* --- P1 / P2: loopback -------------------------------------------------- */

/* Internal loopback never reaches the pins, so it isolates the MCU side.
 * External loopback goes out through U8 and back, and self-acknowledges, so it
 * needs no second node - which is what makes it the useful phase when the only
 * hardware on the bench is this one board. */
static void can_loopback(const char *name, uint32_t mode)
{
	uint32_t sent = 0u, got = 0u, lec;

	printf("\r\n--- %s: %s, %u frames ---\r\n", name, can_mode_name(mode),
	       (unsigned) CAN_FRAMES_PER_LOOPBACK);

	if (can_open(mode) != HAL_OK) {
		printf("   cannot open the peripheral - skipping\r\n");
		can_close();
		return;
	}

	for (uint8_t i = 0u; i < CAN_FRAMES_PER_LOOPBACK; i++) {
		uint8_t  payload[8];
		uint32_t start;

		for (uint8_t b = 0u; b < 8u; b++) {
			payload[b] = (uint8_t) (((uint32_t) i << 4) | b);
		}

		if (can_send((uint32_t) (CAN_TEST_ID + i), payload, 8u) != HAL_OK) {
			printf("   frame %u: the TX FIFO refused it\r\n", (unsigned) i);
			continue;
		}
		sent++;

		start = HAL_GetTick();
		while ((HAL_GetTick() - start) < CAN_RX_WAIT_MS) {
			if (HAL_FDCAN_GetRxFifoFillLevel(&s_h, FDCAN_RX_FIFO0) != 0u) {
				break;
			}
		}
		got += can_drain(1u);
	}

	lec = can_status_report();
	can_diagnose(name, lec, sent, got);
	can_close();
}

/* --- P3: listen only ---------------------------------------------------- */

static void can_p3_listen(void)
{
	uint32_t start, tick, got = 0u, lec;

	printf("\r\n--- P3: %s for %u s, nothing is put on the bus ---\r\n",
	       can_mode_name(FDCAN_MODE_BUS_MONITORING),
	       (unsigned) (CAN_LISTEN_MS / 1000u));
	printf("   Send something from the analyser now; every frame prints as it "
	       "arrives.\r\n");

	if (can_open(FDCAN_MODE_BUS_MONITORING) != HAL_OK) {
		can_close();
		return;
	}

	start = HAL_GetTick();
	tick  = start;
	while ((HAL_GetTick() - start) < CAN_LISTEN_MS) {
		got += can_drain(1u);
		if ((HAL_GetTick() - tick) >= 5000u) {
			tick += 5000u;
			if (got == 0u) {
				printf("[%8" PRIu32 " ms]  quiet - nothing received yet\r\n",
				       HAL_GetTick());
			}
		}
	}

	lec = can_status_report();
	can_diagnose("P3", lec, 0u, got);
	can_close();
}

/* --- P4: normal mode against a real node -------------------------------- */

static void can_p4_normal(void)
{
	uint32_t start, lastTx, sent = 0u, got = 0u, lec;
	uint8_t  counter = 0u;

	printf("\r\n--- P4: %s for %u s, one frame every %u ms ---\r\n",
	       can_mode_name(FDCAN_MODE_NORMAL),
	       (unsigned) (CAN_NORMAL_MS / 1000u),
	       (unsigned) CAN_NORMAL_EVERY_MS);
	printf("   With no second node on the bus every frame fails with an ACK "
	       "error. That\r\n"
	       "   is expected, not a fault.\r\n");

	if (can_open(FDCAN_MODE_NORMAL) != HAL_OK) {
		can_close();
		return;
	}

	start  = HAL_GetTick();
	lastTx = start - CAN_NORMAL_EVERY_MS;
	while ((HAL_GetTick() - start) < CAN_NORMAL_MS) {
		if ((HAL_GetTick() - lastTx) >= CAN_NORMAL_EVERY_MS) {
			uint8_t payload[8] = { 0xDEu, 0xADu, 0xBEu, 0xEFu, 0u, 0u, 0u, 0u };
			lastTx += CAN_NORMAL_EVERY_MS;
			payload[4] = counter++;

			if (can_send(CAN_TEST_ID, payload, 8u) == HAL_OK) {
				sent++;
				printf("[%8" PRIu32 " ms]  TX  id=0x%03X  data= DE AD BE EF "
				       "%02X 00 00 00\r\n", HAL_GetTick(),
				       (unsigned) CAN_TEST_ID, (unsigned) payload[4]);
			} else {
				printf("[%8" PRIu32 " ms]  TX FIFO full - earlier frames have "
				       "not left the chip\r\n", HAL_GetTick());
			}
		}
		got += can_drain(1u);
	}

	lec = can_status_report();
	can_diagnose("P4", lec, sent, got);
	can_close();
}

/* --- Soak: receive and print; transmitting is optional ------------------- */

#define CAN_SOAK_EVERY_MS   1000u
#define CAN_SOAK_ID        0x321u

/* Transmit one frame per interval as well as receiving. Set to 0 for receive
 * only. Either way the mode is FDCAN_MODE_NORMAL rather than bus monitoring, so
 * that arriving frames are acknowledged: an unacknowledged sender retransmits
 * the same frame forever and floods the bus. */
#define CAN_SOAK_TX_ENABLE      1

/* Bounded so that inbound traffic can never starve the transmit cadence. An
 * unacknowledged sender retransmits at line rate - thousands of frames per
 * second - while this console prints about two hundred lines per second, so an
 * unbounded drain would print forever and nothing would ever be sent again. */
#define CAN_SOAK_DRAIN_MAX     8u

static uint32_t can_soak_drain(void)
{
	FDCAN_RxHeaderTypeDef h;
	uint8_t  d[8];
	uint32_t n = 0u;

	while ((n < CAN_SOAK_DRAIN_MAX)
	       && (HAL_FDCAN_GetRxFifoFillLevel(&s_h, FDCAN_RX_FIFO0) != 0u)) {
		if (HAL_FDCAN_GetRxMessage(&s_h, FDCAN_RX_FIFO0, &h, d) != HAL_OK) {
			break;
		}
		n++;
		can_print_frame(&h, d);
	}
	return n;
}

/* Prints TEC/REC on every transmit line: TEC pinned at 0 means a second node is
 * acknowledging, TEC climbing in steps of 8 means nobody is. */
static void can_soak_counters(uint32_t *tec, uint32_t *rec)
{
	FDCAN_ErrorCountersTypeDef ec;

	(void) HAL_FDCAN_GetErrorCounters(&s_h, &ec);
	*tec = ec.TxErrorCnt;
	*rec = ec.RxErrorCnt;
}

/* Keeps retrying rather than parking: a soak that goes silent tells nobody
 * whether the board died or the bus did. */
static void can_soak_open_or_retry(const char *what)
{
	uint32_t attempt = 0u;

	while (can_open(FDCAN_MODE_NORMAL) != HAL_OK) {
		attempt++;
		printf("** CAN: %s FAILED (attempt %" PRIu32 ") - retrying in %u ms\r\n",
		       what, attempt, (unsigned) CAN_SOAK_EVERY_MS);
		can_close();
		HAL_Delay(CAN_SOAK_EVERY_MS);
	}
	if (attempt != 0u) {
		printf("** CAN: %s succeeded after %" PRIu32 " retries\r\n",
		       what, attempt);
	}
}

void CAN_Test_Soak_Run(void)
{
	uint32_t last, sent = 0u, got = 0u, recoveries = 0u;
	uint8_t  counter = 0u;

	printf("\r\n=== CAN soak (TestCase/CAN) ===\r\n");
	printf("   FDCAN1 on PB9/PI9 through U8 (ISO1044BDR, isolated); the bus "
	       "side is powered by U7\r\n");
	printf("   %s, classic CAN, standard IDs, %" PRIu32 " bit/s\r\n",
	       can_mode_name(FDCAN_MODE_NORMAL), CAN_TIMINGS[s_rate].bps);
#if CAN_SOAK_TX_ENABLE
	printf("   transmits id=0x%03X every %u ms and prints every frame that "
	       "arrives\r\n", (unsigned) CAN_SOAK_ID,
	       (unsigned) CAN_SOAK_EVERY_MS);
#else
	printf("   RECEIVE ONLY - transmits nothing, prints every frame that "
	       "arrives,\r\n              and acknowledges it so the sender does "
	       "not retransmit\r\n");
	printf("   a status line every %u ms says the board is still alive\r\n",
	       (unsigned) CAN_SOAK_EVERY_MS);
#endif
	printf("   terminals: CAN_H = J10 pin 1 (C08/A08), CAN_L = J10 pin 2 "
	       "(C07/A07),\r\n              CAN_GND = J11 pin 4 (A09)\r\n");

	if (can_clock_init() == 0u) {
		printf("** CAN: no kernel clock - nothing below is meaningful\r\n");
	}
	can_gpio_init();

	can_soak_open_or_retry("start");

	last = HAL_GetTick() - CAN_SOAK_EVERY_MS;
	for (;;) {
		got += can_soak_drain();

		if ((HAL_GetTick() - last) >= CAN_SOAK_EVERY_MS) {
			FDCAN_ProtocolStatusTypeDef ps;
			uint8_t  payload[8] = { 0xC0u, 0xFFu, 0xEEu, 0u, 0u, 0u, 0u, 0u };
			uint32_t tec, rec;

			last += CAN_SOAK_EVERY_MS;

#if CAN_SOAK_TX_ENABLE
			payload[3] = counter++;
			if (can_send(CAN_SOAK_ID, payload, 8u) == HAL_OK) {
				sent++;
			} else {
				printf("[%8" PRIu32 " ms]  TX rejected - the transmit FIFO is "
				       "full, earlier frames never left the chip\r\n",
				       HAL_GetTick());
			}
#else
			(void) payload;
			(void) counter;
#endif

			can_soak_counters(&tec, &rec);
#if CAN_SOAK_TX_ENABLE
			printf("[%8" PRIu32 " ms]  TX  id=0x%03X  data= C0 FF EE %02X 00 00 "
			       "00 00   sent=%" PRIu32 " rcvd=%" PRIu32 "  TEC=%" PRIu32
			       " REC=%" PRIu32 "  fifo=%" PRIu32 "\r\n", HAL_GetTick(),
			       (unsigned) CAN_SOAK_ID, (unsigned) payload[3], sent, got,
			       tec, rec,
			       HAL_FDCAN_GetRxFifoFillLevel(&s_h, FDCAN_RX_FIFO0));
#else
			printf("[%8" PRIu32 " ms]  listening,  rcvd=%" PRIu32 "  TEC=%"
			       PRIu32 " REC=%" PRIu32 "  fifo=%" PRIu32 "\r\n",
			       HAL_GetTick(), got, tec, rec,
			       HAL_FDCAN_GetRxFifoFillLevel(&s_h, FDCAN_RX_FIFO0));
			(void) sent;
#endif

			(void) HAL_FDCAN_GetProtocolStatus(&s_h, &ps);
			if (ps.BusOff != 0u) {
				recoveries++;
				printf("   BUS_OFF after %" PRIu32 " frames - nobody is "
				       "acknowledging. Restarting FDCAN1 (recovery #%" PRIu32
				       ")\r\n", sent, recoveries);
				can_close();
				can_soak_open_or_retry("restart");
			}
		}
	}
}

/* --- Echo run: reply to every frame with the value plus one -------------- */

#define CAN_ECHO_ALIVE_MS   5000u

/* Big-endian add one across the whole payload, so a value split over several
 * bytes carries instead of each byte wrapping on its own. */
static void can_echo_increment(uint8_t *d, uint8_t len)
{
	for (uint8_t i = len; i > 0u; i--) {
		d[i - 1u]++;
		if (d[i - 1u] != 0u) {
			break;
		}
	}
}

static HAL_StatusTypeDef can_echo_reply(const FDCAN_RxHeaderTypeDef *rx,
                                        const uint8_t *d, uint8_t len)
{
	FDCAN_TxHeaderTypeDef tx = { 0 };

	tx.Identifier          = rx->Identifier;
	tx.IdType              = rx->IdType;          /* answer on what came in */
	tx.TxFrameType         = FDCAN_DATA_FRAME;
	tx.DataLength          = can_dlc(len);
	tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
	tx.BitRateSwitch       = FDCAN_BRS_OFF;
	tx.FDFormat            = FDCAN_CLASSIC_CAN;
	tx.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
	tx.MessageMarker       = 0u;

	return HAL_FDCAN_AddMessageToTxFifoQ(&s_h, &tx, d);
}

void CAN_Test_Echo_Run(void)
{
	uint32_t got = 0u, replied = 0u, recoveries = 0u, lastAlive;

	printf("\r\n=== CAN echo (TestCase/CAN) ===\r\n");
	printf("   FDCAN1 on PB9/PI9 through U8, %s, classic CAN, %" PRIu32
	       " bit/s\r\n", can_mode_name(FDCAN_MODE_NORMAL),
	       CAN_TIMINGS[s_rate].bps);
	printf("   Transmits nothing on its own. Every frame that arrives is "
	       "printed, its\r\n"
	       "   payload is incremented as one big-endian number, and the "
	       "result goes back\r\n"
	       "   out on the SAME identifier with the same length.\r\n");
	printf("   A status line every %u s says the board is still alive.\r\n",
	       (unsigned) (CAN_ECHO_ALIVE_MS / 1000u));

	if (can_clock_init() == 0u) {
		printf("** CAN: no kernel clock - nothing below is meaningful\r\n");
	}
	can_gpio_init();
	can_soak_open_or_retry("start");

	lastAlive = HAL_GetTick();
	for (;;) {
		FDCAN_RxHeaderTypeDef h;
		uint8_t d[8];

		while (HAL_FDCAN_GetRxFifoFillLevel(&s_h, FDCAN_RX_FIFO0) != 0u) {
			uint8_t len;

			if (HAL_FDCAN_GetRxMessage(&s_h, FDCAN_RX_FIFO0, &h, d) != HAL_OK) {
				break;
			}
			got++;
			can_print_frame(&h, d);
			len = can_dlc_to_len(h.DataLength);

			if (h.RxFrameType != FDCAN_DATA_FRAME) {
				printf("            remote frame - nothing to increment, no "
				       "reply\r\n");
				continue;
			}
			if (len == 0u) {
				printf("            no data bytes - nothing to increment, no "
				       "reply\r\n");
				continue;
			}

			can_echo_increment(d, len);
			if (can_echo_reply(&h, d, len) == HAL_OK) {
				replied++;
				printf("[%8" PRIu32 " ms]  TX  id=0x%03" PRIX32 "  reply "
				       "(+1)  data=", HAL_GetTick(), h.Identifier);
				for (uint8_t i = 0u; i < len; i++) {
					printf(" %02X", (unsigned) d[i]);
				}
				printf("\r\n");
			} else {
				printf("            reply REJECTED - transmit FIFO full\r\n");
			}
		}

		if ((HAL_GetTick() - lastAlive) >= CAN_ECHO_ALIVE_MS) {
			FDCAN_ProtocolStatusTypeDef ps;
			uint32_t tec, rec;

			lastAlive += CAN_ECHO_ALIVE_MS;
			can_soak_counters(&tec, &rec);
			printf("[%8" PRIu32 " ms]  waiting,  rcvd=%" PRIu32 " replied=%"
			       PRIu32 "  TEC=%" PRIu32 " REC=%" PRIu32 "\r\n",
			       HAL_GetTick(), got, replied, tec, rec);

			(void) HAL_FDCAN_GetProtocolStatus(&s_h, &ps);
			if (ps.BusOff != 0u) {
				recoveries++;
				printf("   BUS_OFF - restarting FDCAN1 (recovery #%" PRIu32
				       ")\r\n", recoveries);
				can_close();
				can_soak_open_or_retry("restart");
			}
		}
	}
}

/* --- Scope run: make PB9 carry something a scope can actually catch ------ */

#define CAN_SCOPE_ID          0x555u
#define CAN_SCOPE_GPIO_MS     5000u
#define CAN_SCOPE_BLAST_MS   10000u

/* Drives PB9 as an ordinary output. This is the step that separates "the pin
 * cannot be driven" from "FDCAN is not driving it": nothing here involves the
 * CAN peripheral at all. */
static void can_scope_pin_toggle(uint32_t ms)
{
	GPIO_InitTypeDef g = { 0 };
	uint32_t start;

	printf("\r\n--- [1] PB9 as a plain GPIO output, 500 Hz square wave, %u s "
	       "---\r\n", (unsigned) (ms / 1000u));
	printf("    Expect a 500 Hz square wave, 0 V to 3.3 V, at PB9 and at U8 "
	       "pin 2.\r\n");
	printf("    NOTHING HERE means the fault is between the MCU ball and the "
	       "probe\r\n"
	       "    point - the CAN peripheral is not involved in this step.\r\n");

	__HAL_RCC_GPIOB_CLK_ENABLE();
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	g.Pin   = CAN_TX_PIN;
	HAL_GPIO_Init(CAN_TX_PORT, &g);

	start = HAL_GetTick();
	while ((HAL_GetTick() - start) < ms) {
		HAL_GPIO_TogglePin(CAN_TX_PORT, CAN_TX_PIN);
		HAL_Delay(1u);
	}

	HAL_GPIO_WritePin(CAN_TX_PORT, CAN_TX_PIN, GPIO_PIN_SET);
	can_gpio_init();
}

/* Transmits back to back for the whole window instead of one frame per second.
 * One frame is about 110 us, so at one per second the pin is busy 0.011% of the
 * time and a scope on a normal timebase shows a flat line. */
static void can_scope_blast(uint32_t mode, uint32_t ms, const char *step,
                            const char *expect)
{
	static const uint8_t PAYLOAD[8] = {
		0x55u, 0xAAu, 0x55u, 0xAAu, 0x55u, 0xAAu, 0x55u, 0xAAu
	};
	uint32_t start, sent = 0u, tec, rec, offs = 0u;

	printf("\r\n--- %s: %s, back to back for %u s ---\r\n",
	       step, can_mode_name(mode), (unsigned) (ms / 1000u));
	printf("    %s\r\n", expect);

	if (can_open(mode) != HAL_OK) {
		printf("    could not start\r\n");
		can_close();
		return;
	}

	start = HAL_GetTick();
	while ((HAL_GetTick() - start) < ms) {
		FDCAN_ProtocolStatusTypeDef ps;

		while (can_send(CAN_SCOPE_ID, PAYLOAD, 8u) == HAL_OK) {
			sent++;
		}
		(void) can_drain(0u);

		(void) HAL_FDCAN_GetProtocolStatus(&s_h, &ps);
		if (ps.BusOff != 0u) {
			offs++;
			can_close();
			if (can_open(mode) != HAL_OK) {
				printf("    restart FAILED\r\n");
				return;
			}
		}
	}

	can_soak_counters(&tec, &rec);
	printf("    queued %" PRIu32 " frames, TEC=%" PRIu32 " REC=%" PRIu32
	       ", bus-off %" PRIu32 " times\r\n", sent, tec, rec, offs);
	can_close();
}

void CAN_Test_Scope_Run(void)
{
	printf("\r\n=== CAN scope run (TestCase/CAN) ===\r\n");
	printf("   Probe PB9 (FDCAN1_TX, BGA B4) or U8 pin 2. Ground on GNDD for "
	       "the MCU\r\n"
	       "   side - NOT on TP_CAN_GND1, which is the isolated bus side.\r\n");
	printf("   Three steps repeat forever; each one prints what to expect "
	       "before it runs.\r\n");

	if (can_clock_init() == 0u) {
		printf("** CAN: no kernel clock - nothing below is meaningful\r\n");
	}
	can_gpio_init();

	for (;;) {
		can_scope_pin_toggle(CAN_SCOPE_GPIO_MS);

		/* Loop back mode ignores acknowledge errors, so this transmits
		 * continuously with no second node and never reaches bus-off. The
		 * frames are still driven on the TX pin. */
		s_auto_retx = DISABLE;
		can_scope_blast(FDCAN_MODE_EXTERNAL_LOOPBACK, CAN_SCOPE_BLAST_MS, "[2]",
		                "Expect continuous CAN traffic at PB9: 3.3 V idle, "
		                "brief 0 V bits, ~2 us each.");

		/* Auto retransmission on: without an acknowledge the same frame is
		 * retried at line rate, which keeps the pin busy until bus-off. */
		s_auto_retx = ENABLE;
		can_scope_blast(FDCAN_MODE_NORMAL, CAN_SCOPE_BLAST_MS, "[3]",
		                "Same picture as [2], but on the real bus. If [2] "
		                "shows traffic and [3] does not,\r\n    the peripheral "
		                "is fine and the bus is holding the line.");
		s_auto_retx = DISABLE;

		printf("\r\n===== scope round complete =====\r\n");
	}
}

/* --- Entry point -------------------------------------------------------- */

void CAN_Test_Run(void)
{
	printf("\r\n=== CAN bring-up test (TestCase/CAN) ===\r\n");
	printf("   FDCAN1 on PB9/PI9 through U8 (ISO1044BDR, isolated); the bus "
	       "side is powered by U7\r\n");
	printf("   Classic CAN, standard IDs, %" PRIu32 " bit/s\r\n",
	       CAN_TIMINGS[s_rate].bps);

	if (can_clock_init() == 0u) {
		printf("** CAN: no kernel clock - nothing below is meaningful\r\n");
	}
	can_gpio_init();
	can_p0_report();

	for (;;) {
		can_loopback("P1", FDCAN_MODE_INTERNAL_LOOPBACK);
		HAL_Delay(CAN_ROUND_GAP_MS);
		can_loopback("P2", FDCAN_MODE_EXTERNAL_LOOPBACK);
		HAL_Delay(CAN_ROUND_GAP_MS);
		can_p3_listen();
		HAL_Delay(CAN_ROUND_GAP_MS);
		can_p4_normal();
		printf("\r\n===== round complete =====\r\n");
		HAL_Delay(CAN_ROUND_GAP_MS);
	}
}
