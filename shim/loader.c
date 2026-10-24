/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "inflate.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL
#define XNU_BOOT_MPIDR		0x100UL
#define PSCI_CPU_ON		0xc4000003UL	// big側にしか16KBページング対応のコアがなかったので、かなり修正

// gzip
#define GZ_HDR_SIZE		0x30
#define MAGIC_LEN		16	/* "POPLARXNU" / "POPLARGZ1", NUL padded (mkpayload.py <16s) */
#define GZ_SCRATCH_OFF		0x8000000UL	/* phys_base + 128MB */

struct gzhdr {
	char magic[MAGIC_LEN];		/* "POPLARGZ1" */
	uint64_t phys_base, mem_size;	/* copied from the payload header, needed before inflating */
	uint64_t raw_size, comp_size;
};

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[MAGIC_LEN];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2b0, "payload header layout");
_Static_assert(sizeof(struct gzhdr) == GZ_HDR_SIZE, "gz header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int gz_magic_ok(const char *m)
{
	static const char want[] = "POPLARGZ1";
	for (int i = 0; i < (int)sizeof(want); i++)	/* includes the NUL */
		if (m[i] != want[i])
			return 0;
	return 1;
}

static int magic_ok(const char *m)
{
	static const char want[] = "POPLARXNU";
	for (int i = 0; i < (int)sizeof(want); i++)	/* includes the NUL */
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);
 
static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}
 
/*
 * Handoff to the big core, PSCI passes one 64-bit context id (x0);
 * it points at this block, so entry point and boot_args need no absolute pointers in the image.
 * The core starts with the MMU and caches off. Likes the direct branch below.
 */
static volatile struct {
	uint64_t entry, boot_args, fb;	/* 0x00 0x08 0x10: set by the little core */
	uint64_t cpacr;			/* 0x18: boot core's CPACR_EL1 (FP/SIMD enabled), copied to the big core */
	uint64_t exc;			/* 0x20: 1 once the big core took an exception before xnu set VBAR */
	uint64_t esr, elr, far, spsr;	/* 0x28 0x30 0x38 0x40 */
	uint64_t slot;			/* 0x48: vector slot 0..15 */
	uint64_t cpacr0, sctlr0, cntfrq0;	/* 0x50 0x58 0x60: big core's own values at power-on */
	uint64_t arrived;		/* 0x68: 1 once the big core runs the trampoline */
} handoff __attribute__((aligned(64), used));
_Static_assert(__builtin_offsetof(__typeof__(handoff), arrived) == 0x68, "handoff layout (asm uses offsets)");
 
__asm__(
"	.pushsection .text, \"ax\"\n"
"	.balign 4\n"
"	.globl xnu_big_entry\n"
"xnu_big_entry:\n"
"	msr	daifset, #0xf\n"
"	ic	iallu\n"
"	dsb	sy\n"
"	isb\n"
"	mrs	x9, cpacr_el1\n"		// report the freshly powered core's state
"	str	x9, [x0, #0x50]\n"
"	mrs	x9, sctlr_el1\n"
"	str	x9, [x0, #0x58]\n"
"	mrs	x9, cntfrq_el0\n"
"	str	x9, [x0, #0x60]\n"
"	ldr	x9, [x0, #0x18]\n"		// FP/SIMD as on the boot core: a core started by CPU_ON
"	msr	cpacr_el1, x9\n"		// comes up with FP trapped, and xnu's C code uses SIMD
"	adr	x9, big_vectors\n"		// catch anything xnu hits before it installs its own VBAR
"	msr	vbar_el1, x9\n"
"	isb\n"
"	mov	x9, #1\n"
"	str	x9, [x0, #0x68]\n"
"	dsb	sy\n"
"	ldr	x2, [x0, #16]\n"		// breadcrumb: paint a white band (~60 rows) at the bottom of the screen
"	mov	x3, #-1\n"			// so we can see the big core got here before xnu clears the screen
"	mov	x4, #0x6000\n"
"1:	str	x3, [x2], #8\n"
"	subs	x4, x4, #1\n"
"	b.ne	1b\n"
"	dsb	sy\n"
//  below code for debugging
"	mrs	x5, cntpct_el0\n"		// hold the band on screen for ~2 s (19.2 MHz) before xnu clears it
"	mov	x6, #0xf800\n"
"	movk	x6, #0x124, lsl #16\n"
"	lsl	x6, x6, #1\n"
"	add	x6, x6, x5\n"
"2:	mrs	x5, cntpct_el0\n"
"	cmp	x5, x6\n"
"	b.lo	2b\n"
//
"	ldr	x1, [x0]\n"
"	ldr	x0, [x0, #8]\n"
"	br	x1\n"
/* 16 vector slots; each records the exception for the little core and parks the big core */
"	.balign	0x800\n"
"big_vectors:\n"
"	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
"	.balign	0x80\n"
"	mov	x9, #\\n\n"
"	b	big_exc\n"
"	.endr\n"
"big_exc:\n"
"	adr	x10, handoff\n"
"	mrs	x11, esr_el1\n"
"	str	x11, [x10, #0x28]\n"
"	mrs	x11, elr_el1\n"
"	str	x11, [x10, #0x30]\n"
"	mrs	x11, far_el1\n"
"	str	x11, [x10, #0x38]\n"
"	mrs	x11, spsr_el1\n"
"	str	x11, [x10, #0x40]\n"
"	str	x9, [x10, #0x48]\n"
"	mov	x11, #1\n"
"	str	x11, [x10, #0x20]\n"
"	dsb	sy\n"
"3:	wfe\n"
"	b	3b\n"
"	.popsection\n");
extern char xnu_big_entry[];

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;
 
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	int packed = 0;
	const struct gzhdr *g = (const struct gzhdr *)rd_start;
	if (gz_magic_ok(g->magic)) {
		uint64_t scratch = g->phys_base + GZ_SCRATCH_OFF;
		size_t outlen = 0;

		if (g->raw_size < sizeof(struct payload) || GZ_SCRATCH_OFF + g->raw_size > g->mem_size)
			return "gz payload does not fit in xnu ram";
		if (GZ_HDR_SIZE + g->comp_size > rd_end - rd_start)
			return "gz payload truncated";
		con_hex2("INFLATE", "comp=", g->comp_size, "raw=", g->raw_size);
		con_hex2("INFLATE", "to4k=", scratch >> 12, 0, 0);
		int rc = inflate_raw((const uint8_t *)(rd_start + GZ_HDR_SIZE), g->comp_size,
				     (uint8_t *)scratch, g->raw_size, &outlen);
		if (rc || outlen != g->raw_size) {
			con_hex2("INFLATE", "rc=", (uint32_t)rc, "out=", (uint32_t)outlen);
			return "inflate failed";
		}
		con_str("INFLATE", "done");
		rd_start = scratch;
		rd_end = scratch + g->raw_size;
		packed = 1;
	}

	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic))
		return "no POPLARXNU payload";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";
 
	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);
 
	/* Everything the loader will write or xnu will own (some segments sit below phys_base). */
	uint64_t lo = p->phys_base, hi = p->phys_base + p->mem_size;
	for (uint32_t i = 0; i < p->nsegs; i++) {
		uint64_t d = p->segs[i].vmaddr + pa_of_va_base;
		if (d < lo)
			lo = d;
	}
	con_hex2("RANGE", "lo4k=", lo >> 12, "hi4k=", hi >> 12);

	// The payload must not overlap where the kernel goes.
	if (packed) {
		// The inflated copy lives in xnu's RAM: it only has to stay clear of what we write.
		if (rd_start < p->kernel_end + pa_of_va_base + p->adt_size + 2 * PAGE_16K)
			return "inflated payload overlaps kernel image";
	} else if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size) {
		return "ramdisk overlaps kernel memory";
	}
 
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		char sl[4] = { 'S', '0' + i / 10 % 10, '0' + i % 10, 0 };
		con_hex2(sl, "pa4k=", dst >> 12, "sz4k=", s->vmsize >> 12);	/* last line shown = last thing touched */
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);
 
	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	con_hex2("ADT", "pa4k=", adt_pa >> 12, "sz4k=", p->adt_size >> 12);
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;
 
	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);
 
	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");
 
	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
 
	uint64_t mpidr;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	if (((mpidr >> 8) & 0xff) == (XNU_BOOT_MPIDR >> 8)) {
		/* Already on the big cluster: jump directly. */
		((xnu_entry_t)entry_pa)(ba_pa);
		return "xnu returned";
	}
 
	handoff.entry = entry_pa;
	handoff.boot_args = ba_pa;
	handoff.fb = (uintptr_t)con_fb + (uint64_t)con_stride * (con_h - 60);
	uint64_t cpacr;
	__asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
	handoff.cpacr = cpacr | (3UL << 20);	/* FPEN = 0b11: no FP/SIMD trap at EL1/EL0 */
	handoff.exc = 0;
	handoff.arrived = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	int64_t r = (int64_t)smc(PSCI_CPU_ON, XNU_BOOT_MPIDR, (uintptr_t)xnu_big_entry,
				 (uintptr_t)&handoff);
	if (r) {
		con_hex("CPU_ON", (uint64_t)r);
		return "cannot start big cpu";
	}
	con_str("XNU", "started on big cpu");

	/*
	 * The little core stays as a monitor: it reports the big core's power-on state and any
	 * exception the big core takes before xnu installs its own vectors (until then the big
	 * core runs on big_vectors, which record ESR/ELR/FAR and park it).
	 */
	uint64_t f, t0, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
	do
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	while (!handoff.arrived && t - t0 < f);
	if (!handoff.arrived) {
		con_str("BIGCPU", "never reached the trampoline");
	} else {
		con_hex2("BIGCPU", "cpacr=", handoff.cpacr0, "sctlr=", handoff.sctlr0);
		con_hex2("BIGCPU", "cntfrq=", handoff.cntfrq0, "now=", handoff.cpacr);
	}
	int quiet_reported = 0;
	for (;;) {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
		if (handoff.exc) {
			con_hex2("BIGEXC", "slot=", handoff.slot, "sec=", (t - t0) / f);
			con_hex("ESR", handoff.esr);
			con_hex("ELR", handoff.elr);
			con_hex("FAR", handoff.far);
			con_hex("SPSR", handoff.spsr);
			break;
		}
		if (!quiet_reported && t - t0 > 30 * f) {
			con_str("BIGEXC", "none in 30s (xnu owns VBAR or hangs)");
			quiet_reported = 1;
		}
	}
	for (;;)
	__asm__ volatile("msr daifset, #0xf; wfe");
}
