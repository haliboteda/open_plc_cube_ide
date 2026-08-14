/*
 * ota_config.h
 *
 *  Created on: Apr 30, 2025
 *      Author: WhoamIamwhO
 */

#ifndef INC_IAP_CONFIG_H_
#define INC_IAP_CONFIG_H_

/* This bootloader's own version. Reported in the identity string and by "info",
 * both derived from here so the two can never disagree. Not the application's
 * version -- that one is a uint32 supplied at flash time and kept in the state
 * sector's metadata (see "getversion"). */
#ifndef OPENPLC_FW_VERSION
#define OPENPLC_FW_VERSION "0.1.2"
#endif

#define BOOT_LOADER_VERSION "Boot Loader " OPENPLC_FW_VERSION "\r\n"

#define IAP_RX_BUFFER_SIZE 8 * 1024

#ifndef OPENPLC_SERVER_PORT
#define OPENPLC_SERVER_PORT 56865
#endif

#ifndef OPENPLC_DEVICE_NAME
#define OPENPLC_DEVICE_NAME "STM32H743"
#endif

/* Role, the third field of the identity string. The application image defines
 * this as "CUSAPP" -- the two must differ, that is how a PC tool tells the
 * bootloader from a running application. */
#ifndef UDP_SERVER_NAME
#define UDP_SERVER_NAME "BOOTLD"
#endif

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
