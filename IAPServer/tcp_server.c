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

static struct tcp_pcb *server_pcb;
static struct tcp_pcb *client_pcb;

/* Drop a session that goes quiet, so one stalled peer cannot hold the single
 * session slot. Interval is in 500 ms lwIP coarse ticks, so this polls once a
 * second and gives up after 60 s. */
#define TCP_POLL_INTERVAL   2U
#define TCP_IDLE_POLL_LIMIT 60U

static uint32_t idle_polls;

static void tcp_server_close(struct tcp_pcb *pcb) {
	if (pcb == NULL) {
		return;
	}

	tcp_arg(pcb, NULL);
	tcp_sent(pcb, NULL);
	tcp_recv(pcb, NULL);
	tcp_err(pcb, NULL);
	tcp_poll(pcb, NULL, 0);
	tcp_close(pcb);

	/* Only the session that owns the shared receive state may clear it --
	 * otherwise any stranger's connect/disconnect aborts a transfer in
	 * flight. */
	if (pcb == client_pcb) {
		client_pcb = NULL;
		reset_buf();
		TCP_PRINT("Client disconnected.");
	}
}

static err_t _recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
	(void)arg;

	if (err != ERR_OK || p == NULL) {
		tcp_server_close(pcb);
		return ERR_OK;
	}

	if (pcb != client_pcb) {
		/* Not the owning session: drop the data instead of mixing it into
		 * the image being received. */
		tcp_recved(pcb, p->tot_len);
		pbuf_free(p);
		return ERR_OK;
	}

	idle_polls = 0U;

	struct pbuf *q = p;
	while (q != NULL) {
		IAP_data_recv(IAP_ETHERNET, q->payload, q->len);
		q = q->next;
	}

	tcp_recved(pcb, p->tot_len);

	pbuf_free(p);
	return ERR_OK;
}

static void _err(void *arg, err_t err) {
	TCP_PRINT("TCP connection error: %d", err);

	/* lwIP has already freed the pcb; arg carries which one it was. */
	if (arg == (void *)client_pcb) {
		client_pcb = NULL;
		reset_buf();
	}
}

static err_t _poll(void *arg, struct tcp_pcb *pcb) {
	(void)arg;

	if (pcb != client_pcb) {
		return ERR_OK;
	}
	if (++idle_polls > TCP_IDLE_POLL_LIMIT) {
		TCP_PRINT("Idle client timed out.");
		client_pcb = NULL;
		reset_buf();
		tcp_abort(pcb);
		return ERR_ABRT;
	}
	return ERR_OK;
}

static err_t _accept(void *arg, struct tcp_pcb *pcb, err_t err) {
	(void)arg;

	if ((err != ERR_OK) || (pcb == NULL)) {
		return ERR_VAL;
	}

	/* One session at a time. A second connection would otherwise take over
	 * the reply channel and feed the same receive buffer. lwIP aborts the
	 * refused pcb for us. */
	if (client_pcb != NULL) {
		TCP_PRINT("Refused second connection.");
		return ERR_MEM;
	}

	client_pcb = pcb;
	idle_polls = 0U;
	tcp_arg(pcb, pcb);
	tcp_recv(pcb, _recv);
	tcp_err(pcb, _err);
	tcp_poll(pcb, _poll, TCP_POLL_INTERVAL);
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
	// TCP_PRINT("Server listening on port %d", OPENPLC_SERVER_PORT);
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
