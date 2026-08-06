/*
 * fw_pubkey.h
 *
 * *** THIS SHIPS WITH A PLACEHOLDER / TEST-ONLY KEY. ***
 * See IAPServer/keys/README.md before building anything for production.
 * The matching private key (IAPServer/keys/fw_signing_key.TEST_ONLY.pem)
 * is public -- anyone can sign a "valid" firmware image with it. Run
 * IAPServer/keys/generate_keys.sh to create your own keypair, replace
 * fw_public_key below with the output, and keep the private key offline.
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
