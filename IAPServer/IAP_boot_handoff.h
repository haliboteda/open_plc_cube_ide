/*
 * IAP_boot_handoff.h
 *
 * The one and only channel by which a running image asks the bootloader to
 * stay in upload mode after the next reset. Every place that used to poke
 * MAGIC_BKP_REG directly now goes through boot_handoff_request(), and the
 * bootloader's only read is boot_handoff_take().
 *
 * Why the record lives in SRAM4 rather than an RTC backup register:
 *
 *   - No preconditions. Writing an RTC backup register silently does nothing
 *     unless PWR_CR1.DBP happens to be set, and nothing in the compiler or the
 *     HAL return value tells you when it was dropped. That is exactly how the
 *     ethernet upload path came to fail silently: iap_auth's nonce counter
 *     cleared DBP as its last act, so the MAGIC_ETH_FLAG write issued a few
 *     milliseconds later never landed and the board rebooted straight back
 *     into the application. Plain memory has no such hidden gate.
 *   - No init. SRAM4 needs no clock enable, no MPU region and no peripheral
 *     setup, so the bootloader can read the record before it has initialised
 *     anything at all -- which is what lets the boot-mode decision happen
 *     before USB / ethernet / FMC are brought up.
 *   - Correct lifetime. RAM survives a warm reset but not a power cycle, and
 *     "enter upload mode once" is exactly a warm-reset-scoped request. A
 *     battery-backed register would keep a stale request across a power loss.
 *   - No dependency on VBAT or on the RTC being configured.
 *
 * Both linker scripts start RAM_D3 at 0x38000020, so the linker cannot place
 * anything in the reserved 32 bytes -- that is a guarantee, not a convention.
 */

#ifndef BOOT_HANDOFF_H_
#define BOOT_HANDOFF_H_

#include <stdbool.h>
#include <stdint.h>

/* First 32 bytes of SRAM4 (D3 domain). 32 bytes at 32-byte alignment is one
 * Cortex-M7 cache line, which is the granularity the maintenance calls in
 * IAP_boot_handoff.c work on. */
#define BOOT_HANDOFF_ADDR       0x38000000UL
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
 * the return value so that this file needs no knowledge of the bootloader's
 * event journal (the application-side mirror has no journal at all). */
typedef enum {
    BOOT_HANDOFF_OK = 0,           /* record consumed, or legitimately empty */
    BOOT_HANDOFF_COLD_BOOT,        /* power-on reset: record initialised, no request */
    BOOT_HANDOFF_CORRUPT,          /* magic or check failed -- anomaly, report it */
    BOOT_HANDOFF_UNKNOWN_VERSION,  /* written by a newer image -- anomaly, report it */
    BOOT_HANDOFF_BAD_MODE,         /* valid record, unusable mode -- anomaly, report it */
} boot_handoff_status_t;

/* Store the request, verify it actually landed, then reset. Does not return on
 * success.
 *
 * Returns false only when the read-back check fails, i.e. the write did not
 * take effect. In that case the board is NOT reset: the caller should report an
 * error to whoever asked, because a silent failure here is indistinguishable
 * from "the device ignored my command" and costs hours to diagnose. */
bool boot_handoff_request(boot_req_t mode);

/* Read the pending request and clear it in the same breath, so a stale record
 * can never pin the board in the bootloader. Only the bootloader may call this,
 * once, before anything else has had a chance to clear RCC->RSR. Repeat calls
 * return the cached result. */
boot_req_t boot_handoff_take(void);

/* Valid after boot_handoff_take(). */
boot_handoff_status_t boot_handoff_last_status(void);

#endif /* BOOT_HANDOFF_H_ */
