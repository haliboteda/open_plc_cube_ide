#include <string.h>
#include <stdio.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "rtc.h"
#include "IAP_config.h"

extern struct netif gnetif;

//static void (*udp_reboot_callback)(void) = NULL;
static struct udp_pcb *udp_server_pcb = NULL;

static void openplc_uid_hex(char out[25])
{
  uint32_t u0 = HAL_GetUIDw0();
  uint32_t u1 = HAL_GetUIDw1();
  uint32_t u2 = HAL_GetUIDw2();
  (void)snprintf(out, 25, "%08lX%08lX%08lX",
                 (unsigned long)u2, (unsigned long)u1, (unsigned long)u0);
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

static void openplc_set_eth_flag_and_reset(void)
{
  uint32_t regV = HAL_RTCEx_BKUPRead(&hrtc, MAGIC_BKP_REG);
  if (regV != MAGIC_ETH_FLAG) {
    HAL_RTCEx_BKUPWrite(&hrtc, MAGIC_BKP_REG, MAGIC_ETH_FLAG);
  }
  HAL_Delay(20);
  HAL_NVIC_SystemReset();
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
      (strcmp(recv_buf, "openplc_server_where_r_y") == 0)) {
    char uid_hex[25] = {0};
    char reply_msg[96] = {0};
    const char *ip_str = ip4addr_ntoa(netif_ip4_addr(&gnetif));
    openplc_uid_hex(uid_hex);
    (void)snprintf(reply_msg, sizeof(reply_msg), "%s,%s,%s",
                   OPENPLC_DEVICE_NAME, uid_hex, ip_str);
    openplc_udp_reply(pcb, addr, port, reply_msg);
//  } else if (strcmp(recv_buf, "openplc_server_reboot") == 0) {
//    if (udp_reboot_callback != NULL) {
//      udp_reboot_callback();
//    } else {
//      openplc_set_eth_flag_and_reset();
//    }
  } else if (strcmp(recv_buf, "ping") == 0) {
    char reply_msg[96] = {0};
    (void)snprintf(reply_msg, sizeof(reply_msg), "%s_%s_%s",
                   OPENPLC_DEVICE_NAME, UDP_SERVER_NAME, OPENPLC_CUSAPP_VERSION);
    openplc_udp_reply(pcb, addr, port, reply_msg);
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

  if (udp_bind(udp_server_pcb, IP_ADDR_ANY, OPENPLC_UDP_PORT) != ERR_OK) {
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
