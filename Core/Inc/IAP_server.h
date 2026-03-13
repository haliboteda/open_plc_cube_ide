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

void IAP_init(void);
void IAP_task(void);

void IAP_data_recv(IAP_Method iapM, uint8_t *Buf, uint32_t Len);

void IAP_CDC_reboot_trigger(uint32_t bitrate);
void IAP_ETH_reboot_trigger();
///////////


#ifdef __cplusplus
}
#endif

#endif /* INC_IAP_SERVER_H_ */
