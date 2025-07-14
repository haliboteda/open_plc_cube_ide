/*
 * tcp_server_simple.c
 *
 *  Created on: May 7, 2025
 *      Author: WhoamIamwhO
 */

#include <tcp_server_s.h>
#include <IAP_config.h>
#include "lwip/tcp.h"
#include "FreeRTOS.h"
#include "semphr.h"

static struct tcp_pcb *server_pcb;
static struct tcp_pcb *client_pcb;

static err_t _recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
	if (p) {
		printf("_recv: %d\r\n", p->len);
		IAP_Data_Recv(IAP_ETHERNET, p->payload, p->len); // 处理TCP数据包
		tcp_recved(client_pcb, p->len);
		pbuf_free(p);
	} else {
		client_pcb = NULL; // 连接断开
		Reset_Buf();
	}
	return ERR_OK;
}

static err_t _accept(void *arg, struct tcp_pcb *pcb, err_t err) {
	client_pcb = pcb;
	tcp_recv(pcb, _recv);
	return ERR_OK;
}

void tcp_server_start(void) {
	server_pcb = tcp_new();
	if (!server_pcb) {
		TCP_DEBUG("Failed to create PCB");
		return;
	}

	if (tcp_bind(server_pcb, IP_ADDR_ANY, TCP_SERVER_PORT) != ERR_OK) {
		TCP_DEBUG("Bind failed");
		tcp_abort(server_pcb);
		return;
	}

	server_pcb = tcp_listen(server_pcb);
	tcp_accept(server_pcb, _accept);
	TCP_DEBUG("Server listening on port %d", TCP_SERVER_PORT);

	while (1) {
		// 5. 短暂延时
		vTaskDelay(pdMS_TO_TICKS(500));
	}
}

void tcp_server_send(uint8_t *data, uint16_t len) {
	if (client_pcb && data && len) {
		tcp_write(client_pcb, data, len, TCP_WRITE_FLAG_COPY);
		tcp_output(client_pcb);
	}
}
