#include <string.h>
#include <stdio.h>
#include <stdlib.h>      /* strtoul */
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
#include "boot_log.h"
#include "iap_keyderive.h"
#include "owner_slot.h"
#include "udp_server.h"

static struct udp_pcb *udp_server_pcb = NULL;

/* Discovery replies need no authentication (see SECURITY.md, Step 2) -- the
 * UID they carry is the very lookup key a per-device secret would be indexed
 * by, so gating a reply on one would be circular. What they do need is a
 * ceiling on how much traffic a stranger can make this device emit, so a
 * spoofed-source flood of "openplc_server_where_r_y" cannot turn it into a
 * reflection tool against a third party on the LAN.
 *
 * Device-wide, not per source: a per-source budget is shared by every program
 * on one host ($PROD/docs/tables/DECISIONS.md, decision 3). Normal load is
 * ~2 replies/s.
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
    /* Only the first refusal of each window speaks: the log port is far slower
     * than the flood, so per-packet logging would be the worse denial of service.
     * Why, in full: $PROD/docs/modules/M1/BOOT-SEQUENCE.md "网络发现：限流". */
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

  /* Report every failure: a silent one looks like a board that is not there. */
  if (reply_pbuf == NULL) {
    printf("[UDP] reply dropped: out of pbufs (%u bytes)\r\n", (unsigned)len);
    return;
  }
  memcpy(reply_pbuf->payload, msg, len);
  err = udp_sendto(pcb, reply_pbuf, addr, port);
  if (err != ERR_OK) {
    printf("[UDP] reply to %s:%u failed: err %d\r\n",
           ipaddr_ntoa(addr), (unsigned)port, (int)err);
  }
  pbuf_free(reply_pbuf);
}

/* "openplc_log [<from>]": the SRAM4 boot log, one reply-sized piece at a time.
 * Format and limits: $PROD/docs/modules/M1/IAP-PROTOCOL.md "openplc_log".
 * Mirrored in the application: OpenPLC_IAP/src/udp_server.c (mirror 20). */
#define LOG_READ_REPLY_MAX           1024U
#define LOG_READ_HEADER_MAX          32U     /* "log <u32> <0|1>\n" */
#define LOG_READ_MAX_REPLIES_PER_SEC 20U

/* Own budget so log reads cannot starve discovery; device-wide for the same
 * reason as discovery_reply_allowed(). */
static bool log_read_reply_allowed(void)
{
  static uint32_t log_win_start;
  static uint32_t log_replies;

  uint32_t now = HAL_GetTick();

  if ((now - log_win_start) >= 1000U) {
    log_win_start = now;
    log_replies = 0U;
  }
  if (log_replies >= LOG_READ_MAX_REPLIES_PER_SEC) {
    return false;
  }
  log_replies++;
  return true;
}

typedef struct {
  uint32_t from;
  uint32_t next;
  bool full;
  uint16_t used;
  char body[LOG_READ_REPLY_MAX - LOG_READ_HEADER_MAX];
} log_read_ctx_t;

/* boot_log_read_all() cannot stop early, so once full the rest is skipped. */
static void log_read_line(const char *line, size_t len, uint32_t seq, void *ctx)
{
  log_read_ctx_t *c = (log_read_ctx_t *)ctx;
  char seq_txt[12];
  int seq_len;
  size_t i;

  if ((seq < c->from) || c->full) {
    return;
  }
  seq_len = snprintf(seq_txt, sizeof(seq_txt), "%lu ", (unsigned long)seq);
  if ((seq_len <= 0) || ((c->used + (size_t)seq_len + len + 1U) > sizeof(c->body))) {
    c->full = true;
    return;
  }
  memcpy(&c->body[c->used], seq_txt, (size_t)seq_len);
  c->used += (uint16_t)seq_len;
  for (i = 0; i < len; i++) {
    /* One record, one line: the reply is line-framed. */
    c->body[c->used++] = ((line[i] == '\r') || (line[i] == '\n')) ? ' ' : line[i];
  }
  c->body[c->used++] = '\n';
  c->next = seq + 1U;
}

static void log_read_reply(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port, uint32_t from)
{
  /* Static: too big for the lwIP callback's stack; lwIP runs callbacks one at a time. */
  static log_read_ctx_t ctx;
  char header[LOG_READ_HEADER_MAX];
  int header_len;
  struct pbuf *reply_pbuf;

  if (!log_read_reply_allowed()) {
    return;
  }
  ctx.from = from;
  ctx.next = from;
  ctx.full = false;
  ctx.used = 0U;
  boot_log_read_all(log_read_line, &ctx);

  header_len = snprintf(header, sizeof(header), "log %lu %u\n",
                        (unsigned long)ctx.next, ctx.full ? 1U : 0U);
  if ((header_len <= 0) || ((size_t)header_len >= sizeof(header))) {
    return;
  }
  reply_pbuf = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(header_len + ctx.used), PBUF_RAM);
  if (reply_pbuf == NULL) {
    return;
  }
  memcpy(reply_pbuf->payload, header, (size_t)header_len);
  memcpy((char *)reply_pbuf->payload + header_len, ctx.body, ctx.used);
  udp_sendto(pcb, reply_pbuf, addr, port);
  pbuf_free(reply_pbuf);
}

/* Read-only queries: getuid, getpubkey, getowner, getapprevoked.
 * Format and limits: $PROD/docs/modules/M1/IAP-PROTOCOL.md "只读查询".
 * Mirrored in the application: OpenPLC_IAP/src/udp_server.c (mirror 21). The
 * four query_* accessors are this side's; iap_query_reply() and
 * query_reply_allowed() must stay textually identical to the app's. */
#define QUERY_MAX_REPLIES_PER_SEC 20U

/* Own budget, device-wide, for the same reason as discovery_reply_allowed(). */
static bool query_reply_allowed(void)
{
  static uint32_t query_win_start;
  static uint32_t query_replies;

  uint32_t now = HAL_GetTick();

  if ((now - query_win_start) >= 1000U) {
    query_win_start = now;
    query_replies = 0U;
  }
  if (query_replies >= QUERY_MAX_REPLIES_PER_SEC) {
    return false;
  }
  query_replies++;
  return true;
}

static bool query_root(uint8_t out[64])
{
  const uint8_t *root = owner_slot_root();

  if (root == NULL) {
    return false;
  }
  memcpy(out, root, 64U);
  return true;
}

static uint32_t query_generation(void)
{
  return owner_slot_generation();
}

/* The leaf that signed the image this board would run. The metadata record
 * outlives an erase of the app, so it alone does not prove there is one; the
 * boot decision does. */
static bool query_installed_leaf(uint8_t out[64])
{
  iap_fw_metadata_t meta;

  if (!bootloader_state_app_is_valid() || !bootloader_state_get_metadata(&meta)
      || (meta.app_size == 0U)) {
    return false;
  }
  memcpy(out, meta.cert.leaf_pubkey, 64U);
  return true;
}

static bool query_is_revoked(const uint8_t leaf[64])
{
  return owner_slot_is_revoked(leaf);
}

bool iap_query_reply(const char *name, char *out, size_t out_len)
{
  uint8_t key[64];
  uint32_t i;

  if (out_len < IAP_QUERY_REPLY_MAX) {
    return false;
  }
  if (strcmp(name, "getuid") == 0) {
    iap_keyderive_get_machine_id_hex(out);
  } else if (strcmp(name, "getpubkey") == 0) {
    /* "none" without a root: IAPTool then claims the board (decision 72). */
    if (!query_root(key)) {
      (void)strcpy(out, "none");
    } else {
      for (i = 0U; i < 64U; i++) {
        (void)snprintf(&out[i * 2U], 3U, "%02x", key[i]);
      }
    }
  } else if (strcmp(name, "getowner") == 0) {
    (void)snprintf(out, out_len, "%lu", (unsigned long)query_generation());
  } else if (strcmp(name, "getapprevoked") == 0) {
    /* "none" is not "no": a board with nothing runnable has nothing to re-upload. */
    if (!query_installed_leaf(key)) {
      (void)strcpy(out, "none");
    } else {
      (void)strcpy(out, query_is_revoked(key) ? "yes" : "no");
    }
  } else {
    return false;
  }
  return true;
}

static void udp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port)
{
  (void)arg;

  if ((p == NULL) || (p->payload == NULL) || (p->len == 0)) {
    printf("[UDP] dropped a malformed/empty datagram from %s:%u\r\n",
           ipaddr_ntoa(addr), (unsigned)port);
    if (p != NULL) {
      pbuf_free(p);
    }
    return;
  }

  char recv_buf[64] = {0};
  char query_msg[IAP_QUERY_REPLY_MAX];
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
  } else if ((strncmp(recv_buf, "openplc_log", 11) == 0) &&
             ((recv_buf[11] == '\0') || (recv_buf[11] == ' '))) {
    log_read_reply(pcb, addr, port, (uint32_t)strtoul(&recv_buf[11], NULL, 10));
  } else if (iap_query_reply(recv_buf, query_msg, sizeof(query_msg))) {
    if (query_reply_allowed()) {
      openplc_udp_reply(pcb, addr, port, query_msg);
    }
  } else {
    printf("[UDP] unrecognized command from %s:%u: \"%s\"\r\n",
           ipaddr_ntoa(addr), (unsigned)port, recv_buf);
  }
}

void openplc_udp_server_start(void)
{
  if (udp_server_pcb != NULL) {
    return;
  }

  udp_server_pcb = udp_new();
  if (udp_server_pcb == NULL) {
    printf("[UDP] Failed to create PCB\r\n");
    return;
  }

  if (udp_bind(udp_server_pcb, IP_ADDR_ANY, OPENPLC_SERVER_PORT) != ERR_OK) {
    printf("[UDP] Bind failed\r\n");
    udp_remove(udp_server_pcb);
    udp_server_pcb = NULL;
    return;
  }

  udp_recv(udp_server_pcb, udp_server_recv, NULL);
}
