#!/bin/bash
# Builds the .eap on a Linux machine without Docker.
# Unpacks the Axis ACAP Native SDK image once into a cache folder, then builds
# in a chroot of it. Run as root from the repo root:
#   tools/build-lab.sh [aarch64]     (ARTPEC-8 and newer only: aarch64)
# Output: the .eap files in the current folder.
set -euo pipefail

VERSION=12.11.0
UBUNTU=24.04
IMAGE=axisecp/acap-native-sdk
CACHE_ROOT=${ACAP_SDK_CACHE:-/tmp/acap-sdk}
REPO=$(cd "$(dirname "$0")/.." && pwd)
ARCHES=("$@")
[ ${#ARCHES[@]} -eq 0 ] && ARCHES=(aarch64)
for A in "${ARCHES[@]}"; do [ "$A" = aarch64 ] || { echo "Only aarch64 (ARTPEC-8 and newer) is built; $A is not." >&2; exit 2; }; done

for ARCH in "${ARCHES[@]}"; do
	TAG="$VERSION-$ARCH-ubuntu$UBUNTU"
	SDK="$CACHE_ROOT/$TAG"

	# One unpack per SDK version; a lock keeps two builds from unpacking at once
	mkdir -p "$CACHE_ROOT"
	(
		flock 9
		if [ ! -f "$SDK/.complete" ]; then
			echo "== Unpacking $IMAGE:$TAG into $SDK"
			rm -rf "$SDK"
			python3 "$REPO/tools/fetch_sdk.py" "$IMAGE" "$TAG" "$SDK"
			touch "$SDK/.complete"
		fi
	) 9>"$CACHE_ROOT/.lock-$TAG"

	WORK="$SDK/opt/build-$$"
	rm -rf "$WORK"
	mkdir -p "$WORK"
	cp -r "$REPO/app/." "$WORK/"
	# Windows checkouts may carry CRLF line ends
	find "$WORK" -type f \( -name '*.c' -o -name '*.h' -o -name 'Makefile' -o -name '*.json' -o -name '*.html' -o -name '*.js' -o -name '*.css' \) -exec sed -i 's/\r$//' {} +
	# Stamp the app version on the page's script and style links, so browsers fetch new ones after an update
	APPVER=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$WORK/manifest.json" | head -1)
	sed -i "s/?v=[^\"]*\"/?v=$APPVER\"/g" "$WORK/html/index.html"

	mount -t proc proc "$SDK/proc"
	trap 'umount "$SDK/proc" 2>/dev/null || true; rm -rf "$WORK"' EXIT
	echo "== Building $ARCH"
	chroot "$SDK" /bin/bash -c "
		set -e
		. /.image-env
		cd /opt/build-$$
		. /opt/axis/acapsdk/environment-setup*
		acap-build . -a 'settings/settings.json' -a 'settings/events.json'
	"
	umount "$SDK/proc"
	# AXIS OS 13 refuses to start an app whose binary has an executable stack (GNU_STACK RWE): fail the build instead
	command -v readelf >/dev/null || { echo "readelf is missing: cannot check for an executable stack" >&2; exit 3; }
	for EAP in "$WORK"/*.eap; do
		CHK=$(mktemp -d)
		tar xzf "$EAP" -C "$CHK"
		BAD=$(find "$CHK" -type f -exec sh -c 'readelf -lW "$1" 2>/dev/null | grep -q "GNU_STACK.*RWE" && echo "${1#$2/}"' _ {} "$CHK" \;)
		rm -rf "$CHK"
		if [ -n "$BAD" ]; then echo "Executable stack (AXIS OS 13 will not start it) in $(basename "$EAP"): $BAD" >&2; exit 4; fi
		echo "== No executable stack in $(basename "$EAP")"
	done
	cp "$WORK"/*.eap "$PWD/"
	rm -rf "$WORK"
	trap - EXIT
done
ls -l "$PWD"/*.eap
