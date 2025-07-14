#ifndef __TCP_SERVER_H
#define __TCP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

#define TCP_SERVER_PORT 8247
#define TCP_DEBUG(fmt, ...) printf("[TCP] " fmt "\n", ##__VA_ARGS__)

// Function prototype for the TCP server thread
//void tcp_server_thread(void const *argument);
//
//void send_data(uint8_t *data,uint16_t len);
//
//void TcpServerTask(void const *argument);

void tcp_server_start(void);
void tcp_server_send(uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __TCP_SERVER_H */
