/*
 * fw_pubkey.c
 *
 * Rotate with keys/rotate_keys.sh.
 */

#include "fw_pubkey.h"

const uint8_t fw_public_key[64] = {
#include "keys/fw_pubkey.inc"
};
