#!/usr/bin/env python3
"""Build an experimental Sensors 2.1 sub-HAL from separately prepared inputs."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    inputs = args.inputs.resolve()
    source_dir = Path(__file__).resolve().parent
    sysroot = inputs / "ndk/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
    vndk = inputs / "vndk-headers"
    # With INPUTS/xsdc-output the generated XML placement parser is built (needs libxml2
    # and ICU headers). Without it, xsdc_stub/ provides the same types with a read() that
    # finds no file, which is what Google's desktop images (no sensor_hal_configuration.xml)
    # always produced; that drops the xsdc, libxml2 and ICU inputs.
    xsdc = inputs / "xsdc-output"
    use_generated = xsdc.exists()
    include_roots = [
        inputs / "libcxx17",
        inputs / "multihal-headers/V2_1",
        inputs / "multihal-headers",
    ]
    if use_generated:
        include_roots += [xsdc / "include", inputs / "icu"]
    else:
        include_roots.append(source_dir / "xsdc_stub")
    sources = [
        source_dir / name
        for name in ("iio_utils.cpp", "MultiPoll.cpp", "Sensor.cpp",
                     "SensorThread.cpp", "SensorsSubHal.cpp")
    ]
    generated_sources = [
        xsdc / name
        for name in ("sensor_hal_configuration_V1_0.cpp",
                     "sensor_hal_configuration_V1_0_enums.cpp")
    ] if use_generated else []
    libraries = [
        inputs / "vendor-lib64" / name
        for name in (
            "android.hardware.sensors@1.0.so",
            "android.hardware.sensors@2.0.so",
            "android.hardware.sensors@2.1.so",
            "android.hardware.sensors@2.0-ScopedWakelock.so",
            "libcutils.so", "libfmq.so", "libhardware.so", "libhidlbase.so",
            "libbase.so", "libpower.so", "libutils.so",
            *(["libxml2.so"] if use_generated else []), "libc++.so",
        )
    ]
    for path in [args.clang, sysroot, vndk, *include_roots, *sources,
                 *generated_sources, *libraries]:
        if not path.exists():
            parser.error(f"missing build input: {path}")

    includes = []
    for base, dirs, _ in os.walk(vndk):
        relative = Path(base).relative_to(vndk)
        if relative.parts[:2] == ("external", "libcxx") or relative.parts[:1] == ("bionic",):
            dirs[:] = []
            continue
        if Path(base).name in ("include", "include_vndk", "gen"):
            includes.extend(("-isystem", base))

    clang = str(args.clang.resolve())
    target_flags = ["--target=x86_64-linux-android35", f"--sysroot={sysroot}"]
    flags = [
        "-std=gnu++17", "-fPIC", "-fno-rtti", "-nostdinc++",
        "-isystem", str(include_roots[0]), "-I", str(source_dir),
    ]
    for path in include_roots[1:]:
        flags.extend(("-isystem", str(path)))
    flags.extend(includes)

    with tempfile.TemporaryDirectory(prefix="gbos-sensor-build-") as temp:
        objects = []
        for source in sources + generated_sources:
            obj = Path(temp) / (source.stem + ".o")
            warning_flags = ["-Wall", "-Wextra", "-Werror"] if source in sources else []
            subprocess.run(
                [clang, *target_flags, *flags, *warning_flags, "-c", str(source), "-o", str(obj)],
                check=True,
            )
            objects.append(str(obj))
        subprocess.run(
            [clang, *target_flags, "-shared", "-nostdlib++", "-Wl,--no-undefined",
             "-o", str(args.output.resolve()), *objects, *map(str, libraries), "-llog"],
            check=True,
        )


if __name__ == "__main__":
    main()
