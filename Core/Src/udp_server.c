/*
 * udp_server.c
 *
 *  Created on: Jul 30, 2025
 *      Author: WhoamIamwhO
 */

#include "lwip/udp.h"

#include <string.h>
#include <udp_server.h>

extern struct netif gnetif;

static void (*udp_reboot_callback)() = NULL;
static struct udp_pcb *udp_server_pcb = NULL;

static void udp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
		const ip_addr_t *addr, u16_t port) {
	if (p == NULL || p->payload == NULL) {
		return;
	}

	char recv_buf[64] = { 0 };
	memcpy(recv_buf, p->payload, p->len);  // copy msg
	pbuf_free(p);
	UDP_PRINT("Received UDP packet from %s:%d, %s len = %d\r\n",
			ipaddr_ntoa(addr), port, recv_buf, p->len);
	//
	if (strcmp(recv_buf, "openplc_server_where_r_y") == 0) {
		// 1. response for ip required
		const char *ip_str = ip4addr_ntoa(netif_ip4_addr(&gnetif));

		char reply_msg[64] = { 0 };
		snprintf(reply_msg, sizeof(reply_msg), "openplc_web_ip_%s", ip_str);

		struct pbuf *reply_pbuf = pbuf_alloc(PBUF_TRANSPORT, strlen(reply_msg),
				PBUF_RAM);
		if (reply_pbuf != NULL) {
			memcpy(reply_pbuf->payload, reply_msg, strlen(reply_msg));
			udp_sendto(pcb, reply_pbuf, addr, port);
			pbuf_free(reply_pbuf);
			UDP_PRINT("Send back openplc_web_ip_%s", ip_str);
		} else {
			UDP_PRINT("UDP server reply_pbuf alloc error");
		}
	} else if (strcmp(recv_buf, "openplc_server_reboot") == 0) {
		// 2. reboot
		UDP_PRINT("Reboot command received, restarting...\r\n");
		HAL_Delay(100);  //
		if (udp_reboot_callback != NULL) {
			udp_reboot_callback();  //
		}
	} else if (strcmp(recv_buf, "ping") == 0) {
		// response for ping
		char reply_msg[8] = { 0 };
		snprintf(reply_msg, sizeof(reply_msg), "pong");

		struct pbuf *reply_pbuf = pbuf_alloc(PBUF_TRANSPORT, strlen(reply_msg),
				PBUF_RAM);
		if (reply_pbuf != NULL) {
			memcpy(reply_pbuf->payload, reply_msg, strlen(reply_msg));
			udp_sendto(pcb, reply_pbuf, addr, port);
			pbuf_free(reply_pbuf);
		} else {
			UDP_PRINT("UDP server response for ping error");
		}
	}
}

void udp_server_start(void (*reboot_func)(void)) {
	udp_reboot_callback = reboot_func;

	udp_server_pcb = udp_new();
	if (udp_server_pcb == NULL) {
		UDP_PRINT("Cannot create UDP PCB\r\n");
		return;
	}

	// bind
	err_t err = udp_bind(udp_server_pcb, IP_ADDR_ANY, UDP_SERVER_PORT);
	if (err != ERR_OK) {
		udp_remove(udp_server_pcb);
		UDP_PRINT("UDP bind failed: %d\r\n", err);
		return;
	}

	// callback
	udp_recv(udp_server_pcb, udp_server_recv, NULL);

	UDP_PRINT("UDP Server started on port %d\r\n", UDP_SERVER_PORT);
}

void udp_server_stop(void) {
	if (udp_server_pcb != NULL) {
		udp_recv(udp_server_pcb, NULL, NULL);

		udp_disconnect(udp_server_pcb);      //
		udp_remove(udp_server_pcb);          // release
		udp_server_pcb = NULL;
		UDP_PRINT("UDP Server stopped\r\n");
		//
		HAL_Delay(50);  //
	}
}
