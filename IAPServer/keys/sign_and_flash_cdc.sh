#!/bin/sh
# Re-signs a freshly-compiled app .bin and immediately pushes it to a device
# over CDC via IAPTool, so "recompile the Arduino sketch" -> "test on
# hardware" is one command instead of an easily-forgotten manual
# sign_firmware.sh step followed by a separate IAPTool invocation (see
# open_plc_cube_ide/IAPServer/keys/README.md for why signing is a separate
# step from the Arduino build in the first place).
#
# Usage: ./sign_and_flash_cdc.sh <app.bin> <COM_port> [key.pem] [--version=N]
#
# Defaults to the bench-only fw_signing_key.TEST_ONLY.pem -- pass a
# different key.pem for anything other than bench testing.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEFAULT_KEY="$SCRIPT_DIR/fw_signing_key.TEST_ONLY.pem"
IAPTOOL="${IAPTOOL_EXE:-$SCRIPT_DIR/../../../IAPTranfer_Tool/Output/windows/IAPTool.exe}"

BIN=""
PORT=""
KEY=""
VERSION_ARG=""
for arg in "$@"; do
  case "$arg" in
    --version=*) VERSION_ARG="$arg" ;;
    *)
      if [ -z "$BIN" ]; then BIN="$arg"
      elif [ -z "$PORT" ]; then PORT="$arg"
      elif [ -z "$KEY" ]; then KEY="$arg"
      fi
      ;;
  esac
done

: "${BIN:?usage: sign_and_flash_cdc.sh <app.bin> <COM_port> [key.pem] [--version=N]}"
: "${PORT:?usage: sign_and_flash_cdc.sh <app.bin> <COM_port> [key.pem] [--version=N]}"
KEY="${KEY:-$DEFAULT_KEY}"

if [ ! -x "$IAPTOOL" ]; then
  echo "IAPTool.exe not found at $IAPTOOL (build it with IAPTranfer_Tool/compile_tool.sh, or set IAPTOOL_EXE)" >&2
  exit 1
fi

echo "Signing $BIN with $KEY ..."
"$SCRIPT_DIR/sign_firmware.sh" "$BIN" "$KEY" ${VERSION_ARG:+"$VERSION_ARG"}

echo "Flashing $BIN over CDC on $PORT via IAPTool ..."
exec "$IAPTOOL" cdc "$BIN" "$PORT"
