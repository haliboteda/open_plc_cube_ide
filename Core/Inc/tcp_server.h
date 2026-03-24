#ifndef __TCP_SERVER_H
#define __TCP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"


#define TCP_PRINT(fmt, ...) printf("[TCP] " fmt "\n", ##__VA_ARGS__)

void tcp_server_start(void);
void tcp_server_send(uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __TCP_SERVER_H */
