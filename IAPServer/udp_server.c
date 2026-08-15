#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "IAP_config.h"
#include "IAP_server.h"
#include "bootloader_state.h"
#include "iap_keyderive.h"

extern struct netif gnetif;

//static void (*udp_reboot_callback)(void) = NULL;
static struct udp_pcb *udp_server_pcb = NULL;

/* Discovery replies need no authentication (see SECURITY.md, Step 2) -- the
 * UID they carry is the very lookup key a per-device secret would be indexed
 * by, so gating a reply on one would be circular. What they do need is a
 * ceiling on how much traffic a stranger can make this device emit, so a
 * spoofed-source flood of "openplc_server_where_r_y" cannot turn it into a
 * reflection tool against a third party on the LAN.
 *
 * The ceiling is on the device as a whole, deliberately not per source. A
 * per-source budget is shared by every program on one host, and the Arduino
 * IDE's network_discovery polls every 30s from the same host an operator
 * flashes from -- so our own two tools spent a day refusing each other. A
 * legitimate load is ~2 replies/s, twenty-five times under this cap, so normal
 * use never reaches it.
 *
 * Mirrored in the application: OpenPLC_IAP/src/udp_server.c. Change one,
 * change both. */
#define DISCOVERY_MAX_REPLIES_PER_SEC 50U

static bool discovery_reply_allowed(void)
{
  static uint32_t window_start;
  static uint32_t replies_in_window;

  uint32_t now = HAL_GetTick();

  if ((now - window_start) >= 1000U) {
    window_start = now;
    replies_in_window = 0U;
  }

  if (replies_in_window >= DISCOVERY_MAX_REPLIES_PER_SEC) {
    /* Only the first refusal of each window speaks. Printing per dropped packet
     * would let a flood keep the UART busy at 115200 baud, which turns this log
     * into a better denial of service than the flood it reports. */
    if (replies_in_window == DISCOVERY_MAX_REPLIES_PER_SEC) {
      replies_in_window++;
      printf("[UDP] discovery capped at %u replies/s - something is flooding us\r\n",
             (unsigned)DISCOVERY_MAX_REPLIES_PER_SEC);
    }
    return false;
  }

  replies_in_window++;
  return true;
}

static void openplc_udp_reply(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port, const char *msg)
{
  size_t len = strlen(msg);
  err_t err;
  struct pbuf *reply_pbuf = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);

  /* Every way this can fail used to be silent, which made a discovery reply
   * that never went out indistinguishable from a board that was not there. */
  if (reply_pbuf == NULL) {
    printf("[UDP] discovery reply dropped: out of pbufs (%u bytes)\r\n", (unsigned)len);
    return;
  }
  memcpy(reply_pbuf->payload, msg, len);
  err = udp_sendto(pcb, reply_pbuf, addr, port);
  if (err != ERR_OK) {
    printf("[UDP] discovery reply to %s:%u failed: err %d\r\n",
           ipaddr_ntoa(addr), (unsigned)port, (int)err);
  }
  pbuf_free(reply_pbuf);
}

static void udp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port)
{
  (void)arg;

  if ((p == NULL) || (p->payload == NULL) || (p->len == 0)) {
    if (p != NULL) {
      pbuf_free(p);
    }
    return;
  }

  char recv_buf[64] = {0};
  const uint16_t n = (p->len < sizeof(recv_buf) - 1U) ? p->len : (sizeof(recv_buf) - 1U);
  memcpy(recv_buf, p->payload, n);
  pbuf_free(p);

  if ((strcmp(recv_buf, "DISCOVER") == 0) ||
      (strcmp(recv_buf, "openplc_discover") == 0) ||
      (strcmp(recv_buf, "openplc_server_where_r_y") == 0) ||
      (strcmp(recv_buf, "ping") == 0)) {
    char reply_msg[96] = {0};
    if (!discovery_reply_allowed()) {
      return;
    }
    /* Shared with the CDC probe so the two channels cannot drift apart. */
    iap_identity_string(reply_msg, sizeof(reply_msg));
    openplc_udp_reply(pcb, addr, port, reply_msg);
//  } else if (strcmp(recv_buf, "openplc_server_reboot") == 0) {
//    if (udp_reboot_callback != NULL) {
//      udp_reboot_callback();
//    } else {
//      openplc_set_eth_flag_and_reset();
//    }
  }
}

//void openplc_udp_server_start(void (*reboot_cb)(void))
void openplc_udp_server_start()
{
//  udp_reboot_callback = reboot_cb;

  if (udp_server_pcb != NULL) {
    return;
  }

  udp_server_pcb = udp_new();
  if (udp_server_pcb == NULL) {
    return;
  }

  if (udp_bind(udp_server_pcb, IP_ADDR_ANY, OPENPLC_SERVER_PORT) != ERR_OK) {
    udp_remove(udp_server_pcb);
    udp_server_pcb = NULL;
    return;
  }

  udp_recv(udp_server_pcb, udp_server_recv, NULL);
}

void openplc_udp_server_stop(void)
{
  if (udp_server_pcb != NULL) {
    udp_recv(udp_server_pcb, NULL, NULL);
    udp_disconnect(udp_server_pcb);
    udp_remove(udp_server_pcb);
    udp_server_pcb = NULL;
  }
}
