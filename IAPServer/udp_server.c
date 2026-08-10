#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "IAP_config.h"
#include "bootloader_state.h"
#include "iap_keyderive.h"

extern struct netif gnetif;

//static void (*udp_reboot_callback)(void) = NULL;
static struct udp_pcb *udp_server_pcb = NULL;

/* Discovery replies need no authentication (see SECURITY.md, Step 2) -- the
 * UID they carry is the very lookup key a per-device secret would be
 * indexed by, so gating a reply on one would be circular. What they do need
 * is a cap on how often a given source gets a reply, so a spoofed-source
 * flood of "openplc_server_where_r_y" can't turn this device into a
 * reflection/amplification tool against some third IP on the LAN. This is a
 * small fixed-size LRU of recently-seen sources, not a security boundary --
 * it only bounds abuse, it doesn't require the sender to prove anything. */
#define DISCOVERY_MIN_REPLY_INTERVAL_MS 2000U
#define DISCOVERY_RATE_SLOTS 8U

typedef struct {
  ip_addr_t addr;
  uint32_t  last_reply_tick;
  bool      used;
} discovery_rate_slot_t;

static discovery_rate_slot_t s_discovery_rate_slots[DISCOVERY_RATE_SLOTS];

static bool discovery_reply_allowed(const ip_addr_t *addr)
{
  uint32_t now = HAL_GetTick();
  uint32_t i;
  uint32_t victim;

  for (i = 0; i < DISCOVERY_RATE_SLOTS; i++) {
    if (s_discovery_rate_slots[i].used && ip_addr_cmp(&s_discovery_rate_slots[i].addr, addr)) {
      if ((now - s_discovery_rate_slots[i].last_reply_tick) < DISCOVERY_MIN_REPLY_INTERVAL_MS) {
        return false; /* replied to this source too recently */
      }
      s_discovery_rate_slots[i].last_reply_tick = now;
      return true;
    }
  }

  /* Not tracked yet: claim a free slot, or evict the least-recently-used one. */
  victim = 0;
  for (i = 0; i < DISCOVERY_RATE_SLOTS; i++) {
    if (!s_discovery_rate_slots[i].used) {
      victim = i;
      break;
    }
    if (s_discovery_rate_slots[i].last_reply_tick < s_discovery_rate_slots[victim].last_reply_tick) {
      victim = i;
    }
  }
  s_discovery_rate_slots[victim].addr = *addr;
  s_discovery_rate_slots[victim].used = true;
  s_discovery_rate_slots[victim].last_reply_tick = now;
  return true;
}

static void openplc_udp_reply(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port, const char *msg)
{
  size_t len = strlen(msg);
  struct pbuf *reply_pbuf = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
  if (reply_pbuf == NULL) {
    return;
  }
  memcpy(reply_pbuf->payload, msg, len);
  udp_sendto(pcb, reply_pbuf, addr, port);
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
    char uid_hex[IAP_MACHINE_ID_HEX_LEN + 1U] = {0};
    char reply_msg[96] = {0};
    if (!discovery_reply_allowed(addr)) {
      return;
    }
    // Note: the reply is joined/split on "_" (see PC tool's parseBoardInfoFromReply),
    // so this suffix must not itself contain an underscore.
    const char *role = bootloader_state_app_is_valid() ? UDP_SERVER_NAME : UDP_SERVER_NAME "-INVALID";
    iap_keyderive_get_machine_id_hex(uid_hex);
    (void)snprintf(reply_msg, sizeof(reply_msg), "%s_%s_%s_%s",
                   OPENPLC_DEVICE_NAME, uid_hex, role, OPENPLC_CUSAPP_VERSION);
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
