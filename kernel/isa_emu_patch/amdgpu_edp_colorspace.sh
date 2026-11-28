#!/bin/sh
# Ignore the Colorspace property on eDP (AMD). Google's drm_hwcomposer sets it to BT2020_RGB on the built-in panel
# (issue 39, Lenovo 83KG: display.txt "eDP-1 colorspace=BT2020_RGB"), so amdgpu picks COLOR_SPACE_2020_RGB_FULLRANGE and
# sends BT.2020 colorimetry to the panel. SurfaceFlinger really outputs sRGB (has_wide_color_display=false), so a panel
# that honours the colorimetry (the 83KG's LEN145WQ+ lists BT2020RGB in its EDID) maps the colours wrongly and looks pale.
# eDP always gets the default (sRGB) output colour space; the property stays (the HWC may depend on it), external
# DP/HDMI are unchanged. The amdgpu counterpart of i915_edp_colorspace.sh. Idempotent.
set -e
cd "${KSRC:-/build/src}/drivers/gpu/drm/amd/display/amdgpu_dm"
grep -q 'gbos: eDP always gets the default colorimetry' amdgpu_dm.c && { echo already; exit 0; }
python3 - amdgpu_dm.c <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = """	enum dc_color_space color_space = COLOR_SPACE_SRGB;

	switch (connector_state->colorspace) {
"""
new = """	enum dc_color_space color_space = COLOR_SPACE_SRGB;
	enum drm_colorspace colorspace = connector_state->colorspace;

	/* gbos: eDP always gets the default colorimetry (kernel/isa_emu_patch/amdgpu_edp_colorspace.sh). */
	if (connector_state->connector &&
	    connector_state->connector->connector_type == DRM_MODE_CONNECTOR_eDP)
		colorspace = DRM_MODE_COLORIMETRY_DEFAULT;

	switch (colorspace) {
"""
assert s.count(old) == 1, "anchor mismatch"
open(p, 'w').write(s.replace(old, new))
EOF
grep -n -B3 -A4 'gbos: eDP always gets the default colorimetry' amdgpu_dm.c
