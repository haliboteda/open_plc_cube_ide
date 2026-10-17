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

/* --- Pulse counts for the diagnostics ---------------------------------- */

#define KNX_P2_BURST_PULSES    200u   /* ~21 ms of dominant state per burst */
#define KNX_P5_BURST_PULSES    200u   /* per leg of the pull-up A/B test */

/* --- What to put on the bus ---------------------------------------------- */

/* KNX_FRAME_MAX lives in the header: a session caller has to size its own
 * buffer to it. */
#define KNX_TX_BITS_MAX     (KNX_FRAME_MAX * KNX_CHAR_BITS)

/* TP1 wants the line quiet before a device may start sending. */
#define KNX_ARB_IDLE_BITS       50u

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
static volatile uint32_t s_pulseDropped;   /* decoder starved */


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

/* --- Bus state ---------------------------------------------------------- */

typedef enum {
	KNX_BUS_OK = 0,
	KNX_BUS_DEAD,
	KNX_BUS_ODD
} knx_bus_t;

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
	s_prevPulseT    = 0u;
	s_havePrevPulse = 0u;
	s_syncNeeded    = 1u;
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
	s_stHead = 0u;
	s_stTail = 0u;
}

/* --- Bus state ---------------------------------------------------------- */

static knx_bus_t knx_bus_state(uint8_t *vccOk, uint8_t *busOk, uint8_t *rxIdle)
{
	uint8_t v = KNX_VCC_OK_LEVEL() ? 1u : 0u;
	uint8_t k = KNX_OK_LEVEL()     ? 1u : 0u;
	uint8_t r = KNX_RX_LEVEL()     ? 1u : 0u;

	*vccOk = v; *busOk = k; *rxIdle = r;

	/* PH12 alone decides, because it is the only one of the three that was
	 * measured to follow the bus: pulling C03/C04 takes it to 0 and plugging
	 * back takes it to 1, reproduced twice on 2026-09-09.
	 *
	 * The other two are still reported - they are just not judged:
	 *   PA10 is high when the line is idle, not low. /KNX_RX is active low and
	 *   has no board pull-up, so the idle level comes from the MCU's own
	 *   pull-up. Requiring r == 0 here made a healthy bus read as odd.
	 *   PD7 does not move at all on this board, not even with the isolated
	 *   side fully unpowered, so what it means is unknown.
	 *
	 * Frame-level proof that the bus works regardless: 47 GroupValueWrites
	 * sent, 47 heard back with the check octet passing, 47 acknowledged by a
	 * real device on the installation. See $PROD/docs/hardware/HARDWARE-FACTS.md. */
	return (v != 0u) ? KNX_BUS_OK : KNX_BUS_DEAD;
}

static void knx_heartbeat(void)
{
	HAL_GPIO_TogglePin(KNX_PROG_LED_PORT, KNX_PROG_LED_PIN);
}

/* --- Pulse bursts, loss rate and histograms ---------------------------- */

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

/* ---- Session entry points, for pt.start knx ----------------------------
 *
 * ⚠️ KNX_Test_SessionSendChar() is NOT free: knx_tx_send_bits() spins until the
 * bit engine has walked the pattern, about 1.35 ms for one 13-bit character at
 * 104 us a bit. That is less than the ~8.7 ms one frame of console output
 * already costs at 115200, so a session sending one character a period is
 * fine - but it is a spin, and calling it faster than that is not. */

/* Pins, capture and bit engine, once: the session and the pt.run
 * diagnostics share them. */
static uint8_t s_hwUp;

static void knx_hw_up(void)
{
    if (s_hwUp != 0u) {
        return;
    }
    knx_gpio_init();
    knx_capture_start();
    knx_tx_start();
    s_hwUp = 1u;
}

int KNX_Test_SessionInit(void)
{
    knx_hw_up();
    knx_decoder_reset();
    knx_stats_reset();
    return 1;
}

void KNX_Test_SessionStatsReset(void)
{
    knx_decoder_reset();
    knx_stats_reset();
}

int KNX_Test_SessionSendChar(uint8_t b)
{
    uint8_t bits[KNX_CHAR_BITS];

    knx_encode_char(b, bits);
    knx_tx_send_bits(bits, KNX_CHAR_BITS);
    return 1;
}

/* Drains whatever the capture ISR has queued. Returns 1 and fills *out when a
 * character closed - either because a later pulse passed the stop bit, or
 * because the decoder's own idle timeout closed it. */
int KNX_Test_SessionPollChar(uint8_t *out, uint8_t *framing_ok)
{
    uint16_t slots = 0u, t0 = 0u;
    uint8_t got = 0u, sOk = 0u, pOk = 0u, tOk = 0u;

    if (knx_decoder_poll(&slots, &t0) == 0u) {
        return 0;
    }
    uint8_t good = knx_decode_char(slots, &got, &sOk, &pOk, &tOk);
    if (out != NULL)        { *out = got; }
    if (framing_ok != NULL) { *framing_ok = good; }
    return 1;
}

/* --- Session frame layer ------------------------------------------------- */

/* Which reading of the octets passes the check octet. Both are tried because
 * the RX chain's polarity is not settled on this board: the octets as received
 * may be the frame, or the frame may be their bit-inverse. The check octet is
 * the XOR of every earlier octet, inverted. Inverting every octet inverts the
 * expected check octet too, so the inverted reading passes when the received
 * check octet equals the XOR of the inverted octets. */
static uint8_t knx_frame_which(const uint8_t *f, uint8_t n)
{
    uint8_t x = 0u, xi = 0u;

    if (n < 2u) {
        return KNX_FRAME_CRC_BAD;
    }
    for (uint8_t i = 0u; (i + 1u) < n; i++) {
        x  ^= f[i];
        xi ^= (uint8_t) ~f[i];
    }
    if (f[n - 1u] == (uint8_t) ~x)  { return KNX_FRAME_CRC_RAW; }
    if (f[n - 1u] == xi)            { return KNX_FRAME_CRC_INV; }
    return KNX_FRAME_CRC_BAD;
}

/* A lone octet on TP1 is never a frame - it is an acknowledge, and it carries
 * no check octet to judge. The masks are the same ones knx_describe() uses for
 * its prose further down (from knx/tpuart_data_link_layer.cpp:55-64), kept in
 * one function here so the session and the console path cannot disagree about
 * what 0xCC means. */
uint8_t KNX_Test_AckKind(uint8_t octet)
{
    if ((octet & 0x33u) != 0x00u) {
        return KNX_ACK_NONE;
    }
    if (((octet & 0x0Cu) != 0u) && ((octet & 0xC0u) != 0u)) {
        return KNX_ACK_ACK;
    }
    if ((octet & 0xC0u) == 0u) {
        return KNX_ACK_NAK;
    }
    return KNX_ACK_BUSY;
}

static uint8_t  s_sesFrame[KNX_FRAME_MAX];
static uint8_t  s_sesLen;
static uint8_t  s_sesOut[KNX_FRAME_MAX];
static uint8_t  s_sesOutLen;
static uint8_t  s_sesOutWhich;
static uint8_t  s_sesOutReady;
static uint32_t s_sesLastMs;
static uint32_t s_sesPartial;

void KNX_Test_SessionFrameReset(void)
{
    s_sesLen      = 0u;
    s_sesOutLen   = 0u;
    s_sesOutReady = 0u;
    s_sesPartial  = 0u;
    s_sesLastMs   = HAL_GetTick();
}

static void knx_ses_publish(uint8_t which)
{
    for (uint8_t i = 0u; i < s_sesLen; i++) { s_sesOut[i] = s_sesFrame[i]; }
    s_sesOutLen   = s_sesLen;
    s_sesOutWhich = which;
    s_sesOutReady = 1u;
    s_sesLen      = 0u;
}

/* Feeds one received octet. Octet 5 low nibble is the APDU length and a
 * standard frame is 8 + that - but octet 5 is itself subject to the polarity
 * question, so both lengths are tried. Whichever total the octets reach first
 * with a passing check octet is the frame. */
static void knx_ses_feed(uint8_t b)
{
    if (s_sesLen < KNX_FRAME_MAX) {
        s_sesFrame[s_sesLen] = b;
        s_sesLen++;
    }
    s_sesLastMs = HAL_GetTick();

    if (s_sesLen < 6u) {
        return;
    }

    uint8_t rawTotal = (uint8_t) (8u + (s_sesFrame[5] & 0x0Fu));
    uint8_t invTotal = (uint8_t) (8u + ((uint8_t) ~s_sesFrame[5] & 0x0Fu));
    uint8_t longest  = (rawTotal > invTotal) ? rawTotal : invTotal;

    if ((s_sesLen == rawTotal) || (s_sesLen == invTotal)) {
        uint8_t which = knx_frame_which(s_sesFrame, s_sesLen);
        if (which != KNX_FRAME_CRC_BAD) {
            knx_ses_publish(which);
            return;
        }
    }
    if ((s_sesLen >= longest) || (s_sesLen >= KNX_FRAME_MAX)) {
        knx_ses_publish(KNX_FRAME_CRC_BAD);
    }
}

/* Closes a frame that stopped mid-way, so a single-octet ACK or a truncated
 * frame is reported rather than swallowed by the next one. */
static void knx_ses_frame_timeout(void)
{
    if ((s_sesOutReady == 0u) && (s_sesLen != 0u)
        && ((HAL_GetTick() - s_sesLastMs) > 30u)) {
        s_sesPartial++;
        knx_ses_publish(knx_frame_which(s_sesFrame, s_sesLen));
    }
}

int KNX_Test_SessionPollFrame(uint8_t *out, uint8_t *out_len, uint8_t cap,
                              uint8_t *which, uint8_t *bad_chars)
{
    uint8_t b = 0u, ok = 0u;
    uint8_t bad = 0u;

    /* Stops taking characters the moment a frame is ready: only one completed
     * frame is held, so draining past it would overwrite the first of two
     * frames that arrived back to back. The characters stay queued in the
     * decoder's own ring for the next call. */
    while ((s_sesOutReady == 0u) && (KNX_Test_SessionPollChar(&b, &ok) != 0)) {
        if (ok == 0u) { bad++; }
        knx_ses_feed(b);
    }
    knx_ses_frame_timeout();

    if (bad_chars != NULL) { *bad_chars = bad; }
    if (s_sesOutReady == 0u) {
        return 0;
    }
    s_sesOutReady = 0u;

    uint8_t n = (s_sesOutLen > cap) ? cap : s_sesOutLen;
    for (uint8_t i = 0u; i < n; i++) { out[i] = s_sesOut[i]; }
    if (out_len != NULL) { *out_len = n; }
    if (which != NULL)   { *which = s_sesOutWhich; }
    return 1;
}

uint32_t KNX_Test_SessionPartialFrames(void)
{
    return s_sesPartial;
}

/* ⚠️ Spins for about 12 ms - nine characters at 1.35 ms each. Returns the
 * octet count put on the bus, 0 only if out was too small. bus_was_idle says
 * whether TP1 arbitration found the line quiet; the frame goes out either way,
 * because a bus that never falls quiet must not stall a production test. */
uint8_t KNX_Test_SessionSendGroupWrite(uint16_t src, uint16_t ga, uint8_t value,
                                       uint8_t *out, uint8_t cap,
                                       uint8_t *bus_was_idle)
{
    uint8_t frame[KNX_FRAME_MAX];
    uint8_t n = knx_build_group_write(frame, src, ga, value);
    uint8_t idle;

    if (cap < n) {
        return 0u;
    }
    idle = knx_wait_bus_idle();
    if (bus_was_idle != NULL) { *bus_was_idle = idle; }

    knx_send_frame(frame, n);
    for (uint8_t i = 0u; i < n; i++) { out[i] = frame[i]; }
    return n;
}

void KNX_Test_SessionStats(knx_session_stats_t *out)
{
    uint8_t v = 0u, k = 0u, r = 0u;

    if (out == NULL) {
        return;
    }
    out->bus = (uint8_t) knx_bus_state(&v, &k, &r);
    out->vcc_ok = v;
    out->bus_ok = k;
    out->rx_idle = r;

    out->pulses  = s_rxPulses;
    out->dropped = s_pulseDropped;

    /* min/max stay at their reset sentinels until something arrives, so a
     * caller can tell "nothing measured" from "measured zero". */
    out->w_min = s_wMin;
    out->w_max = s_wMax;
    out->w_avg = (s_wCnt != 0u) ? (s_wSum / s_wCnt) : 0u;
    out->w_count = s_wCnt;

    out->d_min = s_dMin;
    out->d_max = s_dMax;
    out->d_avg = (s_dCnt != 0u) ? (s_dSum / s_dCnt) : 0u;
    out->d_count = s_dCnt;

    out->ms_since_edge = HAL_GetTick() - s_lastEdgeMs;
}

/* ---- Diagnostics, for pt.run knx.* - see knx_test.h -------------------- */

_Static_assert(KNX_DIAG_BINS == KNX_HIST_BINS, "knx_test.h KNX_DIAG_BINS must match KNX_HIST_BINS");

static uint8_t knx_diag_up(void)
{
    uint8_t v, k, r;

    knx_hw_up();
    return (knx_bus_state(&v, &k, &r) == KNX_BUS_OK) ? 1u : 0u;
}

int KNX_Test_DiagEdges(uint32_t shots, knx_diag_edges_t *out)
{
    static const uint8_t onePulse[1] = { 1u };
    int32_t  firstSum = 0, widthSum = 0;
    uint32_t firstN = 0u, widthN = 0u;

    *out = (knx_diag_edges_t) { 0 };
    out->bus_ok = knx_diag_up();
    if (out->bus_ok == 0u) {
        return 0;
    }

    for (uint32_t shot = 0u; shot < shots; shot++) {
        uint32_t n = 0u;
        int32_t  prev = 0;

        s_edgeHead    = s_edgeTail;
        s_edgeDropped = 0u;
        knx_stats_reset();
        s_edgeLog = 1u;
        knx_tx_send_bits(onePulse, 1u);
        HAL_Delay(5u);           /* let the whole response land */
        s_edgeLog = 0u;

        uint16_t tx = s_txStamp;
        printf("KNX_TEST: edges shot %" PRIu32 ":", shot + 1u);
        while (s_edgeTail != s_edgeHead) {
            uint32_t e = s_edgeRing[s_edgeTail];
            int32_t off = (int32_t) (int16_t) ((uint16_t) (e & 0xFFFFu) - tx);

            s_edgeTail = (uint16_t) ((s_edgeTail + 1u) & (KNX_EDGE_RING - 1u));
            printf(" %s@%+" PRId32 "us", ((e >> 16) != 0u) ? "H" : "L", off);
            if (n == 0u) {
                firstSum += off;
                firstN++;
            } else if (n == 1u) {
                widthSum += off - prev;
                widthN++;
            }
            prev = off;
            n++;
        }
        if (n == 0u) {
            printf(" no edge, KNX_RX stayed %s", KNX_RX_LEVEL() ? "HIGH" : "LOW");
        }
        printf("\r\n");

        out->shots++;
        out->edges += n;
        out->dropped += s_edgeDropped;
        if (n != 0u) {
            out->moved++;
        }
        knx_heartbeat();
        HAL_Delay(200u);
    }

    out->first_us = (firstN != 0u) ? (firstSum / (int32_t) firstN) : 0;
    out->width_us = (widthN != 0u) ? (widthSum / (int32_t) widthN) : 0;
    return 1;
}

int KNX_Test_DiagHist(uint32_t bursts, knx_diag_hist_t *out)
{
    *out = (knx_diag_hist_t) { 0 };
    out->bus_ok = knx_diag_up();
    if (out->bus_ok == 0u) {
        return 0;
    }

    s_edgeLog = 0u;
    knx_stats_reset();
    for (uint32_t b = 0u; b < bursts; b++) {
        knx_p2_burst(KNX_P2_BURST_PULSES);
        knx_heartbeat();
        if ((b + 1u) < bursts) {
            HAL_Delay(1000u);   /* leave a live bus usable between bursts */
        }
    }
    out->tx = s_txPulses;
    out->rx = s_rxPulses;
    out->miss = s_misses;
    if (s_wCnt != 0u) {
        out->w_min = s_wMin;
        out->w_avg = s_wSum / s_wCnt;
        out->w_max = s_wMax;
    }
    if (s_dCnt != 0u) {
        out->d_min = s_dMin;
        out->d_avg = s_dSum / s_dCnt;
        out->d_max = s_dMax;
    }
    for (uint32_t i = 0u; i < KNX_DIAG_BINS; i++) {
        out->w_hist[i] = s_wHist[i];
        out->d_hist[i] = s_dHist[i];
    }
    return 1;
}

int KNX_Test_DiagPullup(knx_diag_pullup_t *out)
{
    *out = (knx_diag_pullup_t) { 0 };
    out->bus_ok = knx_diag_up();
    if (out->bus_ok == 0u) {
        return 0;
    }

    for (uint8_t leg = 0u; leg < 2u; leg++) {
        uint8_t on = (leg == 0u) ? 1u : 0u;

        knx_rx_pullup(on);
        HAL_Delay(2u);
        knx_stats_reset();
        s_edgeLog = 0u;
        knx_p2_burst(KNX_P5_BURST_PULSES);

        out->tx[leg]    = s_txPulses;
        out->rx[leg]    = s_rxPulses;
        out->miss[leg]  = s_misses;
        out->w_avg[leg] = (s_wCnt != 0u) ? (s_wSum / s_wCnt) : 0u;
        printf("KNX_TEST: pullup %s TX=%" PRIu32 " RX=%" PRIu32 " miss=%" PRIu32
               " width avg=%" PRIu32 " us\r\n", on ? "PULLUP" : "NOPULL",
               out->tx[leg], out->rx[leg], out->miss[leg], out->w_avg[leg]);
    }

    knx_rx_pullup(1u);          /* leave it enabled - the board needs it */
    knx_heartbeat();
    return 1;
}
