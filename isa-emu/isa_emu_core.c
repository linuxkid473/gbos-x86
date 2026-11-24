// SPDX-License-Identifier: GPL-2.0
/*
 * Software implementations of GFNI, AVX-VNNI, SHA-NI, VAES, VPCLMULQDQ and
 * AVX-IFMA (plus ADX), following the pseudocode in the Intel SDM. See isa_emu_core.h.
 */
#include "isa_emu_core.h"

static inline u32 ld32(const u8 *p, int i)
{
	p += 4 * i;
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline void st32(u8 *p, int i, u32 v)
{
	p += 4 * i;
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static inline u32 rol32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
static inline u32 ror32(u32 v, int n) { return (v >> n) | (v << (32 - n)); }

/* ---- GFNI ---------------------------------------------------------------- */

/* Multiply in GF(2^8) with the AES reduction polynomial x^8 + x^4 + x^3 + x + 1. */
static u8 gf_mul(u8 a, u8 b)
{
	u8 r = 0;

	while (b) {
		if (b & 1)
			r ^= a;
		a = (a << 1) ^ ((a & 0x80) ? 0x1b : 0);
		b >>= 1;
	}
	return r;
}

/* Multiplicative inverse; the instruction defines inverse(0) as 0. x^254 = x^-1. */
static u8 gf_inv(u8 x)
{
	u8 r = 1, p = x;
	int e = 254;

	if (!x)
		return 0;
	while (e) {
		if (e & 1)
			r = gf_mul(r, p);
		p = gf_mul(p, p);
		e >>= 1;
	}
	return r;
}

static inline int parity8(u8 v)
{
	v ^= v >> 4;
	v ^= v >> 2;
	v ^= v >> 1;
	return v & 1;
}

static u8 affine_byte(const u8 *matrix_qword, u8 x, u8 imm)
{
	u8 r = 0;
	int i;

	for (i = 0; i < 8; i++)
		r |= (parity8(matrix_qword[7 - i] & x) ^ ((imm >> i) & 1)) << i;
	return r;
}

void emu_gf2p8affineqb(u8 *dst, const u8 *x, const u8 *m, u8 imm, int len)
{
	u8 out[32];
	int i;

	for (i = 0; i < len; i++)
		out[i] = affine_byte(m + (i & ~7), x[i], imm);
	for (i = 0; i < len; i++)
		dst[i] = out[i];
}

void emu_gf2p8affineinvqb(u8 *dst, const u8 *x, const u8 *m, u8 imm, int len)
{
	u8 out[32];
	int i;

	for (i = 0; i < len; i++)
		out[i] = affine_byte(m + (i & ~7), gf_inv(x[i]), imm);
	for (i = 0; i < len; i++)
		dst[i] = out[i];
}

void emu_gf2p8mulb(u8 *dst, const u8 *a, const u8 *b, int len)
{
	u8 out[32];
	int i;

	for (i = 0; i < len; i++)
		out[i] = gf_mul(a[i], b[i]);
	for (i = 0; i < len; i++)
		dst[i] = out[i];
}

/* ---- AVX-VNNI -------------------------------------------------------------- */

static u32 sat32(s64 v)
{
	if (v > 0x7fffffffLL)
		return 0x7fffffff;
	if (v < -0x80000000LL)
		return 0x80000000u;
	return (u32)v;
}

void emu_vpdpbusd(u8 *acc, const u8 *a, const u8 *b, int len, int saturate)
{
	int i, k;

	for (i = 0; i < len / 4; i++) {
		s64 sum = 0;

		for (k = 0; k < 4; k++)
			sum += (s64)a[4 * i + k] * (s8)b[4 * i + k]; /* unsigned x signed */
		if (saturate)
			st32(acc, i, sat32((s64)(s32)ld32(acc, i) + sum));
		else
			st32(acc, i, ld32(acc, i) + (u32)sum);
	}
}

void emu_vpdpwssd(u8 *acc, const u8 *a, const u8 *b, int len, int saturate)
{
	int i, k;

	for (i = 0; i < len / 4; i++) {
		s64 sum = 0;

		for (k = 0; k < 2; k++) {
			s16 x = (s16)(a[4 * i + 2 * k] | a[4 * i + 2 * k + 1] << 8);
			s16 y = (s16)(b[4 * i + 2 * k] | b[4 * i + 2 * k + 1] << 8);

			sum += (s64)x * y;
		}
		if (saturate)
			st32(acc, i, sat32((s64)(s32)ld32(acc, i) + sum));
		else
			st32(acc, i, ld32(acc, i) + (u32)sum);
	}
}

void emu_vpdp8(u8 *acc, const u8 *a, const u8 *b, int len, int saturate, int a_signed, int b_signed)
{
	int i, k;

	for (i = 0; i < len / 4; i++) {
		s64 sum = 0;

		for (k = 0; k < 4; k++) {
			s64 x = a_signed ? (s64)(s8)a[4 * i + k] : (s64)a[4 * i + k];
			s64 y = b_signed ? (s64)(s8)b[4 * i + k] : (s64)b[4 * i + k];

			sum += x * y;
		}
		if (saturate)
			st32(acc, i, sat32((s64)(s32)ld32(acc, i) + sum));
		else
			st32(acc, i, ld32(acc, i) + (u32)sum);
	}
}

/* ---- AVX-IFMA -------------------------------------------------------------- */

static inline u64 ld64(const u8 *p, int i)
{
	return (u64)ld32(p, 2 * i) | (u64)ld32(p, 2 * i + 1) << 32;
}

static inline void st64(u8 *p, int i, u64 v)
{
	st32(p, 2 * i, (u32)v);
	st32(p, 2 * i + 1, (u32)(v >> 32));
}

void emu_vpmadd52(u8 *acc, const u8 *a, const u8 *b, int len, int high)
{
	const u64 m52 = (1ULL << 52) - 1;
	int i;

	for (i = 0; i < len / 8; i++) {
		unsigned __int128 p = (unsigned __int128)(ld64(a, i) & m52) * (ld64(b, i) & m52);
		u64 r = high ? (u64)(p >> 52) & m52 : (u64)p & m52;

		st64(acc, i, ld64(acc, i) + r);
	}
}

/* ---- AES (VAES) ------------------------------------------------------------ */

static u8 aes_sbox[256], aes_isbox[256];
static int aes_ready;

void isa_emu_core_init(void)
{
	int x;

	if (aes_ready)
		return;
	for (x = 0; x < 256; x++) {
		u8 b = gf_inv((u8)x);
		u8 s = b ^ (u8)((b << 1) | (b >> 7)) ^ (u8)((b << 2) | (b >> 6)) ^
		       (u8)((b << 3) | (b >> 5)) ^ (u8)((b << 4) | (b >> 4)) ^ 0x63;

		aes_sbox[x] = s;
		aes_isbox[s] = (u8)x;
	}
	aes_ready = 1;
}

static void aes_lane(u8 *out, const u8 *in, const u8 *rk, int op)
{
	u8 s[16], t[16];
	int inv = op >= EMU_AESDEC, last = op == EMU_AESENCLAST || op == EMU_AESDECLAST;
	int r, c, i;

	for (i = 0; i < 16; i++)
		s[i] = in[i];
	/* (Inv)ShiftRows: state byte index = row + 4 * column */
	for (c = 0; c < 4; c++)
		for (r = 0; r < 4; r++)
			t[r + 4 * c] = s[r + 4 * ((c + (inv ? 4 - r : r)) & 3)];
	for (i = 0; i < 16; i++)
		t[i] = inv ? aes_isbox[t[i]] : aes_sbox[t[i]];
	if (!last) {
		for (c = 0; c < 4; c++) {
			u8 *col = t + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];

			if (!inv) {
				col[0] = gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3;
				col[1] = a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3;
				col[2] = a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3);
				col[3] = gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2);
			} else {
				col[0] = gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9);
				col[1] = gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13);
				col[2] = gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11);
				col[3] = gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14);
			}
		}
	}
	for (i = 0; i < 16; i++)
		out[i] = t[i] ^ rk[i];
}

void emu_vaes(u8 *dst, const u8 *state, const u8 *rk, int len, int op)
{
	u8 out[32];
	int l;

	for (l = 0; l < len / 16; l++)
		aes_lane(out + 16 * l, state + 16 * l, rk + 16 * l, op);
	for (l = 0; l < len; l++)
		dst[l] = out[l];
}

/* ---- VPCLMULQDQ ------------------------------------------------------------ */

void emu_vpclmulqdq(u8 *dst, const u8 *a, const u8 *b, u8 imm, int len)
{
	u8 out[32];
	int l, i;

	for (l = 0; l < len / 16; l++) {
		u64 x = ld64(a + 16 * l, imm & 1 ? 1 : 0), y = ld64(b + 16 * l, imm & 0x10 ? 1 : 0);
		u64 lo = 0, hi = 0;

		for (i = 0; i < 64; i++) {
			if ((y >> i) & 1) {
				lo ^= x << i;
				if (i)
					hi ^= x >> (64 - i);
			}
		}
		st64(out + 16 * l, 0, lo);
		st64(out + 16 * l, 1, hi);
	}
	for (i = 0; i < len; i++)
		dst[i] = out[i];
}

/* ---- SHA-NI ---------------------------------------------------------------- */
/* Dword index 3 is bits 127:96, index 0 is bits 31:0. */

static u32 sha1_f(int fn, u32 b, u32 c, u32 d)
{
	switch (fn) {
	case 0: return (b & c) ^ (~b & d);
	case 2: return (b & c) ^ (b & d) ^ (c & d);
	default: return b ^ c ^ d;
	}
}

void emu_sha1rnds4(u8 *dst, const u8 *src, u8 imm)
{
	static const u32 K[4] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
	u32 a = ld32(dst, 3), b = ld32(dst, 2), c = ld32(dst, 1), d = ld32(dst, 0), e = 0;
	u32 w[4] = { ld32(src, 3), ld32(src, 2), ld32(src, 1), ld32(src, 0) };
	int fn = imm & 3, i;

	for (i = 0; i < 4; i++) {
		u32 t = sha1_f(fn, b, c, d) + rol32(a, 5) + w[i] + e + K[fn];

		e = d; d = c; c = rol32(b, 30); b = a; a = t;
	}
	st32(dst, 3, a); st32(dst, 2, b); st32(dst, 1, c); st32(dst, 0, d);
}

void emu_sha1nexte(u8 *dst, const u8 *src)
{
	u32 t = rol32(ld32(dst, 3), 30);
	int i;

	for (i = 0; i < 3; i++)
		st32(dst, i, ld32(src, i));
	st32(dst, 3, ld32(src, 3) + t);
}

void emu_sha1msg1(u8 *dst, const u8 *src)
{
	u32 w0 = ld32(dst, 3), w1 = ld32(dst, 2), w2 = ld32(dst, 1), w3 = ld32(dst, 0);
	u32 w4 = ld32(src, 3), w5 = ld32(src, 2);

	st32(dst, 3, w2 ^ w0); st32(dst, 2, w3 ^ w1);
	st32(dst, 1, w4 ^ w2); st32(dst, 0, w5 ^ w3);
}

void emu_sha1msg2(u8 *dst, const u8 *src)
{
	u32 w13 = ld32(src, 2), w14 = ld32(src, 1), w15 = ld32(src, 0);
	u32 w16 = rol32(ld32(dst, 3) ^ w13, 1);
	u32 w17 = rol32(ld32(dst, 2) ^ w14, 1);
	u32 w18 = rol32(ld32(dst, 1) ^ w15, 1);
	u32 w19 = rol32(ld32(dst, 0) ^ w16, 1);

	st32(dst, 3, w16); st32(dst, 2, w17); st32(dst, 1, w18); st32(dst, 0, w19);
}

void emu_sha256rnds2(u8 *dst, const u8 *src, const u8 *xmm0)
{
	u32 a = ld32(src, 3), b = ld32(src, 2), c = ld32(dst, 3), d = ld32(dst, 2);
	u32 e = ld32(src, 1), f = ld32(src, 0), g = ld32(dst, 1), h = ld32(dst, 0);
	u32 wk[2] = { ld32(xmm0, 0), ld32(xmm0, 1) };
	u32 an[2], en[2];
	int i;

	for (i = 0; i < 2; i++) {
		u32 ch = (e & f) ^ (~e & g);
		u32 maj = (a & b) ^ (a & c) ^ (b & c);
		u32 s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
		u32 s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
		u32 t = ch + s1 + wk[i] + h;

		an[i] = t + maj + s0;
		en[i] = t + d;
		h = g; g = f; f = e; e = en[i];
		d = c; c = b; b = a; a = an[i];
	}
	st32(dst, 3, an[1]); st32(dst, 2, an[0]); st32(dst, 1, en[1]); st32(dst, 0, en[0]);
}

static u32 sig0(u32 x) { return ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3); }
static u32 sig1(u32 x) { return ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10); }

void emu_sha256msg1(u8 *dst, const u8 *src)
{
	u32 w4 = ld32(src, 0), w3 = ld32(dst, 3), w2 = ld32(dst, 2), w1 = ld32(dst, 1), w0 = ld32(dst, 0);

	st32(dst, 3, w3 + sig0(w4)); st32(dst, 2, w2 + sig0(w3));
	st32(dst, 1, w1 + sig0(w2)); st32(dst, 0, w0 + sig0(w1));
}

void emu_sha256msg2(u8 *dst, const u8 *src)
{
	u32 w14 = ld32(src, 2), w15 = ld32(src, 3);
	u32 w16 = ld32(dst, 0) + sig1(w14);
	u32 w17 = ld32(dst, 1) + sig1(w15);
	u32 w18 = ld32(dst, 2) + sig1(w16);
	u32 w19 = ld32(dst, 3) + sig1(w17);

	st32(dst, 3, w19); st32(dst, 2, w18); st32(dst, 1, w17); st32(dst, 0, w16);
}

/* ---- ADX ----------------------------------------------------------------- */

u64 emu_adx(u64 dst, u64 src, int carry_in, int wide, int *carry_out)
{
	u64 s, r;

	if (!wide) {
		s = (u64)(u32)dst + (u32)src + !!carry_in;
		*carry_out = s >> 32;
		return (u32)s;
	}
	s = dst + src;
	r = s + !!carry_in;
	*carry_out = (s < dst) | (r < s);
	return r;
}
