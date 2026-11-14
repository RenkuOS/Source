#!/bin/bash
# SPDX-License-Identifier: MIT
# Usage: arm64-pins.sh <download-dir> > build/jam/repositories/HaikuPorts/arm64
# Prints the arm64 HaikuPorts repository definition for the packages in
# <download-dir>, which holds the packages/ of the RenkuOSPorts arm64-packages
# release. The release keeps only the newest revision of each package, so the
# nightly writes this file from it before building, with --no-downloads. The
# copy in the tree is a snapshot of the release.
#
# The URL is where the repository is served. jam does not download from it with
# --no-downloads.
set -e
dir=$1
[ -d "$dir" ] || { echo "usage: $0 <download-dir>" >&2; exit 1; }
url=${ARM64_REPO_URL:-https://renkuos.freemyip.com/repo/renkuosports/arm64}

# Package files are <name>-<version>-<revision>-<arch>.hpkg.
list() {
	(cd "$dir" && ls -- *-"$1".hpkg 2> /dev/null) | sed "s/-$1\.hpkg\$//" | LC_ALL=C sort
}
any=$(list any) arm64=$(list arm64)
[ -n "$arm64" ] || { echo "no arm64 packages in $dir" >&2; exit 1; }

echo "RemotePackageRepository HaikuPorts"
echo "	: arm64"
echo "	: $url"
echo "	:"
echo "	# architecture \"any\" packages"
[ -z "$any" ] || echo "$any" | sed 's/^/	/'
echo "	:"
echo "	# repository architecture packages"
echo "	# primary architecture (arm64)"
echo "$arm64" | sed 's/^/	/'
echo "	:"
# A source package is listed by the name of its port, and jam takes its version
# from that port's entry above.
echo "	# source packages"
list source | sed 's/_source-.*$//' | sed 's/^/	/'
echo "	:"
echo "	# debuginfo packages"
echo "	;"
