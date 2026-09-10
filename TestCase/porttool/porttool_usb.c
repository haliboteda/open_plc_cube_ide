// porttool_usb.c
//
// USB session: the CDC virtual serial port on the Type-C connector, driven as
// a data channel rather than as the bootloader's upload path.
//
// *** Starting this session is what brings the USB device stack up, the same
// *** way the ethernet session brings lwIP up (DECISIONS.md 28). An image whose
// *** operator never starts it runs none of it.
// ***
// *** That gate is not a nicety, it is what keeps the IAP server out of the
// *** tool image. CDC_Receive_FS calls IAP_data_recv, so the moment the USB
// *** stack becomes reachable the whole bootloader server is linked in behind
// *** it. usbd_cdc_if.c therefore routes to PortUsb_Received under
// *** PORTTOOL_ENABLE instead - see the USER CODE block in CDC_Receive_FS.
//
// The board is a Device, never a Host (FIXTURE-INTERFACE.md): the PC opens the
// virtual COM port this enumerates as, and that is the peer for every mode.
//
// Parameters:
//   mode=echo    echo | sink | source | info      (default echo)
//   period=1000  milliseconds between frames      (default 1000, floor 50)
//
// The four modes answer four different questions:
//
//   echo    the board sends its counter, the PC sends it straight back.
//           Proves the pipe carries traffic both ways. This is the loop=link
//           counter, and the only mode where seq is the verdict.
//   sink    the PC pushes a large file at the board, which counts bytes and
//           reports the rate. Proves sustained receive throughput.
//   source  the board pushes as fast as the endpoint accepts and counts what
//           it handed over. Proves sustained transmit throughput.
//   info    moves nothing. Reports enumeration state and how many times it has
//           changed - for a cable or a host that keeps re-enumerating, which
//           looks like a dead link in every other mode.
//
// Frame:
//   !usb t=48213 seq=1 rx=1 miss=0 state=cfg enum=1 mode=echo
//        rx_bytes=0 tx_bytes=0 kbps=0 busy=0
//
// *** busy= counts writes the endpoint refused because the previous one had
// *** not gone out yet. In source mode that is the normal back-pressure and
// *** says the host is the limit; in echo mode a climbing busy with a stalled
// *** seq means the host stopped reading, which is a different fault from a
// *** dead cable and has to be told apart from it.

#include "porttool.h"
#include "porttool_cmd.h"

#include "main.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include "usbd_def.h"

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

/* CubeMX defines this in usb_device.c and declares it in no header -
 * usbd_cdc_if.c externs it the same way. */
extern USBD_HandleTypeDef hUsbDeviceFS;

#define USB_PERIOD_DEF_MS 1000U

/* One packet's worth. The CDC endpoint is 64 bytes on full speed; handing over
 * more per write just means the stack fragments it, and a size that matches
 * makes the busy= count mean "one packet was refused". */
#define USB_CHUNK 64U

typedef enum {
    USB_MODE_ECHO = 0,
    USB_MODE_SINK,
    USB_MODE_SOURCE,
    USB_MODE_INFO,
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

/* Enumeration state changes since the session started. A cable or a host that
 * keeps re-enumerating shows up here and nowhere else. */
static uint8_t  usb_last_state;
static uint32_t usb_enum_changes;

/* Counting only starts once the pipe has been configured once.
 *
 * ⚠️ On a board that has just been powered on, pt.start usb is what
 * initialises the USB stack, so the host has not enumerated at all yet - the
 * walk default -> addressed -> configured then happens DURING the session and
 * would be counted as four changes. That is enumeration working, not
 * enumeration going wrong. Counting from "configured" is what makes enum=
 * mean "it re-enumerated after being up", which is the fault worth reporting
 * (a flaky cable, or a host that keeps re-attaching).
 *
 * If the cable is out this never arms, enum stays 0 - and state= is what says
 * so, which the plan judges separately. */
static int usb_enum_armed;

static porttool_echo_t usb_echo;

/* What source mode pushes. Content is irrelevant - only the rate is. */
static const uint8_t usb_filler[USB_CHUNK] = { 0 };

static const char *usb_mode_name(usb_mode_t m)
{
    switch (m) {
    case USB_MODE_SINK:   return "sink";
    case USB_MODE_SOURCE: return "source";
    case USB_MODE_INFO:   return "info";
    default:              return "echo";
    }
}

/* The device state, short enough for a frame. "cfg" is the only one that means
 * the PC has opened the pipe; the others are how far enumeration got. */
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

/* Called from CDC_Receive_FS, which runs in USB interrupt context.
 *
 * Everything here is counters and a decimal scan - no printf, no HAL call that
 * blocks. A frame is pushed from the tick, never from here: printf takes about
 * a hundred blocking UART writes, and doing that inside the USB interrupt
 * would stall the endpoint that is feeding it. */
void PortUsb_Received(const uint8_t *data, uint32_t len)
{
    static uint32_t acc;
    static int      in_number;

    if (!usb_running || data == NULL || len == 0u) {
        return;
    }

    usb_rx_bytes     += len;
    usb_window_bytes += len;

    /* Only echo mode reads the payload. sink deliberately does not look at it:
     * the question there is how fast bytes arrive, and parsing them would put
     * this loop's own cost into the measurement. */
    if (usb_mode != USB_MODE_ECHO) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if ((c >= '0') && (c <= '9')) {
            acc = (acc * 10u) + (uint32_t)(c - '0');
            in_number = 1;
        } else if (in_number) {
            PortTool_EchoGot(&usb_echo, acc);
            acc = 0;
            in_number = 0;
        }
    }
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
        else {
            snprintf(err, err_len,
                     "mode=\"%s\" must be echo, sink, source or info", probe);
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

    /* Deliberately NOT refused when the PC has not opened the port yet.
     *
     * "nobody is connected" is a reading this session exists to report -
     * state= and enum= say exactly that - and a station whose cable is out
     * needs to see which of the two it is. Refusing would leave it with an
     * error message and no reading at all. */
    PortTool_EchoReset(&usb_echo);
    usb_rx_bytes     = 0;
    usb_tx_bytes     = 0;
    usb_window_bytes = 0;
    usb_busy         = 0;
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
    usb_running = 0;

    /* The stack is left up. Tearing the device down would make the PC's COM
     * port disappear and come back, and on Windows it may come back with a
     * different number - which would strand whatever the operator had open.
     * Nothing else in the image contends for it. */
}

static void usb_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    char line[32];
    uint32_t n;
    uint32_t kbps;
    uint8_t st;

    /* Watched every pass, not once per frame: a host that re-enumerates
     * between two frames would otherwise leave no trace at all. */
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
        /* A hard cap per pass, not "until the endpoint says no".
         *
         * On the board this loop ends after a packet or two because
         * CDC_Transmit_FS returns BUSY until the transfer-complete interrupt
         * clears TxState - but the superloop must not be at the mercy of that.
         * An endpoint that kept accepting would starve every other session and
         * the command channel with it, and this loop hung the host harness
         * outright on 2026-09-08 for exactly that reason.
         *
         * Eight 64-byte packets is 512 bytes a pass, which at the superloop's
         * rate is far more than the line can carry anyway. */
        for (int i = 0; i < 8; i++) {
            if (usb_send(usb_filler, (uint16_t)sizeof(usb_filler)) == 0u) {
                break;
            }
        }
    }

    if ((int32_t)(now_ms - usb_due_ms) < 0) {
        return;
    }
    usb_due_ms = now_ms + usb_period_ms;

    /* Only echo hands the host a number to send back, so only echo may count a
     * miss. info moves nothing at all, and sink and source push bytes one way
     * without ever asking for an answer: a mode that reports misses it never
     * gave anybody a chance to answer would read as a dead link on a perfectly
     * good one. */
    if (usb_mode == USB_MODE_ECHO) {
        PortTool_EchoTick(&usb_echo);
    }

    /* Sent on the link under test. The peer sends this number straight back -
     * unchanged, like every other loop=link port (DECISIONS.md 18). */
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
             " kbps=%lu busy=%lu",
             usb_state_name(st), (unsigned long)usb_enum_changes,
             usb_mode_name(usb_mode),
             (unsigned long)usb_rx_bytes, (unsigned long)usb_tx_bytes,
             (unsigned long)kbps, (unsigned long)usb_busy);

    PortTool_Frame("usb", "%s", body);
}

static void usb_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "mode=%s period=%lu",
             usb_mode_name(usb_mode), (unsigned long)usb_period_ms);
}

static void usb_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "mode:echo|sink|source|info period:%lu..",
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
    /* No echo hook on purpose: the reply comes back over the link under test,
     * so accepting one on the control port would let the counter climb while
     * the USB pipe is dead. Same contract as rs485, can and eth. */
    .echo     = NULL,
    .caps     = usb_caps,
    .limits   = usb_limits,
};

#endif /* PORTTOOL_ENABLE */
