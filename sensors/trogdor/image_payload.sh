#!/bin/sh
# Build or stage the experimental sub-HAL for the DMI-scoped boot-hook trial.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME=android.hardware.sensors@2.1-trogdor-subhal.so
case "${1:-}" in
  build)
    [ "$#" -eq 4 ] || { echo "usage: $0 build INPUTS CLANG OUTPUT" >&2; exit 1; }
    inputs=$2; clang=$3; output=$4
    mkdir -p "$(dirname "$output")"
    temp="$output.part.$$"
    trap 'rm -f "$temp"' EXIT HUP INT TERM
    python3 "$ROOT/sensors/trogdor/build_compact.py" --inputs "$inputs" --clang "$clang" --output "$temp"
    [ -s "$temp" ] || { echo "sensor build produced no library" >&2; exit 1; }
    mv -f "$temp" "$output"
    ;;
  build-docker)
    [ "$#" -ge 3 ] && [ "$#" -le 4 ] || {
      echo "usage: $0 build-docker INPUTS OUTPUT [CONTAINER_CLANG]" >&2
      exit 1
    }
    inputs=$(cd "$2" && pwd -P)
    output_dir=$(dirname "$3")
    mkdir -p "$output_dir"
    output_dir=$(cd "$output_dir" && pwd -P)
    output_name=$(basename "$3")
    clang=${4:-/build/inputs/clang-r584948b/bin/clang++}
    temp="$output_dir/$output_name.part.$$"
    trap 'rm -f "$temp"' EXIT HUP INT TERM
    # Mount the parent too, at the same relative place: prepared inputs link into their siblings
    # (build/sensor-hal-inputs/INPUTS/ndk -> ../ndk), which would dangle with only INPUTS mounted.
    inputs_parent=$(dirname "$inputs")
    docker run --rm --pull=never \
      -v "$inputs_parent:/inputs-root:ro" \
      -v "$inputs:/inputs:ro" \
      -v "$ROOT:/src:ro" \
      -v gbos-ksrc:/build:ro \
      -v "$output_dir:/out" \
      --entrypoint python3 gbos-kbuild \
      /src/sensors/trogdor/build_compact.py \
      --inputs "/inputs-root/$(basename "$inputs")" --clang "$clang" --output "/out/$(basename "$temp")"
    [ -s "$temp" ] || { echo "sensor build produced no library" >&2; exit 1; }
    mv -f "$temp" "$output_dir/$output_name"
    ;;
  stage)
    [ "$#" -eq 3 ] || { echo "usage: $0 stage LIBRARY GBOS_DIR" >&2; exit 1; }
    [ -s "$2" ] || { echo "missing or empty experimental sensor library: $2" >&2; exit 1; }
    mkdir -p "$3/sensors"
    cp "$2" "$3/sensors/$NAME"
    ;;
  enable-motion-probe|enable-continuous-probe)
    [ "$#" -eq 2 ] || { echo "usage: $0 $1 GBOS_CFG" >&2; exit 1; }
    cfg=$2
    [ -f "$cfg" ] || { echo "missing gbos.cfg: $cfg" >&2; exit 1; }
    set_flag() {
      key=$1
      value=$2
      temp="$cfg.sensor-probe.$$"
      awk -v key="$key" -v value="$value" '
        BEGIN { prefix = key "=" }
        index($0, prefix) == 1 {
          if (!seen++) print prefix value
          next
        }
        { print }
        END { if (!seen) print prefix value }
      ' "$cfg" > "$temp"
      mv "$temp" "$cfg"
    }
    set_flag SENSOR_EVENT_PROBE 1
    if [ "$1" = enable-continuous-probe ]; then
      set_flag SENSOR_CONTINUOUS_PROBE 1
    else
      set_flag SENSOR_MOTION_PROBE 1
    fi
    ;;
  *)
    echo "usage: $0 build INPUTS CLANG OUTPUT | build-docker INPUTS OUTPUT [CONTAINER_CLANG] | stage LIBRARY GBOS_DIR | enable-motion-probe GBOS_CFG | enable-continuous-probe GBOS_CFG" >&2
    exit 1
    ;;
esac
