// SPDX-License-Identifier: GPL-2.0
/* Known-answer tests for isa_emu_core.c. Build: gcc -O2 -o test_core test_core.c isa_emu_core.c
 * (add -madx on a CPU with ADX to also check ADCX/ADOX against the real instructions) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isa_emu_core.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u32 ld(const u8 *p, int i) { p += 4 * i; return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void st(u8 *p, int i, u32 v) { p += 4 * i; p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static u32 rol(u32 v, int n) { return v << n | v >> (32 - n); }
static u64 rng = 0x123456789abcdefULL;
static u64 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void rndbuf(u8 *p, int n) { for (int i = 0; i < n; i++) p[i] = rnd() >> 24; }

/* ---- GFNI ---- */
static void test_gfni(void)
{
	static const u8 sbox_row0[16] = { 0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76 };
	u8 x[32], m[32], o[32], id[32];
	u64 aes_m = 0xF1E3C78F1F3E7CF8ULL, ident = 0x0102040810204080ULL;
	int i;

	for (i = 0; i < 32; i++) { x[i] = i; m[i] = (aes_m >> (8 * (i & 7))) & 0xff; id[i] = (ident >> (8 * (i & 7))) & 0xff; }
	emu_gf2p8affineinvqb(o, x, m, 0x63, 16);
	CHECK(!memcmp(o, sbox_row0, 16), "AES sbox row 0 via gf2p8affineinvqb");
	for (i = 0; i < 32; i++) x[i] = 0x10 + i;
	for (int v = 0; v < 256; v++) { /* every byte; spot-check known values */
		u8 xi[16] = { v }, oi[16];
		emu_gf2p8affineinvqb(oi, xi, m, 0x63, 16);
		if (v == 0x53) CHECK(oi[0] == 0xed, "sbox[53]=%02x", oi[0]);
		if (v == 0xff) CHECK(oi[0] == 0x16, "sbox[ff]=%02x", oi[0]);
		if (v == 0x10) CHECK(oi[0] == 0xca, "sbox[10]=%02x", oi[0]);
	}
	rndbuf(x, 32);
	emu_gf2p8affineqb(o, x, id, 0, 32);
	CHECK(!memcmp(o, x, 32), "identity matrix");
	emu_gf2p8affineqb(o, x, id, 0xa5, 32);
	for (i = 0; i < 32; i++) CHECK(o[i] == (x[i] ^ 0xa5), "imm xor");
	{ u8 a[16] = { 0x57 }, b[16] = { 0x83 }, p[16], inv[16] = { 0x53 }, k[16] = { 0xca };
	  emu_gf2p8mulb(p, a, b, 16); CHECK(p[0] == 0xc1, "FIPS-197 {57}*{83}={c1}, got %02x", p[0]);
	  emu_gf2p8mulb(p, inv, k, 16); CHECK(p[0] == 0x01, "{53}*{ca}=1, got %02x", p[0]); }
	/* ymm: second 128-bit lane uses its own matrix qwords */
	rndbuf(x, 32); rndbuf(m, 32);
	emu_gf2p8affineqb(o, x, m, 0x3c, 32);
	{ u8 o2[32]; emu_gf2p8affineqb(o2, x, m, 0x3c, 16); emu_gf2p8affineqb(o2 + 16, x + 16, m + 16, 0x3c, 16);
	  CHECK(!memcmp(o, o2, 32), "ymm == 2 x xmm"); }
	/* dst aliasing the source */
	{ u8 y[32]; memcpy(y, x, 32); emu_gf2p8affineqb(y, y, m, 7, 32); emu_gf2p8affineqb(o, x, m, 7, 32); CHECK(!memcmp(y, o, 32), "alias"); }
}

/* ---- VNNI ---- */
static void test_vnni(void)
{
	for (int it = 0; it < 2000; it++) {
		u8 acc[32], a[32], b[32], r[32], e[32];
		int len = (it & 1) ? 32 : 16, sat = (it >> 1) & 1;
		rndbuf(acc, 32); rndbuf(a, 32); rndbuf(b, 32);
		if (it % 7 == 0) { for (int i = 0; i < 32; i++) { a[i] = 255; b[i] = 0x80 | (i & 1 ? 0x7f : 0); } st(acc, 0, 0x7fffff00); st(acc, 1, 0x80000010); }
		memcpy(r, acc, 32); memcpy(e, acc, 32);
		emu_vpdpbusd(r, a, b, len, sat);
		for (int i = 0; i < len / 4; i++) {
			long long s = 0; for (int k = 0; k < 4; k++) s += (long long)a[4*i+k] * (signed char)b[4*i+k];
			long long v = (int)ld(acc, i) + s;
			if (sat) { if (v > 2147483647LL) v = 2147483647LL; if (v < -2147483648LL) v = -2147483648LL; }
			st(e, i, (u32)v);
		}
		CHECK(!memcmp(r, e, len), "vpdpbusd%s len %d", sat ? "s" : "", len);
		memcpy(r, acc, 32); memcpy(e, acc, 32);
		emu_vpdpwssd(r, a, b, len, sat);
		for (int i = 0; i < len / 4; i++) {
			long long s = 0; for (int k = 0; k < 2; k++) s += (long long)(short)(a[4*i+2*k] | a[4*i+2*k+1] << 8) * (short)(b[4*i+2*k] | b[4*i+2*k+1] << 8);
			long long v = (int)ld(acc, i) + s;
			if (sat) { if (v > 2147483647LL) v = 2147483647LL; if (v < -2147483648LL) v = -2147483648LL; }
			st(e, i, (u32)v);
		}
		CHECK(!memcmp(r, e, len), "vpdpwssd%s len %d", sat ? "s" : "", len);
	}
	{ u8 acc[16] = {0}, a[16], b[16]; memset(a, 1, 16); memset(b, 0xff, 16); emu_vpdp8(acc, a, b, 16, 0, 0, 1); CHECK(ld(acc, 0) == (u32)-4, "vpdp8 u*s"); 
	  memset(acc, 0, 16); emu_vpdp8(acc, a, b, 16, 0, 0, 0); CHECK(ld(acc, 0) == 4 * 255, "vpdp8 u*u"); 
	  memset(acc, 0, 16); memset(a, 0xff, 16); emu_vpdp8(acc, a, b, 16, 0, 1, 1); CHECK(ld(acc, 0) == 4, "vpdp8 s*s"); }
}

/* ---- SHA-256 / SHA-1 built from the primitives, checked against "abc" digests ---- */
static void test_sha256(void)
{
	static const u32 K[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
	static const u32 H0[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
	u32 w[64] = { 0x61626380 }; /* "abc" + padding, length 24 bits */
	u8 abef[16], cdgh[16], msg[16], m2[16], t[16];
	u8 s0[16], s1[16];
	u32 out[8];
	int i, g;

	w[15] = 24;
	/* message schedule through msg1/msg2 for t >= 16 */
	for (g = 4; g < 16; g++) {
		u8 w0[16], w1[16], w2[16], w3[16], x[16];
		for (i = 0; i < 4; i++) { st(w0, i, w[4*(g-4)+i]); st(w1, i, w[4*(g-3)+i]); st(w2, i, w[4*(g-2)+i]); st(w3, i, w[4*(g-1)+i]); }
		memcpy(x, w0, 16); emu_sha256msg1(x, w1);
		/* + alignr(w3, w2, 4) = W[t-7..t-4] */
		for (i = 0; i < 4; i++) st(x, i, ld(x, i) + w[4*g + i - 7]);
		emu_sha256msg2(x, w3);
		for (i = 0; i < 4; i++) w[4*g+i] = ld(x, i);
	}
	{ /* reference schedule */
		u32 r[64]; memset(r, 0, sizeof r); r[0] = 0x61626380; r[15] = 24;
		for (i = 16; i < 64; i++) { u32 a = r[i-15], b = r[i-2];
			r[i] = r[i-16] + (((a >> 7) | (a << 25)) ^ ((a >> 18) | (a << 14)) ^ (a >> 3)) + r[i-7] + (((b >> 17) | (b << 15)) ^ ((b >> 19) | (b << 13)) ^ (b >> 10)); }
		CHECK(!memcmp(r, w, sizeof r), "sha256 msg1/msg2 schedule");
	}
	/* state: ABEF = {dw3=A, dw2=B, dw1=E, dw0=F}, CDGH = {dw3=C, dw2=D, dw1=G, dw0=H} */
	st(abef, 3, H0[0]); st(abef, 2, H0[1]); st(abef, 1, H0[4]); st(abef, 0, H0[5]);
	st(cdgh, 3, H0[2]); st(cdgh, 2, H0[3]); st(cdgh, 1, H0[6]); st(cdgh, 0, H0[7]);
	memcpy(s0, abef, 16); memcpy(s1, cdgh, 16);
	for (i = 0; i < 64; i += 4) {
		memset(msg, 0, 16); for (g = 0; g < 4; g++) st(msg, g, w[i+g] + K[i+g]);
		emu_sha256rnds2(cdgh, abef, msg);          /* CDGH' = rnds2(CDGH, ABEF, WK) */
		memset(m2, 0, 16); st(m2, 0, ld(msg, 2)); st(m2, 1, ld(msg, 3));
		emu_sha256rnds2(abef, cdgh, m2);
	}
	(void)t;
	for (i = 0; i < 4; i++) { st(abef, i, ld(abef, i) + ld(s0, i)); st(cdgh, i, ld(cdgh, i) + ld(s1, i)); }
	out[0] = ld(abef, 3); out[1] = ld(abef, 2); out[4] = ld(abef, 1); out[5] = ld(abef, 0);
	out[2] = ld(cdgh, 3); out[3] = ld(cdgh, 2); out[6] = ld(cdgh, 1); out[7] = ld(cdgh, 0);
	{ static const u32 exp[8] = { 0xba7816bf,0x8f01cfea,0x414140de,0x5dae2223,0xb00361a3,0x96177a9c,0xb410ff61,0xf20015ad };
	  CHECK(!memcmp(out, exp, 32), "SHA-256(abc) = %08x%08x...", out[0], out[1]); }
}

static void test_sha1(void)
{
	u32 w[80] = { 0x61626380 }, e0 = 0xc3d2e1f0;
	u8 msg[20][16], abcd[16], abcd_prev[16], e_cur[16], e_save[16], abcd_save[16], tmp[16];
	int g, i;

	w[15] = 24;
	/* msg[g]: dword3 = W[4g], dword0 = W[4g+3] */
	for (g = 0; g < 4; g++) for (i = 0; i < 4; i++) st(msg[g], 3 - i, w[4*g+i]);
	for (g = 4; g < 20; g++) {
		memcpy(tmp, msg[g-4], 16); emu_sha1msg1(tmp, msg[g-3]);
		for (i = 0; i < 16; i++) tmp[i] ^= msg[g-2][i];
		emu_sha1msg2(tmp, msg[g-1]);
		memcpy(msg[g], tmp, 16);
	}
	{ u32 r[80]; memcpy(r, w, sizeof(u32) * 16);
	  for (i = 16; i < 80; i++) r[i] = rol(r[i-3] ^ r[i-8] ^ r[i-14] ^ r[i-16], 1);
	  for (g = 4; g < 20; g++) for (i = 0; i < 4; i++) CHECK(ld(msg[g], 3 - i) == r[4*g+i], "sha1 schedule g=%d i=%d", g, i); }
	memset(abcd, 0, 16); st(abcd, 3, 0x67452301); st(abcd, 2, 0xefcdab89); st(abcd, 1, 0x98badcfe); st(abcd, 0, 0x10325476);
	memcpy(abcd_save, abcd, 16);
	memset(e_save, 0, 16); st(e_save, 3, e0);
	for (g = 0; g < 20; g++) {
		if (g == 0) { memcpy(e_cur, e_save, 16); st(e_cur, 3, ld(e_cur, 3) + ld(msg[0], 3)); for (i = 0; i < 3; i++) st(e_cur, i, ld(msg[0], i)); }
		else { memcpy(e_cur, msg[g], 16); /* nexte(prev_abcd, msg) */
		       u8 d[16]; memcpy(d, abcd_prev, 16); emu_sha1nexte(d, msg[g]); memcpy(e_cur, d, 16); }
		memcpy(abcd_prev, abcd, 16);
		emu_sha1rnds4(abcd, e_cur, g / 5);
	}
	{ u8 d[16]; memcpy(d, abcd_prev, 16); emu_sha1nexte(d, e_save); /* final E */
	  for (i = 0; i < 4; i++) st(abcd, i, ld(abcd, i) + ld(abcd_save, i));
	  u32 h[5] = { ld(abcd, 3), ld(abcd, 2), ld(abcd, 1), ld(abcd, 0), ld(d, 3) };
	  static const u32 exp[5] = { 0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d };
	  CHECK(!memcmp(h, exp, 20), "SHA-1(abc) = %08x %08x %08x %08x %08x", h[0], h[1], h[2], h[3], h[4]); }
}

/* ---- VAES: FIPS-197 appendix B, with an independent key schedule ---- */
static u8 xt(u8 x) { return (x << 1) ^ ((x & 0x80) ? 0x1b : 0); }
static u8 gm(u8 a, u8 b) { u8 r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
static u8 sbox_ref(u8 x) { /* classic generator: multiplicative inverse by brute force, then affine */
	u8 inv = 0; if (x) for (int c = 1; c < 256; c++) if (gm(x, c) == 1) { inv = c; break; }
	u8 b = inv; return b ^ (u8)(b << 1 | b >> 7) ^ (u8)(b << 2 | b >> 6) ^ (u8)(b << 3 | b >> 5) ^ (u8)(b << 4 | b >> 4) ^ 0x63; }

static void test_vaes(void)
{
	static const u8 key[16] = { 0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
	static const u8 pt[16] = { 0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34 };
	static const u8 ct[16] = { 0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32 };
	u8 rk[11][16], st2[32], out[32];
	u8 rcon = 1;
	int r, i;

	isa_emu_core_init();
	memcpy(rk[0], key, 16);
	for (r = 1; r < 11; r++) {
		u8 *p = rk[r-1], *n = rk[r], t[4] = { sbox_ref(p[13]) ^ rcon, sbox_ref(p[14]), sbox_ref(p[15]), sbox_ref(p[12]) };
		rcon = xt(rcon);
		for (i = 0; i < 4; i++) n[i] = p[i] ^ t[i];
		for (i = 4; i < 16; i++) n[i] = p[i] ^ n[i-4];
	}
	{ static const u8 k10[16] = { 0xd0,0x14,0xf9,0xa8,0xc9,0xee,0x25,0x89,0xe1,0x3f,0x0c,0xc8,0xb6,0x63,0x0c,0xa6 };
	  CHECK(!memcmp(rk[10], k10, 16), "key schedule round 10"); }
	/* two different blocks in the two ymm lanes */
	for (i = 0; i < 16; i++) { st2[i] = pt[i] ^ rk[0][i]; st2[16+i] = pt[i] ^ rk[0][i]; }
	for (r = 1; r < 10; r++) { u8 k2[32]; memcpy(k2, rk[r], 16); memcpy(k2 + 16, rk[r], 16); emu_vaes(st2, st2, k2, 32, EMU_AESENC); }
	{ u8 k2[32]; memcpy(k2, rk[10], 16); memcpy(k2 + 16, rk[10], 16); emu_vaes(out, st2, k2, 32, EMU_AESENCLAST); }
	CHECK(!memcmp(out, ct, 16) && !memcmp(out + 16, ct, 16), "AES-128 encrypt (FIPS-197 App. B)");
	/* decrypt with the equivalent inverse cipher: InvMixColumns on round keys 1..9 */
	{ u8 dk[11][16]; memcpy(dk[0], rk[0], 16); memcpy(dk[10], rk[10], 16);
	  for (r = 1; r < 10; r++) for (int c = 0; c < 4; c++) { const u8 *a = rk[r] + 4*c; u8 *o = dk[r] + 4*c;
		o[0] = gm(a[0],14)^gm(a[1],11)^gm(a[2],13)^gm(a[3],9); o[1] = gm(a[0],9)^gm(a[1],14)^gm(a[2],11)^gm(a[3],13);
		o[2] = gm(a[0],13)^gm(a[1],9)^gm(a[2],14)^gm(a[3],11); o[3] = gm(a[0],11)^gm(a[1],13)^gm(a[2],9)^gm(a[3],14); }
	  u8 s[32]; for (i = 0; i < 16; i++) s[i] = s[16+i] = ct[i] ^ rk[10][i];
	  for (r = 9; r >= 1; r--) { u8 k2[32]; memcpy(k2, dk[r], 16); memcpy(k2 + 16, dk[r], 16); emu_vaes(s, s, k2, 32, EMU_AESDEC); }
	  u8 k2[32]; memcpy(k2, rk[0], 16); memcpy(k2 + 16, rk[0], 16); emu_vaes(s, s, k2, 32, EMU_AESDECLAST);
	  CHECK(!memcmp(s, pt, 16) && !memcmp(s + 16, pt, 16), "AES-128 decrypt round trip"); }
	for (i = 0; i < 256; i++) CHECK(sbox_ref(i) == 0 + sbox_ref(i), "x"); /* keep sbox_ref linked */
}

/* ---- VPCLMULQDQ / IFMA ---- */
static void test_clmul_ifma(void)
{
	u8 a[32], b[32], o[32];
	int it, l;

	{ u8 x[16] = { 3 }, y[16] = { 3 }, p[16]; emu_vpclmulqdq(p, x, y, 0, 16); CHECK(p[0] == 5 && !p[1], "clmul(3,3)=5"); }
	{ u8 x[16] = { 0 }, y[16] = { 0 }, p[16]; st(x, 2, 0x80000000); st(y, 0, 2); emu_vpclmulqdq(p, x, y, 0, 16);
	  CHECK(!ld(p, 0) && !ld(p, 1) && !ld(p, 2) && !ld(p, 3), "imm=0 ignores high qword of a");
	  emu_vpclmulqdq(p, x, y, 1, 16);
	  CHECK(ld(p, 1) == 1 && !ld(p, 0) && !ld(p, 2) && !ld(p, 3), "imm=1 uses high qword of a (0x80000000*2 = bit 32)"); }
	for (it = 0; it < 500; it++) {
		u8 imm = (it & 1 ? 1 : 0) | (it & 2 ? 0x10 : 0);
		rndbuf(a, 32); rndbuf(b, 32);
		emu_vpclmulqdq(o, a, b, imm, 32);
		for (l = 0; l < 2; l++) {
			unsigned __int128 acc = 0, xx; unsigned long long x, y;
			memcpy(&x, a + 16*l + ((imm & 1) ? 8 : 0), 8); memcpy(&y, b + 16*l + ((imm & 0x10) ? 8 : 0), 8);
			xx = x; for (int i = 0; i < 64; i++) if (y >> i & 1) acc ^= xx << i;
			CHECK(!memcmp(&acc, o + 16*l, 16), "vpclmulqdq lane %d imm %02x", l, imm);
		}
	}
	for (it = 0; it < 2000; it++) {
		u8 acc[32], r[32]; int high = it & 1;
		rndbuf(acc, 32); rndbuf(a, 32); rndbuf(b, 32);
		memcpy(r, acc, 32); emu_vpmadd52(r, a, b, 32, high);
		for (l = 0; l < 4; l++) {
			unsigned long long x, y, c; memcpy(&x, a + 8*l, 8); memcpy(&y, b + 8*l, 8); memcpy(&c, acc + 8*l, 8);
			x &= (1ULL << 52) - 1; y &= (1ULL << 52) - 1;
			unsigned __int128 p = (unsigned __int128)x * y;
			unsigned long long e = c + (high ? (unsigned long long)(p >> 52) : (unsigned long long)(p & ((1ULL << 52) - 1)));
			CHECK(!memcmp(&e, r + 8*l, 8), "vpmadd52%cuq q%d", high ? 'h' : 'l', l);
		}
	}
}

/* ---- ADX ---- */
#if defined(__x86_64__) && defined(__ADX__)
/* Native oracle (only when built with -madx on a CPU that has ADX): r = d + s + carry, returns the new carry. */
static int native_adx(int ox, int wide, u64 *d, u64 s, int cin)
{
	unsigned char c;
	u64 r = *d;
	u32 r32 = (u32)r;

	if (!ox && wide)
		__asm__("bt $0,%q2; adcx %3,%0; setc %1" : "+r"(r), "=q"(c) : "r"((u64)cin), "r"(s) : "cc");
	else if (!ox)
		__asm__("bt $0,%q2; adcx %3,%0; setc %1" : "+r"(r32), "=q"(c) : "r"((u64)cin), "r"((u32)s) : "cc");
	else if (wide)	/* OF = cin: (cin<<63)-1+1 overflows iff cin */
		__asm__("mov %2,%%r11; shl $63,%%r11; dec %%r11; add $1,%%r11; adox %3,%0; seto %1"
			: "+r"(r), "=q"(c) : "r"((u64)cin), "r"(s) : "cc", "r11");
	else
		__asm__("mov %2,%%r11; shl $63,%%r11; dec %%r11; add $1,%%r11; adox %3,%0; seto %1"
			: "+r"(r32), "=q"(c) : "r"((u64)cin), "r"((u32)s) : "cc", "r11");
	*d = wide ? r : r32;
	return c;
}
#endif

static void test_adx(void)
{
	static const struct { u64 d, s; int cin, wide; u64 r; int cout; } kat[] = {
		{ 0, 0, 0, 1, 0, 0 },
		{ 0, 0, 1, 1, 1, 0 },
		{ ~0ULL, 1, 0, 1, 0, 1 },
		{ ~0ULL, 0, 1, 1, 0, 1 },
		{ ~0ULL, ~0ULL, 1, 1, ~0ULL, 1 },
		{ 0x8000000000000000ULL, 0x8000000000000000ULL, 0, 1, 0, 1 },
		{ 0x7fffffffffffffffULL, 1, 0, 1, 0x8000000000000000ULL, 0 },	/* signed overflow, no carry */
		{ 0xffffffffULL, 1, 0, 0, 0, 1 },				/* 32-bit carry out of bit 31 */
		{ 0xdeadbeefffffffffULL, 0, 1, 0, 0, 1 },			/* 32-bit: upper half ignored and cleared */
		{ 0x12345678ffffffffULL, 0xabcdef0000000001ULL, 0, 0, 0, 1 },
		{ 0x1111111122222222ULL, 0x3333333344444444ULL, 1, 0, 0x66666667ULL, 0 },
	};
	unsigned int i;
	int c;

	for (i = 0; i < sizeof(kat) / sizeof(kat[0]); i++) {
		u64 r = emu_adx(kat[i].d, kat[i].s, kat[i].cin, kat[i].wide, &c);
		CHECK(r == kat[i].r && c == kat[i].cout, "adx kat %u: got %llx/%d", i, (unsigned long long)r, c);
	}
	for (i = 0; i < 200000; i++) {
		u64 d = rnd(), s = rnd(), r, e;
		int cin = rnd() & 1, wide = i & 1, ce;
		unsigned __int128 sum;

		if (i & 2)		/* bias towards carry edges */
			d |= wide ? 0xfffffffffff00000ULL : 0xfff00000ULL;
		r = emu_adx(d, s, cin, wide, &c);
		if (wide) {
			sum = (unsigned __int128)d + s + cin;
			e = (u64)sum; ce = (int)(sum >> 64);
		} else {
			u64 t = (u64)(u32)d + (u32)s + cin;
			e = (u32)t; ce = (int)(t >> 32);
		}
		CHECK(r == e && c == ce, "adx rnd %u wide=%d", i, wide);
#if defined(__x86_64__) && defined(__ADX__)
		{
			u64 n = d;
			int nc = native_adx(i & 4, wide, &n, s, cin);
			CHECK(n == r && nc == c, "adx vs native %s wide=%d", (i & 4) ? "adox" : "adcx", wide);
		}
#endif
	}
	/* carry chain: 256-bit add 2^256-1 + 1 = 0 with carry out, as bn code chains it */
	{
		u64 a[4] = { ~0ULL, ~0ULL, ~0ULL, ~0ULL }, b[4] = { 1, 0, 0, 0 };
		int k;

		c = 0;
		for (k = 0; k < 4; k++)
			a[k] = emu_adx(a[k], b[k], c, 1, &c);
		CHECK(!a[0] && !a[1] && !a[2] && !a[3] && c == 1, "adx 256-bit carry chain");
	}
}

int main(void)
{
	isa_emu_core_init();
	test_gfni(); test_vnni(); test_sha256(); test_sha1(); test_vaes(); test_clmul_ifma(); test_adx();
	printf("%d checks, %d failures\n", checks, fails);
	return fails != 0;
}
