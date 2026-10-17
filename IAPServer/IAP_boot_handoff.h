/*
 * IAP_boot_handoff.h
 *
 * The one and only channel by which a running image asks the bootloader to
 * stay in upload mode after the next reset: boot_handoff_request() writes,
 * boot_handoff_take() in the bootloader reads.
 *
 * The record lives in SRAM4 rather than an RTC backup register because plain
 * RAM has no hidden write gate (PWR_CR1.DBP) and needs no init. Full rationale
 * and why the linker cannot place anything in these 32 bytes:
 * $PROD/docs/modules/M1/BOOT-SEQUENCE.md, "交接记录为什么放 SRAM4".
 */

#ifndef BOOT_HANDOFF_H_
#define BOOT_HANDOFF_H_

#include <stdbool.h>
#include <stdint.h>

/* First 32 bytes of SRAM4 (D3 domain). 32 bytes at 32-byte alignment is one
 * Cortex-M7 cache line, which is the granularity the maintenance calls in
 * IAP_boot_handoff.c work on. */
#ifndef BOOT_HANDOFF_ADDR   /* host tests point this at a RAM buffer */
#define BOOT_HANDOFF_ADDR       0x38000000UL
#endif
#define BOOT_HANDOFF_SIZE       32U

#define BOOT_HANDOFF_MAGIC      0x504C4321UL   /* "PLC!" */
#define BOOT_HANDOFF_VERSION    1U

/* Stored in the record, so the numbers are part of the format: never renumber
 * an existing value, only append. */
typedef enum {
    BOOT_REQ_NONE = 0,   /* no request pending: boot the application */
    BOOT_REQ_CDC  = 1,   /* stay in the bootloader, serve the USB CDC channel */
    BOOT_REQ_ETH  = 2,   /* stay in the bootloader, serve the ethernet channel */
    BOOT_REQ_ALL  = 3,   /* result-only, never written by boot_handoff_request():
                          * a request was pending but its mode could not be
                          * interpreted. Stay in the bootloader and serve every
                          * channel rather than guessing which one the operator
                          * is on. */
} boot_req_t;

/* The first four fields are frozen for every future version of this record.
 * A bootloader that does not recognise `version` can therefore still read
 * `mode`, so a version mismatch degrades to "enter the bootloader" instead of
 * "silently ignore the request" -- always fail towards the recoverable side.
 * Later versions may only append fields after `check`. */
typedef struct {
    uint32_t magic;      /* +0   BOOT_HANDOFF_MAGIC */
    uint16_t version;    /* +4   BOOT_HANDOFF_VERSION of the writer */
    uint16_t size;       /* +6   sizeof(boot_handoff_t) as the writer saw it */
    uint32_t mode;       /* +8   boot_req_t */
    uint32_t check;      /* +12  ~(magic ^ version ^ mode) */
} boot_handoff_t;

/* What boot_handoff_take() found, for callers that want to log it. Kept out of
 * the return value so that a caller can report it however it likes, and so the
 * bootloader and the application-side mirror can differ in that without this
 * file changing. */
typedef enum {
    BOOT_HANDOFF_OK = 0,           /* record consumed, or legitimately empty */
    BOOT_HANDOFF_COLD_BOOT,        /* power-on reset: record initialised, no request */
    BOOT_HANDOFF_CORRUPT,          /* magic or check failed -- anomaly, report it */
    BOOT_HANDOFF_UNKNOWN_VERSION,  /* written by a newer image -- anomaly, report it */
    BOOT_HANDOFF_BAD_MODE,         /* valid record, unusable mode -- anomaly, report it */
} boot_handoff_status_t;

/* Bytes 16..23 of the reserved area: the reset cause the bootloader latched on
 * this boot, published for the application. The bootloader clears RCC->RSR and
 * then jumps, so the application has nothing left to read there. Decision 80.
 * The record above never grows into these bytes. */
#define BOOT_HANDOFF_RSR_OFFSET 16U
#define BOOT_HANDOFF_RSR_MAGIC  0x52535221UL   /* "RSR!" */

/* Bootloader: write this boot's latched RCC->RSR into bytes 16..23.
 * boot_handoff_take() calls it on every boot. */
void boot_handoff_publish_reset_cause(void);

/* Application: the RCC->RSR value the bootloader published for this boot.
 * False when nothing valid is there (check word does not match). */
bool boot_handoff_published_reset_rsr(uint32_t *rsr);

/* Store the request, verify it actually landed, then reset. Does not return on
 * success.
 *
 * Returns false only when the read-back check fails, i.e. the write did not
 * take effect. In that case the board is NOT reset: the caller should report an
 * error to whoever asked, because a silent failure here is indistinguishable
 * from "the device ignored my command" and costs hours to diagnose. */
bool boot_handoff_request(boot_req_t mode);

/* Latch RCC->RSR and clear it. Call this as the first statement of main():
 * the cold-boot test depends on the POR flag, and HAL_RCC_DeInit() wipes the
 * whole register, so the value has to be captured before anything else runs.
 * Idempotent -- boot_handoff_take() calls it too, in case main() did not. */
void boot_handoff_latch_reset_cause(void);

/* The latched RCC->RSR, and that value decoded into a name ("POR", "IWDG", ...).
 * Only the bootloader can report this: it clears the register and then *jumps*
 * to the application, so by the time the application runs there is nothing left
 * in RSR to read. */
uint32_t boot_handoff_reset_rsr(void);
const char *boot_handoff_reset_cause_str(void);

/* Read the pending request and clear it in the same breath, so a stale record
 * can never pin the board in the bootloader. Only the bootloader may call this,
 * once. Repeat calls return the cached result. */
boot_req_t boot_handoff_take(void);

/* Valid after boot_handoff_take(). */
boot_handoff_status_t boot_handoff_last_status(void);

#endif /* BOOT_HANDOFF_H_ */
