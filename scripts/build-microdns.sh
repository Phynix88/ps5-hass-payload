#!/usr/bin/env bash
# Build libmicrodns for the PS5 exactly as pinned here:
# commit, pacbrew-repo patches with checksums, static library.
#   scripts/build-microdns.sh <target dir>      (needs PS5_PAYLOAD_SDK, meson, ninja)
set -euo pipefail
DEST=${1:?usage: build-microdns.sh <target dir>}
COMMIT=02ad4cbd350123130e3a854a2a0104cbd194adb1
PATCHES=(
  "add-SO_REUSEPORT-for-multicast-compatibility.patch 9ebb9595285af04493b7ffa613908dd37d510dcca7a2e3bd7caa513b8f4077b3"
  "avoid-using-MCAST_JOIN_GROUP-when-possible.patch df7b6e071926570d43401a532e679347fcb73a30c00f8143cb0c405d147cca48"
)
PATCH_URL=https://raw.githubusercontent.com/ps5-payload-dev/pacbrew-repo/master/libmicrodns
: "${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK is not set}"

if [ -f "$DEST/libmicrodns/builddir/src/libmicrodns.a" ] && [ -f "$DEST/.done-$COMMIT" ]; then
  echo "microdns already built in $DEST"
  exit 0
fi
rm -rf "$DEST"
mkdir -p "$DEST"
cd "$DEST"
git clone -q https://github.com/videolabs/libmicrodns.git
git -C libmicrodns checkout -q "$COMMIT"
for entry in "${PATCHES[@]}"; do
  read -r name sum <<<"$entry"
  curl -sfL -o "$name" "$PATCH_URL/$name"
  echo "$sum  $name" | sha256sum -c --quiet
  patch -s -d libmicrodns -p1 <"$name"
done
# shellcheck disable=SC1091
source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
cd libmicrodns
"$MESON" setup builddir -Ddefault_library=static -Dtests=disabled -Dexamples=disabled >/dev/null
"$MESON" compile -C builddir >/dev/null
touch "../.done-$COMMIT"
echo "microdns $COMMIT built in $DEST"
