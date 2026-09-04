#!/bin/sh
# Rotates the firmware signing keypair (ECDSA image trust): a new private key,
# the matching fw_pubkey.inc the bootloader compiles in, and the copy of the
# private key that ships beside IAPTool.
#
# Needs nothing but a POSIX shell and the IAPTool binary that already ships
# in the Arduino package -- no openssl, no bash, no Go toolchain.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
IAPSERVER=$(cd "$HERE/.." && pwd)

DRY_RUN=0
ASSUME_YES=0
RESTORE=""
LIST_BACKUPS=0
TOOLS_DIR=""
IAPTOOL=""

usage() {
	cat <<'EOF'
Usage: ./rotate_keys.sh [options]

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

KEY_TARGETS="$IAPSERVER/keys/fw_pubkey.inc"
PEM_TARGETS="$IAPSERVER/keys/fw_signing_key.pem"

if [ -n "$TOOLS_DIR" ]; then
	for plat in win linux macosx; do
		[ -d "$TOOLS_DIR/$plat" ] || continue
		PEM_TARGETS="$PEM_TARGETS
$TOOLS_DIR/$plat/keys/fw_signing_key.pem"
	done
fi

targets() {
	printf '%s\n' "$KEY_TARGETS" "$PEM_TARGETS"
	# The shipped placeholder key becomes a footgun once a real one exists.
	[ -f "$IAPSERVER/keys/fw_signing_key.TEST_ONLY.pem" ] &&
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

echo "IAP signing key rotation"
echo "  project    : $IAPSERVER"
echo "  iaptool    : $IAPTOOL"
echo
echo "Files that will be replaced:"
targets | while read -r f; do
	[ -n "$f" ] && printf '  %s %s\n' "$([ -f "$f" ] && echo '[backup]' || echo '[new]   ')" "$f"
done

[ "$DRY_RUN" = 1 ] && { echo; echo "--dry-run: nothing written."; exit 0; }

if [ "$ASSUME_YES" != 1 ]; then
	cat <<'EOF'

  Every board already in the field will stop responding to IAPTool until you
  rebuild the bootloader and re-flash it over ST-Link or DFU. IAP cannot
  update the bootloader itself.

  A board that has been claimed does NOT follow this key: it verifies against
  the owner in its own flash. Hand it over with "IAPTool setowner" instead.

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

cat <<EOF

Done. Now, in this order:

  1. Rebuild the bootloader (the public key is compiled in).
  2. Flash it over ST-Link or DFU -- IAP cannot update the bootloader.
  3. Rebuild and upload your sketch.

To undo:  ./rotate_keys.sh --restore=$STAMP
The backup holds the private key in the clear -- keep $BACKUP_ROOT out of git.
EOF
