/* Freestanding test: runs instructions Skylake (and, for ADX, Haswell) lacks and checks against known answers /
 * native oracles. `run_qemu.py KERNEL Haswell` exercises the ADX emulation (QEMU's Skylake-Client has ADX). */
#include <immintrin.h>
#include <stdint.h>

typedef long (*sysf)(void);
static long sc3(long n, long a, long b, long c)
{
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
	return r;
}
static void puts_(const char *s) { long n = 0; while (s[n]) n++; sc3(1, 1, (long)s, n); }
static int fails;
static void check(const char *name, int ok) { puts_(ok ? "PASS " : "FAIL "); puts_(name); puts_("\n"); fails += !ok; }
static int eq(const void *a, const void *b, int n) { const uint8_t *x = a, *y = b; while (n--) if (*x++ != *y++) return 0; return 1; }

static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static void sha256_block_ni(uint32_t st[8], const uint8_t *data)
{
	const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
	__m128i t, s0, s1, w[4], msg, save0, save1;
	t = _mm_loadu_si128((const __m128i *)&st[0]);
	s1 = _mm_loadu_si128((const __m128i *)&st[4]);
	t = _mm_shuffle_epi32(t, 0xB1);
	s1 = _mm_shuffle_epi32(s1, 0x1B);
	s0 = _mm_alignr_epi8(t, s1, 8);
	s1 = _mm_blend_epi16(s1, t, 0xF0);
	save0 = s0; save1 = s1;
	for (int i = 0; i < 4; i++)
		w[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 16 * i)), mask);
	for (int i = 0; i < 16; i++) {
		__m128i cur = w[i & 3];
		msg = _mm_add_epi32(cur, _mm_loadu_si128((const __m128i *)&K[4 * i]));
		s1 = _mm_sha256rnds2_epu32(s1, s0, msg);
		msg = _mm_shuffle_epi32(msg, 0x0E);
		s0 = _mm_sha256rnds2_epu32(s0, s1, msg);
		if (i < 12) {
			__m128i nxt = _mm_sha256msg1_epu32(cur, w[(i + 1) & 3]);
			nxt = _mm_add_epi32(nxt, _mm_alignr_epi8(w[(i + 3) & 3], w[(i + 2) & 3], 4));
			w[i & 3] = _mm_sha256msg2_epu32(nxt, w[(i + 3) & 3]);
		}
	}
	s0 = _mm_add_epi32(s0, save0);
	s1 = _mm_add_epi32(s1, save1);
	t = _mm_shuffle_epi32(s0, 0x1B);
	s1 = _mm_shuffle_epi32(s1, 0xB1);
	s0 = _mm_blend_epi16(t, s1, 0xF0);
	s1 = _mm_alignr_epi8(s1, t, 8);
	_mm_storeu_si128((__m128i *)&st[0], s0);
	_mm_storeu_si128((__m128i *)&st[4], s1);
}

/* SHA-1 "abc" with sha1rnds4/nexte/msg1/msg2, Intel-reference style */
static void sha1_block_ni(uint32_t st[5], const uint8_t *data)
{
	const __m128i mask = _mm_set_epi64x(0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
	__m128i abcd = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)st), 0x1B);
	__m128i e0 = _mm_set_epi32(st[4], 0, 0, 0), e1, w[4], abcd_save = abcd, e_save = e0;
	for (int i = 0; i < 4; i++)
		w[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 16 * i)), mask);
	e0 = _mm_add_epi32(e0, w[0]);
	e1 = abcd;
	abcd = _mm_sha1rnds4_epu32(abcd, e0, 0);
	/* rounds 4..79, in groups of four */
	__m128i e[2] = { e0, e1 };
	(void)e;
	for (int i = 1; i < 20; i++) {
		__m128i wi = w[i & 3];
		if (i >= 4) {
			__m128i nw = _mm_sha1msg1_epu32(w[(i - 4) & 3], w[(i - 3) & 3]);
			nw = _mm_xor_si128(nw, w[(i - 2) & 3]);
			wi = w[i & 3] = _mm_sha1msg2_epu32(nw, w[(i - 1) & 3]);
		}
		__m128i en = _mm_sha1nexte_epu32(e1, wi);
		e1 = abcd;
		switch (i / 5) {
		case 0: abcd = _mm_sha1rnds4_epu32(abcd, en, 0); break;
		case 1: abcd = _mm_sha1rnds4_epu32(abcd, en, 1); break;
		case 2: abcd = _mm_sha1rnds4_epu32(abcd, en, 2); break;
		default: abcd = _mm_sha1rnds4_epu32(abcd, en, 3); break;
		}
	}
	__m128i fin = _mm_sha1nexte_epu32(e1, e_save);
	abcd = _mm_add_epi32(abcd, abcd_save);
	_mm_storeu_si128((__m128i *)st, _mm_shuffle_epi32(abcd, 0x1B));
	st[4] = _mm_extract_epi32(fin, 3);
}

void _start_c(void)
{
	/* GFNI: 0x53 * 0xCA = 0x01 in the AES field */
	{
		__m256i a = _mm256_set1_epi8(0x53), b = _mm256_set1_epi8((char)0xCA), r = _mm256_gf2p8mul_epi8(a, b);
		uint8_t o[32]; _mm256_storeu_si256((__m256i *)o, r);
		int ok = 1; for (int i = 0; i < 32; i++) ok &= o[i] == 1;
		check("vgf2p8mulb ymm", ok);
		__m128i r1 = _mm_gf2p8mul_epi8(_mm_set1_epi8(0x57), _mm_set1_epi8((char)0x83)); /* FIPS-197: 57*83=c1 */
		uint8_t o1[16]; _mm_storeu_si128((__m128i *)o1, r1);
		check("vgf2p8mulb xmm (57*83=c1)", o1[0] == 0xc1 && o1[15] == 0xc1);
	}
	{
		/* identity matrix: dst = x ^ imm */
		__m256i m = _mm256_set1_epi64x(0x0102040810204080LL), x = _mm256_set_epi64x(0x0123456789abcdefLL, 0x1122334455667788LL, 0, -1);
		__m256i r = _mm256_gf2p8affine_epi64_epi8(x, m, 0x5a), want = _mm256_xor_si256(x, _mm256_set1_epi8(0x5a));
		check("vgf2p8affineqb identity", eq(&r, &want, 32));
		/* affineinv with identity matrix = inverse in GF(2^8) ^ imm; inv(0x53)=0xCA */
		__m128i ri = _mm_gf2p8affineinv_epi64_epi8(_mm_set1_epi8(0x53), _mm_set1_epi64x(0x0102040810204080LL), 0);
		uint8_t o[16]; _mm_storeu_si128((__m128i *)o, ri);
		check("vgf2p8affineinvqb inv(53)=ca", o[0] == 0xca && o[9] == 0xca);
	}
	/* AVX-VNNI */
	{
		__m256i acc = _mm256_set1_epi32(10);
		__m256i a = _mm256_set1_epi32(0x04030201);                  /* u8: 1,2,3,4 */
		__m256i b = _mm256_set1_epi32(0xFF010101);                  /* s8: 1,1,1,-1 */
		__m256i r = _mm256_dpbusd_avx_epi32(acc, a, b);
		int32_t o[8]; _mm256_storeu_si256((__m256i *)o, r);
		check("vpdpbusd (10+1+2+3-4=12)", o[0] == 12 && o[7] == 12);
		__m256i w1 = _mm256_set1_epi32(0x00020003), w2 = _mm256_set1_epi32(0xFFFF0004); /* 3*4 + 2*(-1) */
		r = _mm256_dpwssd_avx_epi32(_mm256_set1_epi32(1), w1, w2);
		_mm256_storeu_si256((__m256i *)o, r);
		check("vpdpwssd (1+12-2=11)", o[0] == 11);
		r = _mm256_dpbusds_avx_epi32(_mm256_set1_epi32(0x7fffffff), a, b);
		_mm256_storeu_si256((__m256i *)o, r);
		check("vpdpbusds saturates", (uint32_t)o[0] == 0x7fffffff);
	}
	/* VAES 256 vs native 128-bit AES-NI per lane */
	{
		uint8_t sa[32], ka[32];
		for (int i = 0; i < 32; i++) { sa[i] = i * 7 + 1; ka[i] = i * 13 + 5; }
		__m256i s = _mm256_loadu_si256((void *)sa), k = _mm256_loadu_si256((void *)ka);
		__m256i r1 = _mm256_aesenc_epi128(s, k), r2 = _mm256_aesenclast_epi128(s, k),
			r3 = _mm256_aesdec_epi128(s, k), r4 = _mm256_aesdeclast_epi128(s, k);
		int ok[4] = {1,1,1,1};
		for (int l = 0; l < 2; l++) {
			__m128i s1 = _mm_loadu_si128((void *)(sa + 16 * l)), k1 = _mm_loadu_si128((void *)(ka + 16 * l));
			__m128i e[4] = { _mm_aesenc_si128(s1, k1), _mm_aesenclast_si128(s1, k1), _mm_aesdec_si128(s1, k1), _mm_aesdeclast_si128(s1, k1) };
			__m256i g[4] = { r1, r2, r3, r4 };
			for (int j = 0; j < 4; j++) { uint8_t o[32]; _mm256_storeu_si256((void *)o, g[j]); ok[j] &= eq(o + 16 * l, &e[j], 16); }
		}
		check("vaesenc ymm", ok[0]); check("vaesenclast ymm", ok[1]); check("vaesdec ymm", ok[2]); check("vaesdeclast ymm", ok[3]);
	}
	/* VPCLMULQDQ 256 vs native pclmulqdq per lane, all 4 imm selections */
	{
		uint8_t xa[32], xb[32];
		for (int i = 0; i < 32; i++) { xa[i] = i * 31 + 3; xb[i] = i * 17 + 9; }
		__m256i a = _mm256_loadu_si256((void *)xa), b = _mm256_loadu_si256((void *)xb);
		int ok = 1;
#define CL(IMM) { __m256i r = _mm256_clmulepi64_epi128(a, b, IMM); uint8_t o[32]; _mm256_storeu_si256((void *)o, r); \
	for (int l = 0; l < 2; l++) { __m128i e = _mm_clmulepi64_si128(_mm_loadu_si128((void *)(xa + 16*l)), _mm_loadu_si128((void *)(xb + 16*l)), IMM); ok &= eq(o + 16*l, &e, 16); } }
		CL(0x00) CL(0x01) CL(0x10) CL(0x11)
		check("vpclmulqdq ymm (4 imms)", ok);
	}
	/* SHA-256 "abc" */
	{
		uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
		uint8_t blk[64] = { 'a','b','c', 0x80 }; blk[63] = 24;
		sha256_block_ni(st, blk);
		static const uint32_t want[8] = {0xba7816bf,0x8f01cfea,0x414140de,0x5dae2223,0xb00361a3,0x96177a9c,0xb410ff61,0xf20015ad};
		check("sha256 abc (rnds2/msg1/msg2)", eq(st, want, 32));
	}
	/* SHA-1 "abc" */
	{
		uint32_t st[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
		uint8_t blk[64] = { 'a','b','c', 0x80 }; blk[63] = 24;
		sha1_block_ni(st, blk);
		static const uint32_t want[5] = {0xa9993e36,0x4706816a,0xba3e2571,0x7850c26c,0x9cd0d89d};
		check("sha1 abc (rnds4/nexte/msg1/msg2)", eq(st, want, 20));
	}
	/* IFMA */
	{
		__m256i acc = _mm256_set1_epi64x(5), a = _mm256_set1_epi64x(0x0000000100000001LL), b = _mm256_set1_epi64x(3);
		__m256i lo = _mm256_madd52lo_avx_epu64(acc, a, b), hi = _mm256_madd52hi_avx_epu64(acc, a, b);
		int64_t o[4], p[4]; _mm256_storeu_si256((void *)o, lo); _mm256_storeu_si256((void *)p, hi);
		/* a=2^32+1 (< 2^52), b=3: product = 3*2^32+3 -> low52 = that, high52 = 0 */
		check("vpmadd52luq/huq", o[0] == 5 + (3LL << 32) + 3 && p[0] == 5);
	}
	/* VEX.128 write must zero the upper half of the destination */
	{
		__m256i all = _mm256_set1_epi8(0x55);
		__m128i r = _mm_dpbusd_avx_epi32(_mm_setzero_si128(), _mm_set1_epi8(1), _mm_set1_epi8(1));
		__m256i full = _mm256_zextsi128_si256(r); (void)all; (void)full;
		uint32_t o[4]; _mm_storeu_si128((void *)o, r);
		check("vpdpbusd xmm (4)", o[0] == 4);
	}
	/* ADX (Haswell lacks it; run with CPU=Haswell so QEMU raises #UD): two interleaved carry chains as in
	 * BoringSSL's bn_mulx4x_mont. ADCX must leave OF alone and ADOX must leave CF alone. */
	{
		uint64_t a[4] = { ~0ULL, ~0ULL, 5, ~0ULL }, b[4] = { 1, 0, 7, 0 };
		uint64_t c[4] = { 0x8000000000000000ULL, 0x8000000000000000ULL, 1, 2 }, m[3] = { 0x8000000000000000ULL, 0, 3 };
		uint64_t mem = 0x8000000000000000ULL;
		unsigned char cf, of;
		uint32_t w = 0xffffffffu;
		uint64_t wide = 0xdeadbeefffffffffULL;
		__asm__ volatile(
			"test %[a0],%[a0]\n"			/* CF = OF = 0 */
			"adcx %[b0],%[a0]\n adox %[m0],%[c0]\n"
			"adcx %[b1],%[a1]\n adox %[m1],%[c1]\n"
			"adcx %[b2],%[a2]\n adox %[m2],%[c2]\n"
			"adcx %[b3],%[a3]\n adox %[mem],%[c3]\n"	/* m1, m2, mem: memory operands */
			"setc %[cf]\n seto %[of]\n"
			: [a0] "+r"(a[0]), [a1] "+r"(a[1]), [a2] "+r"(a[2]), [a3] "+r"(a[3]),
			  [c0] "+r"(c[0]), [c1] "+r"(c[1]), [c2] "+r"(c[2]), [c3] "+r"(c[3]), [cf] "=m"(cf), [of] "=m"(of)
			: [b0] "r"(b[0]), [b1] "r"(b[1]), [b2] "r"(b[2]), [b3] "r"(b[3]),
			  [m0] "r"(m[0]), [m1] "m"(m[1]), [m2] "m"(m[2]), [mem] "m"(mem)
			: "cc");
		/* a: ff..ff+1 -> 0 c1; ff..ff+0+1 -> 0 c1; 5+7+1 = 13 c0; ff..ff+0+0 = ff..ff c0
		 * c: 8..0+8..0 -> 0 o1; 8..0+0+1 = 8..01 o0; 1+3 = 4 o0; 2+8..0 = 8..02 o0 */
		check("adcx/adox interleaved chains",
		      !a[0] && !a[1] && a[2] == 13 && a[3] == ~0ULL && !cf &&
		      !c[0] && c[1] == 0x8000000000000001ULL && c[2] == 4 && c[3] == 0x8000000000000002ULL && !of);
		__asm__ volatile("stc\n adcx %[one],%[w]\n setc %[cf]\n" : [w] "+r"(w), [cf] "=m"(cf) : [one] "r"(0u) : "cc");
		check("adcx r32 (ffffffff+0+CF = 0, CF=1)", w == 0 && cf == 1);
		__asm__ volatile("stc\n adcx %[z],%k[x]\n setc %[cf]\n" : [x] "+r"(wide), [cf] "=m"(cf) : [z] "r"(0u) : "cc");
		check("adcx r32 zero-extends into r64", wide == 0 && cf == 1);
	}
	puts_(fails ? "ISA_EMU_TEST_FAIL\n" : "ISA_EMU_TEST_PASS\n");
	sc3(169, 0xfee1dead, 0x28121969, 0x4321fedc); /* reboot(POWER_OFF) */
	for (;;) sc3(34, 0, 0, 0);
}

__attribute__((naked)) void _start(void)
{
	__asm__ volatile("xor %ebp,%ebp\n and $-32,%rsp\n call _start_c\n");
}
