#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BRIDGE=$ROOT/usb/installer/bin/ipu6-camera-bridge.sh

sh -n "$BRIDGE"
grep -Fq 'LOOPBACK_LABEL=GBOS_IPU6_Camera' "$BRIDGE"
grep -Fq '"card_label=$LOOPBACK_LABEL"' "$BRIDGE"
grep -Fq 'video_nr=64 "card_label=$LOOPBACK_LABEL" exclusive_caps=1' "$BRIDGE"
grep -Fq 'if [ "$ACTUAL_LABEL" != "$LOOPBACK_LABEL" ]; then' "$BRIDGE"
grep -Fq 'ERROR: loopback label mismatch:' "$BRIDGE"
grep -Fq 'ERROR: /dev/video64 was not created after loading v4l2loopback' "$BRIDGE"
if grep -Fq 'card_label="GBOS IPU6 Camera"' "$BRIDGE"; then
    echo "loopback card label contains spaces and will be split by module option parsing" >&2
    exit 1
fi

LOOPBACK_LABEL=GBOS_IPU6_Camera
set -- video_nr=64 "card_label=$LOOPBACK_LABEL" exclusive_caps=1
[ "$#" -eq 3 ]
[ "$2" = card_label=GBOS_IPU6_Camera ]

echo IPU6_CAMERA_BRIDGE_TEST_PASS
