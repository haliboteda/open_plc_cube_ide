/*
 * tcp_server_simple.c
 *
 *  Created on: May 7, 2025
 *      Author: WhoamIamwhO
 */

#include <IAP_config.h>
#include <tcp_server.h>
#include "lwip/tcp.h"

#include "IAP_server.h"
#include "string.h"

extern struct netif gnetif;
static struct tcp_pcb *server_pcb;
static struct tcp_pcb *client_pcb;

static void tcp_server_close(struct tcp_pcb *pcb) {
	if (pcb) {
		tcp_arg(pcb, NULL);
		tcp_sent(pcb, NULL);
		tcp_recv(pcb, NULL);
		tcp_err(pcb, NULL);
		tcp_close(pcb);
	}
	client_pcb = NULL;
	reset_buf();
	TCP_PRINT("Client disconnected.");
}

static err_t _recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
	if (err != ERR_OK || p == NULL) {
		tcp_server_close(pcb);  // 安全关闭连接
		return ERR_OK;
	}

	struct pbuf *q = p;
	while (q != NULL) {
//		TCP_PRINT("_recv: %d of total: %d\r\n", q->len, q->tot_len);
		IAP_data_recv(IAP_ETHERNET, q->payload, q->len);
		q = q->next;
	}

	tcp_recved(pcb, p->tot_len);

	pbuf_free(p);  //
	return ERR_OK;
}

static void _err(void *arg, err_t err) {
	(void)arg;
	TCP_PRINT("TCP connection error: %d\r\n", err);
	client_pcb = NULL;
	reset_buf();  // 清空 buffer
}

static err_t _accept(void *arg, struct tcp_pcb *pcb, err_t err) {
	client_pcb = pcb;
	tcp_recv(pcb, _recv);
	tcp_err(pcb, _err);
	return ERR_OK;
}

void tcp_server_start(void) {
	server_pcb = tcp_new();
	if (!server_pcb) {
		TCP_PRINT("Failed to create PCB");
		return;
	}

	if (tcp_bind(server_pcb, IP_ADDR_ANY, OPENPLC_SERVER_PORT) != ERR_OK) {
		TCP_PRINT("Bind failed");
		tcp_abort(server_pcb);
		return;
	}

	server_pcb = tcp_listen(server_pcb);
	tcp_accept(server_pcb, _accept);
	TCP_PRINT("Server listening on port %d IP %s", OPENPLC_SERVER_PORT, ip4addr_ntoa(netif_ip4_addr(&gnetif)));
}


uint32_t tcp_server_get_client_ip(void) {
	if (client_pcb == NULL) {
		return 0U;
	}
	return ip4_addr_get_u32(ip_2_ip4(&client_pcb->remote_ip));
}

void tcp_server_send(uint8_t* data, uint16_t len) {
	err_t err;
	if (client_pcb && data && len) {
		err = tcp_write(client_pcb, data, len, TCP_WRITE_FLAG_COPY);
		if (err == ERR_OK) {
			tcp_output(client_pcb);
		} else {
			TCP_PRINT("tcp_write failed: %d\n", err);
		}
	}
}
