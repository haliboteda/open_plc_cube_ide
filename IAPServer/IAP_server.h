/*
 * ota_processor.h
 *
 *  Created on: Apr 30, 2025
 *      Author: ziotier
 */

#ifndef INC_IAP_SERVER_H_
#define INC_IAP_SERVER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <IAP_config.h>
#include "stdint.h"

typedef void (*pFunction)(void);

//// 添加全局同步对象
//extern SemaphoreHandle_t xDataMutex;
//extern TaskHandle_t xCommandTaskHandle;

void reset_buf(void);
void process_command();
//uint8_t Check_Boot0_Pressed(void);

/* Boot sequence, split so that the mode decision happens before any peripheral
 * is initialised. See server_decide() in IAP_server.c for why.
 *
 *   Phase 1 (from USER CODE BEGIN SysInit, only minimal GPIO + UART4 up):
 *       pressed = <relay window, polling boot0_is_pressed() throughout>;
 *       mode    = server_decide(pressed);
 *       if (mode == IAP_NONE) { server_jump_to_app(); }
 *
 *   Phase 2 (from USER CODE BEGIN 2, after the peripherals this mode needs):
 *       IAP_servers_start(mode);
 */

/* Silent single read of SW2 (PG9). Poll it for the length of the startup relay
 * window and latch the result -- a single read right after reset would require
 * the operator to be holding the button within milliseconds of the reset, which
 * is not a reaction time a human has. */
uint8_t boot0_is_pressed(void);

IAP_Method server_decide(uint8_t boot0Pressed);
void server_jump_to_app(void);        /* only valid after IAP_NONE; never returns */
void IAP_servers_start(IAP_Method mode);

void IAP_task(void);

void IAP_data_recv(IAP_Method iapM, uint8_t *Buf, uint32_t Len);

void IAP_CDC_reboot_trigger(uint32_t bitrate);
void IAP_ETH_reboot_trigger();
///////////


#ifdef __cplusplus
}
#endif

#endif /* INC_IAP_SERVER_H_ */
