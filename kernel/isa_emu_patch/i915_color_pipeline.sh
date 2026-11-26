#!/bin/sh
# Give every i915 plane a Bypass-only COLOR_PIPELINE property: Google's drm_hwcomposer rejects
# planes without it (lapis' xe driver has real pipelines; Gen9 i915 has none). Idempotent.
set -e
cd /build/src/drivers/gpu/drm/i915/display
grep -q drm_plane_create_color_pipeline_property skl_universal_plane.c || \
  sed -i 's/^\tdrm_plane_create_zpos_immutable_property(&plane->base, plane_id);$/&\n\tdrm_plane_create_color_pipeline_property(\&plane->base, NULL, 0);/' skl_universal_plane.c
grep -q drm_plane_create_color_pipeline_property intel_cursor.c || \
  sed -i 's/^\tdrm_plane_create_zpos_immutable_property(&cursor->base, zpos);$/&\n\tdrm_plane_create_color_pipeline_property(\&cursor->base, NULL, 0);/' intel_cursor.c
# Pre-Skylake (Gen <= 8: Haswell / Broadwell / Ivy Bridge) primary and sprite planes live in i9xx_plane.c and intel_sprite.c
for f in i9xx_plane.c intel_sprite.c; do
  if [ -f "$f" ] && ! grep -q drm_plane_create_color_pipeline_property "$f"; then
    sed -i 's/^\([[:space:]]*\)drm_plane_create_zpos_immutable_property(\(&[a-z_]*->base\), [^)]*);$/&\n\1drm_plane_create_color_pipeline_property(\2, NULL, 0);/' "$f"
  fi
done
grep -n -B1 drm_plane_create_color_pipeline_property skl_universal_plane.c intel_cursor.c
