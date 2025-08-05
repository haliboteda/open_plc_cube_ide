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

#define UDP_SERVER_PORT 12345
#define UDP_PRINT(fmt, ...) printf("[UDP] " fmt "\n", ##__VA_ARGS__)

void udp_server_start(void (*reboot_func)(void));
void udp_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_UDP_SERVER_H_ */
