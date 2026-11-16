#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Boots a guest and watches its serial console for a clean boot.
#
#   boot-check.sh <serial log> <qemu command and arguments...>
#
# Appends "-serial file:<serial log>" to the QEMU command. Passes when
# "Haiku revision:" appears and the guest then runs SETTLE_SECONDS without
# printing a failure marker, and REQUIRE (a fixed string, optional) has
# appeared by then. Fails on a marker, a QEMU exit, or no banner within
# BANNER_TIMEOUT seconds.
set -uo pipefail

: "${BANNER_TIMEOUT:?}" "${SETTLE_SECONDS:?}"
REQUIRE="${REQUIRE:-}"
LOG="$1"; shift

: > "${LOG}"
"$@" -serial "file:${LOG}" &
QEMU=$!
# shellcheck disable=SC2064
trap "kill ${QEMU} 2>/dev/null || true" EXIT

fail_re='Kernel Debugging Land|^PANIC:|error starting "|main2: find_directory\(\) failed'
seen=0; at=0; t=0
while :; do
  if grep -qE "${fail_re}" "${LOG}" 2>/dev/null; then
    echo "::error::Guest failed:"; grep -nE "${fail_re}" "${LOG}" | head -20; exit 1
  fi
  if [ "${seen}" -eq 0 ] && grep -qF 'Haiku revision:' "${LOG}" 2>/dev/null; then
    seen=1; at=${t}; echo "banner at ${at}s"
  fi
  if [ "${seen}" -eq 1 ] && [ $(( t - at )) -ge "${SETTLE_SECONDS}" ]; then
    if [ -n "${REQUIRE}" ] && ! grep -qF "${REQUIRE}" "${LOG}"; then
      echo "::error::No \"${REQUIRE}\" by ${t}s."; tail -40 "${LOG}"; exit 1
    fi
    echo "booted cleanly"; exit 0
  fi
  if ! kill -0 "${QEMU}" 2>/dev/null; then
    echo "::error::QEMU exited after ${t}s."; tail -40 "${LOG}"; exit 1
  fi
  if [ "${seen}" -eq 0 ] && [ "${t}" -ge "${BANNER_TIMEOUT}" ]; then
    echo "::error::No banner within ${BANNER_TIMEOUT}s."; tail -40 "${LOG}"; exit 1
  fi
  sleep 5; t=$(( t + 5 ))
done
