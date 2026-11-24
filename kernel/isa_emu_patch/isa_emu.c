// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace emulation of x86 instructions that Skylake-family CPUs lack
 * (GFNI, AVX-VNNI(-INT8), SHA-NI, 256-bit VAES/VPCLMULQDQ, AVX-IFMA), and the ADX
 * instructions (ADCX/ADOX) that Haswell lacks.
 * Entered from the #UD handler for user-mode faults. AVX-512 and AMX are not handled.
 */
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/dcache.h>
#include <linux/spinlock.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/stddef.h>
#include <asm/insn.h>
#include <asm/insn-eval.h>
#include <asm/ptrace.h>
#include <asm/traps.h>
#include <asm/fpu/api.h>
#include <asm/fpu/types.h>
#include <asm/fpu/xstate.h>
#include "isa_emu_core.h"
#include "isa_emu.h"

static bool isa_emu_enabled __ro_after_init = true;

static int __init isa_emu_setup(char *s)
{
	isa_emu_enabled = false;
	return 1;
}
__setup("noisaemu", isa_emu_setup);

static int __init isa_emu_boot_init(void)
{
	isa_emu_core_init();
	return 0;
}
early_initcall(isa_emu_boot_init);

enum op_kind {
	K_NONE, K_GF2P8MUL, K_GF2P8AFF, K_GF2P8AFFINV, K_VNNI_BUSD, K_VNNI_WSSD, K_VNNI8,
	K_IFMA_L, K_IFMA_H, K_VAES, K_CLMUL,
	K_SHA1NEXTE, K_SHA1MSG1, K_SHA1MSG2, K_SHA256RNDS2, K_SHA256MSG1, K_SHA256MSG2, K_SHA1RNDS4,
	K_ADCX, K_ADOX,		/* general-purpose registers, handled by try_adx() */
};

struct op {
	enum op_kind kind;
	bool vex, has_imm, three_op;
	int len;            /* 16 or 32 */
	int dst, src1, rm;  /* vector register numbers; rm < 0 means memory */
	int sat, a_signed, b_signed, aes_op;
	u8 imm;
};

/* Classify a decoded instruction; returns false if not one we emulate. */
static bool classify(struct insn *insn, struct op *o)
{
	u8 modrm, opc, pp, m;
	int i, reg, rm, vv = 0, w = 0, l = 0;

	memset(o, 0, sizeof(*o));
	modrm = insn->modrm.value;
	reg = X86_MODRM_REG(modrm);
	rm = X86_MODRM_RM(modrm);

	if (insn->vex_prefix.nbytes) {
		u8 *vp = insn->vex_prefix.bytes;

		if (vp[0] != 0xc4 && vp[0] != 0xc5)
			return false;		/* EVEX/XOP */
		o->vex = true;
		m = insn_vex_m_bits(insn);
		pp = insn_vex_p_bits(insn);
		w = !!X86_VEX_W(vp[2]);
		l = !!X86_VEX_L(vp[2]);
		vv = (~X86_VEX_V(vp[2])) & 0xf;
		if (!X86_VEX_R(vp[1]))
			reg += 8;
		if (insn->vex_prefix.nbytes == 3 && !X86_VEX_B(vp[1]))
			rm += 8;
		if (insn->opcode.nbytes != 1)
			return false;
		opc = insn->opcode.bytes[0];
	} else {
		/* legacy SSE encodings: 0F 38 xx / 0F 3A xx, with optional 66 */
		if (insn->opcode.nbytes != 3 || insn->opcode.bytes[0] != 0x0f)
			return false;
		m = insn->opcode.bytes[1] == 0x38 ? 2 : insn->opcode.bytes[1] == 0x3a ? 3 : 0;
		if (!m)
			return false;
		opc = insn->opcode.bytes[2];
		pp = 0;
		for (i = 0; i < insn->prefixes.nbytes; i++) {
			if (insn->prefixes.bytes[i] == 0x66)
				pp = 1;
			else if (insn->prefixes.bytes[i] == 0xf3)
				pp = 2;
			else if (insn->prefixes.bytes[i] == 0xf2)
				pp = 3;
		}
		if (insn->rex_prefix.nbytes) {
			if (X86_REX_R(insn->rex_prefix.value))
				reg += 8;
			if (X86_REX_B(insn->rex_prefix.value))
				rm += 8;
		}
		/* register-form r/m below; memory form is flagged by mod != 3 */
	}

	o->len = (o->vex && l) ? 32 : 16;
	o->dst = reg;
	o->src1 = o->vex ? vv : reg;
	o->rm = X86_MODRM_MOD(modrm) == 3 ? rm : -1;
	o->three_op = o->vex;

	if (m == 2) {
		if (o->vex) {
			if (pp == 1 && !w && opc == 0xcf) { o->kind = K_GF2P8MUL; return true; }
			if (pp == 1 && !w && opc >= 0x50 && opc <= 0x51) {
				o->kind = K_VNNI_BUSD; o->sat = opc & 1; return true;
			}
			if (pp == 1 && !w && opc >= 0x52 && opc <= 0x53) {
				o->kind = K_VNNI_WSSD; o->sat = opc & 1; return true;
			}
			if (!w && (opc == 0x50 || opc == 0x51) && pp != 1) {
				o->kind = K_VNNI8; o->sat = opc & 1;
				o->a_signed = pp == 3 || pp == 2;	/* F2: bssd, F3: bsud */
				o->b_signed = pp == 3;
				return true;
			}
			if (pp == 1 && w && (opc == 0xb4 || opc == 0xb5)) {
				o->kind = opc == 0xb4 ? K_IFMA_L : K_IFMA_H; return true;
			}
			if (pp == 1 && l && opc >= 0xdc && opc <= 0xdf) {
				o->kind = K_VAES; o->aes_op = opc - 0xdc; return true;
			}
			return false;
		}
		switch (opc) {
		case 0xc8: o->kind = K_SHA1NEXTE; break;
		case 0xc9: o->kind = K_SHA1MSG1; break;
		case 0xca: o->kind = K_SHA1MSG2; break;
		case 0xcb: o->kind = K_SHA256RNDS2; break;
		case 0xcc: o->kind = K_SHA256MSG1; break;
		case 0xcd: o->kind = K_SHA256MSG2; break;
		case 0xcf: if (pp != 1) return false; o->kind = K_GF2P8MUL; break;
		default: return false;
		}
		return true;
	}
	if (m == 3) {
		o->has_imm = true;
		o->imm = insn->immediate.value;
		if (o->vex) {
			if (pp == 1 && w && opc == 0xce) { o->kind = K_GF2P8AFF; return true; }
			if (pp == 1 && w && opc == 0xcf) { o->kind = K_GF2P8AFFINV; return true; }
			if (pp == 1 && l && opc == 0x44) { o->kind = K_CLMUL; return true; }
			return false;
		}
		if (opc == 0xcc && pp == 0) { o->kind = K_SHA1RNDS4; return true; }
		if (pp == 1 && opc == 0xce) { o->kind = K_GF2P8AFF; return true; }
		if (pp == 1 && opc == 0xcf) { o->kind = K_GF2P8AFFINV; return true; }
	}
	return false;
}

/* ---- register file access, on the saved xsave image ---- */

static void vreg_load(struct xregs_state *xs, int n, int len, u8 *out)
{
	if (xs->header.xfeatures & XFEATURE_MASK_SSE)
		memcpy(out, &xs->i387.xmm_space[4 * n], 16);
	else
		memset(out, 0, 16);
	if (len == 32) {
		u8 *hi = get_xsave_addr(xs, XFEATURE_YMM);

		if (hi)
			memcpy(out + 16, hi + 16 * n, 16);
		else
			memset(out + 16, 0, 16);
	}
}

static void vreg_store(struct xregs_state *xs, int n, int len, const u8 *in, bool vex)
{
	if (!(xs->header.xfeatures & XFEATURE_MASK_SSE)) {
		memset(xs->i387.xmm_space, 0, sizeof(xs->i387.xmm_space));
		xs->header.xfeatures |= XFEATURE_MASK_SSE;
	}
	memcpy(&xs->i387.xmm_space[4 * n], in, 16);
	if (vex) {		/* VEX writes zero-extend to the full register */
		u8 *hi = xstate_addr_for_write(xs, XFEATURE_YMM);

		if (hi) {
			if (len == 32)
				memcpy(hi + 16 * n, in + 16, 16);
			else
				memset(hi + 16 * n, 0, 16);
		}
	}
}

static void do_emulate(struct op *o, struct xregs_state *xs, const u8 *mem)
{
	u8 a[32], b[32], d[32];
	int len = o->len;

	vreg_load(xs, o->src1, len, a);
	if (o->rm >= 0)
		vreg_load(xs, o->rm, len, b);
	else
		memcpy(b, mem, len);
	vreg_load(xs, o->dst, len, d);

	switch (o->kind) {
	case K_GF2P8MUL:    emu_gf2p8mulb(d, a, b, len); break;
	case K_GF2P8AFF:    emu_gf2p8affineqb(d, a, b, o->imm, len); break;
	case K_GF2P8AFFINV: emu_gf2p8affineinvqb(d, a, b, o->imm, len); break;
	case K_VNNI_BUSD:   emu_vpdpbusd(d, a, b, len, o->sat); break;
	case K_VNNI_WSSD:   emu_vpdpwssd(d, a, b, len, o->sat); break;
	case K_VNNI8:       emu_vpdp8(d, a, b, len, o->sat, o->a_signed, o->b_signed); break;
	case K_IFMA_L:      emu_vpmadd52(d, a, b, len, 0); break;
	case K_IFMA_H:      emu_vpmadd52(d, a, b, len, 1); break;
	case K_VAES:        emu_vaes(d, a, b, len, o->aes_op); break;
	case K_CLMUL:       emu_vpclmulqdq(d, a, b, o->imm, len); break;
	case K_SHA1NEXTE:   emu_sha1nexte(d, b); break;
	case K_SHA1MSG1:    emu_sha1msg1(d, b); break;
	case K_SHA1MSG2:    emu_sha1msg2(d, b); break;
	case K_SHA1RNDS4:   emu_sha1rnds4(d, b, o->imm); break;
	case K_SHA256MSG1:  emu_sha256msg1(d, b); break;
	case K_SHA256MSG2:  emu_sha256msg2(d, b); break;
	case K_SHA256RNDS2: {
		u8 x0[16];

		vreg_load(xs, 0, 16, x0);
		emu_sha256rnds2(d, b, x0);
		break;
	}
	default: return;
	}
	vreg_store(xs, o->dst, len, d, o->vex);
}

/* ---- ADX: ADCX/ADOX on general-purpose registers (no FPU state) ---- */

/* ModRM register number -> pt_regs slot */
static const unsigned short gpr_off[16] = {
	offsetof(struct pt_regs, ax), offsetof(struct pt_regs, cx), offsetof(struct pt_regs, dx),
	offsetof(struct pt_regs, bx), offsetof(struct pt_regs, sp), offsetof(struct pt_regs, bp),
	offsetof(struct pt_regs, si), offsetof(struct pt_regs, di), offsetof(struct pt_regs, r8),
	offsetof(struct pt_regs, r9), offsetof(struct pt_regs, r10), offsetof(struct pt_regs, r11),
	offsetof(struct pt_regs, r12), offsetof(struct pt_regs, r13), offsetof(struct pt_regs, r14),
	offsetof(struct pt_regs, r15),
};

static unsigned long *gpr(struct pt_regs *regs, int n)
{
	return (unsigned long *)((char *)regs + gpr_off[n]);
}

/*
 * ADCX Gy,Ey = 66 [REX] 0F 38 F6 /r (dst += src + CF, sets CF only)
 * ADOX Gy,Ey = F3 [REX] 0F 38 F6 /r (dst += src + OF, sets OF only)
 * BoringSSL's bn_mulx4x_mont / bn_sqrx8x_internal / ecp_nistz256 *x paths use them unconditionally in
 * Google's builds (compiled for an ADX-capable CPU). The 66 here is a mandatory prefix, not an operand-size
 * override: the operand is 64-bit with REX.W, else 32-bit (zero-extended into the destination).
 * Returns K_ADCX/K_ADOX once the instruction was handled (emulated, or SIGSEGV sent), else K_NONE.
 */
static int try_adx(struct insn *insn, struct pt_regs *regs)
{
	insn_byte_t p;
	u8 modrm, rex = 0;
	int i, pfx = 0, wide, reg, cout, kind;
	unsigned long flag, *dst;
	u64 src = 0;

	if (insn->vex_prefix.nbytes || insn->opcode.nbytes != 3 || insn->opcode.bytes[0] != 0x0f ||
	    insn->opcode.bytes[1] != 0x38 || insn->opcode.bytes[2] != 0xf6)
		return K_NONE;
	for_each_insn_prefix(insn, i, p) {
		if (p == 0x66)
			pfx |= 1;
		else if (p == 0xf3)
			pfx |= 2;
		else if (p == 0xf2 || p == 0xf0)
			return K_NONE;		/* not ADX (LOCK is #UD on real hardware too) */
	}
	if (pfx != 1 && pfx != 2)
		return K_NONE;
	if (insn->rex_prefix.nbytes) {
		if (insn->rex_prefix.nbytes != 1)
			return K_NONE;		/* REX2 (APX) */
		rex = insn->rex_prefix.value;
	}
	kind = pfx == 1 ? K_ADCX : K_ADOX;
	modrm = insn->modrm.value;
	wide = !!X86_REX_W(rex);
	reg = X86_MODRM_REG(modrm) + (X86_REX_R(rex) ? 8 : 0);

	if (X86_MODRM_MOD(modrm) == 3) {
		src = *gpr(regs, X86_MODRM_RM(modrm) + (X86_REX_B(rex) ? 8 : 0));
	} else {
		void __user *addr = insn_get_addr_ref(insn, regs);

		if (addr == (void __user *)-1L)
			return K_NONE;
		if (copy_from_user(&src, addr, wide ? 8 : 4)) {
			force_sig_fault(SIGSEGV, SEGV_MAPERR, addr);
			return kind;
		}
	}

	flag = kind == K_ADCX ? X86_EFLAGS_CF : X86_EFLAGS_OF;
	dst = gpr(regs, reg);
	*dst = emu_adx(*dst, src, !!(regs->flags & flag), wide, &cout);
	if (cout)
		regs->flags |= flag;
	else
		regs->flags &= ~flag;
	regs->ip += insn->length;
	return kind;
}

/* ---- profile: which (file, offset) traps most ---- */
#define PROF_SLOTS 16384
struct prof_ent {
	u64 key;		/* (ino << 24) ^ file offset, never 0 once used */
	u64 off;
	atomic64_t count;
	int kind;
	char path[112];
};
static struct prof_ent prof[PROF_SLOTS];
static DEFINE_SPINLOCK(prof_lock);

static void prof_hit(unsigned long ip, int kind)
{
	struct vm_area_struct *vma;
	struct file *f = NULL;
	u64 off = 0, key;
	char *tmp, *p = NULL;
	unsigned int h, i;

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, ip);
	if (vma && vma->vm_start <= ip) {
		f = vma->vm_file ? get_file(vma->vm_file) : NULL;
		off = ((u64)vma->vm_pgoff << PAGE_SHIFT) + (ip - vma->vm_start);
	}
	mmap_read_unlock(current->mm);
	key = f ? ((u64)file_inode(f)->i_ino << 28) ^ off ^ ((u64)file_inode(f)->i_sb->s_dev << 52) : (u64)ip | (1ULL << 63);
	key |= 1;
	h = (key * 0x9E3779B97F4A7C15ULL) >> 50;
	for (i = 0; i < PROF_SLOTS; i++) {
		struct prof_ent *e = &prof[(h + i) % PROF_SLOTS];

		if (READ_ONCE(e->key) == key) {
			atomic64_inc(&e->count);
			goto out;
		}
		if (!READ_ONCE(e->key)) {
			char name[sizeof(e->path)] = "";

			/* resolve the name outside the lock; d_path may sleep */
			tmp = kmalloc(PATH_MAX, GFP_KERNEL);
			if (f && tmp)
				p = d_path(&f->f_path, tmp, PATH_MAX);
			strscpy(name, IS_ERR_OR_NULL(p) ? (f ? "?" : current->comm) : p, sizeof(name));
			kfree(tmp);
			spin_lock(&prof_lock);
			if (!e->key) {
				memcpy(e->path, name, sizeof(name));
				e->off = off;
				e->kind = kind;
				atomic64_set(&e->count, 1);
				smp_wmb();
				WRITE_ONCE(e->key, key);
				spin_unlock(&prof_lock);
				goto out;
			}
			spin_unlock(&prof_lock);
			i--;	/* slot got taken meanwhile; recheck it */
		}
	}
out:
	if (f)
		fput(f);
}

static void prof_dump(void)
{
	int top[24], n = 0, i, j;

	for (i = 0; i < PROF_SLOTS; i++) {
		long c = READ_ONCE(prof[i].key) ? atomic64_read(&prof[i].count) : 0;

		if (!c)
			continue;
		for (j = n; j > 0 && atomic64_read(&prof[top[j - 1]].count) < c; j--)
			if (j < 24)
				top[j] = top[j - 1];
		if (j < 24) {
			top[j] = i;
			if (n < 24)
				n++;
		}
	}
	for (i = 0; i < n; i++)
		pr_info("isa_emu_prof: %9lld kind=%d off=%#llx %s\n", atomic64_read(&prof[top[i]].count),
			prof[top[i]].kind, prof[top[i]].off, prof[top[i]].path);
}

/* Statistics for one emulated instruction at `ip`. */
static void count_hit(unsigned long ip, int kind)
{
	static atomic64_t total;
	long n = atomic64_inc_return(&total);

	prof_hit(ip, kind);
	if (!(n & 0xfffff)) {
		pr_info("isa_emu: %ld instructions emulated (last: %s[%d] kind=%d)\n",
			n, current->comm, task_pid_nr(current), kind);
		prof_dump();
	}
}

bool isa_emu_handle_ud(struct pt_regs *regs)
{
	unsigned char buf[MAX_INSN_SIZE];
	struct insn insn;
	struct op o;
	u8 mem[32];
	struct xregs_state *xs;
	unsigned long ip = regs->ip;
	int nbuf, kind;

	if (!isa_emu_enabled || !user_mode(regs))
		return false;
	nbuf = insn_fetch_from_user(regs, buf);
	if (nbuf <= 0 || !insn_decode_from_regs(&insn, regs, buf, nbuf))
		return false;
	kind = try_adx(&insn, regs);
	if (kind != K_NONE) {
		count_hit(ip, kind);
		return true;
	}
	if (!boot_cpu_has(X86_FEATURE_XSAVE) || !classify(&insn, &o))
		return false;

	if (o.rm < 0) {
		void __user *addr;

		/* the decoder does not fold VEX.X/B into the REX bits it uses for addressing */
		if (o.vex) {
			u8 *vp = insn.vex_prefix.bytes;
			u8 rex = 0x40;

			if (insn.vex_prefix.nbytes == 3) {
				if (!X86_VEX_X(vp[1]))
					rex |= 2;
				if (!X86_VEX_B(vp[1]))
					rex |= 1;
			}
			insn.rex_prefix.value = rex;
			insn.rex_prefix.nbytes = 1;
		}
		addr = insn_get_addr_ref(&insn, regs);
		if (addr == (void __user *)-1L)
			return false;
		if (copy_from_user(mem, addr, o.len)) {
			force_sig_fault(SIGSEGV, SEGV_MAPERR, addr);
			return true;
		}
	}

	xs = fpu_emu_begin();
	do_emulate(&o, xs, mem);
	fpu_emu_end();

	regs->ip += insn.length;
	count_hit(regs->ip - insn.length, o.kind);
	return true;
}
