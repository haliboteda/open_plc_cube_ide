/*
 * boot_log.h
 *
 * System log ring in SRAM4, written by the bootloader and the application,
 * kept across resets, lost on power-off. Format and rules:
 * $PROD/docs/modules/M1/BOOT-LOG.md.
 *
 * Mirrored byte for byte between open_plc_cube_ide/IAPServer/ and
 * open_plc_arduino/cores/arduino/stm32/ (mirror 19 in
 * $PROD/docs/repo/ARCHITECTURE.md, checked by P2).
 */

#ifndef BOOT_LOG_H_
#define BOOT_LOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Right after the 32-byte handoff record. The three linker scripts start
 * RAM_D3 at BOOT_LOG_ADDR + BOOT_LOG_SIZE, so the linker cannot place
 * anything here. */
#ifndef BOOT_LOG_ADDR   /* host tests point this at a RAM buffer */
#define BOOT_LOG_ADDR       0x38000020UL
#endif
#define BOOT_LOG_SIZE       4096U
#define BOOT_LOG_HDR_SIZE   32U

#define BOOT_LOG_MAGIC      0x474F4C50UL   /* "PLOG" */
#define BOOT_LOG_VERSION    1U
#define BOOT_LOG_SYNC       0xA5U
#define BOOT_LOG_LINE_MAX   200U           /* text bytes per record; longer lines are split */
#define BOOT_LOG_CRC8_POLY  0x07U          /* over len, seq and text */

/* Every line starts with one of these; adding it is the caller's job. */
#define BOOT_LOG_TAG_BL     "[sys][BL] "
#define BOOT_LOG_TAG_BOOT   "[sys][BOOT] "
#define BOOT_LOG_TAG_NET    "[sys][NET] "
#define BOOT_LOG_TAG_FAULT  "[sys][FAULT] "
#define BOOT_LOG_TAG_LOG    "[sys][LOG] "

typedef struct {
    uint32_t magic;      /* +0   BOOT_LOG_MAGIC */
    uint16_t version;    /* +4   BOOT_LOG_VERSION */
    uint16_t size;       /* +6   BOOT_LOG_SIZE */
    uint32_t head;       /* +8   ring offset where the next record goes */
    uint32_t tail;       /* +12  ring offset of the oldest record */
    uint32_t seq;        /* +16  sequence number of the next record */
    uint32_t sent;       /* +20  last sequence number sent to USB port 1, plus one */
    uint32_t check;      /* +24  ~(magic ^ version ^ size ^ head ^ tail ^ seq ^ sent) */
    uint32_t reserved;   /* +28  0 */
} boot_log_hdr_t;

/* line is NUL-terminated (len excludes the NUL); seq is the record's full
 * sequence number. */
typedef void (*boot_log_line_cb)(const char *line, size_t len, uint32_t seq, void *ctx);

/* Call once per boot before anything else here. cold = nothing has written the
 * area since power-on (bootloader: the POR reset flag; application: false):
 * the area is then initialised without being read, because SRAM4 ECC is not
 * valid until written. Otherwise the buffer is validated; a wrong magic
 * initialises it silently, any other fault resets it and logs
 * "[sys][LOG] buffer reset: corrupt". Until this runs, writes are dropped. */
void boot_log_init(bool cold);

/* Append one line (trailing CR/LF dropped, split every BOOT_LOG_LINE_MAX
 * bytes), dropping the oldest whole records when full. Safe from interrupt
 * and fault handlers. Returns the sequence number the next record will get. */
uint32_t boot_log_write(const char *line, size_t len);

/* Every record from oldest to newest; leaves `sent` alone. */
void boot_log_read_all(boot_log_line_cb cb, void *ctx);

/* Every record with seq >= sent, oldest first. Returns the sequence number
 * after the last one passed to cb; hand it to boot_log_mark_sent() once those
 * lines are out. */
uint32_t boot_log_read_unsent(boot_log_line_cb cb, void *ctx);

/* Records below upto count as sent. Never moves `sent` backwards. */
void boot_log_mark_sent(uint32_t upto);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_LOG_H_ */
