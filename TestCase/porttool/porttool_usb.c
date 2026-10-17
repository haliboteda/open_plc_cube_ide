// porttool_usb.c
//
// USB session: the CDC virtual serial port on the Type-C connector, driven as
// a data channel rather than as the bootloader's upload path.
//
// Starting this session brings the USB device stack up (DECISIONS.md 28).
// usbd_cdc_if.c routes CDC_Receive_FS to PortUsb_Received under
// PORTTOOL_ENABLE, which keeps the IAP server out of the image (DECISIONS.md 31).
//
// The board is a Device; the PC's virtual COM port is the peer for every mode
// (FIXTURE-INTERFACE.md).
//
// Parameters:
//   mode=echo    echo | sink | source | info | mirror | file   (default echo)
//   period=1000  milliseconds between frames      (default 1000, floor 50)
//
// Modes:
//
//   echo    the board sends its counter, the PC sends it straight back: the
//           only mode where seq is the verdict.
//   sink    the PC pushes at the board, which counts bytes and the rate.
//   source  the board pushes as fast as the endpoint accepts.
//   info    moves nothing; reports enumeration state and its changes.
//   mirror  the board sends back every byte unchanged; the PC floods numbered
//           packets and measures rate, loss and latency in one run
//           (DECISIONS.md 98, PORTTOOL-FLOW.md B.9).
//   file    the pipe carries an SD file transfer (porttool_sdfile.c,
//           PORTTOOL-FLOW.md B.11). No counter is sent - it would land in
//           the file.
//
// Frame:
//   !usb t=48213 seq=1 rx=1 miss=0 state=cfg enum=1 mode=echo
//        rx_bytes=0 tx_bytes=0 kbps=0 busy=0 drop=0
//
// drop= is mirror bytes lost to a full ring (USB cannot hold the PC back; the
// PC keeps fewer than USB_MIRROR_BUF in flight).
//
// busy= counts writes refused while the previous one was in flight: normal in
// source mode; in echo mode, with seq stalled, the host stopped reading.

#include "porttool.h"
#include "porttool_cmd.h"

#include "main.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include "usbd_def.h"

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

/* Defined in usb_device.c, declared in no header. */
extern USBD_HandleTypeDef hUsbDeviceFS;

#define USB_PERIOD_DEF_MS 1000U

/* One full-speed CDC packet, so busy= counts refused packets. */
#define USB_CHUNK 64U

/* Mirror packets stay one byte short of a full packet, so each write ends the
 * host's bulk transfer without needing a zero-length packet. */
#define USB_MIRROR_CHUNK (USB_CHUNK - 1U)

/* Power of two: the ring indexes wrap with a mask. */
#define USB_MIRROR_BUF 2048U

typedef enum {
    USB_MODE_ECHO = 0,
    USB_MODE_SINK,
    USB_MODE_SOURCE,
    USB_MODE_INFO,
    USB_MODE_MIRROR,
    USB_MODE_FILE,
} usb_mode_t;

static usb_mode_t usb_mode = USB_MODE_ECHO;
static uint32_t   usb_period_ms = USB_PERIOD_DEF_MS;
static uint32_t   usb_due_ms;

static int usb_stack_started;   /* MX_USB_DEVICE_Init has run - only once */
static int usb_running;

static uint32_t usb_rx_bytes;
static uint32_t usb_tx_bytes;
static uint32_t usb_window_bytes;   /* bytes moved since the last frame */
static uint32_t usb_busy;           /* writes the endpoint refused */
static uint32_t usb_drop;           /* mirror bytes lost to a full ring */

/* Mirror ring: filled in USB interrupt context, drained by the tick. */
static uint8_t           usb_mirror_ring[USB_MIRROR_BUF];
static volatile uint32_t usb_mirror_head;
static volatile uint32_t usb_mirror_tail;

/* Two transmit buffers: CDC_Transmit_FS sends from the caller's buffer after
 * it returns, so the one in flight must not be refilled. */
static uint8_t usb_mirror_tx[2][USB_MIRROR_CHUNK];
static uint8_t usb_mirror_tx_sel;

/* Enumeration state changes since the session started. */
static uint8_t  usb_last_state;
static uint32_t usb_enum_changes;

/* Counting starts once configured, so the first enumeration (which pt.start
 * usb itself triggers) is not counted: enum= means re-enumerations. */
static int usb_enum_armed;

static porttool_echo_t usb_echo;

/* What source mode pushes; only the rate matters. */
static const uint8_t usb_filler[USB_CHUNK] = { 0 };

static const char *usb_mode_name(usb_mode_t m)
{
    switch (m) {
    case USB_MODE_SINK:   return "sink";
    case USB_MODE_SOURCE: return "source";
    case USB_MODE_INFO:   return "info";
    case USB_MODE_MIRROR: return "mirror";
    case USB_MODE_FILE:   return "file";
    default:              return "echo";
    }
}

/* The device state; only "cfg" means the PC has opened the pipe. */
static const char *usb_state_name(uint8_t st)
{
    switch (st) {
    case USBD_STATE_DEFAULT:    return "default";
    case USBD_STATE_ADDRESSED:  return "addressed";
    case USBD_STATE_CONFIGURED: return "cfg";
    case USBD_STATE_SUSPENDED:  return "suspended";
    default:                    return "none";
    }
}

static uint8_t usb_dev_state(void)
{
    return usb_stack_started ? (uint8_t)hUsbDeviceFS.dev_state : 0u;
}

static int usb_ready(void)
{
    return usb_stack_started &&
           (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED);
}

/* Hands one buffer to the endpoint. Returns what went out, 0 when refused. */
static uint32_t usb_send(const uint8_t *data, uint16_t len)
{
    if (!usb_ready()) {
        return 0;
    }
    if (CDC_Transmit_FS((uint8_t *)data, len) != USBD_OK) {
        usb_busy++;
        return 0;
    }
    usb_tx_bytes     += len;
    usb_window_bytes += len;
    return len;
}

/* USB interrupt context: counters and a decimal scan only, never printf. */
void PortUsb_Received(const uint8_t *data, uint32_t len)
{
    static porttool_numscan_t scan;

    if (!usb_running || data == NULL || len == 0u) {
        return;
    }

    usb_rx_bytes     += len;
    usb_window_bytes += len;

    if (usb_mode == USB_MODE_FILE) {
        PortSdFile_Rx(data, len);
        return;
    }

    if (usb_mode == USB_MODE_MIRROR) {
        for (uint32_t i = 0; i < len; i++) {
            uint32_t h = usb_mirror_head;
            if ((h - usb_mirror_tail) >= USB_MIRROR_BUF) {
                usb_drop += len - i;
                break;
            }
            usb_mirror_ring[h & (USB_MIRROR_BUF - 1U)] = data[i];
            usb_mirror_head = h + 1U;
        }
        return;
    }

    /* Only echo parses the payload; sink only counts, to measure the rate. */
    if (usb_mode != USB_MODE_ECHO) {
        return;
    }
    PortTool_EchoScan(&usb_echo, &scan, data, len);
}

static int usb_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    usb_mode_t want_mode   = usb_mode;
    uint32_t   want_period = usb_period_ms;

    if (PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        if      (strcmp(probe, "echo")   == 0) { want_mode = USB_MODE_ECHO; }
        else if (strcmp(probe, "sink")   == 0) { want_mode = USB_MODE_SINK; }
        else if (strcmp(probe, "source") == 0) { want_mode = USB_MODE_SOURCE; }
        else if (strcmp(probe, "info")   == 0) { want_mode = USB_MODE_INFO; }
        else if (strcmp(probe, "mirror") == 0) { want_mode = USB_MODE_MIRROR; }
        else if (strcmp(probe, "file")   == 0) { want_mode = USB_MODE_FILE; }
        else {
            snprintf(err, err_len,
                     "mode=\"%s\" must be echo, sink, source, info, mirror or file", probe);
            return 0;
        }
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        want_period = PortTool_ClampPeriod(v);
    }

    usb_mode      = want_mode;
    usb_period_ms = want_period;
    return 1;
}

static int usb_start(const char *args, char *err, uint32_t err_len)
{
    if (!usb_apply(args, err, err_len)) {
        return 0;
    }

    if (!usb_stack_started) {
        MX_USB_DEVICE_Init();
        usb_stack_started = 1;
    }

    /* Not refused when the PC has not opened the port: state= reports that. */
    PortTool_EchoReset(&usb_echo);
    usb_rx_bytes     = 0;
    usb_tx_bytes     = 0;
    usb_window_bytes = 0;
    usb_busy         = 0;
    usb_drop         = 0;
    usb_mirror_tail  = usb_mirror_head;
    usb_enum_changes = 0;
    usb_enum_armed   = 0;
    usb_last_state   = usb_dev_state();
    usb_running      = 1;
    usb_due_ms       = HAL_GetTick();
    return 1;
}

static int usb_set(const char *args, char *err, uint32_t err_len)
{
    return usb_apply(args, err, err_len);
}

static void usb_stop(void)
{
    if (usb_mode == USB_MODE_FILE) {
        PortSdFile_Abort("stopped");
    }
    usb_running = 0;

    /* The stack is left up, so the PC's COM port does not vanish and come
     * back under another number. */
}

/* The SD transfer's sender. CDC_Transmit_FS refuses while the previous
 * packet is still going out, which porttool_sdfile.c relies on. */
static uint32_t usb_file_send(const uint8_t *data, uint32_t len)
{
    if (!usb_ready() || (len > 0xFFFFU)) {
        return 0;
    }
    return usb_send(data, (uint16_t)len);
}

/* Sends back what the ring holds, one short packet per write. */
static void usb_mirror_drain(void)
{
    for (int i = 0; i < 8; i++) {
        uint32_t tail  = usb_mirror_tail;
        uint32_t avail = usb_mirror_head - tail;
        uint32_t n     = (avail < USB_MIRROR_CHUNK) ? avail : USB_MIRROR_CHUNK;
        uint8_t *buf   = usb_mirror_tx[usb_mirror_tx_sel];

        if (n == 0U) {
            return;
        }
        for (uint32_t k = 0; k < n; k++) {
            buf[k] = usb_mirror_ring[(tail + k) & (USB_MIRROR_BUF - 1U)];
        }
        if (usb_send(buf, (uint16_t)n) == 0U) {
            return;
        }
        usb_mirror_tail = tail + n;
        usb_mirror_tx_sel ^= 1U;
    }
}

static void usb_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    char line[32];
    uint32_t n;
    uint32_t kbps;
    uint8_t st;

    /* Watched every pass, so a re-enumeration between frames is counted. */
    st = usb_dev_state();
    if (st != usb_last_state) {
        usb_last_state = st;
        if (usb_enum_armed) {
            usb_enum_changes++;
        }
    }
    if (!usb_enum_armed && (st == USBD_STATE_CONFIGURED)) {
        usb_enum_armed   = 1;
        usb_enum_changes = 0;
    }

    if (usb_mode == USB_MODE_SOURCE) {
        /* A hard cap per pass, so an endpoint that kept accepting (the host
         * harness) cannot starve the superloop. */
        for (int i = 0; i < 8; i++) {
            if (usb_send(usb_filler, (uint16_t)sizeof(usb_filler)) == 0u) {
                break;
            }
        }
    }

    if (usb_mode == USB_MODE_MIRROR) {
        usb_mirror_drain();
    }
    if (usb_mode == USB_MODE_FILE) {
        PortSdFile_Pump(usb_file_send, now_ms);
    }

    if ((int32_t)(now_ms - usb_due_ms) < 0) {
        return;
    }
    usb_due_ms = now_ms + usb_period_ms;

    /* Only echo gives the host a number to answer, so only echo counts misses. */
    if (usb_mode == USB_MODE_ECHO) {
        PortTool_EchoTick(&usb_echo);
    }

    /* Sent on the link under test; the peer returns it unchanged
     * (DECISIONS.md 18). */
    if (usb_mode == USB_MODE_ECHO) {
        n = (uint32_t)snprintf(line, sizeof(line), "%lu\n",
                               (unsigned long)usb_echo.seq);
        (void)usb_send((const uint8_t *)line, (uint16_t)n);
    }

    kbps = (usb_period_ms > 0u) ? ((usb_window_bytes * 8u) / usb_period_ms) : 0u;
    usb_window_bytes = 0;

    n = PortTool_EchoFields(&usb_echo, body, sizeof(body));
    snprintf(body + n, sizeof(body) - n,
             " state=%s enum=%lu mode=%s rx_bytes=%lu tx_bytes=%lu"
             " kbps=%lu busy=%lu drop=%lu",
             usb_state_name(st), (unsigned long)usb_enum_changes,
             usb_mode_name(usb_mode),
             (unsigned long)usb_rx_bytes, (unsigned long)usb_tx_bytes,
             (unsigned long)kbps, (unsigned long)usb_busy,
             (unsigned long)usb_drop);

    PortTool_Frame("usb", "%s", body);
}

static void usb_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "mode=%s period=%lu",
             usb_mode_name(usb_mode), (unsigned long)usb_period_ms);
}

static void usb_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "mode:echo|sink|source|info|mirror|file period:%lu..",
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_usb = {
    .name     = "usb",
    .board    = PORTTOOL_BOARD_BRIDGE,
    .blk      = "-",
    .term     = "J2",          /* the Type-C connector, not a terminal */
    .terms    = NULL,
    .params   = "mode,period",
    .loop     = PORTTOOL_LOOP_LINK,
    .channels = 1,
    .start    = usb_start,
    .set      = usb_set,
    .stop     = usb_stop,
    .tick     = usb_tick,
    /* No echo hook: the reply comes back over the link (DECISIONS.md 9). */
    .echo     = NULL,
    .caps     = usb_caps,
    .limits   = usb_limits,
};

#endif /* PORTTOOL_ENABLE */
