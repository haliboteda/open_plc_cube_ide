#include "lwip/tcp.h"
#include <stdio.h>
#include "tcp_server_s.h"

static struct tcp_pcb *pcb;

// Connection close handler (simplified)
static void tcp_server_close(struct tcp_pcb *tpcb) {
	if (!tpcb)
		return;

	tcp_arg(tpcb, NULL);
	tcp_sent(tpcb, NULL);
	tcp_recv(tpcb, NULL);
	tcp_err(tpcb, NULL);

	if (tcp_close(tpcb) != ERR_OK) {
		tcp_abort(tpcb);
	}
}

// Unified receive callback
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p,
		err_t err) {
	if (p) {
		IAP_Data_Recv(p->payload, p->len); // 处理TCP数据包
		tcp_recved(tpcb, p->len);
		pbuf_free(p);
	}
	return ERR_OK;
}

// Simplified accept callback
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
	if (err != ERR_OK || !newpcb)
		return ERR_VAL;

	// Set callbacks
	tcp_arg(newpcb, arg);
	tcp_recv(newpcb, tcp_server_recv);
	tcp_err(newpcb, (tcp_err_fn) tcp_server_close);

	TCP_DEBUG("Client connected");
	return ERR_OK;
}

// Main server task
void TcpServerTask(void const *arg) {
	pcb = tcp_new();
	if (!pcb) {
		TCP_DEBUG("Failed to create PCB");
		return;
	}

	if (tcp_bind(pcb, IP_ADDR_ANY, TCP_SERVER_PORT) != ERR_OK) {
		TCP_DEBUG("Bind failed");
		tcp_abort(pcb);
		return;
	}

	pcb = tcp_listen(pcb);
	tcp_accept(pcb, tcp_server_accept);
	TCP_DEBUG("Server listening on port %d", TCP_SERVER_PORT);

	while (1) {
		osDelay(25);
	}
}
