/*
 * boot_log.c
 *
 * See boot_log.h. Format: $PROD/docs/modules/M1/BOOT-LOG.md.
 *
 * Mirrored between open_plc_cube_ide/IAPServer/ and
 * open_plc_arduino/cores/arduino/stm32/; the two copies differ only in the
 * device include (main.h / stm32_def.h). P2 compares the rest (mirror 19).
 */

#include "boot_log.h"
#include "main.h"

#define REC_OVERHEAD  5U   /* sync, len, seq lo, seq hi, crc */
#define RING_SIZE     (BOOT_LOG_SIZE - BOOT_LOG_HDR_SIZE)

typedef char boot_log_hdr_size_ok[(sizeof(boot_log_hdr_t) == BOOT_LOG_HDR_SIZE) ? 1 : -1];

static bool     s_ready;
static uint32_t s_count;   /* records between tail and head */

static volatile boot_log_hdr_t *hdr(void)
{
	return (volatile boot_log_hdr_t *)BOOT_LOG_ADDR;
}

static volatile uint8_t *ring(void)
{
	return (volatile uint8_t *)(BOOT_LOG_ADDR + BOOT_LOG_HDR_SIZE);
}

/* off may run up to one record past the end of the ring. */
static uint32_t wrap(uint32_t off)
{
	return off % RING_SIZE;
}

static uint8_t ring_get(uint32_t off)
{
	return ring()[wrap(off)];
}

static uint8_t crc8(uint8_t crc, uint8_t b)
{
	crc ^= b;
	for (uint32_t i = 0U; i < 8U; i++) {
		crc = ((crc & 0x80U) != 0U) ? (uint8_t)((crc << 1) ^ BOOT_LOG_CRC8_POLY)
		                            : (uint8_t)(crc << 1);
	}
	return crc;
}

static uint8_t ring_put(uint32_t off, uint8_t b, uint8_t crc)
{
	ring()[wrap(off)] = b;
	return crc8(crc, b);
}

static uint32_t calc_check(const volatile boot_log_hdr_t *h)
{
	return ~(h->magic ^ (uint32_t)h->version ^ (uint32_t)h->size ^
	         h->head ^ h->tail ^ h->seq ^ h->sent);
}

static uint32_t used_bytes(uint32_t head, uint32_t tail)
{
	return (head + RING_SIZE - tail) % RING_SIZE;
}

static uint16_t seq16_at(uint32_t off)
{
	return (uint16_t)((uint32_t)ring_get(off + 2U) | ((uint32_t)ring_get(off + 3U) << 8));
}

static uint32_t lock(void)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	return primask;
}

static void unlock(uint32_t primask)
{
	__set_PRIMASK(primask);
}

/* Push the area out of the D-cache so a soft reset does not lose it. A no-op
 * when the D-cache is off. */
static void flush(void)
{
	__DSB();
	SCB_CleanDCache_by_Addr((uint32_t *)BOOT_LOG_ADDR, (int32_t)BOOT_LOG_SIZE);
	__DSB();
}

/* Whole words, write-only: SRAM4 ECC is not valid until written. */
static void reset_area(void)
{
	volatile uint32_t *word = (volatile uint32_t *)BOOT_LOG_ADDR;
	volatile boot_log_hdr_t *h = hdr();

	for (uint32_t i = 0U; i < (BOOT_LOG_SIZE / sizeof(uint32_t)); i++) {
		word[i] = 0U;
	}
	h->magic   = BOOT_LOG_MAGIC;
	h->version = (uint16_t)BOOT_LOG_VERSION;
	h->size    = (uint16_t)BOOT_LOG_SIZE;
	h->check   = calc_check(h);
	s_count    = 0U;
	flush();
}

/* Total size of the valid record at off, or 0. room = bytes left before head. */
static uint32_t record_size(uint32_t off, uint32_t room)
{
	uint32_t len;
	uint8_t crc = 0U;

	if ((room <= REC_OVERHEAD) || (ring_get(off) != BOOT_LOG_SYNC)) {
		return 0U;
	}
	len = ring_get(off + 1U);
	if ((len == 0U) || (len > BOOT_LOG_LINE_MAX) || ((REC_OVERHEAD + len) > room)) {
		return 0U;
	}
	for (uint32_t i = 1U; i < (REC_OVERHEAD - 1U + len); i++) {
		crc = crc8(crc, ring_get(off + i));
	}
	if (crc != ring_get(off + REC_OVERHEAD - 1U + len)) {
		return 0U;
	}
	return REC_OVERHEAD + len;
}

/* Header and every record tail to head; sets s_count on success. */
static bool validate(void)
{
	volatile boot_log_hdr_t *h = hdr();
	uint32_t off;
	uint32_t room;
	uint32_t size;
	uint32_t n = 0U;
	uint16_t last = 0U;

	if ((h->version != (uint16_t)BOOT_LOG_VERSION) || (h->size != (uint16_t)BOOT_LOG_SIZE) ||
	    (h->check != calc_check(h)) || (h->head >= RING_SIZE) || (h->tail >= RING_SIZE) ||
	    (h->sent > h->seq)) {
		return false;
	}
	off  = h->tail;
	room = used_bytes(h->head, h->tail);
	while (room > 0U) {
		size = record_size(off, room);
		if ((size == 0U) || ((n > 0U) && (seq16_at(off) != (uint16_t)(last + 1U)))) {
			return false;
		}
		last = seq16_at(off);
		n++;
		off   = wrap(off + size);
		room -= size;
	}
	if ((n > h->seq) || ((n > 0U) && ((uint16_t)(last + 1U) != (uint16_t)h->seq))) {
		return false;
	}
	s_count = n;
	return true;
}

/* One record of 1..BOOT_LOG_LINE_MAX bytes; caller holds the lock. */
static void append(const char *text, uint32_t len)
{
	volatile boot_log_hdr_t *h = hdr();
	const uint32_t need = REC_OVERHEAD + len;
	uint32_t head = h->head;
	uint32_t tail = h->tail;
	const uint32_t seq = h->seq;
	uint8_t crc = 0U;

	while ((RING_SIZE - 1U - used_bytes(head, tail)) < need) {
		tail = wrap(tail + REC_OVERHEAD + ring_get(tail + 1U));
		s_count--;
	}
	ring()[head] = (uint8_t)BOOT_LOG_SYNC;
	crc = ring_put(head + 1U, (uint8_t)len, crc);
	crc = ring_put(head + 2U, (uint8_t)(seq & 0xFFU), crc);
	crc = ring_put(head + 3U, (uint8_t)((seq >> 8) & 0xFFU), crc);
	for (uint32_t i = 0U; i < len; i++) {
		crc = ring_put(head + 4U + i, (uint8_t)text[i], crc);
	}
	(void)ring_put(head + 4U + len, crc, 0U);

	h->head  = wrap(head + need);
	h->tail  = tail;
	h->seq   = seq + 1U;
	h->check = calc_check(h);
	s_count++;
}

void boot_log_init(bool cold)
{
	static const char corrupt[] = BOOT_LOG_TAG_LOG "buffer reset: corrupt";

	s_ready = false;
	if (cold || (hdr()->magic != BOOT_LOG_MAGIC)) {
		reset_area();
	} else if (!validate()) {
		reset_area();
		append(corrupt, (uint32_t)(sizeof(corrupt) - 1U));
		flush();
	}
	s_ready = true;
}

uint32_t boot_log_write(const char *line, size_t len)
{
	uint32_t primask;
	uint32_t next;
	uint32_t chunk;

	if (!s_ready || (line == NULL)) {
		return 0U;
	}
	while ((len > 0U) && ((line[len - 1U] == '\n') || (line[len - 1U] == '\r'))) {
		len--;
	}
	primask = lock();
	while (len > 0U) {
		chunk = (len > BOOT_LOG_LINE_MAX) ? BOOT_LOG_LINE_MAX : (uint32_t)len;
		append(line, chunk);
		line += chunk;
		len  -= chunk;
	}
	flush();
	next = hdr()->seq;
	unlock(primask);
	return next;
}

/* Copies one record at a time under the lock, so cb runs with interrupts as the
 * caller had them. Records dropped meanwhile are skipped. */
static uint32_t emit_from(uint32_t seq, boot_log_line_cb cb, void *ctx)
{
	char buf[BOOT_LOG_LINE_MAX + 1U];
	volatile boot_log_hdr_t *h = hdr();
	uint32_t off = 0U;
	bool have_off = false;
	uint32_t primask;
	uint32_t first;
	uint32_t len;

	for (;;) {
		primask = lock();
		first = h->seq - s_count;
		if (seq < first) {
			seq      = first;
			have_off = false;
		}
		if (seq >= h->seq) {
			unlock(primask);
			break;
		}
		if (!have_off) {
			off = h->tail;
			for (uint32_t s = first; s < seq; s++) {
				off = wrap(off + REC_OVERHEAD + ring_get(off + 1U));
			}
		}
		len = ring_get(off + 1U);
		for (uint32_t i = 0U; i < len; i++) {
			buf[i] = (char)ring_get(off + 4U + i);
		}
		buf[len] = '\0';
		off      = wrap(off + REC_OVERHEAD + len);
		have_off = true;
		unlock(primask);

		cb(buf, len, seq, ctx);
		seq++;
	}
	return seq;
}

void boot_log_read_all(boot_log_line_cb cb, void *ctx)
{
	if (!s_ready || (cb == NULL)) {
		return;
	}
	(void)emit_from(0U, cb, ctx);
}

uint32_t boot_log_read_unsent(boot_log_line_cb cb, void *ctx)
{
	if (!s_ready || (cb == NULL)) {
		return 0U;
	}
	return emit_from(hdr()->sent, cb, ctx);
}

void boot_log_mark_sent(uint32_t upto)
{
	volatile boot_log_hdr_t *h = hdr();
	uint32_t primask;

	if (!s_ready) {
		return;
	}
	primask = lock();
	if ((upto > h->sent) && (upto <= h->seq)) {
		h->sent  = upto;
		h->check = calc_check(h);
		flush();
	}
	unlock(primask);
}
