FROM --platform=linux/amd64 debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    make flex bison bc m4 libssl-dev libelf-dev pkg-config python3 cpio kmod git perl rsync \
    xz-utils lz4 zstd ca-certificates curl file binutils gcc libc6-dev dwarves patch \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
