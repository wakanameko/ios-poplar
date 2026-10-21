#pragma once
#include <stdint.h>

/* Poplar DT: reserved-memory splash_region@9d400000 (cont_splash_mem, 0x2400000).
 * 1080x1920x4 = ~8MB, so it sits inside the 32MB block at 0x9c000000. */
#define FB_PHYS	0x9d400000UL /* sweet: 0x9c000000UL */

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
};

int mmu_enable(struct mmu_info *info);
uint64_t mmu_probe(uintptr_t va);
