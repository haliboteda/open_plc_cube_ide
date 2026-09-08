// porttool_run.c
//
// One-shot actions - see porttool_run.h.

#include "porttool_run.h"
#include "porttool.h"       /* PORTTOOL_ENABLE, PORTTOOL_REPLY_MAX */

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#include "porttool_handover.h"   /* the one-way entries that share this hardware */
#include "ETH/eth_test.h"
#include "SD/sd_test.h"
#include "SDRAM/sdram_test.h"
#include "port_led.h"
#include "main.h"
#include "rtc.h"

/* A target measures, then writes the k=v body of its OK line into `out`.
 *
 * It must not print the OK line itself: the checks it runs print their own
 * prose as they go, and a half-written OK line with a log line inside it is
 * one the PC cannot parse. Measuring first and formatting after keeps the two
 * kinds of output from interleaving. */
typedef struct {
    const char *name;
    const char *port;  /* the hardware this belongs to, for the caps row */
    const char *board; /* which PCB - see porttool.h PORTTOOL_BOARD_* */
    const char *blk;   /* panel group */
    const char *term;  /* terminal or designator, "-" when there is none */
    void      (*body)(char *out, uint32_t len);
    const char *what;
} run_target_t;

static void run_sdram_probe(char *out, uint32_t len)
{
    sdram_probe_t p;

    SDRAM_Test_Probe(&p);

    snprintf(out, len, "base=0x%08lX size=%lu ready=%u databus=%u addrbus=%u",
             (unsigned long)p.base, (unsigned long)p.size_bytes,
             (unsigned)p.ready, (unsigned)p.databus_ok, (unsigned)p.addrbus_ok);
}

/* The stress pass the production test guide asks for: the whole 64 MiB, four
 * patterns. Tens of seconds, so a plan step running this needs a timeout that
 * allows for it - the reply carries the milliseconds it actually took. */
static void run_sdram_sweep(char *out, uint32_t len)
{
    sdram_sweep_t s;

    (void) SDRAM_Test_SweepOnce(&s);

    snprintf(out, len,
             "ready=%u patterns=%lu words_each=%lu mismatches=%lu"
             " first_bad=0x%08lX bad_pattern=0x%08lX write_ms=%lu verify_ms=%lu",
             (unsigned)s.ready, (unsigned long)s.patterns,
             (unsigned long)s.words_each, (unsigned long)s.mismatches,
             (unsigned long)s.first_bad_offset, (unsigned long)s.first_bad_pattern,
             (unsigned long)s.write_ms, (unsigned long)s.verify_ms);
}

/* One write/wait/read-back cycle. How many cycles a station runs is the plan's
 * business, not the firmware's - the handover entry is the one that loops. */
static void run_sdram_retention(char *out, uint32_t len)
{
    sdram_retention_t r;

    (void) SDRAM_Test_RetentionOnce(&r);

    snprintf(out, len,
             "ready=%u checked=%lu failed=%lu wait_ms=%lu first_bad=0x%08lX seed=0x%08lX",
             (unsigned)r.ready, (unsigned long)r.checked, (unsigned long)r.failed,
             (unsigned long)r.wait_ms, (unsigned long)r.first_bad_addr,
             (unsigned long)r.seed);
}

/* The port tool runs from main.c's Phase 1, where MX_RTC_Init() has not run
 * yet, so this brings the RTC up on demand like the SDRAM probe does.
 *
 * Only reads. Setting the calendar is deliberately not offered here: the
 * backup domain also holds iap_auth's nonce counter, and a test that writes
 * there would be a test that can roll a security counter back. */
static void run_rtc_read(char *out, uint32_t len)
{
    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    if (hrtc.State != HAL_RTC_STATE_READY) {
        printf("RTC_TEST: not up yet, running MX_RTC_Init()\r\n");
        MX_RTC_Init();
    }

    /* The time has to be read before the date: reading the time locks the
     * shadow registers, and reading the date is what releases them. Swapped,
     * the two can come from either side of a tick. */
    (void)HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
    (void)HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);

    /* clk=lsi is a fact worth reporting, not decoration: the RTC runs off the
     * internal RC oscillator (Core/Src/rtc.c selects RCC_RTCCLKSOURCE_LSI),
     * so its drift is orders of magnitude worse than a crystal's. Anything
     * about RTC accuracy has to be judged against that. */
    snprintf(out, len,
             "init=%u clk=lsi date=%02u-%02u-%02u time=%02u:%02u:%02u",
             (unsigned)__HAL_RTC_IS_CALENDAR_INITIALIZED(&hrtc),
             (unsigned)d.Year, (unsigned)d.Month, (unsigned)d.Date,
             (unsigned)t.Hours, (unsigned)t.Minutes, (unsigned)t.Seconds);
}

#define LED_PULSES     6U
#define LED_HALF_MS  250U

static void run_led_blink(char *out, uint32_t len)
{
    PortLed_Blink(LED_PULSES, LED_HALF_MS);

    /* Reports what it drove, not whether anybody saw it - there is no readback
     * on this pin, so the verdict is a person's. */
    snprintf(out, len, "pin=PE2 pulses=%lu half_ms=%lu observed=unknown",
             (unsigned long)LED_PULSES, (unsigned long)LED_HALF_MS);
}

static void run_sd_probe(char *out, uint32_t len)
{
    sd_probe_t p;

    SD_Test_Probe(&p);

    snprintf(out, len,
             "detected=%u ready=%u blocks=%lu block_size=%lu mib=%lu v2x=%u class=%lu",
             (unsigned)p.detected, (unsigned)p.ready,
             (unsigned long)p.block_count, (unsigned long)p.block_size,
             (unsigned long)p.capacity_mib, (unsigned)p.version_2x,
             (unsigned long)p.card_class);
}

static void run_sd_integrity(char *out, uint32_t len)
{
    sd_integrity_t r;

    (void) SD_Test_IntegrityOnce(&r);

    /* identical is the whole verdict in one flag, but the parts are reported
     * too: a card that mounts and writes but reads back wrong is a different
     * problem from one that never mounted, and the PC should not have to guess
     * which from a single bit. */
    snprintf(out, len,
             "mounted=%u wrote=%u read_back=%u identical=%u bytes=%lu"
             " write_crc=0x%08lX read_crc=0x%08lX fresult=%d",
             (unsigned)r.mounted, (unsigned)r.wrote, (unsigned)r.read_back,
             (unsigned)r.identical, (unsigned long)r.bytes,
             (unsigned long)r.write_crc, (unsigned long)r.read_crc, r.fresult);
}

/* The PHY over MDIO only - no lwIP, no DMA, no RMII reference clock. See
 * ETH/eth_test.h for what that does and does not prove. */
static void run_eth_link(char *out, uint32_t len)
{
    eth_probe_t p;

    (void) ETH_Test_Probe(&p);

    /* found=0 and link=0 are different answers: no PHY on MDIO is a board
     * fault, a PHY with no link is usually an unplugged cable. A station that
     * saw one bit would not know which to go and look at. */
    snprintf(out, len,
             "found=%u addr=%u id=0x%08lX link=%u autoneg=%u speed=%u fd=%u"
             " bsr=0x%04X scsr=0x%04X mdio_errors=%lu",
             (unsigned)p.found, (unsigned)p.addr, (unsigned long)p.phy_id,
             (unsigned)p.link, (unsigned)p.autoneg_done,
             (unsigned)p.speed_mbit, (unsigned)p.full_duplex,
             (unsigned)p.bsr, (unsigned)p.scsr,
             (unsigned long)p.mdio_errors);
}

static void run_sd_stress(char *out, uint32_t len)
{
    sd_stress_t s;

    (void) SD_Test_StressOnce(&s);

    /* passes is what was attempted and passed is what came back identical, so
     * a run that stopped early is visible as the two differing rather than as
     * a smaller-looking but apparently clean result. */
    snprintf(out, len,
             "mounted=%u passes=%lu passed=%lu bytes_each=%lu bytes_total=%lu"
             " elapsed_ms=%lu first_bad_pass=%lu fresult=%d",
             (unsigned)s.mounted, (unsigned long)s.passes,
             (unsigned long)s.passed, (unsigned long)s.bytes_each,
             (unsigned long)s.bytes_total, (unsigned long)s.elapsed_ms,
             (unsigned long)s.first_bad_pass, s.fresult);
}

static const run_target_t targets[] = {
    { "sd.probe",        "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_probe,
      "detect pin, card identification, capacity and class" },
    { "sd.integrity",    "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_integrity,
      "one write/read/verify round through FatFs; reports both CRCs" },
    { "sd.stress",       "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_stress,
      "64 write/read/verify rounds; reports rounds passed and elapsed time" },
    { "sdram.probe",     "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6", run_sdram_probe,
      "FMC bring-up, data bus and address bus; reports the window and three flags" },
    { "sdram.sweep",     "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6", run_sdram_sweep,
      "all 64 MiB, four patterns; reports mismatches and where the first was" },
    { "sdram.retention", "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6", run_sdram_retention,
      "one write/wait-5s/read-back cycle over 64 random addresses" },
    { "eth.link",        "eth", PORTTOOL_BOARD_BRIDGE, "-", "J1", run_eth_link,
      "PHY identity, link, and the speed/duplex auto-negotiation settled on" },
    { "rtc.read",        "rtc", PORTTOOL_BOARD_BRIDGE, "-", "-",  run_rtc_read,
      "brings the RTC up if needed and reads the calendar; never writes it" },
    { "led.blink",       "led", PORTTOOL_BOARD_BRIDGE, "-", "-",  run_led_blink,
      "blinks the system indicator on PE2; a person decides whether it lit" },
};
#define TARGET_COUNT (sizeof(targets) / sizeof(targets[0]))

uint32_t PortTool_RunTargetsFor(const char *port, char *out, uint32_t out_len)
{
    uint32_t found = 0;
    uint32_t n = 0;

    if (out_len > 0U) {
        out[0] = '\0';
    }

    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        if (strcmp(targets[i].port, port) != 0) {
            continue;
        }
        found++;
        if (n < out_len) {
            int w = snprintf(out + n, out_len - n, "%s%s",
                             (n > 0U) ? "," : "", targets[i].name);
            if (w < 0) { break; }
            n += (uint32_t)w;
        }
    }

    return found;
}

/* True when entry `i` is the first of its port group, so a group is reported
 * once however the table is ordered. */
static int first_of_port(uint32_t i)
{
    for (uint32_t j = 0; j < i; j++) {
        if (strcmp(targets[j].port, targets[i].port) == 0) {
            return 0;
        }
    }
    return 1;
}

/* One row per piece of hardware, not one per target - four rows for eight
 * targets. sd and sdram anchor here rather than on a handover row of their
 * own, and their one-way entries come along in targets=: two caps rows with
 * the same port= would give the panel two cards for one piece of hardware
 * (DECISIONS.md 17). */
uint32_t PortTool_RunCaps(int emit)
{
    char runs[96];
    char deep[96];
    uint32_t lines = 0;

    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        if (!first_of_port(i)) {
            continue;
        }
        /* Hardware that also has a session already printed its row there, with
         * these targets on it as runs=. A second row under the same port= would
         * give the panel two cards for one RJ45 (DECISIONS.md 17, 28). */
        if (PortTool_IsSessionPort(targets[i].port)) {
            continue;
        }
        lines++;
        if (!emit) {
            continue;
        }

        (void) PortTool_RunTargetsFor(targets[i].port, runs, sizeof(runs));
        uint32_t n = PortTool_HandoverTargetsFor(targets[i].port, deep, sizeof(deep));

        printf("OK port=%s board=%s kind=run blk=%s term=%s channels=1 "
               "loop=none runs=%s",
               targets[i].port, targets[i].board, targets[i].blk,
               targets[i].term, runs);
        if (n > 0U) {
            printf(" targets=%s", deep);
        }
        printf("\r\n");
    }

    return lines;
}

int PortTool_RunTarget(const char *name)
{
    char body[PORTTOOL_REPLY_MAX];

    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        if (strcmp(targets[i].name, name) != 0) {
            continue;
        }
        body[0] = '\0';
        targets[i].body(body, (uint32_t)sizeof(body));
        printf("OK %s %s\r\n", targets[i].name, body);
        return 1;
    }
    return 0;
}

void PortTool_RunList(void)
{
    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        printf("OK run=%-16s %s\r\n", targets[i].name, targets[i].what);
    }
}

#endif /* PORTTOOL_ENABLE */
