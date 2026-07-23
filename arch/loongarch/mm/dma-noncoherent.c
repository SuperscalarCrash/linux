// SPDX-License-Identifier: GPL-2.0
/*
 * Non-coherent DMA support for the Gemmont LA32R core.
 *
 * The data cache implements the architectural index/hit maintenance
 * operations as writeback-and-invalidate operations. Using that operation
 * for every DMA direction is conservative, but keeps ownership transitions
 * correct without advertising cache operations the core does not implement.
 */
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/highmem.h>

#include <asm/addrspace.h>
#include <asm/cacheflush.h>

#define GEMMONT_DCACHE_LINE_SIZE	64

static void gemmont_dma_cache_wback_inv(unsigned long start, size_t size)
{
	unsigned long addr, end;

	if (!size)
		return;

	addr = start & ~(GEMMONT_DCACHE_LINE_SIZE - 1);
	end = ALIGN(start + size, GEMMONT_DCACHE_LINE_SIZE);

	for (; addr < end; addr += GEMMONT_DCACHE_LINE_SIZE)
		cache_op(Hit_Writeback_Inv_LEAF1, addr);
}

void arch_dma_prep_coherent(struct page *page, size_t size)
{
	gemmont_dma_cache_wback_inv((unsigned long)page_address(page), size);
}

void *arch_dma_set_uncached(void *addr, size_t size)
{
	return (void *)TO_UNCACHE((unsigned long)addr);
}

static void gemmont_dma_sync_phys(phys_addr_t paddr, size_t size)
{
	struct page *page = pfn_to_page(PHYS_PFN(paddr));
	unsigned long offset = offset_in_page(paddr);
	size_t left = size;

	while (left) {
		size_t len = min_t(size_t, left, PAGE_SIZE - offset);
		void *addr = kmap_local_page(page);

		gemmont_dma_cache_wback_inv((unsigned long)addr + offset, len);
		kunmap_local(addr);

		offset = 0;
		page++;
		left -= len;
	}
}

void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	gemmont_dma_sync_phys(paddr, size);
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	if (dir != DMA_TO_DEVICE)
		gemmont_dma_sync_phys(paddr, size);
}
