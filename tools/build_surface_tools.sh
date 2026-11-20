#!/bin/sh
# Build the static helper binaries of the Surface Laptop 7 support in an amd64 Debian container:
#   usb/surface/iptsd, usb/surface/iptsd-check-device   touchpad daemon: github.com/alex-lentz/iptsd (fork of linux-surface/iptsd with
#       Surface Laptop 7 support) at a pinned commit, plus tools/iptsd/0004-... (Touchpad LiftGraceMs, by nierdal)
#   usb/surface/surface-webcam                          userspace bridge daemon for Lunar Lake IPU7 raw sensor -> v4l2loopback
#   usb/audio/tinymix                                   tinyalsa's mixer tool at a pinned commit, for the sound diagnostics (mixdiag.sh)
# Needs docker and network access (github.com, the Debian mirrors; a few hundred MB). Nothing is installed on the host. The results are
# not committed (.gitignore): run this before tools/make_gbos86_image.sh, or attach them to a release. Everything is linked statically,
# so the binaries run in Android's userspace on the stick.
#   tools/build_surface_tools.sh
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$REPO/usb/surface" "$REPO/usb/audio"
docker run --rm -i --platform linux/amd64 -v "$REPO:/repo" -e HOSTUID=$(id -u) -e HOSTGID=$(id -g) debian:trixie sh -s <<'INNER'
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends g++ cmake meson ninja-build pkg-config libeigen3-dev libcli11-dev libmsgsl-dev git ca-certificates patch binutils file >/dev/null
W=/tmp/iw; rm -rf $W; mkdir -p $W; cd $W; P=$W/prefix
git clone -q --depth 1 -b 10.2.1 https://github.com/fmtlib/fmt
git clone -q --depth 1 -b v1.14.1 https://github.com/gabime/spdlog
cmake -S fmt -B fmt/b -DCMAKE_BUILD_TYPE=Release -DFMT_TEST=OFF -DFMT_DOC=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=$P >/dev/null
cmake --build fmt/b -j8 >/dev/null; cmake --install fmt/b >/dev/null
cmake -S spdlog -B spdlog/b -DCMAKE_BUILD_TYPE=Release -DSPDLOG_BUILD_TESTS=OFF -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_FMT_EXTERNAL=ON -DCMAKE_PREFIX_PATH=$P -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=$P >/dev/null
cmake --build spdlog/b -j8 >/dev/null; cmake --install spdlog/b >/dev/null
mkdir iptsd; cd iptsd; git init -q; git remote add origin https://github.com/alex-lentz/iptsd
git fetch -q --depth 1 origin 3663e96e758145801c3cb1ce7c72f362d0d4a5f5
git checkout -q FETCH_HEAD
git apply /repo/tools/iptsd/0004-iptsd-add-Touchpad-LiftGraceMs-option.patch
PKG_CONFIG_PATH=$P/lib/pkgconfig meson setup build -Dwerror=false -Db_lto=false -Ddebug=false -Doptimization=2 -Ddebug_tools= -Dservice_manager= -Dsample_config=false -Dcpp_link_args=-static
ninja -C build src/iptsd src/iptsd-check-device
strip build/src/iptsd build/src/iptsd-check-device
cp build/src/iptsd build/src/iptsd-check-device /repo/usb/surface/
# tinymix (tinyalsa), static, for audio/mixdiag.sh
cd $W
mkdir tinyalsa; cd tinyalsa; git init -q; git remote add origin https://github.com/tinyalsa/tinyalsa
git fetch -q --depth 1 origin bbdba1e208f402c5b7d5cb3d42307a3db9f6bb1c
git checkout -q FETCH_HEAD
gcc -O2 -static -Iinclude -o /repo/usb/audio/tinymix utils/tinymix.c src/*.c
strip /repo/usb/audio/tinymix
# surface-webcam, static, for Surface Laptop 7 IPU7 webcam bridge
gcc -O3 -static -o /repo/usb/surface/surface-webcam /repo/kernel/surface-webcam.c -lm
strip /repo/usb/surface/surface-webcam
chown $HOSTUID:$HOSTGID /repo/usb/surface/iptsd /repo/usb/surface/iptsd-check-device /repo/usb/surface/surface-webcam /repo/usb/audio/tinymix
ls -la /repo/usb/surface/iptsd /repo/usb/surface/iptsd-check-device /repo/usb/surface/surface-webcam /repo/usb/audio/tinymix
file /repo/usb/surface/iptsd /repo/usb/surface/surface-webcam /repo/usb/audio/tinymix | cut -c1-140
echo BUILD_SURFACE_TOOLS_DONE
INNER
