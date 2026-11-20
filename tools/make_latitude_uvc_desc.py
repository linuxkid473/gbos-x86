#!/usr/bin/env python3
"""Create a Latitude 7320 UVC descriptor matching the fixed loopback mode.

The shared descriptor's 640x480 YUY2 frame is kept as the only frame and resized to the
bridge's output (1920x1080 by default, --size for the CAMERA_SIZE builds; OUT_WIDTH/OUT_HEIGHT
in kernel/ipu6-loopback.c),
with its bit rates and frame buffer size recomputed for 30 fps.
"""

import argparse
import struct
from pathlib import Path

OUTPUT_SIZE = (1920, 1080)
FPS = 30


def descriptors(data: bytes, start: int, end: int):
    offset = start
    while offset < end:
        length = data[offset]
        if length == 0:
            offset += 1
            continue
        if length < 2 or offset + length > end:
            raise ValueError(f"invalid descriptor at offset {offset}")
        yield offset, data[offset : offset + length]
        offset += length


def generate(data: bytes, size=OUTPUT_SIZE) -> bytes:
    config_offset = 18
    if len(data) < config_offset + 9 or data[config_offset + 1] != 0x02:
        raise ValueError("missing USB configuration descriptor")

    config_length = struct.unpack_from("<H", data, config_offset + 2)[0]
    config_end = config_offset + config_length
    if config_length < 9 or config_end != len(data):
        raise ValueError("configuration length does not match descriptor file")

    parsed = list(descriptors(data, config_offset, config_end))
    frames = [
        (offset, desc)
        for offset, desc in parsed
        if desc[1] == 0x24 and len(desc) >= 26 and desc[2] == 0x05
    ]
    target_frames = [
        (offset, desc)
        for offset, desc in frames
        if struct.unpack_from("<HH", desc, 5) == (640, 480)
    ]
    formats = [
        (offset, desc)
        for offset, desc in parsed
        if desc[1] == 0x24 and len(desc) >= 5 and desc[2] == 0x04
    ]
    streaming_headers = [
        (offset, desc)
        for offset, desc in parsed
        if desc[1] == 0x24 and len(desc) == 14 and desc[2] == 0x01
    ]
    if len(target_frames) != 1 or len(frames) < 1:
        raise ValueError("expected exactly one 640x480 uncompressed frame")
    if len(formats) != 1 or len(streaming_headers) != 1:
        raise ValueError("expected one uncompressed format and one VS header")

    target_offset, _ = target_frames[0]
    removed_offsets = {offset for offset, _ in frames if offset != target_offset}
    format_offset = formats[0][0]
    header_offset = streaming_headers[0][0]

    output = bytearray(data[:config_offset])
    new_header_offset = None
    offset = config_offset
    while offset < config_end:
        length = data[offset]
        if length == 0:
            output.append(0)
            offset += 1
            continue

        desc = bytearray(data[offset : offset + length])
        if offset in removed_offsets:
            offset += length
            continue
        if offset == target_offset:
            desc[3] = 1
            width, height = size
            frame_bytes = width * height * 2  # YUY2
            # wWidth, wHeight, dwMinBitRate, dwMaxBitRate, dwMaxVideoFrameBufferSize
            struct.pack_into("<HHIII", desc, 5, width, height, frame_bytes * 8 * FPS,
                             frame_bytes * 8 * FPS, frame_bytes)
        if offset == format_offset:
            desc[4] = 1
        if offset == header_offset:
            new_header_offset = len(output)
        output.extend(desc)
        offset += length

    struct.pack_into("<H", output, config_offset + 2, len(output) - config_offset)
    if new_header_offset is None:
        raise ValueError("video streaming header was not retained")

    new_descriptors = list(descriptors(output, config_offset, len(output)))
    vs_total = 0
    in_streaming_interface = False
    for desc_offset, desc in new_descriptors:
        if desc_offset == new_header_offset:
            in_streaming_interface = True
        if not in_streaming_interface:
            continue
        if desc[1] == 0x05:
            break
        if desc[1] == 0x24:
            vs_total += len(desc)
    struct.pack_into("<H", output, new_header_offset + 4, vs_total)

    result_descs = list(descriptors(output, config_offset, len(output)))
    result_frames = [
        desc
        for _, desc in result_descs
        if desc[1] == 0x24 and len(desc) >= 26 and desc[2] == 0x05
    ]
    result_formats = [
        desc
        for _, desc in result_descs
        if desc[1] == 0x24 and len(desc) >= 5 and desc[2] == 0x04
    ]
    result_headers = [
        (offset, desc)
        for offset, desc in result_descs
        if desc[1] == 0x24 and len(desc) == 14 and desc[2] == 0x01
    ]
    if len(result_frames) != 1 or struct.unpack_from("<HH", result_frames[0], 5) != tuple(size):
        raise ValueError(f"generated descriptor does not expose only {size[0]}x{size[1]}")
    if len(result_formats) != 1 or len(result_headers) != 1:
        raise ValueError("generated format/header descriptors are inconsistent")
    if result_frames[0][3] != 1 or result_formats[0][4] != 1:
        raise ValueError("generated frame indices/count are inconsistent")
    if struct.unpack_from("<H", output, config_offset + 2)[0] != len(output) - config_offset:
        raise ValueError("generated configuration length is inconsistent")
    actual_vs_total = struct.unpack_from("<H", output, result_headers[0][0] + 4)[0]
    expected_vs_total = 0
    for _, desc in result_descs[result_descs.index(result_headers[0]) :]:
        if desc[1] == 0x05:
            break
        if desc[1] == 0x24:
            expected_vs_total += len(desc)
    if actual_vs_total != expected_vs_total:
        raise ValueError("generated video streaming length is inconsistent")
    return bytes(output)


def set_usb_id(data: bytes, usb_id: str) -> bytes:
    """Replace idVendor/idProduct (device descriptor bytes 8-11) with VVVV:PPPP (hex).

    Android's USB camera HAL looks cameras up by this id in
    /vendor/etc/camera/internal_usb_camera_info.txtpb (lens data and facing), so the rear
    camera needs its own id: the front keeps 0408:4052, listed there as FACING_FRONT.
    """
    vendor, sep, product = usb_id.partition(":")
    if not sep or len(vendor) != 4 or len(product) != 4:
        raise ValueError(f"USB id must be VVVV:PPPP, got {usb_id!r}")
    if len(data) < 18 or data[0] != 18 or data[1] != 0x01:
        raise ValueError("missing USB device descriptor")
    output = bytearray(data)
    struct.pack_into("<HH", output, 8, int(vendor, 16), int(product, 16))
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--usb-id", help="VVVV:PPPP to put in the device descriptor")
    parser.add_argument("--size", default="%dx%d" % OUTPUT_SIZE,
                        help="WxH of the single frame (the bridge's output size)")
    args = parser.parse_args()
    width, _, height = args.size.partition("x")
    result = generate(args.source.read_bytes(), (int(width), int(height)))
    if args.usb_id:
        result = set_usb_id(result, args.usb_id)
    args.output.write_bytes(result)


if __name__ == "__main__":
    main()
