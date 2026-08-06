#!/bin/sh
# Generates a NEW ECDSA P-256 (secp256r1) firmware-signing keypair and prints
# the C byte array to paste into IAPServer/fw_pubkey.c.
#
# Run this before building anything for real deployment. The placeholder
# key shipped in this repo (fw_signing_key.TEST_ONLY.pem) is public --
# anyone who has cloned this repo can sign a "valid" firmware image with it.
#
# The private key this script writes is YOURS. Do not commit it. Store it
# offline (password manager, HSM, offline machine -- whatever your release
# process can keep away from this repo and away from the network).
#
# Usage: ./generate_keys.sh [output-name-without-extension]
# Default output name: fw_signing_key

set -eu

NAME="${1:-fw_signing_key}"
PRIV="${NAME}.pem"
PUB="${NAME}.pub.pem"

if [ -e "$PRIV" ]; then
  echo "Refusing to overwrite existing $PRIV" >&2
  exit 1
fi

openssl ecparam -name prime256v1 -genkey -noout -out "$PRIV"
openssl ec -in "$PRIV" -pubout -out "$PUB"

echo
echo "Private key written to: $PRIV  -- KEEP THIS OFFLINE, DO NOT COMMIT IT"
echo "Public key written to:  $PUB"
echo
echo "Paste the following into IAPServer/fw_pubkey.c, replacing the placeholder array:"
echo

PUBHEX=$(openssl ec -in "$PRIV" -pubout -outform DER 2>/dev/null | tail -c 65 | xxd -p | tr -d '\n')
RAWHEX=${PUBHEX:2} # strip the leading 0x04 uncompressed-point marker

echo "const uint8_t fw_public_key[64] = {"
echo "$RAWHEX" | fold -w2 | awk '{
  printf "0x%s, ", $1;
  n++;
  if (n % 16 == 0) printf "\n";
}'
echo "};"
