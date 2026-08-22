/*
 * SDRAM data-bus diagnostics -- see sdram_diag.h for why this exists.
 *
 * Three measurements, in the order they answer questions:
 *
 *   1. Census. Write N pseudo-random 16-bit words, read them back, count
 *      mismatches per bit and split them by the value written. An open line
 *      scores 50 % correct in both columns (no correlation at all); a line
 *      stuck to a rail scores 100 % / 0 %; a weakly conducting line scores
 *      above 50 % in both, which is what this board does.
 *
 *   2. Dwell sweep. Write the same value 1, 2, 4, 8, 16 times before moving on.
 *      Every repeat holds the level on the wire one more bus cycle. If the
 *      error rate falls as the dwell grows, the mechanism is settling time, and
 *      the dwell at which it stops falling is the settling time in bus cycles.
 *
 *   3. Release time. Drive a DQ pin low as a GPIO, arm the internal pull-up,
 *      release the drive, and sample the input register 32 times back to back.
 *      The index at which the pin reads high is proportional to the net's
 *      capacitance: a bigger net charges slower through the same internal
 *      pull-up. Six other data lines (D0 D2 D3 D13 D14 D15) live on the same
 *      GPIO port as D1, so they are released and sampled by the same stores in
 *      the same run -- the comparison needs no calibration, only the spread of
 *      the six.
 *
 *      What it decides: if D1 charges much faster than its neighbours, the MCU
 *      end cannot see the far end's capacitance, so the break is at the MCU
 *      ball or in the trace. If it matches them, the MCU end does see the far
 *      end, so the break is at the SDRAM ball or inside the SDRAM.
 *
 * Everything writes only inside the staging area (scratch by definition) and
 * restores every GPIO register it touches.
 */

#include "sdram_diag.h"
#include "main.h"
#include "IAP_config.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ timing */

#if IAP_SDRAM_DIAG_COMMANDS
static void cyc_start(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_cycles(uint32_t n)
{
  uint32_t t0 = DWT->CYCCNT;
  while ((DWT->CYCCNT - t0) < n)
  {
    /* spin */
  }
}
#endif

/* Tenths of a percent, rounded, so nothing here needs printf's float support. */
static uint32_t permille(uint32_t num, uint32_t den)
{
  if (den == 0U)
  {
    return 0U;
  }
  return (uint32_t)(((uint64_t)num * 1000U + (den / 2U)) / den);
}

#define PM_WHOLE(pm) ((pm) / 10U)
#define PM_FRAC(pm)  ((pm) % 10U)

/* ------------------------------------------------------------------ census */

#define CENSUS_WORDS 32768U
#define LIVE_WORDS    4096U

typedef struct
{
  uint32_t words;
  uint32_t err[16];
  uint32_t n0[16];
  uint32_t ok0[16];
  uint32_t n1[16];
  uint32_t ok1[16];
} census_t;

static census_t s_census;

static uint32_t lcg_next(uint32_t *state)
{
  *state = (*state * 1664525U) + 1013904223U;
  return *state;
}

/*
 * Every write happens before any read, in two passes over the same generator
 * sequence: if two addresses aliased onto one cell, a write-read-write-read
 * order would hide it.
 */
static void census_run(census_t *c, uint32_t seed, uint32_t dwell, uint32_t words)
{
  volatile uint16_t *ram = (volatile uint16_t *)IAP_STAGE_BASE;
  uint32_t state = seed;
  uint32_t i;
  uint32_t b;
  uint32_t d;

  (void)memset(c, 0, sizeof(*c));
  c->words = words;

  for (i = 0U; i < words; i++)
  {
    uint16_t v = (uint16_t)(lcg_next(&state) >> 16);
    for (d = 0U; d < dwell; d++)
    {
      ram[i] = v;
    }
  }

  state = seed;
  for (i = 0U; i < words; i++)
  {
    uint16_t v = (uint16_t)(lcg_next(&state) >> 16);
    uint16_t r = ram[i];
    uint16_t bad = (uint16_t)(v ^ r);

    for (b = 0U; b < 16U; b++)
    {
      uint32_t is_err = ((uint32_t)bad >> b) & 1U;

      if ((((uint32_t)v >> b) & 1U) != 0U)
      {
        c->n1[b]++;
        if (is_err == 0U) { c->ok1[b]++; }
      }
      else
      {
        c->n0[b]++;
        if (is_err == 0U) { c->ok0[b]++; }
      }
      c->err[b] += is_err;
    }
  }
}

static uint32_t census_worst_bit(const census_t *c, uint32_t *out_mask)
{
  uint32_t b;
  uint32_t mask = 0U;
  uint32_t worst = 0U;
  uint32_t worst_bit = 0U;

  for (b = 0U; b < 16U; b++)
  {
    if (c->err[b] != 0U)
    {
      mask |= 1U << b;
      if (c->err[b] > worst)
      {
        worst = c->err[b];
        worst_bit = b;
      }
    }
  }
  if (out_mask != NULL)
  {
    *out_mask = mask;
  }
  return worst_bit;
}

/* D0..D15 in order. Kept next to fmc.c's HAL_FMC_MspInit() pin list; the same
 * mapping is written down in docs/design/HARDWARE-FACTS.md. */
static const char *const s_dq_name[16] = {
  "PD14", "PD15", "PD0", "PD1", "PE7", "PE8", "PE9", "PE10",
  "PE11", "PE12", "PE13", "PE14", "PE15", "PD8", "PD9", "PD10"
};

#if IAP_SDRAM_DIAG_COMMANDS

/* ------------------------------------------------------- release-time probe */

#define CAP_N      32U   /* IDR samples per release */
#define CAP_PHASES  4U   /* NOP offsets, to dither the sampling grid */
#define CAP_REPS   24U

static uint32_t s_cap[CAP_N];

static GPIO_TypeDef *const s_dq_port[16] = {
  GPIOD, GPIOD, GPIOD, GPIOD, GPIOE, GPIOE, GPIOE, GPIOE,
  GPIOE, GPIOE, GPIOE, GPIOE, GPIOE, GPIOD, GPIOD, GPIOD
};
static const uint8_t s_dq_pin[16] = {
  14U, 15U,  0U,  1U,  7U,  8U,  9U, 10U,
  11U, 12U, 13U, 14U, 15U,  8U,  9U, 10U
};

#define MASK_D 0xC703U   /* PD0 PD1 PD8 PD9 PD10 PD14 PD15 */
#define MASK_E 0xFF80U   /* PE7..PE15                      */

/*
 * One release, captured. `rising` charges through the pull-up from a driven
 * low; otherwise it discharges through the pull-down from a driven high.
 */
static void cap_once(GPIO_TypeDef *p, uint16_t mask, uint32_t rising, uint32_t phase)
{
  const uint32_t save_moder  = p->MODER;
  const uint32_t save_otyper = p->OTYPER;
  const uint32_t save_pupdr  = p->PUPDR;
  uint32_t moder_out = save_moder;
  uint32_t moder_in  = save_moder;
  uint32_t pupdr     = save_pupdr;
  uint32_t i;

  for (i = 0U; i < 16U; i++)
  {
    if ((((uint32_t)mask >> i) & 1U) == 0U)
    {
      continue;
    }
    moder_out &= ~(3U << (2U * i));
    moder_out |=  (1U << (2U * i));            /* general purpose output */
    moder_in  &= ~(3U << (2U * i));            /* input                 */
    pupdr     &= ~(3U << (2U * i));
    pupdr     |= (rising != 0U ? 1U : 2U) << (2U * i);
  }

  p->OTYPER &= ~(uint32_t)mask;                /* push-pull */
  p->MODER = moder_out;
  p->BSRR = (rising != 0U) ? ((uint32_t)mask << 16) : (uint32_t)mask;
  delay_cycles(4000U);                         /* ~10 us: fully settled */
  p->PUPDR = pupdr;                            /* arm the pull while still driving */

  __disable_irq();
  p->MODER = moder_in;                         /* release -- transition starts here */
  if (phase > 0U) { __NOP(); }
  if (phase > 1U) { __NOP(); }
  if (phase > 2U) { __NOP(); }
  s_cap[0]  = p->IDR; s_cap[1]  = p->IDR; s_cap[2]  = p->IDR; s_cap[3]  = p->IDR;
  s_cap[4]  = p->IDR; s_cap[5]  = p->IDR; s_cap[6]  = p->IDR; s_cap[7]  = p->IDR;
  s_cap[8]  = p->IDR; s_cap[9]  = p->IDR; s_cap[10] = p->IDR; s_cap[11] = p->IDR;
  s_cap[12] = p->IDR; s_cap[13] = p->IDR; s_cap[14] = p->IDR; s_cap[15] = p->IDR;
  s_cap[16] = p->IDR; s_cap[17] = p->IDR; s_cap[18] = p->IDR; s_cap[19] = p->IDR;
  s_cap[20] = p->IDR; s_cap[21] = p->IDR; s_cap[22] = p->IDR; s_cap[23] = p->IDR;
  s_cap[24] = p->IDR; s_cap[25] = p->IDR; s_cap[26] = p->IDR; s_cap[27] = p->IDR;
  s_cap[28] = p->IDR; s_cap[29] = p->IDR; s_cap[30] = p->IDR; s_cap[31] = p->IDR;
  __enable_irq();

  p->PUPDR  = save_pupdr;
  p->OTYPER = save_otyper;
  p->MODER  = save_moder;                      /* back to AF12 last */
}

static uint32_t cap_cross_index(uint32_t pin, uint32_t rising)
{
  uint32_t k;
  const uint32_t want = (rising != 0U) ? 1U : 0U;

  for (k = 0U; k < CAP_N; k++)
  {
    if (((s_cap[k] >> pin) & 1U) == want)
    {
      return k;
    }
  }
  return CAP_N;   /* did not cross inside the window */
}

/* Mean crossing index x100, accumulated over phases and repeats. */
static void cap_measure(GPIO_TypeDef *p, uint16_t mask, uint32_t rising, uint32_t out_x100[16])
{
  uint32_t sum[16];
  uint32_t phase;
  uint32_t rep;
  uint32_t i;

  (void)memset(sum, 0, sizeof(sum));

  for (phase = 0U; phase < CAP_PHASES; phase++)
  {
    for (rep = 0U; rep < CAP_REPS; rep++)
    {
      cap_once(p, mask, rising, phase);
      for (i = 0U; i < 16U; i++)
      {
        if ((((uint32_t)mask >> i) & 1U) != 0U)
        {
          sum[i] += cap_cross_index(i, rising);
        }
      }
    }
  }

  for (i = 0U; i < 16U; i++)
  {
    out_x100[i] = (sum[i] * 100U) / (CAP_PHASES * CAP_REPS);
  }
}

/* CPU cycles per IDR sample, so the indices above convert to real time. */
static uint32_t cap_sample_cycles_x100(GPIO_TypeDef *p)
{
  uint32_t t0;
  uint32_t t1;

  __disable_irq();
  t0 = DWT->CYCCNT;
  s_cap[0]  = p->IDR; s_cap[1]  = p->IDR; s_cap[2]  = p->IDR; s_cap[3]  = p->IDR;
  s_cap[4]  = p->IDR; s_cap[5]  = p->IDR; s_cap[6]  = p->IDR; s_cap[7]  = p->IDR;
  s_cap[8]  = p->IDR; s_cap[9]  = p->IDR; s_cap[10] = p->IDR; s_cap[11] = p->IDR;
  s_cap[12] = p->IDR; s_cap[13] = p->IDR; s_cap[14] = p->IDR; s_cap[15] = p->IDR;
  s_cap[16] = p->IDR; s_cap[17] = p->IDR; s_cap[18] = p->IDR; s_cap[19] = p->IDR;
  s_cap[20] = p->IDR; s_cap[21] = p->IDR; s_cap[22] = p->IDR; s_cap[23] = p->IDR;
  s_cap[24] = p->IDR; s_cap[25] = p->IDR; s_cap[26] = p->IDR; s_cap[27] = p->IDR;
  s_cap[28] = p->IDR; s_cap[29] = p->IDR; s_cap[30] = p->IDR; s_cap[31] = p->IDR;
  t1 = DWT->CYCCNT;
  __enable_irq();

  return ((t1 - t0) * 100U) / CAP_N;
}

/* ---------------------------------------------------------------- float test */

/*
 * Drive a line, then release it with NO pull at all and read it a millisecond
 * later.
 *
 * This separates the two explanations for a line that charges faster than its
 * neighbours. Less capacitance (the far end is not connected) is symmetric: it
 * speeds up both directions and leaves a released line sitting wherever it was
 * driven, because pad leakage is nanoamps and a millisecond moves such a net by
 * millivolts. A leakage path to a rail is not symmetric: it drags the line one
 * way regardless of where it started, so a line driven low and released reads
 * back high.
 *
 * A 40 kOhm internal pull would win against a weak leak at DC, which is why an
 * earlier "internal pull-up/pull-down both follow" test could not see this.
 * Here nothing competes with the leak.
 */
static void float_test(GPIO_TypeDef *p, uint16_t mask, uint32_t drive_high,
                       uint32_t settle_ms, uint32_t reps, uint32_t out_high[16])
{
  uint32_t rep;
  uint32_t i;

  for (i = 0U; i < 16U; i++)
  {
    out_high[i] = 0U;
  }

  for (rep = 0U; rep < reps; rep++)
  {
    const uint32_t save_moder  = p->MODER;
    const uint32_t save_otyper = p->OTYPER;
    const uint32_t save_pupdr  = p->PUPDR;
    uint32_t moder_out = save_moder;
    uint32_t moder_in  = save_moder;
    uint32_t pupdr     = save_pupdr;
    uint32_t idr;

    for (i = 0U; i < 16U; i++)
    {
      if ((((uint32_t)mask >> i) & 1U) == 0U)
      {
        continue;
      }
      moder_out &= ~(3U << (2U * i));
      moder_out |=  (1U << (2U * i));
      moder_in  &= ~(3U << (2U * i));
      pupdr     &= ~(3U << (2U * i));   /* no pull -- nothing competes with a leak */
    }

    p->OTYPER &= ~(uint32_t)mask;
    p->MODER = moder_out;
    p->BSRR = (drive_high != 0U) ? (uint32_t)mask : ((uint32_t)mask << 16);
    delay_cycles(4000U);
    p->PUPDR = pupdr;
    p->MODER = moder_in;                /* released, floating */
    HAL_Delay(settle_ms);
    idr = p->IDR;

    p->PUPDR  = save_pupdr;
    p->OTYPER = save_otyper;
    p->MODER  = save_moder;

    for (i = 0U; i < 16U; i++)
    {
      if ((((uint32_t)mask >> i) & 1U) != 0U)
      {
        out_high[i] += (idr >> i) & 1U;
      }
    }
  }
}

#endif /* IAP_SDRAM_DIAG_COMMANDS -- the on-demand measurements end here */

/* ------------------------------------------------------------------ reports */

void iap_sdram_diag_boot_summary(void)
{
  uint32_t mask = 0U;
  uint32_t b;
  uint32_t pm;

  census_run(&s_census, 0x13579BDFU, 1U, CENSUS_WORDS);
  b = census_worst_bit(&s_census, &mask);

  if (mask == 0U)
  {
    printf("SDRAM DIAG: %" PRIu32 " words clean -- the boot self-test failure is not a data line\r\n",
           s_census.words);
    return;
  }

  pm = permille(s_census.err[b], s_census.words);
  printf("SDRAM DIAG: bad-bit mask %04" PRIX32 ", worst bit%" PRIu32 " (%s) %" PRIu32 ".%" PRIu32 "%% of %" PRIu32 " words\r\n",
         mask, b, s_dq_name[b], PM_WHOLE(pm), PM_FRAC(pm), s_census.words);

  pm = permille(s_census.ok0[b], s_census.n0[b]);
  printf("SDRAM DIAG: bit%" PRIu32 " wrote-0 read back correct %" PRIu32 ".%" PRIu32 "%%",
         b, PM_WHOLE(pm), PM_FRAC(pm));
  pm = permille(s_census.ok1[b], s_census.n1[b]);
  printf(", wrote-1 %" PRIu32 ".%" PRIu32 "%% (50/50 would be an open line)\r\n",
         PM_WHOLE(pm), PM_FRAC(pm));
#if IAP_SDRAM_DIAG_COMMANDS
  printf("SDRAM DIAG: type \"sdramdiag\" for the full report\r\n");
#else
  /* Do not advertise a command this build does not answer. The full report is
   * one #define away -- see sdram_diag.h. */
  printf("SDRAM DIAG: see docs/work/investigations/sdram-d1-report.html; the full on-board report is\r\n"
         "SDRAM DIAG: compiled out (IAP_SDRAM_DIAG_COMMANDS)\r\n");
#endif
}

#if IAP_SDRAM_DIAG_COMMANDS

static void report_census(void)
{
  uint32_t b;

  printf("\r\n-- 1. per-bit census, %" PRIu32 " words, one write pass then one read pass\r\n",
         (uint32_t)CENSUS_WORDS);
  printf("bit  pin    errors  err%%   wrote0-ok%%  wrote1-ok%%\r\n");

  census_run(&s_census, 0x13579BDFU, 1U, CENSUS_WORDS);

  for (b = 0U; b < 16U; b++)
  {
    uint32_t pe = permille(s_census.err[b], s_census.words);
    uint32_t p0 = permille(s_census.ok0[b], s_census.n0[b]);
    uint32_t p1 = permille(s_census.ok1[b], s_census.n1[b]);

    printf("%2" PRIu32 "   %-5s  %6" PRIu32 "  %2" PRIu32 ".%" PRIu32 "   %3" PRIu32 ".%" PRIu32 "        %3" PRIu32 ".%" PRIu32 "\r\n",
           b, s_dq_name[b], s_census.err[b],
           PM_WHOLE(pe), PM_FRAC(pe),
           PM_WHOLE(p0), PM_FRAC(p0),
           PM_WHOLE(p1), PM_FRAC(p1));
  }
}

static void report_dwell(uint32_t bit)
{
  static const uint32_t dwells[5] = { 1U, 2U, 4U, 8U, 16U };
  uint32_t i;

  printf("\r\n-- 2. dwell sweep on bit%" PRIu32 ": how long the level is held before moving on\r\n", bit);
  printf("writes-per-word  err%%\r\n");

  for (i = 0U; i < 5U; i++)
  {
    uint32_t pm;

    census_run(&s_census, 0x2468ACE0U, dwells[i], CENSUS_WORDS / 4U);
    pm = permille(s_census.err[bit], s_census.words);
    printf("%15" PRIu32 "  %2" PRIu32 ".%" PRIu32 "\r\n", dwells[i], PM_WHOLE(pm), PM_FRAC(pm));
  }
  printf("(falling with dwell = a settling-time mechanism; flat = something else)\r\n");
}

/* Filled in by report_release() so report_float() can combine the two into one
 * verdict: on their own, a fast rise and a line that drifts high mean different
 * things, and only together do they name a location. */
static uint32_t s_rise_ratio;

static void report_release(uint32_t bit)
{
  uint32_t rise[16];
  uint32_t fall[16];
  uint32_t cyc_x100;
  uint32_t i;
  uint32_t peers = 0U;
  uint32_t peer_sum = 0U;
  uint32_t peer_min = 0xFFFFFFFFU;
  uint32_t peer_max = 0U;
  GPIO_TypeDef *port = s_dq_port[bit];
  const uint16_t mask = (port == GPIOD) ? MASK_D : MASK_E;

  cyc_x100 = cap_sample_cycles_x100(port);

  cap_measure(GPIOD, MASK_D, 1U, rise);
  cap_measure(GPIOD, MASK_D, 0U, fall);
  printf("\r\n-- 3. release time, port D lines (mean sample index x100 of %" PRIu32 " releases)\r\n",
         (uint32_t)(CAP_PHASES * CAP_REPS));
  printf("one sample = %" PRIu32 ".%02" PRIu32 " CPU cycles\r\n", cyc_x100 / 100U, cyc_x100 % 100U);
  printf("DQ   pin    rise   fall\r\n");
  for (i = 0U; i < 16U; i++)
  {
    if (s_dq_port[i] != GPIOD)
    {
      continue;
    }
    printf("D%-2" PRIu32 "  %-5s  %5" PRIu32 "  %5" PRIu32 "%s\r\n",
           i, s_dq_name[i], rise[s_dq_pin[i]], fall[s_dq_pin[i]],
           (i == bit) ? "   <== the bad line" : "");
    if (i != bit)
    {
      uint32_t v = rise[s_dq_pin[i]];
      peer_sum += v;
      peers++;
      if (v < peer_min) { peer_min = v; }
      if (v > peer_max) { peer_max = v; }
    }
  }

  if ((peers > 0U) && (mask == MASK_D))
  {
    uint32_t peer_mean = peer_sum / peers;
    uint32_t bad = rise[s_dq_pin[bit]];
    uint32_t ratio = (peer_mean > 0U) ? ((bad * 100U) / peer_mean) : 0U;

    printf("peers on the same port: mean %" PRIu32 ", spread %" PRIu32 "..%" PRIu32 "\r\n",
           peer_mean, peer_min, peer_max);
    printf("bad line / peer mean = %" PRIu32 "%%\r\n", ratio);
    printf("(internal pulls are only specified to 30-50 kOhm, so read the peer spread as the\r\n"
           " noise floor of this comparison. The falling column sits at the resolution floor\r\n"
           " -- one sample is already most of the way down -- so only the rising column is\r\n"
           " worth reading.)\r\n");
    s_rise_ratio = ratio;
  }

  cap_measure(GPIOE, MASK_E, 1U, rise);
  cap_measure(GPIOE, MASK_E, 0U, fall);
  printf("\r\nport E lines, for reference\r\n");
  printf("DQ   pin    rise   fall\r\n");
  for (i = 0U; i < 16U; i++)
  {
    if (s_dq_port[i] != GPIOE)
    {
      continue;
    }
    printf("D%-2" PRIu32 "  %-5s  %5" PRIu32 "  %5" PRIu32 "\r\n",
           i, s_dq_name[i], rise[s_dq_pin[i]], fall[s_dq_pin[i]]);
  }
}

#define FLOAT_REPS 8U
#define FLOAT_MS   2U

static void report_float(uint32_t bit)
{
  uint32_t fl[16];
  uint32_t fh[16];
  uint32_t i;
  uint32_t bad_from_low = 0U;
  uint32_t peer_from_low = 0U;
  uint32_t peers = 0U;
  uint32_t bad_drifts;
  uint32_t peers_drift;
  GPIO_TypeDef *const bad_port = s_dq_port[bit];
  uint32_t pass;

  printf("\r\n-- 4. float test: drive, release with NO pull, read %" PRIu32 " ms later, %" PRIu32 " tries\r\n",
         (uint32_t)FLOAT_MS, (uint32_t)FLOAT_REPS);
  printf("a net with only its own capacitance keeps whatever it was driven to;\r\n"
         "a net with a leak to a rail gets dragged there instead\r\n");
  printf("DQ   pin    from-low reads high   from-high reads high\r\n");

  for (pass = 0U; pass < 2U; pass++)
  {
    GPIO_TypeDef *const p = (pass == 0U) ? GPIOD : GPIOE;
    const uint16_t m = (pass == 0U) ? MASK_D : MASK_E;

    float_test(p, m, 0U, FLOAT_MS, FLOAT_REPS, fl);
    float_test(p, m, 1U, FLOAT_MS, FLOAT_REPS, fh);

    for (i = 0U; i < 16U; i++)
    {
      if (s_dq_port[i] != p)
      {
        continue;
      }
      printf("D%-2" PRIu32 "  %-5s  %13" PRIu32 "/%-3" PRIu32 " %11" PRIu32 "/%-3" PRIu32 "%s\r\n",
             i, s_dq_name[i],
             fl[s_dq_pin[i]], (uint32_t)FLOAT_REPS,
             fh[s_dq_pin[i]], (uint32_t)FLOAT_REPS,
             (i == bit) ? "  <== the bad line" : "");

      if (p == bad_port)
      {
        if (i == bit)
        {
          bad_from_low = fl[s_dq_pin[i]];
        }
        else
        {
          peer_from_low += fl[s_dq_pin[i]];
          peers++;
        }
      }
    }
  }

  bad_drifts  = ((bad_from_low * 2U) > FLOAT_REPS) ? 1U : 0U;
  peers_drift = ((peers > 0U) && ((peer_from_low * 2U) > (peers * FLOAT_REPS))) ? 1U : 0U;

  printf("\r\n");
  if ((bad_drifts != 0U) && (peers_drift == 0U))
  {
    printf("VERDICT: released low, the bad line comes back high while its neighbours stay low.\r\n"
           "         Something is feeding current into this net -- a leak to a high rail, not a\r\n"
           "         plain break. Look for contamination or a bridge at the ball, or a damaged\r\n"
           "         input structure at the SDRAM. This also explains the census: writes of 0\r\n"
           "         fail far more often than writes of 1.\r\n");
  }
  else if ((bad_drifts == 0U) && (peers_drift == 0U) && (s_rise_ratio != 0U) && (s_rise_ratio < 70U))
  {
    printf("VERDICT: no leak, and the bad line charges much faster than its neighbours -- the\r\n"
           "         MCU end cannot see the far end's capacitance. The interruption is at the\r\n"
           "         MCU ball or in the trace, not at the SDRAM.\r\n");
  }
  else if ((bad_drifts == 0U) && (s_rise_ratio >= 70U) && (s_rise_ratio <= 115U))
  {
    printf("VERDICT: no leak, and the bad line matches its neighbours -- the MCU end does see\r\n"
           "         the far end, so the interruption is at the SDRAM ball or inside the SDRAM.\r\n");
  }
  else
  {
    printf("VERDICT: inconclusive -- the two measurements do not agree on one story. Report\r\n"
           "         both tables as they are; do not pick a favourite.\r\n");
  }
}

void iap_sdram_diag_full_report(void)
{
  uint32_t mask = 0U;
  uint32_t bit;

  cyc_start();
  printf("\r\n===== SDRAM data-bus diagnostic =====\r\n");
  printf("staging area %08" PRIX32 ", no probe point exists on this bus (both ends are BGA)\r\n",
         (uint32_t)IAP_STAGE_BASE);

  report_census();
  bit = census_worst_bit(&s_census, &mask);

  if (mask == 0U)
  {
    printf("\r\nno bit errors -- nothing to localise\r\n");
    return;
  }

  report_dwell(bit);
  report_release(bit);
  report_float(bit);
  printf("\r\n===== end of diagnostic =====\r\n");
}

void iap_sdram_diag_live(uint32_t seconds)
{
  uint32_t t;

  cyc_start();
  printf("live error rate, %" PRIu32 " s, %" PRIu32 " words per line -- cool or heat one package now\r\n",
         seconds, (uint32_t)LIVE_WORDS);

  for (t = 0U; t < seconds; t++)
  {
    uint32_t mask = 0U;
    uint32_t bit;
    uint32_t pm;
    uint32_t start = HAL_GetTick();

    census_run(&s_census, 0x13579BDFU + t, 1U, LIVE_WORDS);
    bit = census_worst_bit(&s_census, &mask);
    pm = (mask == 0U) ? 0U : permille(s_census.err[bit], s_census.words);

    printf("t=%3" PRIu32 "s  mask %04" PRIX32 "  bit%" PRIu32 " %2" PRIu32 ".%" PRIu32 "%%\r\n",
           t, mask, bit, PM_WHOLE(pm), PM_FRAC(pm));

    while ((HAL_GetTick() - start) < 1000U)
    {
      /* one line per second, so the operator can correlate with what they do */
    }
  }
  printf("live done\r\n");
}

#endif /* IAP_SDRAM_DIAG_COMMANDS */
