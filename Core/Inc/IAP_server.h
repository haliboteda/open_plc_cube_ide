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

#include "FreeRTOS.h"
#include "semphr.h"

typedef void (*pFunction)(void);

//// 添加全局同步对象
//extern SemaphoreHandle_t xDataMutex;
//extern TaskHandle_t xCommandTaskHandle;

void Reset_Buf(void);
void Process_Command();
uint8_t Check_Boot0_Pressed(void);

void IAP_Init(void);
void IAP_Task(void);

void IAP_Data_Recv(IAP_Method iapM, uint8_t *Buf, uint32_t Len);

void IAP_CDC_Trigger(uint32_t bitrate);
///////////


#ifdef __cplusplus
}
#endif

#endif /* INC_IAP_SERVER_H_ */
