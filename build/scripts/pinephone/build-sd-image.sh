#!/usr/bin/env bash
# Copyright 2026, Ian Dumych, idumych@gmail.com.
# SPDX-License-Identifier: MIT
#
# Build a microSD image for the PinePhone (Allwinner A64) booted by Tow-Boot:
#
#   0-16 MiB  GPT, otherwise empty: no U-Boot on the card, so the A64 boot
#             ROM falls through to Tow-Boot in the eMMC boot partition
#   p1        ESP: EFI/BOOT/BOOTAA64.EFI (Haiku's loader); Tow-Boot passes
#             its own sun50i-a64-pinephone device tree
#   p2        Haiku's BFS volume
#
#   build-sd-image.sh <bfs image> <output image> [haiku_loader.efi]
#
# Needs sfdisk and mtools; no root. Flash the result with dd or Etcher.
set -euo pipefail

BFS_IMAGE=$1
OUTPUT=$2
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD=${PINEPHONE_BUILD:-$HERE/../../../generated.arm64}
LOADER=${3:-$BUILD/objects/haiku/arm64/release/system/boot/efi/haiku_loader.efi}
ESP_MIB=${ESP_MIB:-32}

for tool in sfdisk mformat mmd mcopy; do
	command -v $tool >/dev/null || { echo "missing $tool" >&2; exit 1; }
done
[[ -f $BFS_IMAGE && -f $LOADER ]] \
	|| { echo "missing $BFS_IMAGE or $LOADER" >&2; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pinephone-sd.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "ESP"
truncate -s "${ESP_MIB}M" "$WORK/esp.img"
mformat -i "$WORK/esp.img" -v HAIKU_ESP ::
mmd -i "$WORK/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$WORK/esp.img" "$LOADER" ::/EFI/BOOT/BOOTAA64.EFI

echo "Assembling $OUTPUT"
espStart=32768							# 16 MiB
espCount=$((ESP_MIB * 2048))
bfsStart=$((espStart + espCount))
bfsCount=$((($(stat -c %s "$BFS_IMAGE") + 511) / 512))
total=$(((bfsStart + bfsCount + 33 + 2047) / 2048 * 2048))
rm -f "$OUTPUT"
truncate -s $((total * 512)) "$OUTPUT"
sfdisk --quiet "$OUTPUT" <<EOF
label: gpt
start=$espStart, size=$espCount, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=esp
start=$bfsStart, size=$bfsCount, type=42465331-3BA3-10F1-802A-4861696B7521, name=Haiku
EOF
dd if="$WORK/esp.img" of="$OUTPUT" bs=512 seek=$espStart conv=notrunc,sparse \
	status=none
dd if="$BFS_IMAGE" of="$OUTPUT" bs=512 seek=$bfsStart conv=notrunc,sparse \
	status=none
sfdisk -l "$OUTPUT" | tail -3
echo "$OUTPUT: $(($(stat -c %s "$OUTPUT") / 1048576)) MiB"
