#!/usr/bin/env bash
# Copyright 2026, Ian Dumych, idumych@gmail.com.
# SPDX-License-Identifier: MIT
#
# Pull /boot/system/var/log/syslog out of a copy of the card's Haiku
# partition, made on a Linux PC with e.g.
#
#   pkexec dd if=/dev/sdX2 of=card-haiku.img bs=4M
#
# The phone is usually switched off without unmounting, so bfs_shell
# replays the BFS journal: it works on a private copy, never on the card.
#
#   read-syslog.sh <partition image> [output file]
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD=${PINEPHONE_BUILD:-$HERE/../../../generated.arm64}
BFS_SHELL=${BFS_SHELL:-$BUILD/objects/linux/x86_64/release/tools/bfs_shell/bfs_shell}
IMAGE=$1
OUTPUT=${2:-syslog}

[[ -x $BFS_SHELL ]] || { echo "missing bfs_shell ($BFS_SHELL)" >&2; exit 1; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pinephone-log.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$IMAGE" "$WORK/partition.img"
rm -f "$OUTPUT"
echo "cp myfs/system/var/log/syslog :$(realpath -m "$OUTPUT")" \
	| "$BFS_SHELL" "$WORK/partition.img" >/dev/null
if [[ ! -f $OUTPUT ]]; then
	echo "no syslog on the card: userland stopped before syslog_daemon" \
		"wrote it" >&2
	exit 1
fi
echo "$OUTPUT: $(wc -l < "$OUTPUT") lines"
