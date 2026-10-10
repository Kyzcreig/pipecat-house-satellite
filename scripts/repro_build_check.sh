#!/usr/bin/env bash
# repro_build_check.sh: build the xiao-esp32-s3 image TWICE from one commit and
# require byte-identical src.bin (t_1d4f0294, Fleet v2 IMAGE-SHA).
#
# Each build gets its own copy of the tree at a different absolute path (and
# path LENGTH), runs at a different time, in a fresh container from the pinned
# toolchain image. A path, __DATE__/__TIME__ or ordering leak changes the bytes.
#
#   scripts/repro_build_check.sh [OUT_DIR]
#
# Env: IDF_IMAGE (pinned espressif/idf digest), PIPECAT_BUILD_GIT_SHA (default:
# git rev-parse HEAD). The build env is the fleet-image config (golden flags,
# no baked satellite id). Exit 0 = identical, 1 = differ, 2 = a build failed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/repro-out}"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.4@sha256:b9f2d6ea1c19e0c9f7959bdb74a9e3c775642f9d0f3b841937c5fa3363db892b}"
GIT_SHA="${PIPECAT_BUILD_GIT_SHA:-$(git -C "$ROOT" rev-parse HEAD)}"
mkdir -p "$OUT"

# Same edit the Build workflow applies: PlatformIO's framework-espidf 5.5.4
# (production) comments -Wall/-Werror/-Wextra out of build.cmake.
IDF_EDIT='sed -i -E "s/^( *)(\"-Wall\"|\"-Werror\"|\"-Wextra\")$/\1# \2/" $IDF_PATH/tools/cmake/build.cmake &&
  test $(grep -cE "^ *# \"-(Wall|Werror|Wextra)\"$" $IDF_PATH/tools/cmake/build.cmake) -eq 3'

build_one() {
  local name="$1" dir="$2"
  rm -rf "$dir"
  mkdir -p "$dir"
  # The tree as committed (submodules included), never a previous build dir.
  (cd "$ROOT" && tar --exclude='./repro-out' --exclude='*/build' --exclude='*/managed_components' \
     --exclude='*/sdkconfig' --exclude='*/sdkconfig.old' --exclude='./.git' -cf - .) | tar -C "$dir" -xf -
  echo "== build $name in $dir ($(date -u +%H:%M:%S))"
  docker run --rm -v "$dir:$dir" -w "$dir/xiao-esp32-s3" -u 0 \
    -e HOME=/tmp -e WIFI_SSID=A -e WIFI_PASSWORD=B \
    -e PIPECAT_SMALLWEBRTC_URL=http://ci.invalid:7870/api/offer \
    -e PIPECAT_DUAL_STREAM=1 -e PIPECAT_REDIAL=1 -e PIPECAT_WIFI_FAST_REJOIN=1 \
    -e PIPECAT_BUILD_GIT_SHA="$GIT_SHA" \
    "$IDF_IMAGE" /bin/bash -c "$IDF_EDIT && idf.py set-target esp32s3 >/dev/null && idf.py build >/dev/null" \
    || { echo "build $name FAILED"; return 2; }
  cp "$dir/xiao-esp32-s3/build/src.bin" "$OUT/src-$name.bin"
  cp "$dir/xiao-esp32-s3/build/src.elf" "$OUT/src-$name.elf"
}

build_one a /tmp/repro-a || exit 2
sleep 2   # a second boundary: any time leak is guaranteed to differ
build_one b /tmp/repro-build-bbbbbbbbbbbbbbbbbb || exit 2

sha_a=$(sha256sum "$OUT/src-a.bin" | cut -d' ' -f1)
sha_b=$(sha256sum "$OUT/src-b.bin" | cut -d' ' -f1)
echo "REPRO commit=$GIT_SHA image=$IDF_IMAGE"
echo "REPRO src.bin a=$sha_a b=$sha_b"
if [ "$sha_a" = "$sha_b" ]; then
  echo "REPRO PASS sha256=$sha_a"
  exit 0
fi
echo "REPRO FAIL: the same commit built twice gave two images"
echo "differing bytes: $(cmp -l "$OUT/src-a.bin" "$OUT/src-b.bin" | wc -l) (sizes $(stat -c%s "$OUT/src-a.bin") vs $(stat -c%s "$OUT/src-b.bin"))"
echo "strings only in a (paths/dates usually):"
diff <(strings -n 6 "$OUT/src-a.bin" | sort -u) <(strings -n 6 "$OUT/src-b.bin" | sort -u) | head -40 || true
exit 1
