/*
 * udp_server.h
 *
 *  Created on: Jul 30, 2025
 *      Author: WhoamIamwhO
 */

#ifndef INC_UDP_SERVER_H_
#define INC_UDP_SERVER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

void openplc_udp_server_start(void);

/* Longest read-only query reply plus its NUL: getpubkey's 128 hex characters. */
#define IAP_QUERY_REPLY_MAX 129U

/* Reply text for the read-only query named exactly `name` (getuid, getpubkey,
 * getowner, getapprevoked); false for any other name. Used by UDP and by the
 * TCP/CDC parser, so every channel answers alike.
 * See $PROD/docs/modules/M1/IAP-PROTOCOL.md "只读查询". */
bool iap_query_reply(const char *name, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* INC_UDP_SERVER_H_ */
