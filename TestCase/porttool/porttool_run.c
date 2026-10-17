// porttool_run.c
//
// One-shot actions - see porttool_run.h.

#include "porttool_run.h"
#include "porttool_cmd.h"
#include "porttool.h"       /* PORTTOOL_ENABLE, PORTTOOL_REPLY_MAX */

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#include "ETH/eth_test.h"
#include "KNX/knx_test.h"
#include "SD/sd_test.h"
#include "SDRAM/sdram_test.h"
#include "IAP_boot_handoff.h"   /* the reset cause main() latched at boot */
#include "port_led.h"
#include "port_rs485.h"          /* the PD4/PD5 pin map, shared with the session */
#include "main.h"
#include "rtc.h"

/* A target measures, then writes the k=v body of its OK line into `out`; it
 * must not print the OK line itself, or the checks' own log lines would land
 * inside it. `args` is what followed the target name. */
typedef struct {
    const char *name;
    const char *port;  /* the hardware this belongs to, for the caps row */
    const char *board; /* which PCB - see porttool.h PORTTOOL_BOARD_* */
    const char *blk;   /* panel group */
    const char *term;  /* terminal or designator, "-" when there is none */
    void      (*body)(const char *args, char *out, uint32_t len);
    const char *what;
} run_target_t;

static void run_sdram_probe(const char *args, char *out, uint32_t len)
{
    (void)args;
    sdram_probe_t p;

    SDRAM_Test_Probe(&p);

    snprintf(out, len, "base=0x%08lX size=%lu ready=%u databus=%u addrbus=%u",
             (unsigned long)p.base, (unsigned long)p.size_bytes,
             (unsigned)p.ready, (unsigned)p.databus_ok, (unsigned)p.addrbus_ok);
}

/* The whole 64 MiB, four patterns: tens of seconds, so a plan step needs a
 * timeout to match. */
static void run_sdram_sweep(const char *args, char *out, uint32_t len)
{
    (void)args;
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

/* Brings the RTC up on demand (main.c Phase 1 has not run MX_RTC_Init()) and
 * only reads; the calendar is never set here. */
static void run_rtc_read(const char *args, char *out, uint32_t len)
{
    (void)args;
    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    if (hrtc.State != HAL_RTC_STATE_READY) {
        printf("RTC_TEST: not up yet, running MX_RTC_Init()\r\n");
        MX_RTC_Init();
    }

    /* Time before date: reading the time locks the shadow registers until the
     * date is read (RM0433 RTC). */
    (void)HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
    (void)HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);

    /* The oscillator the calendar actually counts, read from RCC_BDCR so it
     * cannot disagree with the hardware (LSI: a few percent; LSE: tens of ppm). */
    static const char *const rtcsel[] = { "none", "lse", "lsi", "hse" };
    const char *clk = rtcsel[(RCC->BDCR & RCC_BDCR_RTCSEL) >> RCC_BDCR_RTCSEL_Pos];

    snprintf(out, len,
             "init=%u clk=%s date=%02u-%02u-%02u time=%02u:%02u:%02u",
             (unsigned)__HAL_RTC_IS_CALENDAR_INITIALIZED(&hrtc), clk,
             (unsigned)d.Year, (unsigned)d.Month, (unsigned)d.Date,
             (unsigned)t.Hours, (unsigned)t.Minutes, (unsigned)t.Seconds);
}

#define LED_PULSES     6U
#define LED_HALF_MS  250U

static void run_led_blink(const char *args, char *out, uint32_t len)
{
    (void)args;
    PortLed_Blink(LED_PULSES, LED_HALF_MS);

    /* No readback on this pin: the verdict is a person's. */
    snprintf(out, len, "pin=PE2 pulses=%lu half_ms=%lu observed=unknown",
             (unsigned long)LED_PULSES, (unsigned long)LED_HALF_MS);
}

/* exfat is named apart: the card is fine, the format is not (reformat, not bin). */
static const char *run_sd_fs_name(uint8_t t)
{
    switch (t) {
    case SD_FS_FAT12:   return "fat12";
    case SD_FS_FAT16:   return "fat16";
    case SD_FS_FAT32:   return "fat32";
    case SD_FS_EXFAT:   return "exfat";
    case SD_FS_UNKNOWN: return "unknown";
    default:            return "none";
    }
}

static void run_sd_probe(const char *args, char *out, uint32_t len)
{
    (void)args;
    sd_probe_t p;

    SD_Test_Probe(&p);

    snprintf(out, len,
             "detected=%u ready=%u blocks=%lu block_size=%lu mib=%lu v2x=%u"
             " class=%lu fs=%s err=0x%08lX",
             (unsigned)p.detected, (unsigned)p.ready,
             (unsigned long)p.block_count, (unsigned long)p.block_size,
             (unsigned long)p.capacity_mib, (unsigned)p.version_2x,
             (unsigned long)p.card_class, run_sd_fs_name(p.fs_type),
             (unsigned long)p.hal_error);
}

/* Write a file, read it back, compare - passes= times (default 1). identical=1
 * means every round came back byte for byte; the parts are reported too. */
static void run_sd_integrity(const char *args, char *out, uint32_t len)
{
    uint32_t bytes = 0;
    uint32_t passes = 0;

    (void) PortCmd_GetU32(args, "bytes", &bytes);
    (void) PortCmd_GetU32(args, "passes", &passes);
    if (passes == 0U) {
        passes = 1U;
    }

    /* Both shapes carry mounted/identical/passes/passed/bytes; one round adds
     * the two CRCs, many rounds add the first failing round. */
    if (passes == 1U) {
        sd_integrity_t r;

        (void) SD_Test_IntegrityOnce(bytes, &r);
        snprintf(out, len,
                 "mounted=%u identical=%u passes=1 passed=%u wrote=%u read_back=%u"
                 " bytes_each=%lu bytes_total=%lu write_crc=0x%08lX read_crc=0x%08lX"
                 " fresult=%d",
                 (unsigned)r.mounted, (unsigned)r.identical,
                 (unsigned)(r.identical ? 1U : 0U),
                 (unsigned)r.wrote, (unsigned)r.read_back,
                 (unsigned long)r.bytes, (unsigned long)r.bytes,
                 (unsigned long)r.write_crc, (unsigned long)r.read_crc, r.fresult);
        return;
    }

    {
        sd_stress_t s;

        (void) SD_Test_StressOnce(bytes, passes, &s);
        snprintf(out, len,
                 "mounted=%u identical=%u passes=%lu passed=%lu bytes_each=%lu"
                 " bytes_total=%lu elapsed_ms=%lu first_bad_pass=%lu fresult=%d",
                 (unsigned)s.mounted,
                 (unsigned)((s.passes > 0U) && (s.passed == s.passes)),
                 (unsigned long)s.passes, (unsigned long)s.passed,
                 (unsigned long)s.bytes_each, (unsigned long)s.bytes_total,
                 (unsigned long)s.elapsed_ms, (unsigned long)s.first_bad_pass,
                 s.fresult);
    }
}

/* The PHY over MDIO only - no lwIP, no DMA, no RMII reference clock. See
 * ETH/eth_test.h for what that does and does not prove. */
static void run_eth_link(const char *args, char *out, uint32_t len)
{
    (void)args;
    eth_probe_t p;

    (void) ETH_Test_Probe(&p);

    /* found=0 (no PHY: board fault) and link=0 (usually no cable) kept apart. */
    snprintf(out, len,
             "found=%u addr=%u id=0x%08lX link=%u autoneg=%u speed=%u fd=%u"
             " bsr=0x%04X scsr=0x%04X mdio_errors=%lu",
             (unsigned)p.found, (unsigned)p.addr, (unsigned long)p.phy_id,
             (unsigned)p.link, (unsigned)p.autoneg_done,
             (unsigned)p.speed_mbit, (unsigned)p.full_duplex,
             (unsigned)p.bsr, (unsigned)p.scsr,
             (unsigned long)p.mdio_errors);
}

/* B/s, not KB/s: the wire carries the measured number, the PC formats it. */
static void run_sd_speed(const char *args, char *out, uint32_t len)
{
    sd_speed_t r;
    uint32_t bytes = 0;

    (void) PortCmd_GetU32(args, "bytes", &bytes);
    (void) SD_Test_Speed(bytes, &r);

    snprintf(out, len,
             "mounted=%u bytes=%lu write_ms=%lu read_ms=%lu"
             " write_bps=%lu read_bps=%lu fresult=%d",
             (unsigned)r.mounted, (unsigned long)r.bytes,
             (unsigned long)r.write_ms, (unsigned long)r.read_ms,
             (unsigned long)r.write_bps, (unsigned long)r.read_bps,
             r.fresult);
}

/* Why the board last came up, as main() latched it from RCC->RSR
 * (boot_handoff_latch_reset_cause). The PC sees THAT a board restarted from the
 * tick; this says why (PIN vs IWDG). The raw word keeps flags the name's
 * first-match order hides. */
static void run_reset_cause(const char *args, char *out, uint32_t len)
{
    (void)args;

    snprintf(out, len, "cause=%s rsr=0x%08lX",
             boot_handoff_reset_cause_str(),
             (unsigned long)boot_handoff_reset_rsr());
}

/* CRC32 of a window of SDRAM, reading only: checks what a tool wrote there
 * (DECISIONS.md 40). bytes=0 or absent means the whole array; the reply names
 * the window it summed. */
static void run_sdram_crc(const char *args, char *out, uint32_t len)
{
    sdram_crc_t c;
    uint32_t offset = 0;
    uint32_t bytes = 0;

    (void) PortCmd_GetU32(args, "offset", &offset);
    (void) PortCmd_GetU32(args, "bytes", &bytes);

    (void) SDRAM_Test_Crc32Once(offset, bytes, &c);

    snprintf(out, len, "ready=%u offset=%lu bytes=%lu crc=0x%08lX",
             (unsigned)c.ready, (unsigned long)c.offset,
             (unsigned long)c.length, (unsigned long)c.crc);
}

/* PD4 (/RE and DE) and PD5 (TX) driven as plain GPIO and read back, nothing
 * attached: tells a dead pin from a missing cable. Answers busy=1 while the
 * rs485 session owns the pins. */
static void run_rs485_pins(const char *args, char *out, uint32_t len)
{
    GPIO_InitTypeDef gpio = {0};
    int low_ok, high_ok;
    GPIO_PinState dir_low, tx_low, dir_high, tx_high;

    (void)args;

    if (PortTool_PortRunning("rs485")) {
        snprintf(out, len, "checked=0 busy=1");
        return;
    }

    __HAL_RCC_GPIOD_CLK_ENABLE();

    gpio.Pin   = PORT_RS485_DIR_PIN | PORT_RS485_TX_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(PORT_RS485_DIR_PORT, &gpio);

    HAL_GPIO_WritePin(PORT_RS485_DIR_PORT,
                      PORT_RS485_DIR_PIN | PORT_RS485_TX_PIN, GPIO_PIN_RESET);
    dir_low = HAL_GPIO_ReadPin(PORT_RS485_DIR_PORT, PORT_RS485_DIR_PIN);
    tx_low  = HAL_GPIO_ReadPin(PORT_RS485_DIR_PORT, PORT_RS485_TX_PIN);

    HAL_GPIO_WritePin(PORT_RS485_DIR_PORT,
                      PORT_RS485_DIR_PIN | PORT_RS485_TX_PIN, GPIO_PIN_SET);
    dir_high = HAL_GPIO_ReadPin(PORT_RS485_DIR_PORT, PORT_RS485_DIR_PIN);
    tx_high  = HAL_GPIO_ReadPin(PORT_RS485_DIR_PORT, PORT_RS485_TX_PIN);

    /* Left in receive with the driver off, so the pair is not held driven. */
    HAL_GPIO_WritePin(PORT_RS485_DIR_PORT,
                      PORT_RS485_DIR_PIN | PORT_RS485_TX_PIN, GPIO_PIN_RESET);

    low_ok  = (dir_low == GPIO_PIN_RESET) && (tx_low == GPIO_PIN_RESET);
    high_ok = (dir_high == GPIO_PIN_SET) && (tx_high == GPIO_PIN_SET);

    /* Each pin at each level is its own field, so a stuck pin says which way. */
    snprintf(out, len,
             "checked=1 busy=0 dir_low=%u tx_low=%u dir_high=%u tx_high=%u"
             " follows=%u",
             (unsigned)(dir_low == GPIO_PIN_SET),
             (unsigned)(tx_low == GPIO_PIN_SET),
             (unsigned)(dir_high == GPIO_PIN_SET),
             (unsigned)(tx_high == GPIO_PIN_SET),
             (unsigned)(low_ok && high_ok));
}

/* KNX diagnostics (DECISIONS.md 87, PORTTOOL-FLOW.md A.3). They share the knx
 * session's timers and statistics, so a running session makes them answer
 * busy=1 and touch nothing. */
#define KNX_DIAG_REPEAT_DEF 3U
#define KNX_DIAG_REPEAT_MAX 10U

static uint32_t run_knx_count(const char *args, const char *key)
{
    uint32_t n = KNX_DIAG_REPEAT_DEF;

    (void) PortCmd_GetU32(args, key, &n);
    if (n == 0U) {
        n = 1U;
    }
    return (n > KNX_DIAG_REPEAT_MAX) ? KNX_DIAG_REPEAT_MAX : n;
}

static void run_knx_edges(const char *args, char *out, uint32_t len)
{
    knx_diag_edges_t r;

    if (PortTool_PortRunning("knx")) {
        snprintf(out, len, "busy=1");
        return;
    }
    (void) KNX_Test_DiagEdges(run_knx_count(args, "shots"), &r);
    snprintf(out, len,
             "busy=0 bus=%s shots=%lu moved=%lu edges=%lu dropped=%lu"
             " first_us=%ld width_us=%ld",
             r.bus_ok ? "ok" : "dead", (unsigned long)r.shots,
             (unsigned long)r.moved, (unsigned long)r.edges,
             (unsigned long)r.dropped, (long)r.first_us, (long)r.width_us);
}

static uint32_t run_knx_list(char *out, uint32_t len, const uint32_t *v)
{
    uint32_t n = 0U;

    for (uint32_t i = 0U; (i < KNX_DIAG_BINS) && (n < len); i++) {
        n += (uint32_t)snprintf(out + n, len - n, "%s%lu", (i != 0U) ? "," : "",
                                (unsigned long)v[i]);
    }
    return n;
}

static void run_knx_hist(const char *args, char *out, uint32_t len)
{
    knx_diag_hist_t r;
    char w[64], d[64];

    if (PortTool_PortRunning("knx")) {
        snprintf(out, len, "busy=1");
        return;
    }
    (void) KNX_Test_DiagHist(run_knx_count(args, "bursts"), &r);
    (void) run_knx_list(w, sizeof(w), r.w_hist);
    (void) run_knx_list(d, sizeof(d), r.d_hist);
    snprintf(out, len,
             "busy=0 bus=%s tx=%lu rx=%lu miss=%lu w_min=%lu w_avg=%lu w_max=%lu"
             " d_min=%lu d_avg=%lu d_max=%lu w_hist=%s d_hist=%s",
             r.bus_ok ? "ok" : "dead", (unsigned long)r.tx, (unsigned long)r.rx,
             (unsigned long)r.miss, (unsigned long)r.w_min, (unsigned long)r.w_avg,
             (unsigned long)r.w_max, (unsigned long)r.d_min, (unsigned long)r.d_avg,
             (unsigned long)r.d_max, w, d);
}

static void run_knx_pullup(const char *args, char *out, uint32_t len)
{
    knx_diag_pullup_t r;

    (void)args;
    if (PortTool_PortRunning("knx")) {
        snprintf(out, len, "busy=1");
        return;
    }
    (void) KNX_Test_DiagPullup(&r);
    snprintf(out, len,
             "busy=0 bus=%s pu_tx=%lu pu_rx=%lu pu_miss=%lu pu_w_avg=%lu"
             " np_tx=%lu np_rx=%lu np_miss=%lu np_w_avg=%lu",
             r.bus_ok ? "ok" : "dead",
             (unsigned long)r.tx[0], (unsigned long)r.rx[0],
             (unsigned long)r.miss[0], (unsigned long)r.w_avg[0],
             (unsigned long)r.tx[1], (unsigned long)r.rx[1],
             (unsigned long)r.miss[1], (unsigned long)r.w_avg[1]);
}

static const run_target_t targets[] = {
    { "sd.probe",        "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_probe,
      "detect pin, card identification, capacity and class" },
    { "sd.integrity",    "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_integrity,
      "write/read/verify through FatFs, passes= rounds of bytes= each" },
    { "sd.speed",        "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_speed,
      "how fast the card moves data each way; does not verify content" },
    { "sd.put",          "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", PortSdFile_RunPut,
      "arm a write of name= size= into PORTTOOL/; data on eth or usb mode=file" },
    { "sd.get",          "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", PortSdFile_RunGet,
      "send PORTTOOL/name= back on eth or usb mode=file; reply size and crc" },
    { "sd.ls",           "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", PortSdFile_RunList,
      "list PORTTOOL/, one !sd.file line per file" },
    { "sd.rm",           "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", PortSdFile_RunRemove,
      "delete PORTTOOL/name=" },
    { "sd.eject",        "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", PortSdFile_RunEject,
      "stop any transfer and unmount so the card can be pulled" },
    { "sdram.probe",     "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6", run_sdram_probe,
      "FMC bring-up, data bus and address bus; reports the window and three flags" },
    { "sdram.sweep",     "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6", run_sdram_sweep,
      "all 64 MiB, four patterns; reports mismatches and where the first was" },
    { "eth.link",        "eth", PORTTOOL_BOARD_BRIDGE, "-", "J1", run_eth_link,
      "PHY identity, link, and the speed/duplex auto-negotiation settled on" },
    { "rtc.read",        "rtc", PORTTOOL_BOARD_BRIDGE, "-", "-",  run_rtc_read,
      "brings the RTC up if needed and reads the calendar; never writes it" },
    { "sdram.crc",       "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6",
      run_sdram_crc,
      "CRC32 of one window; reads only, so it checks what a tool wrote there" },
    { "rs485.pins",      "rs485", PORTTOOL_BOARD_UPPER, "C", "C10,C11",
      run_rs485_pins,
      "drives PD4 and PD5 as plain GPIO and reads them back; nothing attached" },
    { "reset.cause",     "reset", PORTTOOL_BOARD_BRIDGE, "-", "-",
      run_reset_cause,
      "why the board last came up - POR, PIN, IWDG, WWDG, SOFT or BOR" },
    { "led.blink",       "led", PORTTOOL_BOARD_BRIDGE, "-", "-",  run_led_blink,
      "blinks the system indicator on PE2; a person decides whether it lit" },
    { "knx.edges",       "knx", PORTTOOL_BOARD_UPPER, "C", "C03,C04", run_knx_edges,
      "single pulses, every KNX_RX edge after each; shots= how many" },
    { "knx.hist",        "knx", PORTTOOL_BOARD_UPPER, "C", "C03,C04", run_knx_hist,
      "pulse bursts; KNX_RX width and TX->RX delay, min/avg/max and histograms" },
    { "knx.pullup",      "knx", PORTTOOL_BOARD_UPPER, "C", "C03,C04", run_knx_pullup,
      "one burst with PA10's pull-up on, one with it off; leaves it on" },
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

/* One caps row per piece of hardware, not per target (DECISIONS.md 17). */
uint32_t PortTool_RunCaps(int emit)
{
    char runs[96];
    uint32_t lines = 0;

    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        if (!first_of_port(i)) {
            continue;
        }
        /* A session port already printed these as runs= (DECISIONS.md 17, 28). */
        if (PortTool_IsSessionPort(targets[i].port)) {
            continue;
        }
        lines++;
        if (!emit) {
            continue;
        }

        (void) PortTool_RunTargetsFor(targets[i].port, runs, sizeof(runs));

        printf("OK port=%s board=%s kind=run blk=%s term=%s channels=1 "
               "loop=none runs=%s",
               targets[i].port, targets[i].board, targets[i].blk,
               targets[i].term, runs);
        printf("\r\n");
    }

    return lines;
}

int PortTool_RunTarget(const char *name, const char *args)
{
    char body[PORTTOOL_REPLY_MAX];

    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        if (strcmp(targets[i].name, name) != 0) {
            continue;
        }
        body[0] = '\0';
        targets[i].body(args, body, (uint32_t)sizeof(body));
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
