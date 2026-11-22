#!/usr/bin/env bash
# Copyright 2026, Ian Dumych, idumych@gmail.com.
# SPDX-License-Identifier: MIT
#
# QEMU smoke test of a PinePhone build before a hardware iteration: packs the
# BFS image into the microSD layout (build-sd-image.sh) and boots it on QEMU's
# virt machine with what is closest to the A64: Cortex-A53, a GICv2 and an
# EFI firmware that hands over a device tree (no ACPI) and loads
# EFI/BOOT/BOOTAA64.EFI, as Tow-Boot does. It does not exercise any A64
# peripheral.
#
#   qemu-smoke.sh [bfs image] [seconds]
#
# Passes when the serial log shows userland starting.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD=${PINEPHONE_BUILD:-$HERE/../../../generated.arm64}
IMAGE=${1:-$BUILD/haiku-minimum.image}
SECONDS_TO_RUN=${2:-180}
QEMU=${QEMU:-qemu-system-aarch64}
FIRMWARE=${QEMU_EFI:-/usr/share/edk2/aarch64/QEMU_EFI.fd}
MARKER=${MARKER:-'launch_daemon|app_server|net_server'}
LOG=${LOG:-$BUILD/pinephone-qemu-smoke.log}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pinephone-qemu.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
"$HERE/build-sd-image.sh" "$IMAGE" "$WORK/sd.img" >/dev/null
"$QEMU" -M virt,gic-version=2,acpi=off -cpu cortex-a53 -smp 4 -m 2048 \
	-bios "$FIRMWARE" \
	-drive if=none,file="$WORK/sd.img",format=raw,id=hd0 \
	-device virtio-blk-device,drive=hd0 -nographic -serial mon:stdio \
	</dev/null > "$LOG" 2>&1 &
pid=$!
deadline=$((SECONDS + SECONDS_TO_RUN))
result=1
while (( SECONDS < deadline )) && kill -0 $pid 2>/dev/null; do
	if grep -a -q -E "$MARKER" "$LOG"; then
		result=0
		break
	fi
	if grep -a -q -E "PANIC|Kernel Debugging Land" "$LOG"; then
		break
	fi
	sleep 2
done
kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true
if (( result == 0 )); then
	echo "QEMU smoke test passed ($(grep -a -m1 -o -E "$MARKER" "$LOG"))"
else
	echo "QEMU smoke test FAILED, see $LOG" >&2
	grep -a -E "PANIC|panic" "$LOG" | head -5 >&2 || true
fi
exit $result
