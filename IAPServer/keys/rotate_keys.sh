#!/bin/sh
# Rotates the two IAP secrets: the shared fixed password (HMAC session auth)
# and the firmware signing keypair (ECDSA image trust).
#
# Needs nothing but a POSIX shell and the IAPTool binary that already ships
# in the Arduino package -- no openssl, no bash, no Go toolchain.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
IAPSERVER=$(cd "$HERE/.." && pwd)

PASSWORD=""
ROTATE_KEY=1
ROTATE_PW=1
DRY_RUN=0
ASSUME_YES=0
RESTORE=""
LIST_BACKUPS=0
CORE_DIR=""
TOOLS_DIR=""
IAPTOOL=""

usage() {
	cat <<'EOF'
Usage: ./rotate_keys.sh [options]

  --password=<text>    Use this password instead of a generated one.
  --password-only      Rotate the password, leave the signing key alone.
  --keep-signing-key   Same as --password-only.
  --key-only           Rotate the signing key, leave the password alone.
  --core=<dir>         Arduino core OpenPLC_IAP/src directory (auto-detected).
  --tools=<dir>        STM32Tools directory holding win/ linux/ macosx/ (auto-detected).
  --iaptool=<path>     IAPTool binary (auto-detected).
  --dry-run            Print what would change, write nothing.
  --yes                Skip the confirmation prompt.
  --list-backups       Show previous rotations and exit.
  --restore=<stamp>    Put a previous rotation's files back and exit.
EOF
}

die() { printf 'error: %s\n' "$1" >&2; exit 1; }

for arg in "$@"; do
	case "$arg" in
		--password=*)   PASSWORD="${arg#--password=}" ;;
		--password-only|--keep-signing-key) ROTATE_KEY=0 ;;
		--key-only)     ROTATE_PW=0 ;;
		--core=*)       CORE_DIR="${arg#--core=}" ;;
		--tools=*)      TOOLS_DIR="${arg#--tools=}" ;;
		--iaptool=*)    IAPTOOL="${arg#--iaptool=}" ;;
		--dry-run)      DRY_RUN=1 ;;
		--yes|-y)       ASSUME_YES=1 ;;
		--list-backups) LIST_BACKUPS=1 ;;
		--restore=*)    RESTORE="${arg#--restore=}" ;;
		-h|--help)      usage; exit 0 ;;
		*)              usage; die "unknown option $arg" ;;
	esac
done

[ "$ROTATE_PW" = 1 ] || [ "$ROTATE_KEY" = 1 ] || die "--password-only and --key-only are mutually exclusive"

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*|Windows*) PLATFORM=win;    EXESUF=.exe ;;
	Darwin)                        PLATFORM=macosx; EXESUF= ;;
	*)                             PLATFORM=linux;  EXESUF= ;;
esac

# --- locate the Arduino package -------------------------------------------

arduino_bases() {
	[ -n "${LOCALAPPDATA:-}" ] && printf '%s\n' "$LOCALAPPDATA/Arduino15"
	printf '%s\n' "$HOME/AppData/Local/Arduino15" "$HOME/Library/Arduino15" "$HOME/.arduino15"
}

if [ -z "$CORE_DIR" ]; then
	for base in $(arduino_bases); do
		found=$(ls -d "$base"/packages/OpenPLC_Alpha/hardware/stm32/*/libraries/OpenPLC_IAP/src 2>/dev/null | tail -1 || true)
		[ -n "$found" ] && { CORE_DIR="$found"; break; }
	done
fi

if [ -z "$TOOLS_DIR" ]; then
	for base in $(arduino_bases); do
		found=$(ls -d "$base"/packages/OpenPLC_Alpha/tools/STM32Tools/* 2>/dev/null | tail -1 || true)
		[ -n "$found" ] && { TOOLS_DIR="$found"; break; }
	done
fi

if [ -z "$IAPTOOL" ]; then
	[ -n "$TOOLS_DIR" ] && [ -x "$TOOLS_DIR/$PLATFORM/IAPTool$EXESUF" ] && IAPTOOL="$TOOLS_DIR/$PLATFORM/IAPTool$EXESUF"
fi
[ -n "$IAPTOOL" ] && [ -x "$IAPTOOL" ] || die "IAPTool not found -- pass --iaptool=<path>"

# --- the files a rotation touches -----------------------------------------

PW_TARGETS="$IAPSERVER/keys/iap_fixed_password.txt"
KEY_TARGETS="$IAPSERVER/keys/fw_pubkey.inc"
PEM_TARGETS="$IAPSERVER/keys/fw_signing_key.pem"

[ -n "$CORE_DIR" ] && PW_TARGETS="$PW_TARGETS
$CORE_DIR/keys/iap_fixed_password.txt"

if [ -n "$TOOLS_DIR" ]; then
	for plat in win linux macosx; do
		[ -d "$TOOLS_DIR/$plat" ] || continue
		PW_TARGETS="$PW_TARGETS
$TOOLS_DIR/$plat/keys/iap_fixed_password.txt"
		PEM_TARGETS="$PEM_TARGETS
$TOOLS_DIR/$plat/keys/fw_signing_key.pem"
	done
fi

targets() {
	[ "$ROTATE_PW" = 1 ] && printf '%s\n' "$PW_TARGETS"
	[ "$ROTATE_KEY" = 1 ] && printf '%s\n' "$KEY_TARGETS" "$PEM_TARGETS"
	# The shipped placeholder key becomes a footgun once a real one exists.
	[ "$ROTATE_KEY" = 1 ] && [ -f "$IAPSERVER/keys/fw_signing_key.TEST_ONLY.pem" ] &&
		printf '%s\n' "$IAPSERVER/keys/fw_signing_key.TEST_ONLY.pem"
	true
}

# --- backup / restore ------------------------------------------------------

BACKUP_ROOT="$HERE/backup"

if [ "$LIST_BACKUPS" = 1 ]; then
	[ -d "$BACKUP_ROOT" ] || { echo "no backups yet"; exit 0; }
	for d in "$BACKUP_ROOT"/*/; do
		[ -f "$d/manifest.txt" ] || continue
		printf '%s  (%s files)\n' "$(basename "$d")" "$(wc -l < "$d/manifest.txt" | tr -d ' ')"
	done
	exit 0
fi

if [ -n "$RESTORE" ]; then
	SNAP="$BACKUP_ROOT/$RESTORE"
	[ -f "$SNAP/manifest.txt" ] || die "no backup $RESTORE (try --list-backups)"
	while IFS="	" read -r stored original; do
		[ -n "$stored" ] || continue
		if [ "$stored" = "-" ]; then
			# Created by that rotation; leaving it behind would orphan a
			# private key that no longer matches fw_pubkey.inc.
			rm -f "$original"
			printf 'removed  %s\n' "$original"
		else
			mkdir -p "$(dirname "$original")"
			cp "$SNAP/$stored" "$original"
			printf 'restored %s\n' "$original"
		fi
	done < "$SNAP/manifest.txt"
	echo
	echo "Rebuild and re-flash the bootloader for this to take effect on a board."
	exit 0
fi

# --- plan ------------------------------------------------------------------

echo "IAP key rotation"
echo "  project    : $IAPSERVER"
echo "  arduino    : ${CORE_DIR:-<not found - app will keep the old password>}"
echo "  iaptool    : $IAPTOOL"
echo "  password   : $([ "$ROTATE_PW" = 1 ] && echo "rotate" || echo "keep")"
echo "  signing key: $([ "$ROTATE_KEY" = 1 ] && echo "rotate" || echo "keep")"
echo
echo "Files that will be replaced:"
targets | while read -r f; do
	[ -n "$f" ] && printf '  %s %s\n' "$([ -f "$f" ] && echo '[backup]' || echo '[new]   ')" "$f"
done

[ "$DRY_RUN" = 1 ] && { echo; echo "--dry-run: nothing written."; exit 0; }

if [ -z "$CORE_DIR" ] && [ "$ROTATE_PW" = 1 ]; then
	echo
	echo "WARNING: no Arduino core found. The app half of the password will NOT be"
	echo "         updated, and authenticated reboot will stop working. Pass --core=<dir>."
fi

if [ "$ASSUME_YES" != 1 ]; then
	cat <<'EOF'

  Every board already in the field will stop responding to IAPTool until you
  rebuild the bootloader and re-flash it over ST-Link or DFU. IAP cannot
  update the bootloader itself.

EOF
	printf 'Type yes to continue: '
	read -r answer
	[ "$answer" = "yes" ] || die "aborted"
fi

# --- generate --------------------------------------------------------------

STAMP=$(date +%Y%m%d-%H%M%S)
SNAP="$BACKUP_ROOT/$STAMP"
mkdir -p "$SNAP"
: > "$SNAP/manifest.txt"

n=0
targets | while read -r f; do
	[ -n "$f" ] || continue
	if [ -f "$f" ]; then
		n=$((n + 1))
		stored="$n-$(basename "$f")"
		cp "$f" "$SNAP/$stored"
		cp "$f" "$f.bak"
	else
		stored="-"
	fi
	printf '%s\t%s\n' "$stored" "$f" >> "$SNAP/manifest.txt"
done
echo "Backed up to $SNAP (and .bak beside each file)"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ "$ROTATE_PW" = 1 ]; then
	if [ -n "$PASSWORD" ]; then
		case "$PASSWORD" in
			*'"'*|*'\'*) die 'password must not contain " or \' ;;
		esac
		cat > "$WORK/iap_fixed_password.txt" <<EOF
/* IAP fixed password. Shared by the bootloader, the Arduino app and IAPTool.
 * Rotate with IAPServer/keys/rotate_keys.sh -- do not hand-edit. Keep the
 * quotes: this file is #included directly as a C string literal. */
"$PASSWORD"
EOF
	else
		"$IAPTOOL" genpw > "$WORK/iap_fixed_password.txt"
	fi
	printf '%s\n' "$PW_TARGETS" | while read -r f; do
		[ -n "$f" ] || continue
		mkdir -p "$(dirname "$f")"
		cp "$WORK/iap_fixed_password.txt" "$f"
		printf 'wrote %s\n' "$f"
	done
fi

if [ "$ROTATE_KEY" = 1 ]; then
	( cd "$WORK" && "$IAPTOOL" genkey fw_signing_key > fw_pubkey.inc )
	cp "$WORK/fw_pubkey.inc" "$IAPSERVER/keys/fw_pubkey.inc"
	printf 'wrote %s\n' "$IAPSERVER/keys/fw_pubkey.inc"
	printf '%s\n' "$PEM_TARGETS" | while read -r f; do
		[ -n "$f" ] || continue
		mkdir -p "$(dirname "$f")"
		cp "$WORK/fw_signing_key.pem" "$f"
		chmod 600 "$f" 2>/dev/null || true
		printf 'wrote %s\n' "$f"
	done
	rm -f "$IAPSERVER/keys/fw_signing_key.TEST_ONLY.pem"
fi

cat <<EOF

Done. Now, in this order:

  1. Rebuild the bootloader (the password and public key are compiled in).
  2. Flash it over ST-Link or DFU -- IAP cannot update the bootloader.
  3. Rebuild and upload your sketch.

To undo:  ./rotate_keys.sh --restore=$STAMP
The backup holds the private key in the clear -- keep $BACKUP_ROOT out of git.
EOF
