/*
 * fw_pubkey.h
 *
 * *** THIS SHIPS WITH A PLACEHOLDER / TEST-ONLY KEY. ***
 * The matching private key is public -- anyone can sign a "valid" firmware
 * image with it. Run IAPServer/keys/rotate_keys.sh before shipping.
 */

#ifndef IAPSERVER_FW_PUBKEY_H_
#define IAPSERVER_FW_PUBKEY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t fw_public_key[64];

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_FW_PUBKEY_H_ */
