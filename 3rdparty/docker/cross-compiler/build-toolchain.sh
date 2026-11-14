#!/bin/bash
# Copyright 2016 The Rust Project Developers. See the COPYRIGHT
# file at the top-level directory of this distribution and at
# http://rust-lang.org/COPYRIGHT.
#
# Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
# http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
# <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
# option. This file may not be copied, modified, or distributed
# except according to those terms.

set -ex

BUILDTOOLS_REV=$1
HAIKU_REV=$2
ARCH=$3
SECONDARY_ARCH=$4
# Repository URLs are parameters so a fork can build its own toolchain against
# its own tree. They default to upstream, so omitting them preserves the
# original behaviour exactly.
HAIKU_REPO=${5:-https://review.haiku-os.org/haiku}
BUILDTOOLS_REPO=${6:-https://review.haiku-os.org/buildtools}
# Revision to use when no hrev tag is reachable. Haiku's own
# determine_haiku_revision falls back to "0", so that is the default here too.
HAIKU_REVISION_PIN=${7:-0}

# Clone $1 at revision $2 into $3. The revision may be a branch, a tag, or a
# full commit SHA: `git clone --branch` accepts only the first two, so fall
# back to a full clone and a detached checkout for a SHA. Shallow stays the
# fast path because that is what a branch or tag pin will normally be.
fetch_repo() {
	_repo=$1
	_rev=$2
	_dir=$3
	if git clone --depth=1 --branch "$_rev" "$_repo" "$_dir" 2>/dev/null; then
		return 0
	fi
	echo "shallow clone of '$_rev' failed; assuming a commit and cloning fully"
	git clone "$_repo" "$_dir"
	git -C "$_dir" checkout --detach "$_rev"
}

TOP=$(pwd)

BUILDTOOLS=$TOP/buildtools
HAIKU=$TOP/haiku
OUTPUT=/tools
SYSROOT=$OUTPUT/cross-tools-$ARCH/sysroot
SYSROOT_SECONDARY=$OUTPUT/cross-tools-$SECONDARY_ARCH/sysroot
PACKAGE_ROOT=/system

# Get the source trees
fetch_repo "$HAIKU_REPO" "$HAIKU_REV" haiku
fetch_repo "$BUILDTOOLS_REPO" "$BUILDTOOLS_REV" buildtools

# The Haiku build requires the ability to find a hrev tag. In case a specific branch is selected
# (like `r1beta4`)`, we will get the entire history just to be sure that the tag will exist.
cd haiku
if [ ! "$(git describe --dirty --tags --match=hrev* --abbrev=1)" ]; then
	# --unshallow fails on a complete repository, which is what the SHA path in
	# fetch_repo leaves behind, so only use it when the clone really is shallow.
	if [ -f "$(git rev-parse --git-dir)/shallow" ]; then
		git fetch --unshallow
	fi
	git fetch --tags || true
fi

# The hrev tags live only on Haiku's Gerrit: the github.com/haiku/haiku mirror
# carries none of them, so neither does any fork made from it. Without a tag
# the build aborts with "you are using a Haiku clone without tags". Rather than
# require every fork to mirror 57k tags, set the revision explicitly -- which
# is what that error message asks for, and what FileRules honours by writing
# HAIKU_REVISION straight out instead of consulting git.
if [ ! "$(git describe --dirty --tags --match=hrev* --abbrev=1)" ]; then
	echo "no hrev tag reachable; using HAIKU_REVISION=$HAIKU_REVISION_PIN"
	export HAIKU_REVISION="$HAIKU_REVISION_PIN"
	echo "HAIKU_REVISION = \"$HAIKU_REVISION_PIN\" ;" > build/jam/UserBuildConfig
fi
cd "$TOP"

# Scale up cores to speed up, but don't go crazy since Jam starts
# to lose its mind at 8+
NCPU=$(nproc)
if [ $NCPU -gt 8 ]; then NCPU=8; fi

# Build a cross-compiler
cd $BUILDTOOLS/jam
make && ./jam0 install
mkdir -p $OUTPUT
cd $OUTPUT
configureArgs="--build-cross-tools $ARCH --cross-tools-source $TOP/buildtools"
if [ -n "$SECONDARY_ARCH" ]; then
	configureArgs="$configureArgs --build-cross-tools $SECONDARY_ARCH"
fi
$HAIKU/configure $configureArgs

# Set up sysroot to redirect to /system
mkdir -p $SYSROOT/boot
mkdir -p $PACKAGE_ROOT
ln -s $PACKAGE_ROOT $SYSROOT/boot/system
if [ -n "$SECONDARY_ARCH" ]; then
	mkdir -p $SYSROOT_SECONDARY/boot
	ln -s $PACKAGE_ROOT $SYSROOT_SECONDARY/boot/system
fi

# Build needed packages and tools for the cross-compiler
jam -j$NCPU -q haiku.hpkg haiku_devel.hpkg '<build>package'
if [ -n "$SECONDARY_ARCH" ]; then
	jam -j$NCPU -q haiku_${SECONDARY_ARCH}.hpkg haiku_${SECONDARY_ARCH}_devel.hpkg
fi

# Set up our sysroot
HOST_ARCH=$(uname -m)
case $HOST_ARCH in
	aarch64)
		HOST_ARCH=arm64
		;;
	*)
		;;
esac

cp $OUTPUT/objects/linux/lib/*.so /lib/$(uname -m)-linux-gnu
cp $OUTPUT/objects/linux/$HOST_ARCH/release/tools/package/package /bin/
for file in $SYSROOT/../bin/*; do
	ln -s $file /bin/$(basename $file)
done
#find $SYSROOT/../bin/ -type f -exec ln -s {} /bin/ \;
if [ -n "$SECONDARY_ARCH" ]; then
	for file in $SYSROOT_SECONDARY/../bin/*; do
		ln -s $file /bin/$(basename $file)-$SECONDARY_ARCH
	done
	#find $SYSROOT_SECONDARY/../bin/ -type f -exec ln -s {} /bin/{}-$SECONDARY_ARCH \;
fi

# Extract packages
package extract -C $PACKAGE_ROOT $OUTPUT/objects/haiku/$ARCH/packaging/packages/haiku.hpkg
package extract -C $PACKAGE_ROOT $OUTPUT/objects/haiku/$ARCH/packaging/packages/haiku_devel.hpkg
if [ -n "$SECONDARY_ARCH" ]; then
	package extract -C $PACKAGE_ROOT $OUTPUT/objects/haiku/$ARCH/packaging/packages/haiku_${SECONDARY_ARCH}.hpkg
	package extract -C $PACKAGE_ROOT $OUTPUT/objects/haiku/$ARCH/packaging/packages/haiku_${SECONDARY_ARCH}_devel.hpkg
fi
find $OUTPUT/download/ -name '*.hpkg' -exec package extract -C $PACKAGE_ROOT {} \;

# Clean up
rm -rf $BUILDTOOLS
rm -rf $HAIKU
rm -rf $OUTPUT/Jamfile $OUTPUT/attributes $OUTPUT/build $OUTPUT/build_packages $OUTPUT/download $OUTPUT/objects

if [ -n "$SECONDARY_ARCH" ]; then
	echo "Cross compilers for $ARCH-unknown-haiku and $SECONDARY_ARCH-unknown-haiku built and configured"
else
	echo "Cross compiler for $ARCH-unknown-haiku built and configured"
fi
