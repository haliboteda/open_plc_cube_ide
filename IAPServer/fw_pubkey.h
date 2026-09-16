/*
 * fw_pubkey.h
 *
 * *** THE MATCHING PRIVATE KEY IS PUBLIC, BY DESIGN. ***
 * Anyone with this repository can sign an image a factory board accepts. That
 * is not a defect awaiting a rotation -- users sign their own PLC programs, so
 * the private key has to be on the user's machine. A board becomes defended
 * when it is claimed (IAPTool takeown) or when a customer compiles the board
 * package with their own root (keys/rotate_keys.sh). See
 * $PROD/docs/security/OWNERSHIP.md for the derivation.
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
