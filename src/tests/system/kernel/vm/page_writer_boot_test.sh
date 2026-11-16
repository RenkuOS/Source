#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Checks that file data reaches the disk without a sync: boots a copy of an
# arm64 image in QEMU, writes a file from a launch job, cuts the power
# WAIT seconds later and checks the file on the next boot. Before the page
# writer wrote back on timeout, the file came back zero-filled.
#
#   page_writer_boot_test.sh <haiku-mmc.image> <bfs_shell> [<UEFI firmware>]
#
# bfs_shell is built with the image (objects/<host>/.../tools/bfs_shell).
# Needs qemu-system-aarch64. Exits 0 on a pass.
set -uo pipefail

IMAGE="$1"
BFS_SHELL="$2"
BIOS="${3:-/usr/share/qemu-efi-aarch64/QEMU_EFI.fd}"
WAIT="${WAIT:-20}"
WORK="$(mktemp -d)"
trap 'kill -9 "${QEMU:-}" 2>/dev/null; rm -rf "${WORK}"' EXIT

cp "${IMAGE}" "${WORK}/disk.img" || exit 1

# The BFS partition is the MBR entry of type 0xeb.
offset=
for i in 0 1 2 3; do
	entry=$((446 + 16 * i))
	type=$(od -An -tu1 -j $((entry + 4)) -N1 "${WORK}/disk.img" | tr -d ' ')
	if [ "${type}" = 235 ]; then
		lba=$(od -An -tu4 -j $((entry + 8)) -N4 "${WORK}/disk.img" | tr -d ' ')
		offset=$((lba * 512))
	fi
done
[ -n "${offset}" ] || { echo "FAIL: no BFS partition in ${IMAGE}"; exit 1; }

# A system launch job, which runs without a desktop session.
cat > "${WORK}/job" <<'JOB'
job x-vnd.RenkuOS-page_writer_test {
	launch /bin/sh /boot/system/settings/page_writer_test.sh
	on initial_volumes_mounted
}
JOB
cat > "${WORK}/script" <<'SCRIPT'
f=/boot/home/page_writer_test
if [ -e $f ]; then
	if [ "$(cat $f)" = "page writer test" ]; then
		echo "page_writer_test: intact" > /dev/dprintf
	else
		echo "page_writer_test: lost" > /dev/dprintf
	fi
else
	echo "page writer test" > $f
	echo "page_writer_test: written" > /dev/dprintf
fi
SCRIPT
printf '%s\n' "mkdir -p /myfs/system/settings/launch" \
	"cp :${WORK}/job /myfs/system/settings/launch/page_writer_test" \
	"cp :${WORK}/script /myfs/system/settings/page_writer_test.sh" quit \
	| "${BFS_SHELL}" --start-offset "${offset}" "${WORK}/disk.img" > /dev/null \
	|| { echo "FAIL: could not install the test job"; exit 1; }

# boot <log>: starts QEMU on the copy, serial to <log>.
boot()
{
	: > "$1"
	qemu-system-aarch64 -M virt -cpu cortex-a72 -smp 2 -m 2048 \
		-bios "${BIOS}" -device ramfb \
		-drive file="${WORK}/disk.img",format=raw,if=none,id=d0 \
		-device virtio-blk-device,drive=d0 -display none -no-reboot \
		-serial file:"$1" &
	QEMU=$!
}

# wait_for <log> <regex>: waits up to 900 s for <regex> in <log>.
wait_for()
{
	for ((t = 0; t < 900; t += 5)); do
		grep -aqE "$2" "$1" && return 0
		if grep -aqE 'Kernel Debugging Land|^PANIC:' "$1"; then
			echo "FAIL: kernel panic"; return 1
		fi
		kill -0 "${QEMU}" 2>/dev/null || { echo "FAIL: QEMU exited"; return 1; }
		sleep 5
	done
	echo "FAIL: no \"$2\" in 900 s"; return 1
}

boot "${WORK}/serial1.log"
wait_for "${WORK}/serial1.log" 'page_writer_test: written' || exit 1
sleep "${WAIT}"
kill -9 "${QEMU}"; wait "${QEMU}" 2>/dev/null

boot "${WORK}/serial2.log"
wait_for "${WORK}/serial2.log" 'page_writer_test: (intact|lost|written)' \
	|| exit 1
kill -9 "${QEMU}"; wait "${QEMU}" 2>/dev/null
result=$(grep -aoE 'page_writer_test: [a-z]+' "${WORK}/serial2.log" | tail -1)
if [ "${result}" != "page_writer_test: intact" ]; then
	echo "FAIL: after power-off ${WAIT} s after the write: ${result}"
	exit 1
fi
echo "PASS"
