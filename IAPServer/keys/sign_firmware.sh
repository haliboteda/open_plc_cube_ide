#!/bin/sh
# Signs a compiled application .bin with the firmware-signing private key,
# producing the raw (size, sha256, r||s signature) tuple that the IAP
# transfer tool sends to the bootloader alongside the image, and that the
# bootloader stores in its state journal (see bootloader_state_save_metadata
# in IAPServer/bootloader_state.c) for boot-time verification.
#
# Usage: ./sign_firmware.sh <app.bin> <private-key.pem> [output-prefix] [--version=N]
#
# Writes:
#   <output-prefix>.sha256   - hex SHA-256 of app.bin
#   <output-prefix>.sig      - raw 64-byte r||s signature (binary)
#   <output-prefix>.size     - decimal file size in bytes
#   <output-prefix>.version  - decimal firmware version, only if --version=N given
#
# --version is optional: without it, no .version file is written and the IAP
# transfer tool flashes the image without a version number (same as before
# this flag existed -- the bootloader stores it as 0, no downgrade warning is
# possible for that image). Pass it to let the PC tool warn an operator
# before it pushes an older version over a newer one already installed.

set -eu

VERSION=""
BIN=""
KEY=""
OUT_ARG=""
for arg in "$@"; do
  case "$arg" in
    --version=*) VERSION="${arg#--version=}" ;;
    *)
      if [ -z "$BIN" ]; then BIN="$arg"
      elif [ -z "$KEY" ]; then KEY="$arg"
      else OUT_ARG="$arg"
      fi
      ;;
  esac
done

: "${BIN:?usage: sign_firmware.sh <app.bin> <private-key.pem> [output-prefix] [--version=N]}"
: "${KEY:?usage: sign_firmware.sh <app.bin> <private-key.pem> [output-prefix] [--version=N]}"
OUT="${OUT_ARG:-${BIN%.bin}}"

SHA256=$(openssl dgst -sha256 -binary "$BIN" | xxd -p | tr -d '\n')
echo "$SHA256" > "${OUT}.sha256"

SIZE=$(wc -c < "$BIN" | tr -d ' ')
echo "$SIZE" > "${OUT}.size"

# openssl produces a DER-encoded (r,s) signature; the bootloader/micro-ecc
# side expects raw r||s, each left-padded to 32 bytes (P-256 order size).
DERSIG=$(mktemp)
openssl dgst -sha256 -sign "$KEY" -out "$DERSIG" "$BIN"

R=$(openssl asn1parse -inform DER -in "$DERSIG" | awk -F'INTEGER *: *' '/INTEGER/{c++; if (c==1) print $2}')
S=$(openssl asn1parse -inform DER -in "$DERSIG" | awk -F'INTEGER *: *' '/INTEGER/{c++; if (c==2) print $2}')
rm -f "$DERSIG"

# left-pad each to exactly 64 hex chars (32 bytes); DER integers can be
# shorter (leading zero bytes stripped) or one byte longer (leading 0x00
# added to keep the value non-negative) -- normalize both cases.
pad_or_trim() {
  hex="$1"
  len=${#hex}
  if [ "$len" -gt 64 ]; then
    # strip leading 00 padding byte(s) added for ASN.1 sign-bit purposes
    hex=$(echo "$hex" | sed 's/^00*//')
    while [ "${#hex}" -lt 64 ]; do hex="0$hex"; done
  fi
  while [ "${#hex}" -lt 64 ]; do hex="0$hex"; done
  echo "$hex"
}

R=$(pad_or_trim "$R")
S=$(pad_or_trim "$S")

echo "${R}${S}" | xxd -r -p > "${OUT}.sig"

if [ -n "$VERSION" ]; then
  echo "$VERSION" > "${OUT}.version"
  echo "Wrote ${OUT}.sha256 ${OUT}.sig ${OUT}.size ${OUT}.version for $BIN"
  echo "sha256=$SHA256 size=$SIZE version=$VERSION"
else
  echo "Wrote ${OUT}.sha256 ${OUT}.sig ${OUT}.size for $BIN"
  echo "sha256=$SHA256 size=$SIZE"
fi
