// knx_test.c - see knx_test.h for the hardware facts this is built on.

#include <stdarg.h>
#include <stdio.h>
#include <inttypes.h>

#include "main.h"
#include "knx_test.h"

/* --- KNX TP1 timing, datasheet DocID031327 Rev 1 section 5.1 ------------- */

#define KNX_BIT_US             104u   /* bit period, 9600 bit/s */
#define KNX_PULSE_US            35u   /* active pulse = logic 0 */
#define KNX_CHAR_BITS           13u   /* start + 8 data + parity + stop + 2 idle */

/* Idle gap that guarantees the next pulse is a start bit, used to re-lock the
 * decoder after a framing error. Inside one character the widest possible gap
 * between pulses is start-bit to parity-bit with all eight data bits at logic
 * 1, which is 9 bit periods; 10 is therefore unambiguously between characters.
 * Telegrams are separated by far more than this, so a lost telegram tail
 * re-locks cleanly on the next one. */
#define KNX_RESYNC_GAP_US      (10u * KNX_BIT_US)

/* --- Phase sizing ------------------------------------------------------- */

/* Everything this test transmits happens once every KNX_TX_EVERY_MS and prints
 * one line, so the console stays readable and a live bus stays usable. */
#define KNX_TX_EVERY_MS       5000u
#define KNX_P0_WAIT_MS       30000u   /* how long to wait for bus power */
#define KNX_P1_SHOTS              3u  /* single pulses per round */
#define KNX_P2_BURSTS             3u
#define KNX_P2_BURST_PULSES    200u   /* ~21 ms of dominant state per burst */
#define KNX_P4_MS            20000u
#define KNX_P5_BURST_PULSES    200u   /* per leg of the pull-up A/B test */

/* Console caps for the receive side. Real characters get priority; malformed
 * frames and raw edges are counted and summarised instead of printed. */
#define KNX_P4_CHARS_PER_S       8u
#define KNX_P4_EDGES_PER_S      24u

#define KNX_CHAR_RESULT_RING   128u   /* power of two */

/* --- Slim mode ----------------------------------------------------------- */

/* 1 = one TP1 character every KNX_TX_EVERY_MS and listening every other
 * moment, nothing else. 0 = the full P1..P5 round with loss statistics,
 * histograms and the pull-up A/B test, which costs a 20 s silent window. */
#define KNX_SLIM_MODE            1u
#define KNX_SLIM_TX_BYTE      0xAAu

/* --- Receive-only bring-up ----------------------------------------------- */

/* 0 suspends every transmit in the slim loop. The bit engine keeps running
 * with CCR1 held at 0, so PB14 stays driven low and the transceiver puts
 * nothing on the bus. 1 restores the group-write cadence. */
#define KNX_TX_ENABLE            0u

/* Everything that arrives is printed, valid or not: octets are collected until
 * the line has been quiet this long, then the burst prints raw and inverted.
 * KNX inter-frame silence is ~50 bit periods, so 10 ms never splits a frame. */
#define KNX_RX_FLUSH_MS         10u
#define KNX_BURST_MAX           64u

/* --- What to put on the bus ---------------------------------------------- */

/* A real L_Data_Standard group-write frame, which is the minimum an ETS bus or
 * group monitor will display. Field values verified against the OpenPLC_KNX
 * reference stack: knx/knx_types.h (StandardFrame 0x80, Broadcast 0x10,
 * LowPriority 0x0C, GroupAddress 0x80, GroupValueWrite 0x080), the wire layout
 * in knx/tp_frame.h and the check octet in knx/cemi_frame.cpp:205. */
#define KNX_SEND_FRAMES          1u   /* 0 = send a lone character instead */
#define KNX_SRC_ADDR        0xFFFAu   /* 15.15.250 - no real device owns this */
#define KNX_GROUP_ADDR      0x0801u   /* 1/0/1  = main<<11 | middle<<8 | sub */
#define KNX_FRAME_MAX           16u   /* octets */
#define KNX_TX_BITS_MAX     (KNX_FRAME_MAX * KNX_CHAR_BITS)

/* TP1 wants the line quiet before a device may start sending. */
#define KNX_ARB_IDLE_BITS       50u

/* Print every decoded character as well as the assembled frame. Off by
 * default: the frame line already carries the content. */
#define KNX_PRINT_CHARS          0u
/* The burst printer below already shows every octet, so the frame-assembler
 * line is off. Set to 1 to get it back alongside. */
#define KNX_PRINT_FRAMES         0u
#define KNX_LOOPBACK_MS         50u   /* a character this soon after a TX is
                                       * our own echo, not foreign traffic */
/* Transmits with no loopback at all before the one-and-only diagnosis fires.
 * There is no periodic status output of any kind. */
#define KNX_SLIM_DIAG_AFTER      6u

/* --- Pins --------------------------------------------------------------- */

#define KNX_TX_PORT            GPIOB
#define KNX_TX_PIN             GPIO_PIN_14
#define KNX_RX_PORT            GPIOA
#define KNX_RX_PIN             GPIO_PIN_10
#define KNX_RX_PIN_NR              10u
#define KNX_OK_PORT            GPIOD
#define KNX_OK_PIN             GPIO_PIN_7
#define KNX_VCC_OK_PORT        GPIOH
#define KNX_VCC_OK_PIN         GPIO_PIN_12
#define KNX_PROG_LED_PORT      GPIOG
#define KNX_PROG_LED_PIN       GPIO_PIN_11

#define KNX_RX_LEVEL()         ((KNX_RX_PORT->IDR & KNX_RX_PIN) != 0u)
#define KNX_OK_LEVEL()         ((KNX_OK_PORT->IDR & KNX_OK_PIN) != 0u)
#define KNX_VCC_OK_LEVEL()     ((KNX_VCC_OK_PORT->IDR & KNX_VCC_OK_PIN) != 0u)

/* --- Ring sizes (powers of two) ----------------------------------------- */

#define KNX_EDGE_RING          256u
#define KNX_PULSE_RING         256u
#define KNX_STATUS_RING          8u

/* --- Histograms --------------------------------------------------------- */

#define KNX_HIST_BINS            8u

/* Upper bound of each bin, in microseconds; the last bin is everything above. */
static const uint16_t KNX_W_EDGES[KNX_HIST_BINS - 1u] = { 20u, 25u, 30u, 33u, 37u, 42u, 50u };
static const uint16_t KNX_D_EDGES[KNX_HIST_BINS - 1u] = {  2u,  3u,  4u,  6u,  8u, 12u, 20u };

/* --- Transmit engine modes ---------------------------------------------- */

typedef enum {
	KNX_TX_IDLE = 0,   /* CCR1 held at 0, KNX_TX stays low */
	KNX_TX_CONT,       /* one active pulse every bit period, forever */
	KNX_TX_SEQ         /* walk s_txBits once, then fall back to IDLE */
} knx_tx_mode_t;

/* --- ISR-shared state --------------------------------------------------- */

static TIM_HandleTypeDef s_htim1;    /* KNX_RX capture, 1 MHz, 1 tick = 1 us */
static TIM_HandleTypeDef s_htim12;   /* KNX_TX bit engine */

static uint32_t s_pulseTicks;

/* Transmit */
static volatile knx_tx_mode_t s_txMode;
static volatile uint8_t  s_txBits[KNX_TX_BITS_MAX];
static volatile uint16_t s_txLen;
static volatile uint16_t s_txIdx;
static volatile uint8_t  s_txDone;
static volatile uint32_t s_txPulses;
static volatile uint16_t s_txStamp;

/* Per-pulse loss detection: the update ISR judges the previous pulse. */
static volatile uint8_t  s_expectRx;
static volatile uint8_t  s_rxSeen;
static volatile uint32_t s_misses;
static volatile uint32_t s_missFirst;   /* pulse index of the first miss */
static volatile uint32_t s_missLast;

/* Receive statistics, microseconds */
static volatile uint32_t s_rxPulses;
static volatile uint32_t s_wMin, s_wMax, s_wSum, s_wCnt;
static volatile uint32_t s_dMin, s_dMax, s_dSum, s_dCnt;
static volatile uint32_t s_wHist[KNX_HIST_BINS];
static volatile uint32_t s_dHist[KNX_HIST_BINS];

/* Raw edge log: (level << 16) | 1 MHz timestamp */
static volatile uint32_t s_edgeRing[KNX_EDGE_RING];
static volatile uint16_t s_edgeHead, s_edgeTail;
static volatile uint32_t s_edgeDropped;
static volatile uint8_t  s_edgeLog;

/* Rising-edge timestamps feeding the TP1 character decoder */
static volatile uint16_t s_pulseRing[KNX_PULSE_RING];
static volatile uint16_t s_pulseHead, s_pulseTail;
static volatile uint32_t s_pulseDropped;   /* decoder starved - see P4 */

/* Finished slot patterns, so the decode loop never waits on printf */
static uint16_t s_charRing[KNX_CHAR_RESULT_RING];
static uint16_t s_charHead, s_charTail;
static uint32_t s_charDropped;

static volatile uint16_t s_rxRise;
static volatile uint8_t  s_rxInPulse;
static volatile uint32_t s_lastEdgeMs;   /* bus arbitration: last activity */

/* KNX_OK / KNX_VCC_OK transition log, sampled every bit period */
static volatile uint8_t  s_stPrev;
static volatile uint8_t  s_stValid;
static volatile uint32_t s_stFlips;        /* while transmitting */
static volatile uint32_t s_stFlipsIdle;    /* while not transmitting */
static volatile uint32_t s_stRing[KNX_STATUS_RING];   /* (state << 16) | timestamp */
static volatile uint16_t s_stHead, s_stTail;

/* KNX_RX idle-level watchdog: how often the pin sat high with TX idle */
static volatile uint32_t s_idleHighSamples;
static volatile uint32_t s_idleSamples;

/* --- Round results feeding the diagnosis -------------------------------- */

typedef enum {
	KNX_BUS_OK = 0,
	KNX_BUS_DEAD,
	KNX_BUS_ODD
} knx_bus_t;

typedef struct {
	knx_bus_t bus;
	uint8_t   vccOk, busOk, rxIdle;
	uint32_t  p2Tx, p2Rx, p2Miss, p2wAvg, p2dAvg, p2wCnt;
	uint8_t   p3Pass, p3Total;
	uint32_t  p4Chars, p4Pulses;
	uint32_t  p5MissPullup, p5MissNopull;
	uint32_t  p5wAvgPullup, p5wAvgNopull;
	uint32_t  statusFlips;
	uint32_t  statusFlipsIdle;
	uint32_t  idleHighPermille;
} knx_round_t;

static knx_round_t s_round;

/* --- Clock helpers ------------------------------------------------------ */

/* An APB timer runs at 2x PCLK whenever the APB prescaler divides, and at
 * PCLK when it does not. Valid while RCC_CFGR TIMPRE keeps its reset value. */
static uint32_t knx_timer_clk(uint32_t pclk, uint32_t ppre)
{
	return (ppre >= 4u) ? (pclk * 2u) : pclk;
}

static uint32_t knx_tim1_clk(void)
{
	uint32_t ppre = (RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) >> RCC_D2CFGR_D2PPRE2_Pos;
	return knx_timer_clk(HAL_RCC_GetPCLK2Freq(), ppre);
}

static uint32_t knx_tim12_clk(void)
{
	uint32_t ppre = (RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) >> RCC_D2CFGR_D2PPRE1_Pos;
	return knx_timer_clk(HAL_RCC_GetPCLK1Freq(), ppre);
}

static uint32_t knx_us_to_ticks(uint32_t clk, uint32_t us)
{
	return (uint32_t) (((uint64_t) clk * (uint64_t) us) / 1000000u);
}

/* --- Pin setup ---------------------------------------------------------- */

/* Drive KNX_TX low, the STKNX transmitter's idle state. Never leave this pin
 * floating - see the TX chain note in knx_test.h. */
static void knx_tx_park_gpio(void)
{
	GPIO_InitTypeDef g = { 0 };

	HAL_GPIO_DeInit(KNX_TX_PORT, KNX_TX_PIN);
	g.Pin   = KNX_TX_PIN;
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(KNX_TX_PORT, &g);
	HAL_GPIO_WritePin(KNX_TX_PORT, KNX_TX_PIN, GPIO_PIN_RESET);
}

/* Switch PA10's internal pull-up without disturbing its mode, so the A/B test
 * can toggle it while TIM1 keeps capturing. */
static void knx_rx_pullup(uint8_t on)
{
	uint32_t r = KNX_RX_PORT->PUPDR;

	r &= ~(3u << (KNX_RX_PIN_NR * 2u));
	if (on != 0u) {
		r |= (1u << (KNX_RX_PIN_NR * 2u));   /* 01 = pull-up */
	}
	KNX_RX_PORT->PUPDR = r;
}

static void knx_gpio_init(void)
{
	GPIO_InitTypeDef g = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();

	/* PG9 (KNX_Prog_KEY) is the BOOT0 net - left alone on purpose. */

	knx_tx_park_gpio();

	/* U13's open-collector output has no board pull-up on /KNX_RX. */
	g.Pin   = KNX_RX_PIN;
	g.Mode  = GPIO_MODE_INPUT;
	g.Pull  = GPIO_PULLUP;
	HAL_GPIO_Init(KNX_RX_PORT, &g);

	g.Pull = GPIO_NOPULL;
	g.Pin  = KNX_OK_PIN;
	HAL_GPIO_Init(KNX_OK_PORT, &g);
	g.Pin  = KNX_VCC_OK_PIN;
	HAL_GPIO_Init(KNX_VCC_OK_PORT, &g);

	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	g.Pin   = KNX_PROG_LED_PIN;
	HAL_GPIO_Init(KNX_PROG_LED_PORT, &g);
	HAL_GPIO_WritePin(KNX_PROG_LED_PORT, KNX_PROG_LED_PIN, GPIO_PIN_RESET);
}

/* --- Histograms --------------------------------------------------------- */

static void knx_hist_add(volatile uint32_t *h, const uint16_t *edges, uint32_t v)
{
	for (uint8_t i = 0u; i < (KNX_HIST_BINS - 1u); i++) {
		if (v < edges[i]) {
			h[i]++;
			return;
		}
	}
	h[KNX_HIST_BINS - 1u]++;
}

static void knx_hist_print(const char *what, const char *unit,
                           volatile uint32_t *h, const uint16_t *edges)
{
	uint32_t total = 0u;

	for (uint8_t i = 0u; i < KNX_HIST_BINS; i++) {
		total += h[i];
	}
	printf("   %s histogram (%s), %" PRIu32 " samples:\r\n", what, unit, total);
	if (total == 0u) {
		printf("      (none)\r\n");
		return;
	}
	for (uint8_t i = 0u; i < KNX_HIST_BINS; i++) {
		char label[16];
		if (i == 0u) {
			(void) snprintf(label, sizeof(label), "   <%u", (unsigned) edges[0]);
		} else if (i == (KNX_HIST_BINS - 1u)) {
			(void) snprintf(label, sizeof(label), ">=%u  ",
			                (unsigned) edges[KNX_HIST_BINS - 2u]);
		} else {
			(void) snprintf(label, sizeof(label), "%u-%u",
			                (unsigned) edges[i - 1u], (unsigned) edges[i]);
		}
		uint32_t pm  = (h[i] * 1000u) / total;
		uint32_t bar = (pm * 40u) / 1000u;
		printf("      %7s : %8" PRIu32 "  %2" PRIu32 ".%" PRIu32 "%%  ",
		       label, h[i], pm / 10u, pm % 10u);
		for (uint32_t b = 0u; b < bar; b++) { printf("#"); }
		printf("\r\n");
	}
}

/* --- KNX_RX capture: TIM1_CH3, both edges, 1 MHz ------------------------- */

static void knx_capture_start(void)
{
	GPIO_InitTypeDef   g  = { 0 };
	TIM_IC_InitTypeDef ic = { 0 };
	uint32_t clk = knx_tim1_clk();

	__HAL_RCC_TIM1_CLK_ENABLE();

	g.Pin       = KNX_RX_PIN;
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_PULLUP;
	g.Speed     = GPIO_SPEED_FREQ_HIGH;
	g.Alternate = GPIO_AF1_TIM1;
	HAL_GPIO_Init(KNX_RX_PORT, &g);

	/* One tick is one microsecond; the 16-bit count wraps every 65.5 ms,
	 * comfortably longer than the 1.35 ms a TP1 character takes. */
	s_htim1.Instance               = TIM1;
	s_htim1.Init.Prescaler         = (clk / 1000000u) - 1u;
	s_htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
	s_htim1.Init.Period            = 0xFFFFu;
	s_htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
	s_htim1.Init.RepetitionCounter = 0u;
	s_htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_IC_Init(&s_htim1) != HAL_OK) {
		printf("** KNX: TIM1 init FAILED\r\n");
		return;
	}

	ic.ICPolarity  = TIM_ICPOLARITY_BOTHEDGE;
	ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
	ic.ICPrescaler = TIM_ICPSC_DIV1;
	ic.ICFilter    = 0u;
	if (HAL_TIM_IC_ConfigChannel(&s_htim1, &ic, TIM_CHANNEL_3) != HAL_OK) {
		printf("** KNX: TIM1 CH3 config FAILED\r\n");
		return;
	}

	HAL_NVIC_SetPriority(TIM1_CC_IRQn, 5u, 0u);
	HAL_NVIC_EnableIRQ(TIM1_CC_IRQn);
	(void) HAL_TIM_IC_Start(&s_htim1, TIM_CHANNEL_3);
	__HAL_TIM_ENABLE_IT(&s_htim1, TIM_IT_CC3);

	printf("   TIM1 %" PRIu32 " Hz / prescaler %" PRIu32 " -> 1 us per tick\r\n",
	       clk, s_htim1.Init.Prescaler + 1u);
}

void TIM1_CC_IRQHandler(void)
{
	if ((TIM1->SR & TIM_SR_CC3IF) == 0u) {
		return;
	}

	uint16_t now = (uint16_t) TIM1->CCR3;
	TIM1->SR = ~(TIM_SR_CC3IF | TIM_SR_CC3OF);

	uint32_t level = KNX_RX_LEVEL() ? 1u : 0u;

	s_lastEdgeMs = HAL_GetTick();

	if (level != 0u) {
		/* Rising edge: the STKNX receiver reports an active pulse. */
		s_rxRise    = now;
		s_rxInPulse = 1u;
		s_rxPulses++;
		s_rxSeen    = 1u;

		uint32_t d = (uint32_t) ((uint16_t) (now - s_txStamp));
		if (d < KNX_BIT_US) {          /* ignore pulses we did not cause */
			if (d < s_dMin) { s_dMin = d; }
			if (d > s_dMax) { s_dMax = d; }
			s_dSum += d;
			s_dCnt++;
			knx_hist_add(s_dHist, KNX_D_EDGES, d);
		}

		uint16_t pn = (uint16_t) ((s_pulseHead + 1u) & (KNX_PULSE_RING - 1u));
		if (pn != s_pulseTail) {
			s_pulseRing[s_pulseHead] = now;
			s_pulseHead = pn;
		} else {
			s_pulseDropped++;
		}
	} else if (s_rxInPulse != 0u) {
		uint32_t w = (uint32_t) ((uint16_t) (now - s_rxRise));
		s_rxInPulse = 0u;
		if (w < (4u * KNX_BIT_US)) {
			if (w < s_wMin) { s_wMin = w; }
			if (w > s_wMax) { s_wMax = w; }
			s_wSum += w;
			s_wCnt++;
			knx_hist_add(s_wHist, KNX_W_EDGES, w);
		}
	} else {
		/* falling edge with no matching rise - nothing to measure */
	}

	if (s_edgeLog != 0u) {
		uint16_t en = (uint16_t) ((s_edgeHead + 1u) & (KNX_EDGE_RING - 1u));
		if (en != s_edgeTail) {
			s_edgeRing[s_edgeHead] = (level << 16) | (uint32_t) now;
			s_edgeHead = en;
		} else {
			s_edgeDropped++;
		}
	}
}

/* --- KNX_TX bit engine: TIM12_CH1 PWM ----------------------------------- */

static void knx_tx_start(void)
{
	GPIO_InitTypeDef   g  = { 0 };
	TIM_OC_InitTypeDef oc = { 0 };
	uint32_t clk    = knx_tim12_clk();
	uint32_t period = knx_us_to_ticks(clk, KNX_BIT_US);

	s_pulseTicks = knx_us_to_ticks(clk, KNX_PULSE_US);

	__HAL_RCC_TIM12_CLK_ENABLE();

	/* PWM mode 1 counting up: the output is high from the update event until
	 * the CC1 match, so CCR1 is the pulse width and CCR1 = 0 holds it low. */
	s_htim12.Instance               = TIM12;
	s_htim12.Init.Prescaler         = 0u;
	s_htim12.Init.CounterMode       = TIM_COUNTERMODE_UP;
	s_htim12.Init.Period            = period - 1u;
	s_htim12.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
	s_htim12.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
	if (HAL_TIM_PWM_Init(&s_htim12) != HAL_OK) {
		printf("** KNX: TIM12 init FAILED\r\n");
		return;
	}

	oc.OCMode     = TIM_OCMODE_PWM1;
	oc.Pulse      = 0u;                /* start silent */
	oc.OCPolarity = TIM_OCPOLARITY_HIGH;
	oc.OCFastMode = TIM_OCFAST_DISABLE;
	if (HAL_TIM_PWM_ConfigChannel(&s_htim12, &oc, TIM_CHANNEL_1) != HAL_OK) {
		printf("** KNX: TIM12 CH1 config FAILED\r\n");
		return;
	}

	/* Drop CCR1 preload so a write inside the update ISR applies to the bit
	 * period that is starting rather than the one after it. */
	TIM12->CCMR1 &= ~TIM_CCMR1_OC1PE;

	s_txMode = KNX_TX_IDLE;
	HAL_NVIC_SetPriority(TIM8_BRK_TIM12_IRQn, 5u, 0u);
	HAL_NVIC_EnableIRQ(TIM8_BRK_TIM12_IRQn);
	(void) HAL_TIM_PWM_Start(&s_htim12, TIM_CHANNEL_1);
	__HAL_TIM_ENABLE_IT(&s_htim12, TIM_IT_UPDATE);

	/* Only now hand the pin over: the channel is already driving it low. */
	g.Pin       = KNX_TX_PIN;
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_NOPULL;
	g.Speed     = GPIO_SPEED_FREQ_HIGH;
	g.Alternate = GPIO_AF2_TIM12;
	HAL_GPIO_Init(KNX_TX_PORT, &g);

	printf("   TIM12 %" PRIu32 " Hz -> bit %" PRIu32 " ticks (%u us), pulse "
	       "%" PRIu32 " ticks (%u us)\r\n",
	       clk, period, (unsigned) KNX_BIT_US,
	       s_pulseTicks, (unsigned) KNX_PULSE_US);
}

void TIM8_BRK_TIM12_IRQHandler(void)
{
	if ((TIM12->SR & TIM_SR_UIF) == 0u) {
		return;
	}
	TIM12->SR = ~TIM_SR_UIF;

	uint16_t now = (uint16_t) TIM1->CNT;   /* the bit period starts here */

	/* Status lines, sampled once per bit period. */
	{
		uint8_t st = (uint8_t) ((KNX_OK_LEVEL() ? 1u : 0u)
		                     | (KNX_VCC_OK_LEVEL() ? 2u : 0u));
		if (s_stValid == 0u) {
			s_stPrev  = st;
			s_stValid = 1u;
		} else if (st != s_stPrev) {
			s_stPrev = st;
			if (s_txMode == KNX_TX_IDLE) { s_stFlipsIdle++; } else { s_stFlips++; }
			uint16_t sn = (uint16_t) ((s_stHead + 1u) & (KNX_STATUS_RING - 1u));
			if (sn != s_stTail) {
				s_stRing[s_stHead] = ((uint32_t) st << 16) | (uint32_t) now;
				s_stHead = sn;
			}
		} else {
			/* unchanged */
		}
	}

	switch (s_txMode) {
	case KNX_TX_CONT:
		/* Judge the previous pulse before starting this one. */
		if (s_expectRx != 0u) {
			if (s_rxSeen == 0u) {
				s_misses++;
				if (s_missFirst == 0u) { s_missFirst = s_txPulses; }
				s_missLast = s_txPulses;
			}
		}
		s_rxSeen   = 0u;
		s_expectRx = 1u;
		s_txStamp  = now;
		s_txPulses++;
		break;

	case KNX_TX_SEQ:
		s_expectRx = 0u;
		if (s_txIdx < s_txLen) {
			uint8_t emit = s_txBits[s_txIdx];
			TIM12->CCR1 = emit ? s_pulseTicks : 0u;
			if (emit != 0u) {
				s_txStamp = now;
				s_txPulses++;
			}
			s_txIdx++;
		} else {
			TIM12->CCR1 = 0u;
			s_txMode    = KNX_TX_IDLE;
			s_txDone    = 1u;
		}
		break;

	case KNX_TX_IDLE:
	default:
		/* s_expectRx is deliberately left alone: knx_burst() judges the last
		 * pulse of a burst after the engine has already gone idle. */
		s_idleSamples++;
		if (KNX_RX_LEVEL()) { s_idleHighSamples++; }
		break;
	}
}

static void knx_tx_set_continuous(uint8_t on)
{
	if (on != 0u) {
		s_expectRx  = 0u;
		s_rxSeen    = 0u;
		TIM12->CCR1 = s_pulseTicks;
		s_txMode    = KNX_TX_CONT;
	} else {
		s_txMode    = KNX_TX_IDLE;
		TIM12->CCR1 = 0u;
	}
}

/* Queue a bit pattern (1 = active pulse) and wait for the engine to finish. */
static void knx_tx_send_bits(const uint8_t *bits, uint16_t len)
{
	uint32_t start;

	if (len > KNX_TX_BITS_MAX) {
		len = KNX_TX_BITS_MAX;
	}
	for (uint16_t i = 0u; i < len; i++) {
		s_txBits[i] = bits[i];
	}
	s_txLen  = len;
	s_txIdx  = 0u;
	s_txDone = 0u;
	s_txMode = KNX_TX_SEQ;

	start = HAL_GetTick();
	while ((s_txDone == 0u) && ((HAL_GetTick() - start) < 500u)) {
		/* the update ISR walks the pattern */
	}
}

/* --- Group-write frame --------------------------------------------------- */

static void knx_encode_char(uint8_t b, uint8_t *bits);   /* defined below */

#if KNX_TX_ENABLE

/* Builds an L_Data_Standard GroupValueWrite carrying a 6-bit small payload,
 * which is how DPT 1.001 switch commands travel. Returns the octet count. */
static uint8_t knx_build_group_write(uint8_t *f, uint16_t src, uint16_t ga,
                                     uint8_t value)
{
	uint8_t x = 0u;

	f[0] = 0xBCu;   /* standard frame | not repeated | broadcast | low prio */
	f[1] = (uint8_t) (src >> 8);
	f[2] = (uint8_t) (src & 0xFFu);
	f[3] = (uint8_t) (ga >> 8);
	f[4] = (uint8_t) (ga & 0xFFu);
	f[5] = 0xE1u;   /* group address | hop count 6 | APDU length 1 */
	f[6] = 0x00u;   /* TPCI: T_Data_Group */
	f[7] = (uint8_t) (0x80u | (value & 0x3Fu));   /* GroupValueWrite + value */

	for (uint8_t i = 0u; i < 8u; i++) { x ^= f[i]; }
	f[8] = (uint8_t) ~x;
	return 9u;
}

/* The whole frame goes to the bit engine in one piece, so the inter-character
 * gap stays at the two idle bits the encoding already carries instead of
 * whatever the main loop takes to re-arm. */
static void knx_send_frame(const uint8_t *f, uint8_t n)
{
	static uint8_t bits[KNX_TX_BITS_MAX];
	uint16_t nb = 0u;

	if (n > KNX_FRAME_MAX) { n = KNX_FRAME_MAX; }
	for (uint8_t i = 0u; i < n; i++) {
		knx_encode_char(f[i], &bits[nb]);
		nb = (uint16_t) (nb + KNX_CHAR_BITS);
	}
	knx_tx_send_bits(bits, nb);
}

/* TP1 forbids starting to send while the line is busy. Waits for the bus to be
 * quiet for KNX_ARB_IDLE_BITS bit periods, giving up after 200 ms so a stuck
 * bus cannot stall the test. Returns 1 if the line really was idle. */
static uint8_t knx_wait_bus_idle(void)
{
	uint32_t quietMs = ((KNX_ARB_IDLE_BITS * KNX_BIT_US) / 1000u) + 1u;
	uint32_t start   = HAL_GetTick();

	while ((HAL_GetTick() - start) < 200u) {
		if ((HAL_GetTick() - s_lastEdgeMs) >= quietMs) {
			return 1u;
		}
	}
	return 0u;
}

#endif /* KNX_TX_ENABLE */

/* --- TP1 character encode / decode -------------------------------------- */

static uint8_t knx_even_parity(uint8_t b)
{
	uint8_t p = 0u;
	for (uint8_t i = 0u; i < 8u; i++) {
		p = (uint8_t) (p ^ ((b >> i) & 1u));
	}
	return p;   /* 1 when the data holds an odd number of ones */
}

/* Logic 0 becomes an active pulse, logic 1 becomes a silent bit period. */
static void knx_encode_char(uint8_t b, uint8_t *bits)
{
	bits[0] = 1u;                                   /* start bit = logic 0 */
	for (uint8_t i = 0u; i < 8u; i++) {
		bits[1u + i] = (uint8_t) (((b >> i) & 1u) ? 0u : 1u);
	}
	bits[9]  = (uint8_t) (knx_even_parity(b) ? 0u : 1u);
	bits[10] = 0u;                                  /* stop bit = logic 1 */
	bits[11] = 0u;                                  /* idle */
	bits[12] = 0u;                                  /* idle */
}

typedef struct {
	uint8_t  active;
	uint16_t t0;
	uint16_t slots;
} knx_rx_char_t;

static knx_rx_char_t s_rxChar;

/* Frame lock. Without this the decoder treats any pulse as a start bit, so one
 * mis-lock inside a telegram misaligns every character after it - which reads
 * as an endless run of stop/parity errors. */
static uint16_t s_prevPulseT;
static uint8_t  s_havePrevPulse;
static uint8_t  s_syncNeeded;

static uint8_t knx_slot_level(uint16_t slots, uint8_t i)
{
	return (uint8_t) (((slots >> i) & 1u) ? 0u : 1u);
}

static uint8_t knx_decode_char(uint16_t slots, uint8_t *out,
                               uint8_t *startOk, uint8_t *parityOk,
                               uint8_t *stopOk)
{
	uint8_t b = 0u;

	for (uint8_t i = 0u; i < 8u; i++) {
		if (knx_slot_level(slots, (uint8_t) (1u + i)) != 0u) {
			b = (uint8_t) (b | (1u << i));
		}
	}
	*out      = b;
	*startOk  = (uint8_t) (knx_slot_level(slots, 0u) == 0u);
	*parityOk = (uint8_t) (knx_slot_level(slots, 9u) == knx_even_parity(b));
	*stopOk   = (uint8_t) (knx_slot_level(slots, 10u) == 1u);
	return (uint8_t) (*startOk && *parityOk && *stopOk);
}

static void knx_decoder_reset(void)
{
	s_rxChar.active = 0u;
	s_rxChar.slots  = 0u;
	s_pulseTail     = s_pulseHead;
	s_charHead      = 0u;
	s_charTail      = 0u;
	s_charDropped   = 0u;
	s_prevPulseT    = 0u;
	s_havePrevPulse = 0u;
	s_syncNeeded    = 1u;
}

/* Called after a frame fails its checks: drop it and refuse to open another
 * until the line has been quiet long enough to be certain of the next start
 * bit. */
static void knx_decoder_resync(void)
{
	s_rxChar.active = 0u;
	s_syncNeeded    = 1u;
}

static void knx_report_char(uint16_t slots)
{
	uint8_t b, startOk, parityOk, stopOk;
	uint8_t ok = knx_decode_char(slots, &b, &startOk, &parityOk, &stopOk);

	printf("[%8" PRIu32 " ms]  RX char 0x%02X  '%c'  %s",
	       HAL_GetTick(), b,
	       ((b >= 0x20u) && (b < 0x7Fu)) ? (char) b : '.',
	       ok ? "OK" : "BAD");
	if (ok == 0u) {
		printf(" (%s%s%s)", startOk ? "" : "start ",
		       parityOk ? "" : "parity ", stopOk ? "" : "stop");
	}
	printf("   slots=0x%04X\r\n", slots);
}

static uint8_t knx_decoder_poll(uint16_t *slots, uint16_t *t0)
{
	while (s_pulseTail != s_pulseHead) {
		uint16_t t = s_pulseRing[s_pulseTail];
		uint16_t next = (uint16_t) ((s_pulseTail + 1u) & (KNX_PULSE_RING - 1u));

		if (s_rxChar.active == 0u) {
			uint32_t gap = (s_havePrevPulse != 0u)
			             ? (uint32_t) ((uint16_t) (t - s_prevPulseT))
			             : 0xFFFFu;
			s_prevPulseT    = t;
			s_havePrevPulse = 1u;

			if ((s_syncNeeded != 0u) && (gap < KNX_RESYNC_GAP_US)) {
				s_pulseTail = next;   /* still mid-garbage, keep waiting */
				continue;
			}
			s_syncNeeded    = 0u;
			s_rxChar.active = 1u;
			s_rxChar.t0     = t;
			s_rxChar.slots  = 1u;      /* this pulse is the start bit */
			s_pulseTail     = next;
			continue;
		}

		uint32_t off = (uint32_t) ((uint16_t) (t - s_rxChar.t0));
		uint32_t idx = (off + (KNX_BIT_US / 2u)) / KNX_BIT_US;

		if (idx <= 10u) {
			s_rxChar.slots  = (uint16_t) (s_rxChar.slots | (1u << idx));
			s_prevPulseT    = t;
			s_havePrevPulse = 1u;
			s_pulseTail     = next;
			continue;
		}

		/* Past the stop bit: close this character; the next pass opens a new
		 * one on the same pulse. */
		*slots          = s_rxChar.slots;
		*t0             = s_rxChar.t0;
		s_rxChar.active = 0u;
		return 1u;
	}

	if (s_rxChar.active != 0u) {
		uint32_t age = (uint32_t) ((uint16_t) ((uint16_t) TIM1->CNT - s_rxChar.t0));
		if (age > (KNX_CHAR_BITS * KNX_BIT_US)) {
			*slots          = s_rxChar.slots;
			*t0             = s_rxChar.t0;
			s_rxChar.active = 0u;
			return 1u;
		}
	}
	return 0u;
}

/* --- Frame assembly: characters back into an L_Data frame ---------------- */

static uint8_t  s_frBuf[KNX_FRAME_MAX];
static uint8_t  s_frLen;
static uint16_t s_frPrevT0;
static uint8_t  s_frActive;
static uint32_t s_frLastMs;
static uint32_t s_frPartial;

static void knx_frame_reset(void)
{
	s_frLen    = 0u;
	s_frActive = 0u;
}

/* Feeds one decoded character in. Characters inside a frame are exactly
 * KNX_CHAR_BITS apart, so anything outside that window starts a new frame.
 * Returns the octet count once a frame is complete, else 0. */
static uint8_t knx_frame_feed(uint8_t b, uint16_t t0)
{
	if (s_frActive != 0u) {
		uint32_t gap = (uint32_t) ((uint16_t) (t0 - s_frPrevT0));
		if ((gap < (11u * KNX_BIT_US)) || (gap > (17u * KNX_BIT_US))) {
			if (s_frLen != 0u) { s_frPartial++; }
			s_frActive = 0u;
			s_frLen    = 0u;
		}
	}
	if (s_frActive == 0u) { s_frActive = 1u; s_frLen = 0u; }

	if (s_frLen < KNX_FRAME_MAX) { s_frBuf[s_frLen++] = b; }
	s_frPrevT0 = t0;
	s_frLastMs = HAL_GetTick();

	/* Octet 5 low nibble is the APDU length; a standard frame is 8 + that. */
	if (s_frLen >= 6u) {
		uint8_t total = (uint8_t) (8u + (s_frBuf[5] & 0x0Fu));
		if ((total <= KNX_FRAME_MAX) && (s_frLen >= total)) {
			s_frActive = 0u;
			return total;
		}
	}
	return 0u;
}

/* Abandons a frame that stopped mid-way, so the next one starts clean. */
static void knx_frame_timeout(void)
{
	if ((s_frActive != 0u) && ((HAL_GetTick() - s_frLastMs) > 30u)) {
		if (s_frLen != 0u) { s_frPartial++; }
		s_frActive = 0u;
		s_frLen    = 0u;
	}
}

static void knx_frame_report(const uint8_t *f, uint8_t n, uint8_t isEcho)
{
	uint8_t x = 0u;
	uint8_t crcOk;

	for (uint8_t i = 0u; (i + 1u) < n; i++) { x ^= f[i]; }
	x = (uint8_t) (x ^ 0xFFu);                 /* the check octet is inverted */
	crcOk = (f[n - 1u] == x) ? 1u : 0u;

	printf("[%8" PRIu32 " ms]  %s FRAME ", HAL_GetTick(),
	       isEcho ? "TX-echo" : "BUS    ");
	for (uint8_t i = 0u; i < n; i++) { printf(" %02X", f[i]); }
	printf("  %s", crcOk ? "crc OK " : "crc BAD");

	if ((crcOk != 0u) && (n >= 9u)) {
		uint16_t sa = (uint16_t) (((uint16_t) f[1] << 8) | f[2]);
		uint16_t da = (uint16_t) (((uint16_t) f[3] << 8) | f[4]);

		printf("  %u.%u.%u ->", (unsigned) ((sa >> 12) & 0x0Fu),
		       (unsigned) ((sa >> 8) & 0x0Fu), (unsigned) (sa & 0xFFu));
		if ((f[5] & 0x80u) != 0u) {
			printf(" %u/%u/%u", (unsigned) ((da >> 11) & 0x1Fu),
			       (unsigned) ((da >> 8) & 0x07u), (unsigned) (da & 0xFFu));
		} else {
			printf(" %u.%u.%u", (unsigned) ((da >> 12) & 0x0Fu),
			       (unsigned) ((da >> 8) & 0x0Fu), (unsigned) (da & 0xFFu));
		}
		if ((f[6] & 0xFCu) == 0x00u) {
			if ((f[7] & 0xC0u) == 0x80u) {
				printf("  GroupValueWrite = %u", (unsigned) (f[7] & 0x3Fu));
			} else if (f[7] == 0x00u) {
				printf("  GroupValueRead");
			} else if ((f[7] & 0xC0u) == 0x40u) {
				printf("  GroupValueResponse = %u", (unsigned) (f[7] & 0x3Fu));
			} else {
				printf("  APCI %02X%02X", f[6], f[7]);
			}
		}
	}
	printf("\r\n");
}

/* --- Statistics --------------------------------------------------------- */

static void knx_stats_reset(void)
{
	s_txPulses = 0u;
	s_rxPulses = 0u;
	s_misses   = 0u;
	s_missFirst = 0u;
	s_missLast  = 0u;
	s_wMin = 0xFFFFFFFFu; s_wMax = 0u; s_wSum = 0u; s_wCnt = 0u;
	s_dMin = 0xFFFFFFFFu; s_dMax = 0u; s_dSum = 0u; s_dCnt = 0u;
	for (uint8_t i = 0u; i < KNX_HIST_BINS; i++) { s_wHist[i] = 0u; s_dHist[i] = 0u; }
	s_idleHighSamples = 0u;
	s_idleSamples     = 0u;
	/* Status-line flips are NOT cleared here: they accumulate across the whole
	 * round so the diagnosis can tell transmit-time flips from idle ones. */
	s_stHead = 0u;
	s_stTail = 0u;
}

static void knx_stats_line(void)
{
	uint32_t wc = s_wCnt, dc = s_dCnt;

	printf("[%8" PRIu32 " ms]  TX=%" PRIu32 "  RX=%" PRIu32 "  miss=%" PRIu32,
	       HAL_GetTick(), s_txPulses, s_rxPulses, s_misses);
	if (wc != 0u) {
		printf("  width %" PRIu32 "/%" PRIu32 "/%" PRIu32 " us",
		       s_wMin, s_wSum / wc, s_wMax);
	}
	if (dc != 0u) {
		printf("  delay %" PRIu32 "/%" PRIu32 "/%" PRIu32 " us",
		       s_dMin, s_dSum / dc, s_dMax);
	}
	printf("\r\n");
}

static void knx_status_flush(void)
{
	while (s_stTail != s_stHead) {
		uint32_t e = s_stRing[s_stTail];
		s_stTail = (uint16_t) ((s_stTail + 1u) & (KNX_STATUS_RING - 1u));
		printf("   ** status line changed: KNX_OK=%s KNX_VCC_OK=%s "
		       "(TIM1 t=%" PRIu32 ")\r\n",
		       (((e >> 16) & 1u) != 0u) ? "HIGH" : "LOW",
		       (((e >> 16) & 2u) != 0u) ? "HIGH" : "LOW",
		       e & 0xFFFFu);
	}
}

/* --- Bus state ---------------------------------------------------------- */

static knx_bus_t knx_bus_state(uint8_t *vccOk, uint8_t *busOk, uint8_t *rxIdle)
{
	uint8_t v = KNX_VCC_OK_LEVEL() ? 1u : 0u;
	uint8_t k = KNX_OK_LEVEL()     ? 1u : 0u;
	uint8_t r = KNX_RX_LEVEL()     ? 1u : 0u;

	*vccOk = v; *busOk = k; *rxIdle = r;

	if ((v != 0u) && (k != 0u) && (r == 0u)) { return KNX_BUS_OK;   }
	if ((v == 0u) && (k == 0u) && (r != 0u)) { return KNX_BUS_DEAD; }
	return KNX_BUS_ODD;
}

static void knx_bus_print(knx_bus_t st, uint8_t v, uint8_t k, uint8_t r)
{
	printf("   KNX_VCC_OK(PH12)=%s  KNX_OK(PD7)=%s  KNX_RX(PA10)=%s   ->  %s\r\n",
	       v ? "HIGH" : "LOW ", k ? "HIGH" : "LOW ", r ? "HIGH" : "LOW ",
	       (st == KNX_BUS_OK)   ? "bus powered, transceiver idle"
	     : (st == KNX_BUS_DEAD) ? "NO BUS POWER - STKNX is unpowered"
	                            : "UNEXPECTED combination");
}

static void knx_heartbeat(void)
{
	HAL_GPIO_TogglePin(KNX_PROG_LED_PORT, KNX_PROG_LED_PIN);
}

/* --- Is anything actually driving these pins? --------------------------- */

/* U14/U15 on the Upper Deck are push-pull CMOS, so they hold their level
 * against the MCU's ~40 kohm internal pulls. A pin that follows both pulls has
 * nothing on the far end - the deck is not mated, or its 3V3 is missing. */
static uint8_t knx_pin_driven(GPIO_TypeDef *port, uint32_t pinNr)
{
	uint32_t save = port->PUPDR;
	uint32_t mask = 3u << (pinNr * 2u);
	uint8_t  followedUp, followedDown;

	port->PUPDR = (save & ~mask) | (1u << (pinNr * 2u));   /* pull-up */
	HAL_Delay(1u);
	followedUp = ((port->IDR & (1u << pinNr)) != 0u) ? 1u : 0u;

	port->PUPDR = (save & ~mask) | (2u << (pinNr * 2u));   /* pull-down */
	HAL_Delay(1u);
	followedDown = ((port->IDR & (1u << pinNr)) == 0u) ? 1u : 0u;

	port->PUPDR = save;
	return (uint8_t) ((followedUp && followedDown) ? 0u : 1u);
}

static void knx_presence_report(void)
{
	uint8_t okDrv  = knx_pin_driven(KNX_OK_PORT, 7u);
	uint8_t vccDrv = knx_pin_driven(KNX_VCC_OK_PORT, 12u);

	printf("   Upper Deck presence: KNX_OK(PD7) %s, KNX_VCC_OK(PH12) %s\r\n",
	       okDrv  ? "DRIVEN" : "FLOATING",
	       vccDrv ? "DRIVEN" : "FLOATING");

	if ((okDrv != 0u) && (vccDrv != 0u)) {
		printf("      -> the deck is mated and U14/U15 are powered, so their "
		       "LOW output is real: the STKNX side genuinely has no bus "
		       "power.\r\n");
	} else if ((okDrv == 0u) && (vccDrv == 0u)) {
		printf("      -> nothing is driving either pin, so every KNX_OK and "
		       "KNX_VCC_OK reading in this log is a floating input, not a "
		       "status line, and any \"flapping\" is just noise. U14/U15 run "
		       "off the DIGITAL 3V3, not the bus, so this means the Upper Deck "
		       "is not mated (J2 on the Bridge to J8 on the deck) or the "
		       "deck's 3V3 is missing.\r\n"
		       "      -> the same missing 3V3 would also leave U13's output "
		       "stage unpowered, which is exactly why KNX_RX never moves. Fix "
		       "this before believing anything else here.\r\n");
	} else {
		printf("      -> one driven, one floating: a single broken connection "
		       "on J2/J8 or one dead inverter (U14 = VCC_OK, U15 = "
		       "KNX_OK).\r\n");
	}
}

/* --- P0: bus and pin self-check ----------------------------------------- */

static knx_bus_t knx_p0_selfcheck(void)
{
	uint8_t v, k, r;
	knx_bus_t st = knx_bus_state(&v, &k, &r);
	knx_bus_t last = st;
	uint32_t start = HAL_GetTick();

	printf("\r\n--- P0: bus and pin self-check ---\r\n");
	knx_bus_print(st, v, k, r);

	if (st == KNX_BUS_OK) {
		return st;
	}

	printf("   Waiting up to %u s for KNX bus power (21-32 V on Klemmblock "
	       "C03/C04)...\r\n", (unsigned) (KNX_P0_WAIT_MS / 1000u));

	while ((HAL_GetTick() - start) < KNX_P0_WAIT_MS) {
		st = knx_bus_state(&v, &k, &r);
		if (st != last) {
			last = st;
			knx_bus_print(st, v, k, r);
		}
		if (st == KNX_BUS_OK) {
			return st;
		}
		HAL_Delay(100u);
		knx_heartbeat();
	}

	printf("   ** Continuing without bus power. P1/P2 still drive KNX_TX so a "
	       "scope on PB14 is meaningful; nothing on KNX_RX is. **\r\n");
	return st;
}

/* --- P1: one pulse per second, full KNX_RX response ---------------------- */

static void knx_p1_single_pulse(void)
{
	static const uint8_t onePulse[1] = { 1u };

	printf("\r\n--- P1: %u single %u us pulses, one every %u s, KNX_RX "
	       "response edge by edge ---\r\n", (unsigned) KNX_P1_SHOTS,
	       (unsigned) KNX_PULSE_US, (unsigned) (KNX_TX_EVERY_MS / 1000u));

	for (uint8_t shot = 0u; shot < KNX_P1_SHOTS; shot++) {
		s_edgeHead    = s_edgeTail;
		s_edgeDropped = 0u;
		knx_stats_reset();
		s_edgeLog = 1u;

		knx_tx_send_bits(onePulse, 1u);
		HAL_Delay(5u);           /* let the whole response land */
		s_edgeLog = 0u;

		uint16_t tx = s_txStamp;
		printf("[%8" PRIu32 " ms]  TX pulse at t=0", HAL_GetTick());
		if (s_edgeTail == s_edgeHead) {
			printf("   ->  KNX_RX did not move (stayed %s)\r\n",
			       KNX_RX_LEVEL() ? "HIGH" : "LOW");
		} else {
			printf("   ->  KNX_RX:");
			while (s_edgeTail != s_edgeHead) {
				uint32_t e = s_edgeRing[s_edgeTail];
				s_edgeTail = (uint16_t) ((s_edgeTail + 1u) & (KNX_EDGE_RING - 1u));
				printf("  %s@%+d us", ((e >> 16) != 0u) ? "H" : "L",
				       (int) (int16_t) ((uint16_t) (e & 0xFFFFu) - tx));
			}
			printf("\r\n");
		}

		knx_heartbeat();
		HAL_Delay(KNX_TX_EVERY_MS);
	}
}

/* --- P2: pulse bursts, loss rate and histograms ------------------------- */

/* Fires `pulses` back-to-back active pulses without clearing the statistics,
 * so several bursts accumulate into one result. A burst of 200 is 21 ms of
 * dominant state - short enough to leave a live bus usable. */
static void knx_p2_burst(uint32_t pulses)
{
	uint32_t target = s_txPulses + pulses;
	uint32_t start  = HAL_GetTick();

	knx_tx_set_continuous(1u);
	while ((s_txPulses < target) && ((HAL_GetTick() - start) < 1000u)) {
		/* the update ISR does the work */
	}
	knx_tx_set_continuous(0u);

	/* The update ISR judges each pulse when the next one starts, so the last
	 * pulse of a burst has nobody to judge it. Do it here, once its response
	 * has had a couple of bit periods to arrive - otherwise every burst
	 * reports one fewer miss than it really had. */
	HAL_Delay(1u);
	if ((s_expectRx != 0u) && (s_rxSeen == 0u)) {
		s_misses++;
		if (s_missFirst == 0u) { s_missFirst = s_txPulses; }
		s_missLast = s_txPulses;
	}
	s_expectRx = 0u;
	HAL_Delay(1u);
}

static void knx_p2_long_run(void)
{
	printf("\r\n--- P2: %u bursts of %u pulses (%u us high every %u us), one "
	       "burst every %u s ---\r\n",
	       (unsigned) KNX_P2_BURSTS, (unsigned) KNX_P2_BURST_PULSES,
	       (unsigned) KNX_PULSE_US, (unsigned) KNX_BIT_US,
	       (unsigned) (KNX_TX_EVERY_MS / 1000u));

	s_edgeLog = 0u;
	knx_stats_reset();

	for (uint8_t b = 0u; b < KNX_P2_BURSTS; b++) {
		knx_p2_burst(KNX_P2_BURST_PULSES);
		knx_stats_line();
		knx_status_flush();
		knx_heartbeat();
		HAL_Delay(KNX_TX_EVERY_MS);
	}

	s_round.p2Tx   = s_txPulses;
	s_round.p2Rx   = s_rxPulses;
	s_round.p2Miss = s_misses;
	s_round.p2wCnt = s_wCnt;
	s_round.p2wAvg = (s_wCnt != 0u) ? (s_wSum / s_wCnt) : 0u;
	s_round.p2dAvg = (s_dCnt != 0u) ? (s_dSum / s_dCnt) : 0u;

	printf("   --- P2 result ---\r\n");
	knx_stats_line();
	if (s_misses != 0u) {
		uint32_t ppm = (s_txPulses != 0u)
		             ? (uint32_t) (((uint64_t) s_misses * 1000000u) / s_txPulses)
		             : 0u;
		printf("   LOST PULSES: %" PRIu32 " of %" PRIu32 "  (%" PRIu32 " ppm), "
		       "first at pulse #%" PRIu32 ", last at #%" PRIu32 "\r\n",
		       s_misses, s_txPulses, ppm, s_missFirst, s_missLast);
	} else {
		printf("   no lost pulses\r\n");
	}
	knx_hist_print("KNX_RX pulse width", "us", s_wHist, KNX_W_EDGES);
	knx_hist_print("TX->RX delay", "us", s_dHist, KNX_D_EDGES);
	knx_status_flush();
}

/* --- P3: send a TP1 character and decode the loopback -------------------- */

static void knx_p3_loopback(void)
{
	static const uint8_t testBytes[] = { 0xAAu, 0x55u, 0x00u, 0xFFu, 0x5Au };
	uint8_t bits[KNX_CHAR_BITS];
	uint8_t passes = 0u;

	printf("\r\n--- P3: TP1 character loopback (MCU -> STKNX -> bus -> STKNX "
	       "-> MCU) ---\r\n");

	s_edgeLog = 0u;

	for (uint8_t i = 0u; i < (uint8_t) sizeof(testBytes); i++) {
		uint8_t  want  = testBytes[i];
		uint16_t slots  = 0u;
		uint16_t charT0 = 0u;
		uint8_t  got    = 0u;
		uint32_t waitStart;
		uint8_t  closed = 0u;

		knx_decoder_reset();
		knx_stats_reset();
		knx_encode_char(want, bits);
		knx_tx_send_bits(bits, KNX_CHAR_BITS);

		waitStart = HAL_GetTick();
		while ((HAL_GetTick() - waitStart) < 20u) {
			if (knx_decoder_poll(&slots, &charT0) != 0u) {
				closed = 1u;
				break;
			}
		}

		printf("   sent 0x%02X  ->  ", want);
		if (closed == 0u) {
			printf("nothing decoded (RX pulses seen: %" PRIu32 ")\r\n",
			       s_rxPulses);
		} else {
			uint8_t sOk, pOk, tOk;
			uint8_t good = knx_decode_char(slots, &got, &sOk, &pOk, &tOk);
			printf("got 0x%02X  slots=0x%04X  %s\r\n", got, slots,
			       (good && (got == want)) ? "PASS"
			     : good                    ? "FAIL (framing ok, byte differs)"
			                               : "FAIL (framing/parity)");
			if (good && (got == want)) { passes++; }
		}
		HAL_Delay(KNX_TX_EVERY_MS);
	}

	s_round.p3Pass  = passes;
	s_round.p3Total = (uint8_t) sizeof(testBytes);
	printf("   %u/%u passed\r\n", (unsigned) passes,
	       (unsigned) sizeof(testBytes));
	knx_heartbeat();
}

/* --- P4: passive listen -------------------------------------------------- */

static void knx_p4_listen(void)
{
	uint32_t start = HAL_GetTick();
	uint32_t tick  = start;
	uint32_t chars = 0u, bad = 0u, strays = 0u;
	uint32_t secChars = 0u, secBad = 0u, secStrays = 0u;
	uint32_t charBudget = KNX_P4_CHARS_PER_S;
	uint32_t edgeBudget = KNX_P4_EDGES_PER_S;
	uint32_t edgeSkipped = 0u;
	uint16_t prev = 0u;
	uint8_t  havePrev = 0u;
	uint8_t  onLine = 0u;

	printf("\r\n--- P4: passive listen for %u s, KNX_TX parked low ---\r\n",
	       (unsigned) (KNX_P4_MS / 1000u));
	printf("   Well-formed characters print as they arrive. Lone pulses and "
	       "malformed frames are counted, not printed.\r\n");

	knx_decoder_reset();
	knx_stats_reset();
	s_pulseDropped = 0u;
	s_edgeHead    = s_edgeTail;
	s_edgeDropped = 0u;
	s_edgeLog     = 1u;

	while ((HAL_GetTick() - start) < KNX_P4_MS) {
		uint16_t slots;
		uint16_t charT0;

		/* 1. Drain the decoder flat out. Never print inside this loop: a
		 *    single 60-character line costs 5 ms at 115200 baud, which is
		 *    48 bit periods, and the pulse ring only holds 64 entries. */
		while (knx_decoder_poll(&slots, &charT0) != 0u) {
			if (slots == 1u) {
				secStrays++;      /* one pulse alone is not a character */
				continue;
			}
			uint16_t n = (uint16_t) ((s_charHead + 1u) & (KNX_CHAR_RESULT_RING - 1u));
			if (n != s_charTail) {
				s_charRing[s_charHead] = slots;
				s_charHead = n;
			} else {
				s_charDropped++;
			}
		}

		/* 2. Characters get the console first. */
		while ((s_charTail != s_charHead) && (charBudget != 0u)) {
			uint16_t sl = s_charRing[s_charTail];
			uint8_t  b, sOk, pOk, tOk;
			s_charTail = (uint16_t) ((s_charTail + 1u) & (KNX_CHAR_RESULT_RING - 1u));
			charBudget--;
			if (onLine != 0u) { printf("\r\n"); onLine = 0u; }
			if (knx_decode_char(sl, &b, &sOk, &pOk, &tOk) != 0u) {
				secChars++;
			} else {
				secBad++;
			}
			knx_report_char(sl);
		}

		/* 3. Raw edges only while nothing decodes - once characters come out
		 *    they say everything the edges would. */
		while (s_edgeTail != s_edgeHead) {
			uint32_t e = s_edgeRing[s_edgeTail];
			s_edgeTail = (uint16_t) ((s_edgeTail + 1u) & (KNX_EDGE_RING - 1u));
			if ((chars != 0u) || (secChars != 0u) || (edgeBudget == 0u)) {
				edgeSkipped++;
				continue;
			}
			edgeBudget--;

			uint16_t t = (uint16_t) (e & 0xFFFFu);
			uint32_t d = havePrev ? (uint32_t) ((uint16_t) (t - prev)) : 0u;
			prev = t; havePrev = 1u;

			if (onLine == 0u) { printf("[%8" PRIu32 " ms]  EDGE", HAL_GetTick()); }
			printf("  %s:%" PRIu32 "us", ((e >> 16) != 0u) ? "H" : "L", d);
			onLine++;
			if (onLine == 8u) { printf("\r\n"); onLine = 0u; }
		}

		/* 4. One summary line a second. */
		if ((HAL_GetTick() - tick) >= 1000u) {
			tick  += 1000u;
			knx_heartbeat();
			if (onLine != 0u) { printf("\r\n"); onLine = 0u; }

			if ((secChars | secBad | secStrays) != 0u) {
				printf("[%8" PRIu32 " ms]  1 s: %" PRIu32 " good, %" PRIu32
				       " malformed, %" PRIu32 " lone pulses, %" PRIu32
				       " pulses total\r\n", HAL_GetTick(), secChars, secBad,
				       secStrays, s_rxPulses);
			} else {
				printf("[%8" PRIu32 " ms]  quiet - KNX_RX steady %s\r\n",
				       HAL_GetTick(), KNX_RX_LEVEL() ? "HIGH" : "LOW");
			}

			if ((s_pulseDropped | s_charDropped | edgeSkipped | s_edgeDropped) != 0u) {
				printf("            not shown: %" PRIu32 " pulses lost while "
				       "the console was busy, %" PRIu32 " characters over the "
				       "print cap, %" PRIu32 " edges suppressed\r\n",
				       s_pulseDropped, s_charDropped,
				       edgeSkipped + s_edgeDropped);
				if (s_pulseDropped != 0u) {
					printf("            ^ pulses lost here are a CONSOLE limit, "
					       "not a bus fault; the lone-pulse count above is "
					       "inflated by exactly this.\r\n");
				}
				s_pulseDropped = 0u; s_charDropped = 0u;
				edgeSkipped = 0u; s_edgeDropped = 0u;
			}

			chars  += secChars;  bad += secBad;  strays += secStrays;
			secChars = 0u; secBad = 0u; secStrays = 0u;
			charBudget = KNX_P4_CHARS_PER_S;
			edgeBudget = KNX_P4_EDGES_PER_S;
		}
	}

	if (onLine != 0u) { printf("\r\n"); }
	s_edgeLog = 0u;
	chars += secChars;  bad += secBad;  strays += secStrays;

	s_round.p4Chars  = chars;
	s_round.p4Pulses = s_rxPulses;
	s_round.idleHighPermille = (s_idleSamples != 0u)
	                         ? ((s_idleHighSamples * 1000u) / s_idleSamples) : 0u;

	printf("   P4 total: %" PRIu32 " good characters, %" PRIu32 " malformed, "
	       "%" PRIu32 " lone pulses, %" PRIu32 " pulses in %u s.\r\n",
	       chars, bad, strays, s_rxPulses, (unsigned) (KNX_P4_MS / 1000u));
	printf("   KNX_RX was high in %" PRIu32 ".%" PRIu32 "%% of %" PRIu32
	       " idle samples\r\n",
	       s_round.idleHighPermille / 10u, s_round.idleHighPermille % 10u,
	       s_idleSamples);
	knx_status_flush();
}

/* --- P5: does the internal pull-up matter? ------------------------------ */

static void knx_p5_pullup_ab(void)
{
	printf("\r\n--- P5: KNX_RX internal pull-up A/B (%u pulses per leg) ---\r\n",
	       (unsigned) KNX_P5_BURST_PULSES);
	printf("   U13's TLP2362 output is open collector and the board has no "
	       "pull-up on /KNX_RX.\r\n");

	for (uint8_t leg = 0u; leg < 2u; leg++) {
		uint8_t on = (leg == 0u) ? 1u : 0u;

		knx_rx_pullup(on);
		HAL_Delay(2u);
		knx_stats_reset();
		s_edgeLog = 0u;
		knx_p2_burst(KNX_P5_BURST_PULSES);

		uint32_t wAvg = (s_wCnt != 0u) ? (s_wSum / s_wCnt) : 0u;
		printf("   PUPDR=%-8s TX=%" PRIu32 "  RX=%" PRIu32 "  miss=%" PRIu32
		       "  width avg=%" PRIu32 " us  max=%" PRIu32 " us%s\r\n",
		       on ? "PULLUP" : "NOPULL", s_txPulses, s_rxPulses, s_misses,
		       wAvg, (s_wMax != 0u) ? s_wMax : 0u,
		       (s_rxPulses == 0u) ? "   INCONCLUSIVE - no RX at all" : "");

		if (on != 0u) {
			s_round.p5MissPullup = s_misses;
			s_round.p5wAvgPullup = wAvg;
		} else {
			s_round.p5MissNopull = s_misses;
			s_round.p5wAvgNopull = wAvg;
		}
	}

	knx_rx_pullup(1u);          /* leave it enabled - the board needs it */
	knx_heartbeat();
}

/* --- Diagnosis: symptom -> what to measure ------------------------------ */

static void knx_diagnose(void)
{
	const knx_round_t *r = &s_round;
	uint8_t issues = 0u;

	s_round.statusFlips     = s_stFlips;
	s_round.statusFlipsIdle = s_stFlipsIdle;

	printf("\r\n===== DIAGNOSIS =====\r\n");

	/* --- Bus power ------------------------------------------------------ */
	if (r->bus == KNX_BUS_DEAD) {
		printf("\r\n[1] NO BUS POWER. STKNX takes all its power from the bus "
		       "(pin 11 KNX_A), so the receive chain cannot produce anything "
		       "at all and nothing below this line means a thing.\r\n");
		knx_presence_report();
		printf("    CHECK IN THIS ORDER:\r\n"
		       "      1. KNX+ / KNX- POLARITY at C03/C04. D31 (datasheet D1, "
		       "\"protection from reverse polarity connection\") blocks a "
		       "reversed bus outright - no damage, but the board stays "
		       "completely dead, exactly as seen here. Swap the two wires and "
		       "watch this line: it updates within a second.\r\n"
		       "      2. Is this board wired to the bus you are talking on at "
		       "all? C03 = KNX+, C04 = KNX-.\r\n"
		       "      3. MEASURE, meter's black lead on KNX- (C04), NOT on "
		       "digital ground:\r\n"
		       "           C03 to C04            expect 21 V .. 32 V DC\r\n"
		       "           C19 (VCCCORE) to KNX- expect 3.3 V\r\n"
		       "           C27 (VDDHV)   to KNX- expect bus minus a few V\r\n"
		       "         Bus voltage present but VCCCORE 0 V points at the "
		       "STKNX or its network: R85 68R, D30/D31, C22, C27.\r\n");
		printf("\r\n=====================\r\n");
		return;
	}

	if (r->bus == KNX_BUS_ODD) {
		issues++;
		printf("\r\n[1] UNEXPECTED PIN COMBINATION (VCC_OK=%u KNX_OK=%u "
		       "RX_idle=%u). Expected 1/1/0 (bus up) or 0/0/1 (bus down).\r\n",
		       (unsigned) r->vccOk, (unsigned) r->busOk, (unsigned) r->rxIdle);
		if ((r->vccOk != 0u) && (r->busOk != 0u)) {
			printf("    Both power-good lines are high, so VCCCORE exists and "
			       "the STKNX is alive - but KNX_RX is stuck high, which is "
			       "the receive chain producing nothing. See [2].\r\n");
		} else {
			printf("    VCC_OK and KNX_OK each run through their own Q6/Q7 -> "
			       "U10/U11 -> U14/U15 chain, so one line low on its own "
			       "points at that chain.\r\n"
			       "    MEASURE: C19 to KNX- (VCCCORE, expect 3.3 V) and C20 "
			       "to KNX- (VREF, must exceed 13.5 V for KNX_OK to rise).\r\n");
		}
	}

	/* --- KNX_RX stuck high --------------------------------------------- */
	if ((r->vccOk != 0u) && (r->busOk != 0u) && (r->idleHighPermille > 100u)) {
		issues++;
		printf("\r\n[2] KNX_RX SAT HIGH %" PRIu32 ".%" PRIu32 "%% OF IDLE TIME. "
		       "With the bus up and nobody transmitting it should be LOW, "
		       "because STKNX pin 23 is low and lights U13's LED.\r\n"
		       "    MEASURE:\r\n"
		       "      a) C19 to KNX-           expect 3.3 V (feeds U13's LED "
		       "through R89)\r\n"
		       "      b) voltage across R89    expect ~1.8 V, i.e. ~5.4 mA\r\n"
		       "      c) scope U13 pin 5 / PA10 - is it being pulled down at all?\r\n"
		       "    Zero across R89 with 3.3 V present means STKNX pin 23 is "
		       "high (receiver not seeing the bus) or U13 is dead.\r\n",
		       r->idleHighPermille / 10u, r->idleHighPermille % 10u);
	}

	/* --- Transmit chain ------------------------------------------------- */
	if ((r->p2Tx != 0u) && (r->p2Rx == 0u)) {
		issues++;
		printf("\r\n[3] TX RUNS BUT KNX_RX NEVER RESPONDS (%" PRIu32 " pulses "
		       "sent, 0 seen). The loop is broken somewhere.\r\n"
		       "    SCOPE these four points in order; the first one that is "
		       "wrong is the fault:\r\n"
		       "      1. PB14                  expect %u us high every %u us\r\n"
		       "      2. U12 pin 5 / STKNX 24  same pulse, 0 .. 3.3 V "
		       "(isolated side)\r\n"
		       "      3. bus C03-C04           expect a 6 V .. 9 V dip lasting "
		       "%u us\r\n"
		       "      4. STKNX pin 23 / U13.3  expect a pulse\r\n"
		       "    1 wrong -> MCU/timer. 2 wrong -> R88 / U12 / R94. 3 wrong "
		       "-> STKNX transmitter, R85 68R 1W, D31. 4 wrong -> STKNX "
		       "receiver, C23 (CAC), D29.\r\n",
		       r->p2Tx, (unsigned) KNX_PULSE_US, (unsigned) KNX_BIT_US,
		       (unsigned) KNX_PULSE_US);
	}

	/* --- Intermittent loss --------------------------------------------- */
	if (r->p2Miss != 0u) {
		uint32_t ppm = (r->p2Tx != 0u)
		             ? (uint32_t) (((uint64_t) r->p2Miss * 1000000u) / r->p2Tx)
		             : 0u;
		issues++;
		printf("\r\n[4] INTERMITTENT PULSE LOSS: %" PRIu32 " ppm.\r\n"
		       "    The four TLP2362 optocouplers are driven through 332 R "
		       "from 3.3 V, i.e. ~5.4 mA typical and ~4.3 mA worst case, "
		       "against a datasheet recommended IF(ON) of 7.5 mA minimum and "
		       "a guaranteed switching threshold of 5.0 mA maximum. Loss here "
		       "is the expected symptom.\r\n"
		       "    MEASURE:\r\n"
		       "      a) voltage across R88 (TX side, from digital 3V3)\r\n"
		       "      b) voltage across R89 (RX side, from VCCCORE)\r\n"
		       "      current = V / 332 R; anything under 7.5 mA is out of "
		       "spec.\r\n"
		       "      c) C19 to KNX- while P2 runs - VCCCORE sagging under "
		       "three simultaneous LEDs would show here (IREG limit is "
		       "20 mA).\r\n", ppm);
	}

	/* --- Pulse width fidelity ------------------------------------------ */
	if ((r->p2wCnt != 0u) && ((r->p2wAvg < 28u) || (r->p2wAvg > 45u))) {
		issues++;
		printf("\r\n[5] KNX_RX PULSE WIDTH AVERAGES %" PRIu32 " us, expected "
		       "near %u us.\r\n"
		       "    Too short usually means a slow rising edge: /KNX_RX has no "
		       "board pull-up, so the edge is shaped by the MCU's ~40 kohm "
		       "internal one.\r\n"
		       "    SCOPE PA10 and measure the rise time. Over ~3 us confirms "
		       "it; fit an external 4.7 kohm pull-up from /KNX_RX to +3V3 and "
		       "re-run.\r\n", r->p2wAvg, (unsigned) KNX_PULSE_US);
	}

	/* --- Pull-up A/B verdict ------------------------------------------- */
	if ((r->p5MissNopull > (r->p5MissPullup + 10u))
	 || ((r->p5wAvgPullup != 0u) && (r->p5wAvgNopull != 0u)
	  && ((r->p5wAvgPullup - r->p5wAvgNopull) > 4u))) {
		issues++;
		printf("\r\n[6] THE MISSING /KNX_RX PULL-UP IS MEASURABLE. miss "
		       "%" PRIu32 " with the internal pull-up vs %" PRIu32 " without; "
		       "width %" PRIu32 " us vs %" PRIu32 " us.\r\n"
		       "    Nothing to measure - this is proof. Fit an external "
		       "4.7 kohm from /KNX_RX to +3V3, matching R92/R93/R94 on the "
		       "other three couplers.\r\n",
		       r->p5MissPullup, r->p5MissNopull,
		       r->p5wAvgPullup, r->p5wAvgNopull);
	}

	/* --- Status line instability --------------------------------------- */
	if ((r->statusFlips != 0u) || (r->statusFlipsIdle != 0u)) {
		issues++;
		printf("\r\n[7] STATUS LINES UNSTABLE: %" PRIu32 " changes while "
		       "transmitting, %" PRIu32 " while idle.\r\n",
		       r->statusFlips, r->statusFlipsIdle);
		if (r->statusFlipsIdle != 0u) {
			printf("    Changes while IDLE cannot be our own doing - the bus "
			       "supply itself is marginal.\r\n");
		}
		if ((r->vccOk != 0u) && (r->busOk != 0u)) {
			printf("    KNX_VCC_OK steady while KNX_OK flaps pins this down "
			       "further: VCCCORE only needs VDD_REGIN >= 6.8 V "
			       "(Table 6), whereas KNX_OK needs VREF above 13.5 V to rise "
			       "and drops it below 9.7 V (Table 7). So the isolated "
			       "high-voltage rail is sitting around 10 V instead of the "
			       "20-32 V it should be.\r\n"
			       "    LEADING HYPOTHESIS - the two under-rated capacitors "
			       "are clamping those rails at their own 10 V rating: C27 "
			       "(CVDDHV, 220 uF/10 V on VDDHV) and C20 (CREF, 470 nF/10 V "
			       "on VREF), where Table 2 requires 35 V or more. That single "
			       "cause also explains a dead transmitter and a silent "
			       "receiver.\r\n");
		}
		printf("    MEASURE, black lead on KNX- (C04):\r\n"
		       "      a) C03-C04 DC level      must stay above 20 V\r\n"
		       "      b) C27 (VDDHV) to KNX-   expect bus minus a few V\r\n"
		       "      c) C20 (VREF)  to KNX-\r\n"
		       "    THEN unplug this board from the bus and read C03-C04 "
		       "again. If the voltage recovers once the board is off, the "
		       "board is dragging the bus down - which is what a leaking "
		       "10 V electrolytic across a 30 V rail does.\r\n");
	}

	/* --- The quiet-bus case -------------------------------------------- */
	if (r->p4Chars == 0u) {
		printf("\r\n[8] P4 HEARD NOTHING. Read it together with P3:\r\n");
		if (r->p3Pass == r->p3Total) {
			printf("    P3 passed %u/%u, so the receive chain provably works. "
			       "Nothing else is talking on the bus - this is NOT a fault.\r\n"
			       "    To exercise P4 you need a second talker: another KNX "
			       "device, an ETS interface, or a second board of this kind.\r\n",
			       (unsigned) r->p3Pass, (unsigned) r->p3Total);
		} else {
			issues++;
			printf("    P3 only passed %u/%u, so the receive chain itself is "
			       "suspect - the silence is a symptom, not a quiet bus.\r\n"
			       "    SCOPE in this order:\r\n"
			       "      1. STKNX pin 23 / U13 pin 3   does the receiver "
			       "output move at all?\r\n"
			       "      2. U13 pin 5 / PA10           does the optocoupler "
			       "pass it?\r\n"
			       "    1 dead with a good bus waveform -> STKNX receiver "
			       "front end: check C23 (CAC, 10 nF) and D29.\r\n"
			       "    1 alive, 2 dead -> U13 or R89, or the missing pull-up "
			       "(see P5 above).\r\n",
			       (unsigned) r->p3Pass, (unsigned) r->p3Total);
		}
	}

	/* --- Always-on reminders ------------------------------------------- */
	printf("\r\n[*] TWO THINGS SOFTWARE CAN NEVER SEE - measure them once, by "
	       "hand, black lead on KNX- (C02/C04):\r\n"
	       "      C27 (CVDDHV) to KNX-   the part fitted is 220 uF / 10 V, "
	       "but the STKNX datasheet Table 2 requires 35 V or more\r\n"
	       "      C20 (CREF)   to KNX-   the part fitted is 470 nF / 10 V, "
	       "same 35 V requirement; VREF's own thresholds sit at 9.7-13.5 V\r\n"
	       "    If either reads above 10 V the fitted part is over-voltage and "
	       "must be replaced.\r\n");

	if (issues == 0u) {
		printf("\r\nNo faults detected this round: bus up, loopback clean, no "
		       "lost pulses, status lines stable.\r\n");
	}
	printf("\r\n=====================\r\n");
}

/* --- Dead-bus holding pattern ------------------------------------------- */

/* Running the full 42 s round against an unpowered transceiver only buries the
 * one line that matters. Hold here instead, one pulse a second so a scope on
 * PB14 still shows something, and report the moment the bus appears. */
static void knx_wait_for_bus(void)
{
	static const uint8_t onePulse[1] = { 1u };
	uint8_t v, k, r;
	knx_bus_t st   = knx_bus_state(&v, &k, &r);
	knx_bus_t last = st;
	uint32_t  n    = 0u;

	printf("\r\n--- holding: transceiver unpowered, polling for the bus ---\r\n");
	printf("   Still sending one %u us pulse per second on PB14.\r\n",
	       (unsigned) KNX_PULSE_US);

	while (st != KNX_BUS_OK) {
		knx_tx_send_bits(onePulse, 1u);
		HAL_Delay(1000u);
		knx_heartbeat();

		st = knx_bus_state(&v, &k, &r);
		n++;
		if (st != last) {
			last = st;
			printf("[%8" PRIu32 " ms]  ", HAL_GetTick());
			knx_bus_print(st, v, k, r);
		} else if ((n % 15u) == 0u) {
			printf("[%8" PRIu32 " ms]  still no bus power  "
			       "(PH12=%s PD7=%s PA10=%s)\r\n", HAL_GetTick(),
			       v ? "H" : "L", k ? "H" : "L", r ? "H" : "L");
		} else {
			/* quiet second */
		}
	}

	printf("[%8" PRIu32 " ms]  ** BUS CAME UP ** starting a full round\r\n",
	       HAL_GetTick());
}

/* --- What does this octet string mean in KNX terms? ---------------------- */

/* Field layout and every constant below come from the OpenPLC_KNX reference
 * stack, not from memory: control field masks and the acknowledge octets in
 * knx/tpuart_data_link_layer.cpp:55-64 and :299-303, the octet order and
 * length rule in knx/tp_frame.h, the TPCI split in knx/tpdu.cpp:8-36, the APCI
 * normalisation in knx/apdu.cpp:9-20 and the service names in
 * knx/knx_types.h:141-220.
 *
 * Nothing here validates: the check octet is not looked at and a short string
 * is described as far as it goes. Only a control field that matches no frame
 * type at all yields "null". */

static const struct {
	uint16_t    code;
	const char *name;
} KNX_APCI_NAMES[] = {
	{ 0x000u, "GroupValueRead"                    },
	{ 0x040u, "GroupValueResponse"                },
	{ 0x080u, "GroupValueWrite"                   },
	{ 0x0C0u, "IndividualAddressWrite"            },
	{ 0x100u, "IndividualAddressRead"             },
	{ 0x140u, "IndividualAddressResponse"         },
	{ 0x180u, "ADCRead"                           },
	{ 0x1C0u, "ADCResponse"                       },
	{ 0x1C8u, "SystemNetworkParameterRead"        },
	{ 0x1C9u, "SystemNetworkParameterResponse"    },
	{ 0x1CAu, "SystemNetworkParameterWrite"       },
	{ 0x200u, "MemoryRead"                        },
	{ 0x240u, "MemoryResponse"                    },
	{ 0x280u, "MemoryWrite"                       },
	{ 0x2C0u, "UserMemoryRead"                    },
	{ 0x2C1u, "UserMemoryResponse"                },
	{ 0x2C2u, "UserMemoryWrite"                   },
	{ 0x2C5u, "UserManufacturerInfoRead"          },
	{ 0x2C6u, "UserManufacturerInfoResponse"      },
	{ 0x2C7u, "FunctionPropertyCommand"           },
	{ 0x2C8u, "FunctionPropertyState"             },
	{ 0x2C9u, "FunctionPropertyStateResponse"     },
	{ 0x300u, "DeviceDescriptorRead"              },
	{ 0x340u, "DeviceDescriptorResponse"          },
	{ 0x380u, "Restart"                           },
	{ 0x381u, "RestartMasterReset"                },
	{ 0x3C0u, "RoutingTableOpen"                  },
	{ 0x3C1u, "RoutingTableRead"                  },
	{ 0x3C2u, "RoutingTableReadResponse"          },
	{ 0x3C3u, "RoutingTableWrite"                 },
	{ 0x3D1u, "AuthorizeRequest"                  },
	{ 0x3D2u, "AuthorizeResponse"                 },
	{ 0x3D3u, "KeyWrite"                          },
	{ 0x3D4u, "KeyResponse"                       },
	{ 0x3D5u, "PropertyValueRead"                 },
	{ 0x3D6u, "PropertyValueResponse"             },
	{ 0x3D7u, "PropertyValueWrite"                },
	{ 0x3D8u, "PropertyDescriptionRead"           },
	{ 0x3D9u, "PropertyDescriptionResponse"       },
	{ 0x3DCu, "IndividualAddressSerialNumberRead" },
	{ 0x3DDu, "IndividualAddressSerialNumberResp" },
	{ 0x3DEu, "IndividualAddressSerialNumberWrite"},
	{ 0x3E0u, "DomainAddressWrite"                },
	{ 0x3E1u, "DomainAddressRead"                 },
	{ 0x3E2u, "DomainAddressResponse"             },
	{ 0x3F1u, "SecureService"                     },
};

static const char *knx_apci_name(uint16_t apci)
{
	for (uint8_t i = 0u; i < (uint8_t) (sizeof(KNX_APCI_NAMES)
	                                  / sizeof(KNX_APCI_NAMES[0])); i++) {
		if (KNX_APCI_NAMES[i].code == apci) {
			return KNX_APCI_NAMES[i].name;
		}
	}
	return NULL;
}

/* Appends to out[] and returns the new length, never past cap. */
static uint16_t knx_cat(char *out, uint16_t cap, uint16_t used,
                        const char *fmt, ...)
{
	va_list ap;
	int     r;

	if (used >= (cap - 1u)) {
		return used;
	}
	va_start(ap, fmt);
	r = vsnprintf(&out[used], (size_t) (cap - used), fmt, ap);
	va_end(ap);

	if (r < 0) {
		return used;
	}
	used = (uint16_t) (used + (uint16_t) r);
	return (used >= cap) ? (uint16_t) (cap - 1u) : used;
}

static void knx_describe(const uint8_t *f, uint8_t n, char *out, uint16_t cap)
{
	static const char *PRIO[4] = { "system", "normal", "urgent", "low" };
	uint16_t used = 0u;
	uint8_t  ctrl, ext, si, di, li, ti, isGroup;
	uint16_t sa, da;

	out[0] = '\0';
	if (n == 0u) {
		(void) knx_cat(out, cap, 0u, "null");
		return;
	}
	ctrl = f[0];

	/* A lone octet is only ever an acknowledge; the two masks say which. */
	if (n == 1u) {
		if ((ctrl & 0x33u) != 0x00u) {
			(void) knx_cat(out, cap, 0u, "null");
		} else if (((ctrl & 0x0Cu) != 0u) && ((ctrl & 0xC0u) != 0u)) {
			(void) knx_cat(out, cap, 0u, "L_Ack ACK");
		} else {
			used = knx_cat(out, cap, 0u, "L_Ack");
			if ((ctrl & 0xC0u) == 0u) { used = knx_cat(out, cap, used, " NAK"); }
			if ((ctrl & 0x0Cu) == 0u) { used = knx_cat(out, cap, used, " BUSY"); }
		}
		return;
	}

	if ((ctrl & 0xD3u) == 0x90u) {
		ext = 0u; si = 1u; di = 3u; li = 5u; ti = 6u;
	} else if ((ctrl & 0xD3u) == 0x10u) {
		ext = 1u; si = 2u; di = 4u; li = 6u; ti = 7u;
	} else if (ctrl == 0xF0u) {
		(void) knx_cat(out, cap, 0u, "L_Poll_Data");
		return;
	} else {
		(void) knx_cat(out, cap, 0u, "null");
		return;
	}

	used = knx_cat(out, cap, used, "%s prio=%s%s",
	               ext ? "L_Data_Extended" : "L_Data_Standard",
	               PRIO[(ctrl >> 2) & 3u],
	               ((ctrl & 0x20u) == 0u) ? " repeated" : "");

	/* The address-type bit lives in the length octet on a standard frame and
	 * in the second control octet on an extended one, so both are needed
	 * before the destination can be printed at all. */
	if (n <= li) {
		(void) knx_cat(out, cap, used, "  (only %u octets)", (unsigned) n);
		return;
	}
	isGroup = ext ? ((f[1] >> 7) & 1u) : ((f[li] >> 7) & 1u);
	sa = (uint16_t) (((uint16_t) f[si] << 8) | f[si + 1u]);
	da = (uint16_t) (((uint16_t) f[di] << 8) | f[di + 1u]);

	used = knx_cat(out, cap, used, "  %u.%u.%u ->",
	               (unsigned) ((sa >> 12) & 0x0Fu),
	               (unsigned) ((sa >> 8) & 0x0Fu), (unsigned) (sa & 0xFFu));
	if (isGroup != 0u) {
		used = knx_cat(out, cap, used, " %u/%u/%u",
		               (unsigned) ((da >> 11) & 0x1Fu),
		               (unsigned) ((da >> 8) & 0x07u), (unsigned) (da & 0xFFu));
	} else {
		used = knx_cat(out, cap, used, " %u.%u.%u",
		               (unsigned) ((da >> 12) & 0x0Fu),
		               (unsigned) ((da >> 8) & 0x0Fu), (unsigned) (da & 0xFFu));
	}
	used = knx_cat(out, cap, used, "  hop=%u len=%u",
	               (unsigned) ((f[li] >> 4) & 0x07u),
	               (unsigned) (ext ? f[6] : (f[5] & 0x0Fu)));

	if (n <= ti) {
		(void) knx_cat(out, cap, used, "  (no TPCI)");
		return;
	}

	/* TPCI: bit 7 selects control vs data, bit 6 selects numbered. */
	if ((f[ti] & 0x80u) == 0u) {
		if ((f[ti] & 0x40u) == 0u) {
			used = knx_cat(out, cap, used, "  T_Data_%s",
			               (isGroup == 0u) ? "Individual"
			             : (da == 0u)      ? "Broadcast" : "Group");
		} else {
			used = knx_cat(out, cap, used, "  T_Data_Connected seq=%u",
			               (unsigned) ((f[ti] >> 2) & 0x0Fu));
		}
	} else {
		if ((f[ti] & 0x40u) == 0u) {
			(void) knx_cat(out, cap, used, "  %s",
			               ((f[ti] & 1u) != 0u) ? "T_Disconnect" : "T_Connect");
		} else {
			(void) knx_cat(out, cap, used, "  %s seq=%u",
			               ((f[ti] & 1u) != 0u) ? "T_NAK" : "T_ACK",
			               (unsigned) ((f[ti] >> 2) & 0x0Fu));
		}
		return;   /* a control TPDU carries no APDU */
	}

	if (n <= (uint8_t) (ti + 1u)) {
		(void) knx_cat(out, cap, used, "  (no APCI)");
		return;
	}

	{
		uint16_t apci = (uint16_t) (((uint16_t) (f[ti] & 0x03u) << 8)
		                          | f[ti + 1u]);
		uint16_t code = apci;
		uint8_t  isShort = 0u;
		const char *name;

		if (((apci >> 6) < 11u) && ((apci >> 6) != 7u)) {
			code    = (uint16_t) (apci & 0x3C0u);
			isShort = 1u;
		}
		name = knx_apci_name(code);
		if (name == NULL) {
			(void) knx_cat(out, cap, used, "  APCI 0x%03X unknown",
			               (unsigned) apci);
		} else if (isShort != 0u) {
			(void) knx_cat(out, cap, used, "  %s = %u", name,
			               (unsigned) (apci & 0x3Fu));
		} else {
			(void) knx_cat(out, cap, used, "  %s", name);
		}
	}
}

/* --- Burst printer: everything the bus produced, raw and inverted -------- */

/* Octets accumulate here until the line falls quiet, then one block prints.
 * Collecting first is not a nicety: a 60-character console line costs 5 ms at
 * 115200 baud, which is 48 bit periods, so printing per character would starve
 * the decoder and destroy the very data being looked for. */
static uint8_t  s_bstBuf[KNX_BURST_MAX];
static uint8_t  s_bstErr[KNX_BURST_MAX];   /* framing/parity flags per octet */
static uint8_t  s_bstLen;
static uint8_t  s_bstOverflow;
static uint32_t s_bstStrays;
static uint32_t s_bstLastMs;
static uint8_t  s_bstPending;
static uint32_t s_bstPulseBase;   /* s_rxPulses as of the last flush */

static void knx_burst_add(uint8_t b, uint8_t err)
{
	if (s_bstLen < KNX_BURST_MAX) {
		s_bstBuf[s_bstLen] = b;
		s_bstErr[s_bstLen] = err;
		s_bstLen++;
	} else {
		s_bstOverflow = 1u;
	}
	s_bstLastMs  = HAL_GetTick();
	s_bstPending = 1u;
}

static void knx_burst_stray(void)
{
	s_bstStrays++;
	s_bstLastMs  = HAL_GetTick();
	s_bstPending = 1u;
}

/* Any pulse at all opens a burst, even one the decoder never turns into a
 * character. Without this a bus carrying undecodable activity - a mis-locked
 * decoder discards pulses inside knx_decoder_poll without counting them -
 * would look exactly like a dead one. */
static void knx_burst_watch_pulses(void)
{
	if (s_rxPulses != s_bstPulseBase) {
		if (s_bstPending == 0u) { s_bstLastMs = HAL_GetTick(); }
		s_bstPending = 1u;
	}
}

static void knx_burst_flush(void)
{
	static uint8_t inv[KNX_BURST_MAX];
	char     text[224];
	uint32_t pulses = s_rxPulses - s_bstPulseBase;
	uint8_t  errs = 0u;

	s_bstPending   = 0u;
	s_bstPulseBase = s_rxPulses;

	if (s_bstLen == 0u) {
		if ((s_bstStrays | pulses) != 0u) {
			printf("[%8" PRIu32 " ms]  BUS  %" PRIu32 " pulses, %" PRIu32
			       " lone pulses, NO character decoded\r\n",
			       HAL_GetTick(), pulses, s_bstStrays);
			s_bstStrays = 0u;
		}
		return;
	}

	for (uint8_t i = 0u; i < s_bstLen; i++) {
		inv[i] = (uint8_t) ~s_bstBuf[i];
		if (s_bstErr[i] != 0u) { errs++; }
	}

	printf("[%8" PRIu32 " ms]  BUS  %u octets, %" PRIu32 " pulses",
	       HAL_GetTick(), (unsigned) s_bstLen, pulses);
	if (errs != 0u) {
		printf(", %u framing/parity errors marked !", (unsigned) errs);
	}
	if (s_bstStrays != 0u) {
		printf(", %" PRIu32 " lone pulses", s_bstStrays);
	}
	if (s_bstOverflow != 0u) {
		printf(", TRUNCATED at %u", (unsigned) KNX_BURST_MAX);
	}
	printf("\r\n");

	printf("   raw:");
	for (uint8_t i = 0u; i < s_bstLen; i++) {
		printf(" %02X%s", (unsigned) s_bstBuf[i], s_bstErr[i] ? "!" : "");
	}
	printf("\r\n");
	knx_describe(s_bstBuf, s_bstLen, text, (uint16_t) sizeof(text));
	printf("     -> %s\r\n", text);

	printf("   inv:");
	for (uint8_t i = 0u; i < s_bstLen; i++) {
		printf(" %02X", (unsigned) inv[i]);
	}
	printf("\r\n");
	knx_describe(inv, s_bstLen, text, (uint16_t) sizeof(text));
	printf("     -> %s\r\n", text);

	s_bstLen      = 0u;
	s_bstStrays   = 0u;
	s_bstOverflow = 0u;
}

/* --- Slim mode: send one character every 5 s, listen the rest of the time - */

/* Two things print, and nothing else: one line per transmit, one line per
 * received character. No per-second summary, no periodic status, no raw edges.
 * The one exception fires exactly once in the lifetime of the run: if
 * KNX_SLIM_DIAG_AFTER transmits go by with no loopback at all, the diagnosis
 * block prints once and never again. */
static void knx_slim_loop(void)
{
#if KNX_TX_ENABLE
	uint8_t  bits[KNX_CHAR_BITS];
	uint32_t lastTx    = HAL_GetTick() - KNX_TX_EVERY_MS;   /* fire at once */
	uint32_t txStampMs = 0u;
	uint8_t  frame[KNX_FRAME_MAX];
	uint32_t lastGood = 0u, lastBad = 0u, lastDropped = 0u;
	uint32_t lastFrames = 0u;
	uint8_t  txValue = 0u;
#endif
	uint32_t sent = 0u, echoes = 0u, foreign = 0u, bad = 0u, strays = 0u;
	uint32_t frames = 0u;
	uint8_t  diagnosed = 0u;

#if KNX_TX_ENABLE
	printf("\r\n=== one TP1 character (0x%02X) every %u s; receive prints only "
	       "when a character arrives ===\r\n",
	       (unsigned) KNX_SLIM_TX_BYTE, (unsigned) (KNX_TX_EVERY_MS / 1000u));
#else
	printf("\r\n=== RECEIVE ONLY: transmit suspended, PB14 parked low, nothing "
	       "goes on the bus ===\r\n");
	printf("   Every octet that arrives is printed raw and bit-inverted, "
	       "valid or not. '!' marks a framing or parity error.\r\n");
	knx_presence_report();
#endif

	knx_decoder_reset();
	knx_stats_reset();
	s_pulseDropped = 0u;
	s_edgeLog      = 0u;      /* raw edges are not data - stay silent */

	for (;;) {
		uint16_t slots;
		uint16_t charT0;

#if KNX_TX_ENABLE
		/* --- transmit on a fixed cadence, never drifting ---------------- */
		if ((HAL_GetTick() - lastTx) >= KNX_TX_EVERY_MS) {
			uint8_t idle;
			lastTx += KNX_TX_EVERY_MS;
			txValue = (uint8_t) (txValue ^ 1u);      /* alternate ON / OFF */
			idle = knx_wait_bus_idle();              /* TP1 arbitration */

			if (KNX_SEND_FRAMES != 0u) {
				uint8_t n = knx_build_group_write(frame, KNX_SRC_ADDR,
				                                  KNX_GROUP_ADDR, txValue);
				txStampMs = HAL_GetTick();
				knx_send_frame(frame, n);
				sent++;
				printf("[%8" PRIu32 " ms]  TX>", txStampMs);
				for (uint8_t i = 0u; i < n; i++) { printf(" %02X", frame[i]); }
				printf("   %u/%u/%u = %s%s   rx since last: %" PRIu32 " frames, "
				       "%" PRIu32 " chars, %" PRIu32 " bad, %" PRIu32
				       " dropped\r\n",
				       (unsigned) ((KNX_GROUP_ADDR >> 11) & 0x1Fu),
				       (unsigned) ((KNX_GROUP_ADDR >> 8) & 0x07u),
				       (unsigned) (KNX_GROUP_ADDR & 0xFFu),
				       txValue ? "ON " : "OFF",
				       idle ? "" : "  [bus was busy - sent anyway]",
				       frames - lastFrames, echoes + foreign - lastGood,
				       bad - lastBad, s_charDropped - lastDropped);
			} else {
				knx_encode_char(KNX_SLIM_TX_BYTE, bits);
				txStampMs = HAL_GetTick();
				knx_tx_send_bits(bits, KNX_CHAR_BITS);
				sent++;
				printf("[%8" PRIu32 " ms]  TX> char 0x%02X\r\n", txStampMs,
				       (unsigned) KNX_SLIM_TX_BYTE);
			}
			knx_heartbeat();
			lastFrames  = frames;
			lastGood    = echoes + foreign;
			lastBad     = bad;
			lastDropped = s_charDropped;
		}
#endif

		/* --- decode flat out; no printf inside this loop ---------------- */
		while (knx_decoder_poll(&slots, &charT0) != 0u) {
			if (slots == 1u) {
				strays++;
				knx_burst_stray();   /* a lone pulse is still bus activity */
				continue;
			}
			uint16_t n = (uint16_t) ((s_charHead + 1u) & (KNX_CHAR_RESULT_RING - 1u));
			if (n != s_charTail) {
				s_charRing[s_charHead] = slots;
				s_charHead = n;
			} else {
				s_charDropped++;
			}
		}

		/* --- every character joins the burst, valid or not --------------- */
		while (s_charTail != s_charHead) {
			uint16_t sl = s_charRing[s_charTail];
			uint8_t  b, sOk, pOk, tOk, n, isEcho;
			s_charTail = (uint16_t) ((s_charTail + 1u) & (KNX_CHAR_RESULT_RING - 1u));

			if (knx_decode_char(sl, &b, &sOk, &pOk, &tOk) == 0u) {
				bad++;
				knx_burst_add(b, 1u);   /* keep the octet - it is still data */
				knx_decoder_resync();   /* misaligned - re-lock on an idle gap */
				knx_frame_reset();
				continue;
			}
			knx_burst_add(b, 0u);

#if KNX_TX_ENABLE
			isEcho = ((HAL_GetTick() - txStampMs) < KNX_LOOPBACK_MS) ? 1u : 0u;
#else
			isEcho = 0u;            /* nothing is transmitting, so nothing echoes */
#endif
			if (isEcho != 0u) { echoes++; } else { foreign++; }

			if (KNX_PRINT_CHARS != 0u) {
				printf("[%8" PRIu32 " ms]  RX< char 0x%02X\r\n",
				       HAL_GetTick(), b);
			}

			n = knx_frame_feed(b, charT0);
			if (n != 0u) {
				frames++;
				if (KNX_PRINT_FRAMES != 0u) {
					knx_frame_report(s_frBuf, n, isEcho);
				}
			}
		}
		knx_frame_timeout();

		/* --- the line has fallen quiet: print what arrived --------------- */
		knx_burst_watch_pulses();
		if ((s_bstPending != 0u)
		 && ((HAL_GetTick() - s_bstLastMs) >= KNX_RX_FLUSH_MS)) {
			knx_burst_flush();
			knx_heartbeat();
		}

		/* --- fires once, ever, and only when the loopback never came ---- */
		if ((diagnosed == 0u) && (echoes == 0u) && (sent >= KNX_SLIM_DIAG_AFTER)) {
			uint8_t v, k, r;
			diagnosed = 1u;

			printf("\r\n** %" PRIu32 " characters sent, not one came back. "
			       "Diagnosing once; this will not repeat. **\r\n", sent);
			printf("   sent %" PRIu32 ", from bus %" PRIu32 ", malformed "
			       "%" PRIu32 ", lone pulses %" PRIu32 ", pulses %" PRIu32
			       ", characters lost to the console %" PRIu32 "\r\n",
			       sent, foreign, bad, strays, s_rxPulses, s_charDropped);

			s_round.bus = knx_bus_state(&v, &k, &r);
			s_round.vccOk = v; s_round.busOk = k; s_round.rxIdle = r;
			knx_bus_print(s_round.bus, v, k, r);
			knx_presence_report();
			knx_status_flush();

			s_round.p2Tx    = s_txPulses;
			s_round.p2Rx    = s_rxPulses;
			s_round.p2wCnt  = s_wCnt;
			s_round.p2wAvg  = (s_wCnt != 0u) ? (s_wSum / s_wCnt) : 0u;
			s_round.p3Pass  = 0u;
			s_round.p3Total = (uint8_t) ((sent > 255u) ? 255u : sent);
			s_round.p4Chars = foreign;
			s_round.idleHighPermille = (s_idleSamples != 0u)
			    ? ((s_idleHighSamples * 1000u) / s_idleSamples) : 0u;
			knx_diagnose();
		}
	}
}

/* --- Entry point -------------------------------------------------------- */

void KNX_Test_Run(void)
{
	printf("\r\n=== STKNX bring-up test (TestCase/KNX) ===\r\n");
	printf("   KNX_TX=PB14  KNX_RX=PA10  KNX_OK=PD7  KNX_VCC_OK=PH12  "
	       "ProgLED=PG11\r\n");
	printf("   TP1: bit %u us, active pulse %u us (logic 0), idle low\r\n",
	       (unsigned) KNX_BIT_US, (unsigned) KNX_PULSE_US);

	knx_gpio_init();
	(void) knx_p0_selfcheck();

	knx_capture_start();
	knx_tx_start();

	if (KNX_SLIM_MODE != 0u) {
		knx_slim_loop();   /* never returns */
	}

	for (;;) {
		uint8_t v, k, r;

		s_round = (knx_round_t) { 0 };
		s_stFlips     = 0u;
		s_stFlipsIdle = 0u;

		printf("\r\n===== round start =====\r\n");
		s_round.bus = knx_bus_state(&v, &k, &r);
		s_round.vccOk = v; s_round.busOk = k; s_round.rxIdle = r;
		knx_bus_print(s_round.bus, v, k, r);

		/* Always, not just when the bus looks dead: PD7 and PH12 are NOPULL
		 * inputs, so a disconnected deck reads as an arbitrary level that
		 * mimics a live status line - and mimics it flapping. */
		knx_presence_report();

		if (s_round.bus == KNX_BUS_DEAD) {
			knx_diagnose();
			knx_wait_for_bus();
			continue;
		}

		knx_p1_single_pulse();
		knx_p2_long_run();
		knx_p3_loopback();
		knx_p4_listen();
		knx_p5_pullup_ab();
		knx_diagnose();
	}
}
