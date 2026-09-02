FROM debian:bookworm-slim

# docker build --no-cache --tag ghcr.io/haiku/cross-compiler:x86_64 .
# docker push ghcr.io/haiku/cross-compiler:x86_64

RUN apt-get update && apt-get install -y --no-install-recommends \
  autoconf \
  automake \
  bison \
  bzip2 \
  ca-certificates \
  cmake \
  curl \
  file \
  flex \
  g++ \
  gawk \
  git \
  libcurl4-openssl-dev \
  libssl-dev \
  libzstd-dev \
  make \
  nasm \
  ninja-build \
  python3 \
  texinfo \
  vim \
  wget \
  xz-utils \
  zlib1g-dev; \
  if [ $(uname -m) = "x86_64" ]; then apt-get install -y g++-multilib; fi

# source revision to build
ARG BUILDTOOLS_REV=master
ARG HAIKU_REV=master
# architectures to build
ARG ARCHITECTURE=x86_64
ARG SECONDARY_ARCHITECTURE=
# source repositories to build from (default to upstream)
ARG HAIKU_REPO=https://review.haiku-os.org/haiku
ARG BUILDTOOLS_REPO=https://review.haiku-os.org/buildtools
# revision to use when the tree has no reachable hrev tag
ARG HAIKU_REVISION=0

# Build Haiku cross-compiler toolchain, and haiku package tool
WORKDIR /tmp
COPY build-toolchain.sh /tmp/
RUN chmod 755 /tmp/build-toolchain.sh
RUN /tmp/build-toolchain.sh "$BUILDTOOLS_REV" "$HAIKU_REV" "$ARCHITECTURE" \
  "$SECONDARY_ARCHITECTURE" "$HAIKU_REPO" "$BUILDTOOLS_REPO" "$HAIKU_REVISION"

WORKDIR /
