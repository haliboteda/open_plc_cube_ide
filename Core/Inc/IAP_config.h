/*
 * ota_config.h
 *
 *  Created on: Apr 30, 2025
 *      Author: WhoamIamwhO
 */

#ifndef INC_IAP_CONFIG_H_
#define INC_IAP_CONFIG_H_

#define BOOT_LOADER_VERSION "Boot Loader 0.1.2\r\n"

#define IAP_RX_BUFFER_SIZE 8 * 1024

#ifndef OPENPLC_SERVER_PORT
#define OPENPLC_SERVER_PORT 56865
#endif

#ifndef OPENPLC_DEVICE_NAME
#define OPENPLC_DEVICE_NAME "STM32H743"
#endif

#ifndef OPENPLC_CUSAPP_VERSION
#define OPENPLC_CUSAPP_VERSION "0.1.2"
#endif

#ifndef UDP_SERVER_NAME
#define UDP_SERVER_NAME "BOOTLD"
#endif

/* The baud rate the PC tool opens the CDC port at to ask for upload mode. Not a
 * flag -- this one stays. */
#ifndef MAGIC_CDC_RATE
#define MAGIC_CDC_RATE 1200U
#endif

/* MAGIC_APP_FLAG / MAGIC_ETH_FLAG / MAGIC_CDC_FLAG / MAGIC_BKP_REG are gone.
 * The boot-mode request no longer lives in an RTC backup register; see
 * IAPServer/IAP_boot_handoff.h for the replacement and for why the register was
 * the wrong place for it. */

typedef enum {
	IDLE, FLASH_RECEIVE
} IAP_STATUS;

typedef enum {
    IAP_NONE,
	IAP_CDC,
	IAP_ETHERNET,
	/* Decision result only, never a transport identity: stay in the bootloader
	 * and serve every channel. Used when we know an upload was asked for but not
	 * over which channel (unreadable handoff record), and when the board has no
	 * runnable application or BOOT0 was held -- in all three cases we have no
	 * idea how the operator intends to reach us, so guessing one channel would
	 * be worse than opening both. IAP_data_recv() is never called with it. */
	IAP_ALL
} IAP_Method;

#endif /* INC_IAP_CONFIG_H_ */
