/*
 * net_rand.h
 *
 * Random numbers for lwIP, drawn from the RNG peripheral. See
 * $PROD/docs/tables/DECISIONS.md, decision 67.
 */

#ifndef NET_RAND_H_
#define NET_RAND_H_

#include <stdint.h>

/* Seeds rand(), which is what LWIP_RAND() calls for DHCP transaction IDs and
 * ephemeral ports. Call after MX_RNG_Init() and before MX_LWIP_Init(). If the
 * RNG fails, rand() is left as it was. */
void net_rand_seed(void);

/* Initial sequence number for a new TCP connection, via LWIP_HOOK_TCP_ISN in
 * lwipopts.h. Falls back to rand() if the RNG fails. */
uint32_t net_rand_tcp_isn(void);

#endif /* NET_RAND_H_ */
