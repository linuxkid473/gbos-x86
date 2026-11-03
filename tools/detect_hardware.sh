#!/bin/sh
# detect_hardware.sh: Probe host system hardware and provide tailored gbOS86 configuration.
# Can be sourced (defines variables and functions) or executed directly (prints hardware report).

detect_host_hardware() {
  # --- 1. Device Model & Vendor ---
  DETECTED_VENDOR=""
  DETECTED_PRODUCT=""
  DETECTED_DEVICE_MODEL=""

  DMI_DIR="${DMI_DIR:-/sys/class/dmi/id}"
  [ -d "$DMI_DIR" ] || DMI_DIR="/sys/devices/virtual/dmi/id"

  if [ -d "$DMI_DIR" ]; then
    [ -f "$DMI_DIR/sys_vendor" ] && DETECTED_VENDOR=$(head -n1 "$DMI_DIR/sys_vendor" 2>/dev/null | tr -d '\r\n')
    
    _fam=""
    _ver=""
    _name=""
    [ -f "$DMI_DIR/product_family" ] && _fam=$(head -n1 "$DMI_DIR/product_family" 2>/dev/null | tr -d '\r\n')
    [ -f "$DMI_DIR/product_version" ] && _ver=$(head -n1 "$DMI_DIR/product_version" 2>/dev/null | tr -d '\r\n')
    [ -f "$DMI_DIR/product_name" ] && _name=$(head -n1 "$DMI_DIR/product_name" 2>/dev/null | tr -d '\r\n')

    # Filter dummy or OEM-internal strings
    for _v in _fam _ver _name; do
      eval "_val=\$$_v"
      case "$_val" in
        "None"|"Default string"|"To be filled by O.E.M."|"System Version"|"System Product Name"|*"Type1ProductConfigId"*|*"X=Null"*|"")
          eval "$_v=''"
          ;;
      esac
    done

    # If product_name is descriptive and not a 4-hex-char board ID (e.g. 83DA), prefer product_name
    if [ -n "$_name" ] && ! echo "$_name" | grep -Eq '^[0-9A-Fa-f]{4}$'; then
      DETECTED_PRODUCT="$_name"
    elif [ -n "$_fam" ] && [ ${#_fam} -ge ${#_name} ]; then
      DETECTED_PRODUCT="$_fam"
    elif [ -n "$_ver" ] && [ ${#_ver} -ge ${#_name} ]; then
      DETECTED_PRODUCT="$_ver"
    elif [ -n "$_name" ]; then
      DETECTED_PRODUCT="$_name"
    fi
  elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.model >/dev/null 2>&1; then
    # macOS host
    DETECTED_VENDOR="Apple"
    DETECTED_PRODUCT=$(sysctl -n hw.model 2>/dev/null)
  fi

  # Clean and combine vendor + product
  if [ -n "$DETECTED_VENDOR" ] && [ -n "$DETECTED_PRODUCT" ]; then
    # Format vendor: remove Inc., Corporation, etc.
    _vclean=$(echo "$DETECTED_VENDOR" | sed -e 's/,.*//' -e 's/ Inc\.//I' -e 's/ Corporation//I' -e 's/ Computer.*//I')
    # Capitalize first letter if all uppercase
    case "$_vclean" in
      LENOVO) _vclean="Lenovo" ;;
      DELL) _vclean="Dell" ;;
      HP) _vclean="HP" ;;
      ASUS) _vclean="ASUS" ;;
      ACER) _vclean="Acer" ;;
    esac
    case "$DETECTED_PRODUCT" in
      *"$_vclean"*) DETECTED_DEVICE_MODEL="$DETECTED_PRODUCT" ;;
      *) DETECTED_DEVICE_MODEL="$_vclean $DETECTED_PRODUCT" ;;
    esac
  elif [ -n "$DETECTED_PRODUCT" ]; then
    DETECTED_DEVICE_MODEL="$DETECTED_PRODUCT"
  elif [ -n "$DETECTED_VENDOR" ]; then
    DETECTED_DEVICE_MODEL="$DETECTED_VENDOR PC"
  else
    DETECTED_DEVICE_MODEL="Generic x86 PC"
  fi
  DETECTED_DEVICE_MODEL=$(echo "$DETECTED_DEVICE_MODEL" | sed -e 's/[[:space:]]\+/ /g' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')

  # --- 2. CPU Model & AVX / AVX2 Capability ---
  DETECTED_CPU_NAME=""
  DETECTED_CPU_AVX=0
  DETECTED_CPU_AVX2=0

  if [ -f /proc/cpuinfo ]; then
    DETECTED_CPU_NAME=$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^[ \t]*//')
    grep -qw avx /proc/cpuinfo 2>/dev/null && DETECTED_CPU_AVX=1
    grep -qw avx2 /proc/cpuinfo 2>/dev/null && DETECTED_CPU_AVX2=1
  elif command -v sysctl >/dev/null 2>&1; then
    DETECTED_CPU_NAME=$(sysctl -n machdep.cpu.brand_string 2>/dev/null)
    _feats=$(sysctl -n machdep.cpu.features machdep.cpu.leaf7_features 2>/dev/null | tr '[:upper:]' '[:lower:]')
    case "$_feats" in *avx*) DETECTED_CPU_AVX=1 ;; esac
    case "$_feats" in *avx2*) DETECTED_CPU_AVX2=1 ;; esac
  fi

  # --- 3. GPU Architecture & Vulkan Driver ---
  DETECTED_GPU_VENDOR=""
  DETECTED_GPU_DRIVER=""
  DETECTED_GPU_NAME=""
  DETECTED_GPU_SW="auto"
  DETECTED_GPU_VK="auto"
  DETECTED_LOAD_I915=1
  DETECTED_XE_PARAMS=""
  DETECTED_EXTRA_SLEEP=0

  _pci_vendor=""
  _pci_driver=""

  # Search DRM cards:
  # 1. Prefer DRM card that actually drives a connected display (e.g. eDP/HDMI)
  for _card in /sys/class/drm/card[0-9]*; do
    [ -d "$_card" ] || continue
    case "$_card" in *-*) continue ;; esac
    _card_name=$(basename "$_card")
    for _conn in /sys/class/drm/${_card_name}-*/status; do
      if [ -f "$_conn" ] && grep -q '^connected$' "$_conn" 2>/dev/null; then
        _pci_vendor=$(cat "$_card/device/vendor" 2>/dev/null | tr '[:upper:]' '[:lower:]')
        if [ -L "$_card/device/driver" ]; then
          _pci_driver=$(basename "$(readlink "$_card/device/driver")" 2>/dev/null)
        fi
        break 2
      fi
    done
  done

  # 2. In dual-GPU laptops (Intel/AMD iGPU + NVIDIA dGPU), prefer the integrated display GPU
  if [ -z "$_pci_vendor" ] || [ "$_pci_vendor" = "0x10de" ]; then
    for _card in /sys/class/drm/card[0-9]*; do
      [ -d "$_card" ] || continue
      case "$_card" in *-*) continue ;; esac
      _v=$(cat "$_card/device/vendor" 2>/dev/null | tr '[:upper:]' '[:lower:]')
      if [ "$_v" = "0x8086" ] || [ "$_v" = "0x1002" ]; then
        _pci_vendor="$_v"
        if [ -L "$_card/device/driver" ]; then
          _pci_driver=$(basename "$(readlink "$_card/device/driver")" 2>/dev/null)
        fi
        break
      fi
    done
  fi

  # Fallback to lspci if DRM card inspection was inconclusive (prefer VGA over 3D controller)
  if [ -z "$_pci_vendor" ] && command -v lspci >/dev/null 2>&1; then
    _vga_line=$(lspci -nn 2>/dev/null | grep -Ei 'vga compatible' | head -n1)
    [ -z "$_vga_line" ] && _vga_line=$(lspci -nn 2>/dev/null | grep -Ei 'vga|3d|display' | head -n1)
    case "$_vga_line" in
      *8086:*) _pci_vendor="0x8086" ;;
      *1002:*) _pci_vendor="0x1002" ;;
      *10de:*) _pci_vendor="0x10de" ;;
    esac
    DETECTED_GPU_NAME=$(echo "$_vga_line" | sed -e 's/^.*: //' -e 's/\[[0-9a-f:]*\]//g')
  fi

  # Check CPU / GPU info for modern Intel Xe / Meteor / Lunar / Arrow Lake / Arc
  _intel_modern=0
  if echo "$DETECTED_CPU_NAME $_pci_driver" | grep -Ei 'ultra|meteor|lunar|arrow|arc|battlemage|xe' >/dev/null 2>&1; then
    _intel_modern=1
  fi

  case "$_pci_vendor" in
    0x8086)
      DETECTED_GPU_VENDOR="Intel"
      if [ "$_intel_modern" = 1 ] || [ "$_pci_driver" = "xe" ]; then
        DETECTED_GPU_NAME="Intel Xe Graphics / Arc"
        DETECTED_LOAD_I915=0
        DETECTED_XE_PARAMS="enable_psr=0 enable_fbc=0 enable_panel_replay=0"
        DETECTED_EXTRA_SLEEP=2
        DETECTED_GPU_SW=auto
        DETECTED_GPU_VK=auto
      else
        DETECTED_GPU_NAME="Intel HD/UHD Graphics"
        DETECTED_LOAD_I915=1
        DETECTED_GPU_SW=auto
        DETECTED_GPU_VK=auto
      fi
      ;;
    0x1002)
      DETECTED_GPU_VENDOR="AMD"
      DETECTED_GPU_NAME="AMD Radeon Graphics"
      DETECTED_LOAD_I915=0
      DETECTED_GPU_SW=auto
      DETECTED_GPU_VK=radv
      ;;
    0x10de)
      DETECTED_GPU_VENDOR="NVIDIA"
      DETECTED_GPU_NAME="NVIDIA GeForce / RTX"
      DETECTED_LOAD_I915=0
      DETECTED_GPU_SW=1
      DETECTED_GPU_VK=swiftshader
      ;;
    *)
      DETECTED_GPU_VENDOR="Generic/Other"
      DETECTED_GPU_NAME="x86 Display Controller"
      DETECTED_LOAD_I915=1
      DETECTED_GPU_SW=auto
      DETECTED_GPU_VK=auto
      ;;
  esac

  # --- 4. Connected Display Resolution & Optimal Density ---
  DETECTED_RESOLUTION=""
  DETECTED_DENSITY=180
  _conn_name=""

  for _status in /sys/class/drm/card*-*/status; do
    [ -f "$_status" ] || continue
    if grep -q '^connected$' "$_status" 2>/dev/null; then
      _conn_dir=$(dirname "$_status")
      _conn_name=$(basename "$_conn_dir")
      if [ -f "$_conn_dir/modes" ] && [ -s "$_conn_dir/modes" ]; then
        DETECTED_RESOLUTION=$(head -n1 "$_conn_dir/modes" 2>/dev/null | tr -d '\r\n')
        break
      fi
    fi
  done

  # Calculate optimal UI density for Android
  if [ -n "$DETECTED_RESOLUTION" ]; then
    _w=$(echo "$DETECTED_RESOLUTION" | cut -d'x' -f1)
    _h=$(echo "$DETECTED_RESOLUTION" | cut -d'x' -f2)
    if [ "$_w" -le 1366 ]; then
      DETECTED_DENSITY=140
    elif [ "$_w" -le 1600 ]; then
      DETECTED_DENSITY=160
    elif [ "$_w" -le 1920 ]; then
      if [ "$_h" -ge 1200 ]; then
        DETECTED_DENSITY=180
      else
        DETECTED_DENSITY=180
      fi
    elif [ "$_w" -le 2560 ]; then
      DETECTED_DENSITY=220
    elif [ "$_w" -le 2880 ]; then
      DETECTED_DENSITY=240
    else
      DETECTED_DENSITY=320
    fi
  fi

  # --- 5. Storage Devices Detection ---
  DETECTED_DISKS=""
  DETECTED_TARGET_DISK=""
  if command -v lsblk >/dev/null 2>&1; then
    # List physical block devices (exclude loop, ram, zram)
    _disks=$(lsblk -dn -o NAME,SIZE,TYPE,TRAN,MODEL 2>/dev/null | grep -E 'disk' | grep -vE 'loop|ram|zram' || true)
    DETECTED_DISKS="$_disks"
    # Choose smart default target disk: prefer non-removable NVMe or SATA
    for _d in $(echo "$_disks" | awk '{print $1}'); do
      case "$_d" in
        nvme*|sd[a-z]|mmcblk*)
          # Check if internal / non-USB
          _tran=$(lsblk -dn -o TRAN "/dev/$_d" 2>/dev/null | tr -d ' ')
          if [ "$_tran" != "usb" ]; then
            DETECTED_TARGET_DISK="/dev/$_d"
            break
          fi
          ;;
      esac
    done
  fi
}

# Apply detected settings directly into a gbos.cfg file
apply_hardware_config() {
  _cfg="$1"
  [ -f "$_cfg" ] || { echo "apply_hardware_config: $_cfg not found" >&2; return 1; }

  # Set DEVICE_MODEL
  if [ -n "$DETECTED_DEVICE_MODEL" ]; then
    if grep -q '^#*DEVICE_MODEL=' "$_cfg"; then
      sed -i "s|^#*DEVICE_MODEL=.*|DEVICE_MODEL=\"$DETECTED_DEVICE_MODEL\"|" "$_cfg"
    else
      echo "DEVICE_MODEL=\"$DETECTED_DEVICE_MODEL\"" >> "$_cfg"
    fi
  fi

  # Set DENSITY
  if [ -n "$DETECTED_DENSITY" ]; then
    if grep -q '^#*DENSITY=' "$_cfg"; then
      sed -i "s|^#*DENSITY=.*|DENSITY=$DETECTED_DENSITY|" "$_cfg"
    else
      echo "DENSITY=$DETECTED_DENSITY" >> "$_cfg"
    fi
  fi

  # Set RESOLUTION
  if [ -n "$DETECTED_RESOLUTION" ]; then
    if grep -q '^#*RESOLUTION=' "$_cfg"; then
      sed -i "s|^#*RESOLUTION=.*|RESOLUTION=\"$DETECTED_RESOLUTION\"|" "$_cfg"
    else
      echo "RESOLUTION=\"$DETECTED_RESOLUTION\"" >> "$_cfg"
    fi
  fi

  # Set GPU_SOFTWARE
  if [ -n "$DETECTED_GPU_SW" ]; then
    if grep -q '^#*GPU_SOFTWARE=' "$_cfg"; then
      sed -i "s|^#*GPU_SOFTWARE=.*|GPU_SOFTWARE=$DETECTED_GPU_SW|" "$_cfg"
    else
      echo "GPU_SOFTWARE=$DETECTED_GPU_SW" >> "$_cfg"
    fi
  fi

  # Set GPU_VULKAN
  if [ -n "$DETECTED_GPU_VK" ]; then
    if grep -q '^#*GPU_VULKAN=' "$_cfg"; then
      sed -i "s|^#*GPU_VULKAN=.*|GPU_VULKAN=$DETECTED_GPU_VK|" "$_cfg"
    else
      echo "GPU_VULKAN=$DETECTED_GPU_VK" >> "$_cfg"
    fi
  fi

  # Set LOAD_I915
  if [ -n "$DETECTED_LOAD_I915" ]; then
    if grep -q '^#*LOAD_I915=' "$_cfg"; then
      sed -i "s|^#*LOAD_I915=.*|LOAD_I915=$DETECTED_LOAD_I915|" "$_cfg"
    else
      echo "LOAD_I915=$DETECTED_LOAD_I915" >> "$_cfg"
    fi
  fi

  # Set XE_PARAMS if needed
  if [ -n "$DETECTED_XE_PARAMS" ]; then
    if grep -q '^#*XE_PARAMS=' "$_cfg"; then
      sed -i "s|^#*XE_PARAMS=.*|XE_PARAMS=\"$DETECTED_XE_PARAMS\"|" "$_cfg"
    else
      echo "XE_PARAMS=\"$DETECTED_XE_PARAMS\"" >> "$_cfg"
    fi
  fi

  # Set EXTRA_SLEEP if needed
  if [ "${DETECTED_EXTRA_SLEEP:-0}" -gt 0 ]; then
    if grep -q '^#*EXTRA_SLEEP=' "$_cfg"; then
      sed -i "s|^#*EXTRA_SLEEP=.*|EXTRA_SLEEP=$DETECTED_EXTRA_SLEEP|" "$_cfg"
    else
      echo "EXTRA_SLEEP=$DETECTED_EXTRA_SLEEP" >> "$_cfg"
    fi
  fi
}

# Print a formatted human-readable report
print_hardware_report() {
  printf '\n================ gbOS86 Host Hardware Profile ================\n'
  printf '  Device Model:  %s\n' "${DETECTED_DEVICE_MODEL:-Unknown Device}"
  printf '  CPU:           %s\n' "${DETECTED_CPU_NAME:-Unknown CPU}"
  if [ "$DETECTED_CPU_AVX2" = 1 ]; then
    printf '  CPU Features:  AVX=yes, AVX2=yes (Fully compatible with gbOS86)\n'
  elif [ "$DETECTED_CPU_AVX" = 1 ]; then
    printf '  CPU Features:  AVX=yes, AVX2=NO [WARNING: AVX2 missing; AOSP init will SIGILL]\n'
  else
    printf '  CPU Features:  AVX=NO,  AVX2=NO [WARNING: Incompatible legacy CPU]\n'
  fi
  printf '  GPU:           %s (%s)\n' "${DETECTED_GPU_NAME}" "${DETECTED_GPU_VENDOR}"
  printf '  GPU Config:    GPU_SOFTWARE=%s, GPU_VULKAN=%s, LOAD_I915=%s\n' \
    "${DETECTED_GPU_SW}" "${DETECTED_GPU_VK}" "${DETECTED_LOAD_I915}"
  if [ -n "$DETECTED_XE_PARAMS" ]; then
    printf '  Xe Graphics:   XE_PARAMS="%s"\n' "$DETECTED_XE_PARAMS"
  fi
  if [ -n "$DETECTED_RESOLUTION" ]; then
    printf '  Display:       %s (Optimal UI Density: %s DPI)\n' "$DETECTED_RESOLUTION" "$DETECTED_DENSITY"
  else
    printf '  Display:       No active display detected (using default %s DPI)\n' "$DETECTED_DENSITY"
  fi
  if [ -n "$DETECTED_TARGET_DISK" ]; then
    printf '  Target Disk:   %s\n' "$DETECTED_TARGET_DISK"
  fi
  printf '===============================================================\n'
}

# If executed directly, run detection and print report
if [ "$(basename "$0")" = "detect_hardware.sh" ]; then
  detect_host_hardware
  print_hardware_report
fi
