// Shared declarations of the ipu6-loopback compute shaders (kernel/ipu6-gpu.c). Each shader is
// the GPU form of one CPU stage in kernel/ipu6-loopback.c and uses the same integer arithmetic
// (every intermediate fits 32 bits), so their output matches the CPU path; only the two places
// the CPU computes in double (the colour guard and the ratio mix) may differ by one step.

#define LINEAR_MAX 4095

layout(std430, binding = 0) readonly buffer Params {
	int width, height, stride_px;		// raw frame; stride in pixels (uint16)
	int col_w, col_h, out_w, out_h;
	int gain0, gain2;			// white balance, Q12
	int ccm_active, saturation_q12;
	int ccm[9];				// Q12
	int nir_keep[3];			// Q12
	int ir_visible_q12;
	int has_ir;
	int highlight_start, highlight_full;	// linear
	int lsc_active;
	int chroma_guard;
	float wb_nir_ratio;
	float nir_free, nir_half, hf_start, hf_full, hf_min;
	int radius, edges;
	float edge_desaturation;
	int detail_q12, tnr_threshold, tnr_have_previous, rotate180;
	int cfa[16];				// channel of site (y & 3) * 4 + (x & 3): 0 R, 1 G, 2 B, 3 IR
	int cfa_count[4];
	int crop_x, crop_y, crop_w, crop_h;	/* sensor area shown (sensor_crop) */
} P;

layout(std430, binding = 1) readonly buffer Raw { uint raw[]; };		// uint16 pairs
layout(std430, binding = 2) readonly buffer LutRaw { uint lut_raw[]; };	// 1024
layout(std430, binding = 3) readonly buffer LutGamma { uint lut_gamma[]; };	// LINEAR_MAX + 1
layout(std430, binding = 4) readonly buffer LutDetail { uint lut_detail[]; };	// 1025
layout(std430, binding = 5) readonly buffer Lsc { uint lsc[]; };		// 3 planes, colour grid
layout(std430, binding = 6) buffer Lin { int lin[]; };			// 3 planes
layout(std430, binding = 7) buffer Avg { int avg[]; };			// 3 planes
layout(std430, binding = 8) buffer Tmp { int tmp[]; };			// 3 planes
layout(std430, binding = 9) buffer Green { uint green[]; };		// colour grid
layout(std430, binding = 10) buffer Yuv { int yuv[]; };			// Y', U, V planes
layout(std430, binding = 11) buffer Tnr { uint tnr[]; };		// previous luma, output size
layout(std430, binding = 12) buffer Out { uint outw[]; };		// YUYV, one word per pixel pair

uint raw_code(int x, int y)
{
	uint i = uint(y * P.stride_px + x);
	uint w = raw[i >> 1];
	return ((i & 1u) != 0u ? (w >> 16) : w) & 1023u;
}

// soft_clip_rgb: fit into 0..LINEAR_MAX keeping luminance and hue.
void soft_clip(inout ivec3 c)
{
	int mx = max(c.x, max(c.y, c.z));
	if (mx <= LINEAR_MAX)
		return;
	int y = (2126 * c.x + 7152 * c.y + 722 * c.z + 5000) / 10000;
	for (int k = 0; k < 3; k++) {
		int v;
		if (y >= LINEAR_MAX)
			v = LINEAR_MAX;
		else
			v = y + (c[k] - y) * (LINEAR_MAX - y) / (mx - y);
		c[k] = clamp(v, 0, LINEAR_MAX);
	}
}
