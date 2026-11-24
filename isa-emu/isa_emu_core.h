/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Software implementations of x86 instructions that Skylake-family CPUs
 * (e.g. Whiskey Lake i5-8365U) lack: GFNI, AVX-VNNI, SHA-NI, 256-bit VAES and
 * VPCLMULQDQ, and AVX-IFMA.
 *
 * Pure functions on byte vectors, with no kernel dependencies, so the same
 * file builds into the kernel and into a userspace test harness.
 * Vectors are little-endian byte arrays; `len` is 16 (xmm) or 32 (ymm).
 */
#ifndef ISA_EMU_CORE_H
#define ISA_EMU_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif

/* GFNI. `x` is the data source, `m` the matrix (or second multiplicand) source. */
void emu_gf2p8affineqb(u8 *dst, const u8 *x, const u8 *m, u8 imm, int len);
void emu_gf2p8affineinvqb(u8 *dst, const u8 *x, const u8 *m, u8 imm, int len);
void emu_gf2p8mulb(u8 *dst, const u8 *a, const u8 *b, int len);

/* AVX-VNNI. `acc` is the destination/accumulator, `a` = first source (vvvv), `b` = second. */
void emu_vpdpbusd(u8 *acc, const u8 *a, const u8 *b, int len, int saturate);
void emu_vpdpwssd(u8 *acc, const u8 *a, const u8 *b, int len, int saturate);

/* AVX-VNNI-INT8: dword += sum of four byte products. a_signed/b_signed pick the byte signedness. */
void emu_vpdp8(u8 *acc, const u8 *a, const u8 *b, int len, int saturate, int a_signed, int b_signed);

/* AVX-IFMA: acc.qword += low/high 52 bits of the 104-bit product of the low 52 bits of a and b. */
void emu_vpmadd52(u8 *acc, const u8 *a, const u8 *b, int len, int high);

/* VAES: one AES round per 128-bit lane. `state` is the data, `rk` the round key. */
enum { EMU_AESENC, EMU_AESENCLAST, EMU_AESDEC, EMU_AESDECLAST };
void emu_vaes(u8 *dst, const u8 *state, const u8 *rk, int len, int op);

/* VPCLMULQDQ: per 128-bit lane; imm bit 0 picks a's qword, bit 4 picks b's qword. */
void emu_vpclmulqdq(u8 *dst, const u8 *a, const u8 *b, u8 imm, int len);

/* Builds the AES tables; call once before emu_vaes(). */
void isa_emu_core_init(void);

/* SHA-NI. All operate on 128-bit values; `dst` is also the first source. */
void emu_sha1rnds4(u8 *dst, const u8 *src, u8 imm);
void emu_sha1nexte(u8 *dst, const u8 *src);
void emu_sha1msg1(u8 *dst, const u8 *src);
void emu_sha1msg2(u8 *dst, const u8 *src);
void emu_sha256rnds2(u8 *dst, const u8 *src, const u8 *xmm0);
void emu_sha256msg1(u8 *dst, const u8 *src);
void emu_sha256msg2(u8 *dst, const u8 *src);

/*
 * ADX (ADCX/ADOX, Broadwell and newer; Haswell lacks it): dst + src + carry_in, at 64 bits when `wide`
 * (REX.W), else at 32 bits with the result zero-extended. *carry_out gets the carry out of the top bit,
 * which the caller stores in CF (ADCX) or OF (ADOX); no other flag changes.
 */
u64 emu_adx(u64 dst, u64 src, int carry_in, int wide, int *carry_out);

#endif
