// porttool_sdfile.c
//
// SD file transfer: a file from the PC onto the card's PORTTOOL folder and
// back, the data riding on the eth or usb session in mode=file while the
// control port carries only commands and results (PORTTOOL-FLOW.md B.11,
// DECISIONS.md 120).
//
// Targets (pt.run, port sd):
//   sd.put name=<n> size=<bytes>   arm a write; reply ok=1, then the data
//   sd.get name=<n>                reply ok=1 size= crc=, then the data
//   sd.ls                          one !sd.file line each, reply ok=1 count=
//   sd.rm name=<n>
//   sd.eject                       stop any transfer, unmount; the card can go
//
// Events:
//   !sd.put name=<n> size=<want> wrote=<n> crc=0x........ ok=1|0 [error=]
//   !sd.get name=<n> sent=<n> ok=1|0 [error=]
//
// Writing is paced by the PC: at most SDF_CHUNK bytes, then it waits for
// "A<total>\n" on the data line, since USB has no back-pressure of its own.
//
// The CRC is of what was read back from the card after closing, so a dropped
// sector shows.

#include "porttool.h"
#include "porttool_cmd.h"
#include "SD/sd_test.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#define SDF_CHUNK      4096U   /* the most the PC sends before an ack */
#define SDF_RING       8192U   /* power of two, twice the chunk */
#define SDF_GET_CHUNK  512U
#define SDF_TIMEOUT_MS 10000U
#define SDF_NAME_MAX   64U

typedef enum { SDF_IDLE = 0, SDF_PUT, SDF_GET } sdf_state_t;

static sdf_state_t sdf_state;
static char        sdf_name[SDF_NAME_MAX + 1U];
static uint32_t    sdf_want;      /* bytes the transfer moves */
static uint32_t    sdf_done;      /* written (put) or handed to the link (get) */
static uint32_t    sdf_acked;     /* the last total acknowledged to the PC */
static uint32_t    sdf_last_ms;   /* the last time anything moved */
static int         sdf_overrun;

/* Filled from the session's receive path (USB interrupt context on usb),
 * drained by the tick. */
static uint8_t           sdf_ring[SDF_RING];
static volatile uint32_t sdf_head;
static uint32_t          sdf_tail;

/* Two read buffers on get: CDC_Transmit_FS sends from the caller's buffer
 * after it returns, so the one in flight is never refilled. */
static uint8_t  sdf_gbuf[2][SDF_GET_CHUNK];
static uint32_t sdf_glen[2];
static uint8_t  sdf_gsel;

static uint8_t sdf_scratch[512];

/* CRC-32 (IEEE) and size of a file on the card, read from the start. */
static int sdf_file_crc(const char *name, uint32_t *size, uint32_t *crc)
{
    uint32_t c = 0xFFFFFFFFUL, got = 0, total = 0;
    int r = SD_File_OpenRead(name, size);

    if (r != 0) {
        return r;
    }
    do {
        r = SD_File_Read(sdf_scratch, sizeof(sdf_scratch), &got);
        if (r != 0) {
            break;
        }
        c = PortTool_Crc32Update(c, sdf_scratch, got);
        total += got;
    } while (got > 0U);
    (void)SD_File_Close();
    *crc = c ^ 0xFFFFFFFFUL;
    if ((r == 0) && (total != *size)) {
        r = 1;   /* FR_DISK_ERR: the file read short */
    }
    return r;
}

/* ASCII only, no path characters: FatFs runs code page 850 here (ffconf.h),
 * so anything else lands on the card as a different name. */
static int sdf_name_ok(const char *n)
{
    size_t len = strlen(n);

    if ((len == 0U) || (len > SDF_NAME_MAX)) {
        return 0;
    }
    if ((strcmp(n, ".") == 0) || (strcmp(n, "..") == 0)) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if ((c < 0x20U) || (c > 0x7EU) || (strchr("/\\:*?\"<>|", c) != NULL)) {
            return 0;
        }
    }
    return 1;
}

static void sdf_finish(const char *event, int ok, const char *error)
{
    uint32_t size = 0, crc = 0;
    int r = 0;

    (void)SD_File_Close();
    if ((sdf_state == SDF_PUT) && ok) {
        r = sdf_file_crc(sdf_name, &size, &crc);
        if (r != 0) {
            ok = 0;
            error = "readback";
        }
        PortTool_Frame(event, "name=%s size=%lu wrote=%lu crc=0x%08lX ok=%d%s%s",
                       sdf_name, (unsigned long)sdf_want, (unsigned long)size,
                       (unsigned long)crc, ok, ok ? "" : " error=", ok ? "" : error);
    } else if (sdf_state == SDF_PUT) {
        PortTool_Frame(event, "name=%s size=%lu wrote=%lu crc=0x00000000 ok=0 error=%s",
                       sdf_name, (unsigned long)sdf_want, (unsigned long)sdf_done, error);
    } else {
        PortTool_Frame(event, "name=%s sent=%lu ok=%d%s%s", sdf_name,
                       (unsigned long)sdf_done, ok, ok ? "" : " error=", ok ? "" : error);
    }
    sdf_state = SDF_IDLE;
}

void PortSdFile_Abort(const char *why)
{
    if (sdf_state == SDF_PUT) {
        sdf_finish("sd.put", 0, why);
    } else if (sdf_state == SDF_GET) {
        sdf_finish("sd.get", 0, why);
    }
}

/* From the eth recv callback or the USB interrupt: copy only. */
void PortSdFile_Rx(const uint8_t *data, uint32_t len)
{
    if (sdf_state != SDF_PUT) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint32_t h = sdf_head;
        if ((h - sdf_tail) >= SDF_RING) {
            sdf_overrun = 1;
            return;
        }
        sdf_ring[h & (SDF_RING - 1U)] = data[i];
        sdf_head = h + 1U;
    }
}

static void sdf_ack(sdf_send_t send)
{
    char line[16];
    int n;

    if (sdf_acked == sdf_done) {
        return;
    }
    n = snprintf(line, sizeof(line), "A%lu\n", (unsigned long)sdf_done);
    if (send((const uint8_t *)line, (uint32_t)n) == (uint32_t)n) {
        sdf_acked = sdf_done;
    }
}

static void sdf_pump_put(sdf_send_t send, uint32_t now_ms)
{
    uint32_t head = sdf_head;

    if (sdf_overrun) {
        sdf_finish("sd.put", 0, "overrun");
        return;
    }
    while (sdf_tail != head) {
        uint32_t at  = sdf_tail & (SDF_RING - 1U);
        uint32_t n   = head - sdf_tail;
        uint32_t end = SDF_RING - at;
        if (n > end) {
            n = end;
        }
        if (sdf_done + n > sdf_want) {
            sdf_finish("sd.put", 0, "too_many_bytes");
            return;
        }
        if (SD_File_Write(&sdf_ring[at], n) != 0) {
            sdf_finish("sd.put", 0, "write");
            return;
        }
        sdf_tail += n;
        sdf_done += n;
        sdf_last_ms = now_ms;
    }
    sdf_ack(send);
    /* Finished only once the PC has been told the last total: it is waiting
     * for that ack before it looks for the result. */
    if ((sdf_done == sdf_want) && (sdf_acked == sdf_done)) {
        sdf_finish("sd.put", 1, "");
    }
}

static void sdf_pump_get(sdf_send_t send, uint32_t now_ms)
{
    for (int pass = 0; pass < 8; pass++) {
        uint8_t s = sdf_gsel;

        if (sdf_glen[s] == 0U) {
            uint32_t got = 0;
            uint32_t want = sdf_want - sdf_done;
            if (want == 0U) {
                sdf_finish("sd.get", 1, "");
                return;
            }
            if (want > SDF_GET_CHUNK) {
                want = SDF_GET_CHUNK;
            }
            if ((SD_File_Read(sdf_gbuf[s], want, &got) != 0) || (got == 0U)) {
                sdf_finish("sd.get", 0, "read");
                return;
            }
            sdf_glen[s] = got;
        }
        if (send(sdf_gbuf[s], sdf_glen[s]) != sdf_glen[s]) {
            return;   /* the link is full; try again next pass */
        }
        sdf_done += sdf_glen[s];
        sdf_last_ms = now_ms;
        sdf_glen[s] = 0U;
        sdf_gsel ^= 1U;
    }
}

void PortSdFile_Pump(sdf_send_t send, uint32_t now_ms)
{
    if (sdf_state == SDF_IDLE) {
        return;
    }
    if ((now_ms - sdf_last_ms) > SDF_TIMEOUT_MS) {
        PortSdFile_Abort("timeout");
        return;
    }
    if (sdf_state == SDF_PUT) {
        sdf_pump_put(send, now_ms);
    } else {
        sdf_pump_get(send, now_ms);
    }
}

static int sdf_take_name(const char *args, char *out, uint32_t len)
{
    if (!PortCmd_GetStr(args, "name", sdf_name, sizeof(sdf_name)) || !sdf_name_ok(sdf_name)) {
        snprintf(out, len, "ok=0 error=name");
        return 0;
    }
    return 1;
}

void PortSdFile_RunPut(const char *args, char *out, uint32_t len)
{
    uint32_t size = 0;
    int r;

    PortSdFile_Abort("replaced");
    if (!sdf_take_name(args, out, len)) {
        return;
    }
    if (!PortCmd_GetU32(args, "size", &size)) {
        snprintf(out, len, "ok=0 error=size");
        return;
    }
    r = SD_File_OpenWrite(sdf_name);
    if (r != 0) {
        snprintf(out, len, "ok=0 name=%s error=%s fresult=%d", sdf_name,
                 (r < 0) ? "nocard" : "open", r);
        return;
    }
    sdf_want = size;
    sdf_done = 0;
    sdf_acked = 0;
    sdf_overrun = 0;
    sdf_tail = sdf_head;
    sdf_last_ms = HAL_GetTick();
    sdf_state = SDF_PUT;
    snprintf(out, len, "ok=1 name=%s size=%lu chunk=%lu", sdf_name,
             (unsigned long)size, (unsigned long)SDF_CHUNK);
    if (size == 0U) {
        sdf_finish("sd.put", 1, "");
    }
}

void PortSdFile_RunGet(const char *args, char *out, uint32_t len)
{
    uint32_t size = 0, crc = 0, again = 0;
    int r;

    PortSdFile_Abort("replaced");
    if (!sdf_take_name(args, out, len)) {
        return;
    }
    r = sdf_file_crc(sdf_name, &size, &crc);
    if (r == 0) {
        r = SD_File_OpenRead(sdf_name, &again);
    }
    if (r != 0) {
        snprintf(out, len, "ok=0 name=%s error=%s fresult=%d", sdf_name,
                 (r < 0) ? "nocard" : "open", r);
        return;
    }
    sdf_want = size;
    sdf_done = 0;
    sdf_glen[0] = sdf_glen[1] = 0U;
    sdf_gsel = 0;
    sdf_last_ms = HAL_GetTick();
    sdf_state = SDF_GET;
    snprintf(out, len, "ok=1 name=%s size=%lu crc=0x%08lX", sdf_name,
             (unsigned long)size, (unsigned long)crc);
    if (size == 0U) {
        sdf_finish("sd.get", 1, "");
    }
}

static void sdf_each(const char *name, uint32_t size, uint16_t fdate, uint16_t ftime, void *ctx)
{
    (void)ctx;
    PortTool_Frame("sd.file", "name=%s size=%lu date=%04u-%02u-%02u time=%02u:%02u",
                   name, (unsigned long)size,
                   (unsigned)(1980U + (fdate >> 9)), (unsigned)((fdate >> 5) & 15U),
                   (unsigned)(fdate & 31U), (unsigned)(ftime >> 11), (unsigned)((ftime >> 5) & 63U));
}

void PortSdFile_RunList(const char *args, char *out, uint32_t len)
{
    uint32_t n = 0;
    int r;

    (void)args;
    if (sdf_state != SDF_IDLE) {
        snprintf(out, len, "ok=0 error=busy");
        return;
    }
    r = SD_File_List(sdf_each, NULL, &n);
    if (r != 0) {
        snprintf(out, len, "ok=0 error=%s fresult=%d", (r < 0) ? "nocard" : "list", r);
        return;
    }
    snprintf(out, len, "ok=1 count=%lu", (unsigned long)n);
}

void PortSdFile_RunRemove(const char *args, char *out, uint32_t len)
{
    int r;

    if (sdf_state != SDF_IDLE) {
        snprintf(out, len, "ok=0 error=busy");
        return;
    }
    if (!sdf_take_name(args, out, len)) {
        return;
    }
    r = SD_File_Remove(sdf_name);
    if (r != 0) {
        snprintf(out, len, "ok=0 name=%s error=%s fresult=%d", sdf_name,
                 (r < 0) ? "nocard" : "remove", r);
        return;
    }
    snprintf(out, len, "ok=1 name=%s", sdf_name);
}

void PortSdFile_RunEject(const char *args, char *out, uint32_t len)
{
    (void)args;
    PortSdFile_Abort("ejected");
    snprintf(out, len, "ok=1 fresult=%d", SD_File_Eject());
}

#endif /* PORTTOOL_ENABLE */
