// porttool_run.c
//
// One-shot actions - see porttool_run.h.

#include "porttool_run.h"
#include "porttool_cmd.h"
#include "porttool.h"       /* PORTTOOL_ENABLE, PORTTOOL_REPLY_MAX */

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#include "porttool_handover.h"   /* the one-way entries that share this hardware */
#include "ETH/eth_test.h"
#include "SD/sd_test.h"
#include "SDRAM/sdram_test.h"
#include "port_led.h"
#include "port_rs485.h"          /* the PD4/PD5 pin map, shared with the session */
#include "main.h"
#include "rtc.h"

/* A target measures, then writes the k=v body of its OK line into `out`.
 *
 * `args` is whatever followed the target name on the pt.run line, so a target
 * can take a parameter the way a session does - "pt.run sd.integrity
 * bytes=1048576". Most ignore it. It comes first for the same reason it does
 * in a session's apply(): inputs before outputs.
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

/* The stress pass the production test guide asks for: the whole 64 MiB, four
 * patterns. Tens of seconds, so a plan step running this needs a timeout that
 * allows for it - the reply carries the milliseconds it actually took. */
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

/* One write/wait/read-back cycle. How many cycles a station runs is the plan's
 * business, not the firmware's - the handover entry is the one that loops. */
static void run_sdram_retention(const char *args, char *out, uint32_t len)
{
    (void)args;
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
static void run_rtc_read(const char *args, char *out, uint32_t len)
{
    (void)args;
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

    /* Which oscillator the calendar is actually counting, read out of
     * RCC_BDCR rather than written here as text.
     *
     * *** It used to be the literal "clk=lsi", and on 2026-09-10 the project
     * *** moved to LSE - so the reply went on saying lsi against a board that
     * *** had a crystal. A field that cannot disagree with the hardware is not
     * *** a reading; it is a comment that happens to be inside a frame.
     *
     * *** Worth a field at all because everything about RTC accuracy hangs on
     * *** it: LSI is an internal RC, a few percent and drifting with
     * *** temperature; LSE is a crystal, tens of ppm. A timestamp means
     * *** different things under the two. */
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

    /* Reports what it drove, not whether anybody saw it - there is no readback
     * on this pin, so the verdict is a person's. */
    snprintf(out, len, "pin=PE2 pulses=%lu half_ms=%lu observed=unknown",
             (unsigned long)LED_PULSES, (unsigned long)LED_HALF_MS);
}

/* ⚠️ exfat is a PASS-able state for the card and a FAIL for the station: the
 * card is fine, the format is not. Saying which is the difference between
 * "reformat it" and "bin it". */
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

static void run_sd_integrity(const char *args, char *out, uint32_t len)
{
    sd_integrity_t r;
    uint32_t bytes = 0;

    /* 0 leaves the default. A plan says how much to move; nothing here caps it
     * beyond what the card and the filesystem will take, because "how big a
     * transfer counts as proof" is a production decision, not a firmware one
     * (DECISIONS.md 22). */
    (void) PortCmd_GetU32(args, "bytes", &bytes);

    (void) SD_Test_IntegrityOnce(bytes, &r);

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
static void run_eth_link(const char *args, char *out, uint32_t len)
{
    (void)args;
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

/* ⚠️ B/s, not KB/s: a limit written against a rounded kilobyte figure cannot
 * express "at least 400 kB/s" without the plan doing arithmetic. The PC
 * formats it for a person; the wire carries the number it measured. */
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

static void run_sd_stress(const char *args, char *out, uint32_t len)
{
    sd_stress_t s;
    uint32_t bytes = 0;
    uint32_t passes = 0;

    /* 0 leaves the defaults. Both are the plan's call: fewer larger rounds and
     * more smaller ones stress different things about a card. */
    (void) PortCmd_GetU32(args, "bytes", &bytes);
    (void) PortCmd_GetU32(args, "passes", &passes);

    (void) SD_Test_StressOnce(bytes, passes, &s);

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

/* CRC32 of a window of SDRAM, reading only.
 *
 * The pair to STM32CubeProgrammer's "Read & Write Memory": a tool writes a
 * file into the array and this says what actually landed, so a matching CRC32
 * proves the bytes survived the round trip through the physical part. That
 * used to be reachable only by handing the board over, which meant nothing on
 * the PC could read a result out of it (DECISIONS.md 40).
 *
 * offset= and bytes= come from the plan. bytes=0, or none given, means the
 * whole array - and the reply says which window it actually summed, because a
 * CRC without the range it covers cannot be compared to anything. */
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

/* PD4 (/RE and DE together) and PD5 (TX) driven as plain GPIO and read back.
 *
 * This is phase R1 of the standalone RS485 test (../RS485/rs485_test.c), and
 * it is the one thing that test could do which the session could not, so it
 * comes across as a target rather than keeping a whole handover entry alive
 * for it (DECISIONS.md 40).
 *
 * *** What it separates: "no cable" from "this pin is dead". *** Nothing needs
 * to be attached. A pin that will not read back what was just written to it is
 * a board fault, and without this check it looks exactly like a pair with
 * nobody on the other end - which is the far more common and far less serious
 * case.
 *
 * *** Refused while the session is running. *** The session has PD4/PD5 muxed
 * to USART2's alternate function; re-muxing them to GPIO underneath it would
 * leave the session running, reporting misses, and blaming the wire. pt.run
 * deliberately does not stop sessions, so this target has to decline instead.
 */
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

    /* Left in receive with the driver off. Walking away from this target with
     * PD4 high would hold the transceiver driving the pair, and every other
     * node on it would be deaf until somebody noticed. */
    HAL_GPIO_WritePin(PORT_RS485_DIR_PORT,
                      PORT_RS485_DIR_PIN | PORT_RS485_TX_PIN, GPIO_PIN_RESET);

    low_ok  = (dir_low == GPIO_PIN_RESET) && (tx_low == GPIO_PIN_RESET);
    high_ok = (dir_high == GPIO_PIN_SET) && (tx_high == GPIO_PIN_SET);

    /* Each pin at each level is its own field. One rolled-up flag would say a
     * pin is stuck without saying which pin or which way, and those point at
     * different things on the board. */
    snprintf(out, len,
             "checked=1 busy=0 dir_low=%u tx_low=%u dir_high=%u tx_high=%u"
             " follows=%u",
             (unsigned)(dir_low == GPIO_PIN_SET),
             (unsigned)(tx_low == GPIO_PIN_SET),
             (unsigned)(dir_high == GPIO_PIN_SET),
             (unsigned)(tx_high == GPIO_PIN_SET),
             (unsigned)(low_ok && high_ok));
}

static const run_target_t targets[] = {
    { "sd.probe",        "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_probe,
      "detect pin, card identification, capacity and class" },
    { "sd.integrity",    "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_integrity,
      "one write/read/verify round through FatFs; reports both CRCs" },
    { "sd.stress",       "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_stress,
      "64 write/read/verify rounds; reports rounds passed and elapsed time" },
    { "sd.speed",        "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6", run_sd_speed,
      "how fast the card moves data each way; does not verify content" },
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
    { "sdram.crc",       "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6",
      run_sdram_crc,
      "CRC32 of one window; reads only, so it checks what a tool wrote there" },
    { "rs485.pins",      "rs485", PORTTOOL_BOARD_UPPER, "C", "C10,C11",
      run_rs485_pins,
      "drives PD4 and PD5 as plain GPIO and reads them back; nothing attached" },
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
