// SPDX-License-Identifier: GPL-2.0
/*
 *	linux/mm/madvise.c
 *
 * Copyright (C) 1999  Linus Torvalds
 * Copyright (C) 2002  Christoph Hellwig
 */

#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>
#include <linux/mempolicy.h>
#include <linux/page-isolation.h>
#include <linux/page_idle.h>
#include <linux/userfaultfd_k.h>
#include <linux/hugetlb.h>
#include <linux/falloc.h>
#include <linux/sched.h>
#include <linux/ksm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/shmem_fs.h>
#include <linux/mmu_notifier.h>
#include <linux/mm_stat.h>
#include <linux/types.h>
#include <linux/timekeeping.h>
#include "linux/mm.h"

#include <asm/tlb.h>

#include "internal.h"

/*
 * Any behaviour which results in changes to the vma->vm_flags needs to
 * take mmap_sem for writing. Others, which simply traverse vmas, need
 * to only take it for reading.
 */
static int madvise_need_mmap_write(int behavior)
{
	switch (behavior) {
	case MADV_REMOVE:
	case MADV_WILLNEED:
	case MADV_DONTNEED:
	case MADV_COLD:
	case MADV_FREE:
		return 0;
	default:
		/* be safe, default to 1. list exceptions explicitly */
		return 1;
	}
}

/*
 * We can potentially split a vm area into separate
 * areas, each area with its own behavior.
 */
static long madvise_behavior(struct vm_area_struct *vma,
		     struct vm_area_struct **prev,
		     unsigned long start, unsigned long end, int behavior)
{
	struct mm_struct *mm = vma->vm_mm;
	int error = 0;
	pgoff_t pgoff;
	unsigned long new_flags = vma->vm_flags;

	switch (behavior) {
	case MADV_NORMAL:
		new_flags = new_flags & ~VM_RAND_READ & ~VM_SEQ_READ;
		break;
	case MADV_SEQUENTIAL:
		new_flags = (new_flags & ~VM_RAND_READ) | VM_SEQ_READ;
		break;
	case MADV_RANDOM:
		new_flags = (new_flags & ~VM_SEQ_READ) | VM_RAND_READ;
		break;
	case MADV_DONTFORK:
		new_flags |= VM_DONTCOPY;
		break;
	case MADV_DOFORK:
		if (vma->vm_flags & VM_IO) {
			error = -EINVAL;
			goto out;
		}
		new_flags &= ~VM_DONTCOPY;
		break;
	case MADV_WIPEONFORK:
		/* MADV_WIPEONFORK is only supported on anonymous memory. */
		if (vma->vm_file || vma->vm_flags & VM_SHARED) {
			error = -EINVAL;
			goto out;
		}
		new_flags |= VM_WIPEONFORK;
		break;
	case MADV_KEEPONFORK:
		new_flags &= ~VM_WIPEONFORK;
		break;
	case MADV_DONTDUMP:
		new_flags |= VM_DONTDUMP;
		break;
	case MADV_DODUMP:
		if (!is_vm_hugetlb_page(vma) && new_flags & VM_SPECIAL) {
			error = -EINVAL;
			goto out;
		}
		new_flags &= ~VM_DONTDUMP;
		break;
	case MADV_MERGEABLE:
	case MADV_UNMERGEABLE:
		error = ksm_madvise(vma, start, end, behavior, &new_flags);
		if (error) {
			/*
			 * madvise() returns EAGAIN if kernel resources, such as
			 * slab, are temporarily unavailable.
			 */
			if (error == -ENOMEM)
				error = -EAGAIN;
			goto out;
		}
		break;
	case MADV_HUGEPAGE:
	case MADV_NOHUGEPAGE:
		error = hugepage_madvise(vma, &new_flags, behavior);
		if (error) {
			/*
			 * madvise() returns EAGAIN if kernel resources, such as
			 * slab, are temporarily unavailable.
			 */
			if (error == -ENOMEM)
				error = -EAGAIN;
			goto out;
		}
		break;
	}

	if (new_flags == vma->vm_flags) {
		*prev = vma;
		goto out;
	}

	pgoff = vma->vm_pgoff + ((start - vma->vm_start) >> PAGE_SHIFT);
	*prev = vma_merge(mm, *prev, start, end, new_flags, vma->anon_vma,
			  vma->vm_file, pgoff, vma_policy(vma),
			  vma->vm_userfaultfd_ctx, vma_get_anon_name(vma));
	if (*prev) {
		vma = *prev;
		goto success;
	}

	*prev = vma;

	if (start != vma->vm_start) {
		if (unlikely(mm->map_count >= sysctl_max_map_count)) {
			error = -ENOMEM;
			goto out;
		}
		error = __split_vma(mm, vma, start, 1);
		if (error) {
			/*
			 * madvise() returns EAGAIN if kernel resources, such as
			 * slab, are temporarily unavailable.
			 */
			if (error == -ENOMEM)
				error = -EAGAIN;
			goto out;
		}
	}

	if (end != vma->vm_end) {
		if (unlikely(mm->map_count >= sysctl_max_map_count)) {
			error = -ENOMEM;
			goto out;
		}
		error = __split_vma(mm, vma, end, 0);
		if (error) {
			/*
			 * madvise() returns EAGAIN if kernel resources, such as
			 * slab, are temporarily unavailable.
			 */
			if (error == -ENOMEM)
				error = -EAGAIN;
			goto out;
		}
	}

success:
	/*
	 * vm_flags is protected by the mmap_sem held in write mode.
	 */
	vma->vm_flags = new_flags;
out:
	return error;
}

#ifdef CONFIG_SWAP
static int swapin_walk_pmd_entry(pmd_t *pmd, unsigned long start,
	unsigned long end, struct mm_walk *walk)
{
	pte_t *orig_pte;
	struct vm_area_struct *vma = walk->private;
	unsigned long index;

	if (pmd_none_or_trans_huge_or_clear_bad(pmd))
		return 0;

	for (index = start; index != end; index += PAGE_SIZE) {
		pte_t pte;
		swp_entry_t entry;
		struct page *page;
		spinlock_t *ptl;

		orig_pte = pte_offset_map_lock(vma->vm_mm, pmd, start, &ptl);
		pte = *(orig_pte + ((index - start) / PAGE_SIZE));
		pte_unmap_unlock(orig_pte, ptl);

		if (pte_present(pte) || pte_none(pte))
			continue;
		entry = pte_to_swp_entry(pte);
		if (unlikely(non_swap_entry(entry)))
			continue;

		page = read_swap_cache_async(entry, GFP_HIGHUSER_MOVABLE,
							vma, index, false);
		if (page)
			put_page(page);
	}

	return 0;
}

static void force_swapin_readahead(struct vm_area_struct *vma,
		unsigned long start, unsigned long end)
{
	struct mm_walk walk = {
		.mm = vma->vm_mm,
		.pmd_entry = swapin_walk_pmd_entry,
		.private = vma,
	};

	walk_page_range(start, end, &walk);

	lru_add_drain();	/* Push any new pages onto the LRU now */
}

static void force_shm_swapin_readahead(struct vm_area_struct *vma,
		unsigned long start, unsigned long end,
		struct address_space *mapping)
{
	pgoff_t index;
	struct page *page;
	swp_entry_t swap;

	for (; start < end; start += PAGE_SIZE) {
		index = ((start - vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;

		page = find_get_entry(mapping, index);
		if (!radix_tree_exceptional_entry(page)) {
			if (page)
				put_page(page);
			continue;
		}
		swap = radix_to_swp_entry(page);
		page = read_swap_cache_async(swap, GFP_HIGHUSER_MOVABLE,
							NULL, 0, false);
		if (page)
			put_page(page);
	}

	lru_add_drain();	/* Push any new pages onto the LRU now */
}
#endif		/* CONFIG_SWAP */

/*
 * Schedule all required I/O operations.  Do not wait for completion.
 */
static long madvise_willneed(struct vm_area_struct *vma,
			     struct vm_area_struct **prev,
			     unsigned long start, unsigned long end)
{
	struct file *file = vma->vm_file;

	*prev = vma;
#ifdef CONFIG_SWAP
	if (!file) {
		force_swapin_readahead(vma, start, end);
		return 0;
	}

	if (shmem_mapping(file->f_mapping)) {
		force_shm_swapin_readahead(vma, start, end,
					file->f_mapping);
		return 0;
	}
#else
	if (!file)
		return -EBADF;
#endif

	if (IS_DAX(file_inode(file))) {
		/* no bad return value, but ignore advice */
		return 0;
	}

	start = ((start - vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;
	if (end > vma->vm_end)
		end = vma->vm_end;
	end = ((end - vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;

	force_page_cache_readahead(file->f_mapping, file, start, end - start);
	return 0;
}

static int madvise_cold_pte_range(pmd_t *pmd, unsigned long addr,
				unsigned long end, struct mm_walk *walk)
{
	struct mmu_gather *tlb = walk->private;
	struct mm_struct *mm = tlb->mm;
	struct vm_area_struct *vma = walk->vma;
	pte_t *orig_pte, *pte, ptent;
	spinlock_t *ptl;
	struct page *page;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (pmd_trans_huge(*pmd)) {
		pmd_t orig_pmd;
		unsigned long next = pmd_addr_end(addr, end);

		tlb_change_page_size(tlb, HPAGE_PMD_SIZE);
		ptl = pmd_trans_huge_lock(pmd, vma);
		if (!ptl)
			return 0;

		orig_pmd = *pmd;
		if (is_huge_zero_pmd(orig_pmd))
			goto huge_unlock;

		if (unlikely(!pmd_present(orig_pmd))) {
			VM_BUG_ON(thp_migration_supported() &&
					!is_pmd_migration_entry(orig_pmd));
			goto huge_unlock;
		}

		page = pmd_page(orig_pmd);
		if (next - addr != HPAGE_PMD_SIZE) {
			int err;

			if (page_mapcount(page) != 1)
				goto huge_unlock;

			get_page(page);
			spin_unlock(ptl);
			lock_page(page);
			err = split_huge_page(page);
			unlock_page(page);
			put_page(page);
			if (!err)
				goto regular_page;
			return 0;
		}

		if (pmd_young(orig_pmd)) {
			pmdp_invalidate(vma, addr, pmd);
			orig_pmd = pmd_mkold(orig_pmd);

			set_pmd_at(mm, addr, pmd, orig_pmd);
			tlb_remove_pmd_tlb_entry(tlb, pmd, addr);
		}

		test_and_clear_page_young(page);
		deactivate_page(page);
huge_unlock:
		spin_unlock(ptl);
		return 0;
	}

	if (pmd_trans_unstable(pmd))
		return 0;
regular_page:
#endif
	tlb_remove_check_page_size_change(tlb, PAGE_SIZE);
	orig_pte = pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	flush_tlb_batched_pending(mm);
	arch_enter_lazy_mmu_mode();
	for (; addr < end; pte++, addr += PAGE_SIZE) {
		ptent = *pte;

		if (pte_none(ptent))
			continue;

		if (!pte_present(ptent))
			continue;

		page = vm_normal_page(vma, addr, ptent);
		if (!page)
			continue;

		/*
		 * Creating a THP page is expensive so split it only if we
		 * are sure it's worth. Split it if we are only owner.
		 */
		if (PageTransCompound(page)) {
			if (page_mapcount(page) != 1)
				break;
			get_page(page);
			if (!trylock_page(page)) {
				put_page(page);
				break;
			}
			pte_unmap_unlock(orig_pte, ptl);
			if (split_huge_page(page)) {
				unlock_page(page);
				put_page(page);
				pte_offset_map_lock(mm, pmd, addr, &ptl);
				break;
			}
			unlock_page(page);
			put_page(page);
			pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
			pte--;
			addr -= PAGE_SIZE;
			continue;
		}

		VM_BUG_ON_PAGE(PageTransCompound(page), page);

		if (pte_young(ptent)) {
			ptent = ptep_get_and_clear_full(mm, addr, pte,
							tlb->fullmm);
			ptent = pte_mkold(ptent);
			set_pte_at(mm, addr, pte, ptent);
			tlb_remove_tlb_entry(tlb, pte, addr);
		}

		/*
		 * We are deactivating a page for accelerating reclaiming.
		 * VM couldn't reclaim the page unless we clear PG_young.
		 * As a side effect, it makes confuse idle-page tracking
		 * because they will miss recent referenced history.
		 */
		test_and_clear_page_young(page);
		deactivate_page(page);
	}

	arch_leave_lazy_mmu_mode();
	pte_unmap_unlock(orig_pte, ptl);
	cond_resched();

	return 0;
}

static void madvise_cold_page_range(struct mmu_gather *tlb,
			     struct vm_area_struct *vma,
			     unsigned long addr, unsigned long end)
{
	struct mm_walk cold_walk = {
		.pmd_entry = madvise_cold_pte_range,
		.mm = vma->vm_mm,
		.private = tlb,
	};

	tlb_start_vma(tlb, vma);
	walk_page_range(addr, end, &cold_walk);
	tlb_end_vma(tlb, vma);
}

static long madvise_cold(struct vm_area_struct *vma,
			struct vm_area_struct **prev,
			unsigned long start_addr, unsigned long end_addr)
{
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_gather tlb;

	*prev = vma;
	if (!can_madv_lru_vma(vma))
		return -EINVAL;

	lru_add_drain();
	tlb_gather_mmu(&tlb, mm, start_addr, end_addr);
	madvise_cold_page_range(&tlb, vma, start_addr, end_addr);
	tlb_finish_mmu(&tlb, start_addr, end_addr);

	return 0;
}

static int __maybe_unused madvise_free_pte_range(pmd_t *pmd, unsigned long addr,
				unsigned long end, struct mm_walk *walk)

{
	struct mmu_gather *tlb = walk->private;
	struct mm_struct *mm = tlb->mm;
	struct vm_area_struct *vma = walk->vma;
	spinlock_t *ptl;
	pte_t *orig_pte, *pte, ptent;
	struct page *page;
	int nr_swap = 0;
	unsigned long next;

	next = pmd_addr_end(addr, end);
	if (pmd_trans_huge(*pmd))
		if (madvise_free_huge_pmd(tlb, vma, pmd, addr, next))
			goto next;

	if (pmd_trans_unstable(pmd))
		return 0;

	tlb_remove_check_page_size_change(tlb, PAGE_SIZE);
	orig_pte = pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	flush_tlb_batched_pending(mm);
	arch_enter_lazy_mmu_mode();
	for (; addr != end; pte++, addr += PAGE_SIZE) {
		ptent = *pte;

		if (pte_none(ptent))
			continue;
		/*
		 * If the pte has swp_entry, just clear page table to
		 * prevent swap-in which is more expensive rather than
		 * (page allocation + zeroing).
		 */
		if (!pte_present(ptent)) {
			swp_entry_t entry;

			entry = pte_to_swp_entry(ptent);
			if (non_swap_entry(entry))
				continue;
			nr_swap--;
			free_swap_and_cache(entry);
			pte_clear_not_present_full(mm, addr, pte, tlb->fullmm);
			continue;
		}

		page = _vm_normal_page(vma, addr, ptent, true);
		if (!page)
			continue;

		/*
		 * If pmd isn't transhuge but the page is THP and
		 * is owned by only this process, split it and
		 * deactivate all pages.
		 */
		if (PageTransCompound(page)) {
			if (page_mapcount(page) != 1)
				goto out;
			get_page(page);
			if (!trylock_page(page)) {
				put_page(page);
				goto out;
			}
			pte_unmap_unlock(orig_pte, ptl);
			if (split_huge_page(page)) {
				unlock_page(page);
				put_page(page);
				pte_offset_map_lock(mm, pmd, addr, &ptl);
				goto out;
			}
			unlock_page(page);
			put_page(page);
			pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
			pte--;
			addr -= PAGE_SIZE;
			continue;
		}

		VM_BUG_ON_PAGE(PageTransCompound(page), page);

		if (PageSwapCache(page) || PageDirty(page)) {
			if (!trylock_page(page))
				continue;
			/*
			 * If page is shared with others, we couldn't clear
			 * PG_dirty of the page.
			 */
			if (page_mapcount(page) != 1) {
				unlock_page(page);
				continue;
			}

			if (PageSwapCache(page) && !try_to_free_swap(page)) {
				unlock_page(page);
				continue;
			}

			ClearPageDirty(page);
			unlock_page(page);
		}

		if (pte_young(ptent) || pte_dirty(ptent)) {
			/*
			 * Some of architecture(ex, PPC) don't update TLB
			 * with set_pte_at and tlb_remove_tlb_entry so for
			 * the portability, remap the pte with old|clean
			 * after pte clearing.
			 */
			ptent = ptep_get_and_clear_full(mm, addr, pte,
							tlb->fullmm);

			ptent = pte_mkold(ptent);
			ptent = pte_mkclean(ptent);
			set_pte_at(mm, addr, pte, ptent);
			tlb_remove_tlb_entry(tlb, pte, addr);
		}
		mark_page_lazyfree(page);
	}
out:
	if (nr_swap) {
		if (current->mm == mm)
			sync_mm_rss(mm);

		add_mm_counter(mm, MM_SWAPENTS, nr_swap);
	}
	arch_leave_lazy_mmu_mode();
	pte_unmap_unlock(orig_pte, ptl);
	cond_resched();
next:
	return 0;
}

static int __maybe_unused madvise_free_pte_range_profiling(pmd_t *pmd, unsigned long addr,
				unsigned long end, struct mm_walk *walk)

{
	struct mmu_gather *tlb = walk->private;
	struct mm_struct *mm = tlb->mm;
	struct vm_area_struct *vma = walk->vma;
	spinlock_t *ptl;
	pte_t *orig_pte, *pte, ptent;
	struct page *page;
	int nr_swap = 0;
	unsigned long next;

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	uint64_t *madv_breakdown = walk->madv_breakdown;
	uint64_t ts_stt = 0;
	uint64_t ts_walk_pmd_stt = ktime_get_ns();
	uint64_t ts_walk_ptes_stt = 0;
	uint64_t ts_pte_not_p = 0, ts_page_lock = 0, ts_make_pte = 0, ts_lru = 0;
#endif

	next = pmd_addr_end(addr, end);
	if (pmd_trans_huge(*pmd))
		if (madvise_free_huge_pmd(tlb, vma, pmd, addr, next))
			goto next;

	if (pmd_trans_unstable(pmd)) {
#ifdef PROFILE_MADV_FREE_BREAKDOWN
		// yizhe: ADC_MADV_FREE_WALK_PMD Begin
		adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_WALK_PMD, ktime_get_ns() - ts_walk_pmd_stt);
		// yizhe: ADC_MADV_FREE_WALK_PMD End
#endif
		return 0;
	}

	tlb_remove_check_page_size_change(tlb, PAGE_SIZE);

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_PTE_LOCK Start
	ts_stt = ktime_get_ns();
	orig_pte = pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_PTE_LOCK, ktime_get_ns() - ts_stt);
	// yizhe: ADC_MADV_FREE_PTE_LOCK End
#else
	orig_pte = pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
#endif

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_FLUSH_TLB_PEND Start
	ts_stt = ktime_get_ns();
	flush_tlb_batched_pending(mm);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_FLUSH_TLB_PEND, ktime_get_ns() - ts_stt);
	// yizhe: ADC_MADV_FREE_FLUSH_TLB_PEND End
#else
	flush_tlb_batched_pending(mm);
#endif
	arch_enter_lazy_mmu_mode();

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	ts_walk_ptes_stt = ktime_get_ns();
#endif
	for (; addr != end; pte++, addr += PAGE_SIZE) {
		ptent = *pte;

		if (pte_none(ptent))
			continue;
		/*
		 * If the pte has swp_entry, just clear page table to
		 * prevent swap-in which is more expensive rather than
		 * (page allocation + zeroing).
		 */
#ifdef PROFILE_MADV_FREE_BREAKDOWN
		// yizhe: ADC_MADV_FREE_PTE_NOT_P Begin
		ts_stt = ktime_get_ns();
#endif
		if (!pte_present(ptent)) {
			swp_entry_t entry;

			entry = pte_to_swp_entry(ptent);
			if (non_swap_entry(entry))
				continue;
			nr_swap--;
			free_swap_and_cache(entry);
			pte_clear_not_present_full(mm, addr, pte, tlb->fullmm);
#ifdef PROFILE_MADV_FREE_BREAKDOWN
			ts_pte_not_p += ktime_get_ns() - ts_stt;
#endif
			continue;
		}

		page = _vm_normal_page(vma, addr, ptent, true);
		if (!page)
			continue;

		/*
		 * If pmd isn't transhuge but the page is THP and
		 * is owned by only this process, split it and
		 * deactivate all pages.
		 */
		if (PageTransCompound(page)) {
			if (page_mapcount(page) != 1)
				goto out;
			get_page(page);
			if (!trylock_page(page)) {
				put_page(page);
				goto out;
			}
			pte_unmap_unlock(orig_pte, ptl);
			if (split_huge_page(page)) {
				unlock_page(page);
				put_page(page);
				pte_offset_map_lock(mm, pmd, addr, &ptl);
				goto out;
			}
			unlock_page(page);
			put_page(page);
			pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
			pte--;
			addr -= PAGE_SIZE;
			continue;
		}

		VM_BUG_ON_PAGE(PageTransCompound(page), page);

#ifdef PROFILE_MADV_FREE_BREAKDOWN
		ts_stt = ktime_get_ns();
#endif
		if (PageSwapCache(page) || PageDirty(page)) {
			if (!trylock_page(page)) {
#ifdef PROFILE_MADV_FREE_BREAKDOWN
				ts_page_lock += ktime_get_ns() - ts_stt;
#endif
				continue;
			}
			/*
			 * If page is shared with others, we couldn't clear
			 * PG_dirty of the page.
			 */
			if (page_mapcount(page) != 1) {
				unlock_page(page);
#ifdef PROFILE_MADV_FREE_BREAKDOWN
				ts_page_lock += ktime_get_ns() - ts_stt;
#endif
				continue;
			}

			if (PageSwapCache(page) && !try_to_free_swap(page)) {
				unlock_page(page);
#ifdef PROFILE_MADV_FREE_BREAKDOWN
				ts_page_lock += ktime_get_ns() - ts_stt;
#endif
				continue;
			}

			ClearPageDirty(page);
			unlock_page(page);
		}
#ifdef PROFILE_MADV_FREE_BREAKDOWN
		ts_page_lock += ktime_get_ns() - ts_stt;
		ts_stt = ktime_get_ns();
#endif

		if (pte_young(ptent) || pte_dirty(ptent)) {
			/*
			 * Some of architecture(ex, PPC) don't update TLB
			 * with set_pte_at and tlb_remove_tlb_entry so for
			 * the portability, remap the pte with old|clean
			 * after pte clearing.
			 */
			ptent = ptep_get_and_clear_full(mm, addr, pte,
							tlb->fullmm);

			ptent = pte_mkold(ptent);
			ptent = pte_mkclean(ptent);
			set_pte_at(mm, addr, pte, ptent);
			tlb_remove_tlb_entry(tlb, pte, addr);
		}
#ifdef PROFILE_MADV_FREE_BREAKDOWN
		ts_make_pte += ktime_get_ns() - ts_stt;
		ts_stt = ktime_get_ns();
		mark_page_lazyfree_profiling(page, madv_breakdown);
		ts_lru += ktime_get_ns() - ts_stt;
#else
		mark_page_lazyfree(page);
#endif
	}
out:
	if (nr_swap) {
		if (current->mm == mm)
			sync_mm_rss(mm);

		add_mm_counter(mm, MM_SWAPENTS, nr_swap);
	}
#ifdef PROFILE_MADV_FREE_BREAKDOWN
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_WALK_PTEs, ktime_get_ns() - ts_walk_ptes_stt);
#endif
	arch_leave_lazy_mmu_mode();

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_PTE_LOCK Start
	ts_stt = ktime_get_ns();
	pte_unmap_unlock(orig_pte, ptl);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_PTE_LOCK, ktime_get_ns() - ts_stt);
	// yizhe: ADC_MADV_FREE_PTE_LOCK End
#else
	pte_unmap_unlock(orig_pte, ptl);
#endif
	cond_resched();
next:
#ifdef PROFILE_MADV_FREE_BREAKDOWN
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_PTE_NOT_P,
					ts_pte_not_p);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_PAGE_LOCK,
					ts_page_lock);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_MAKE_PTE,
					ts_make_pte);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_LRU,
					ts_lru);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_WALK_PMD,
					ktime_get_ns() - ts_walk_pmd_stt);
#endif
	return 0;
}

static void __maybe_unused madvise_free_page_range(struct mmu_gather *tlb,
			     struct vm_area_struct *vma,
			     unsigned long addr, unsigned long end)
{
	struct mm_walk free_walk = {
		.pmd_entry = madvise_free_pte_range,
		.mm = vma->vm_mm,
		.private = tlb,
	};

	tlb_start_vma(tlb, vma);
	walk_page_range(addr, end, &free_walk);
	tlb_end_vma(tlb, vma);
}

static void __maybe_unused madvise_free_page_range_profiling(struct mmu_gather *tlb,
			     struct vm_area_struct *vma,
			     unsigned long addr, unsigned long end, uint64_t madv_breakdown[])
{
	struct mm_walk free_walk = {
		.pmd_entry = madvise_free_pte_range_profiling,
		.mm = vma->vm_mm,
		.private = tlb,
#ifdef PROFILE_MADV_FREE_BREAKDOWN
		.madv_breakdown = madv_breakdown,
#endif
	};

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	uint64_t ts_stt = 0;
#endif

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_FLUSH_TLB Begin
	ts_stt = ktime_get_ns();
	tlb_start_vma(tlb, vma);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_FLUSH_TLB, ktime_get_ns() - ts_stt);
	// yizhe: ADC_MADV_FREE_FLUSH_TLB End
#else
	tlb_start_vma(tlb, vma);
#endif

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_WALK_RANGE Start
	ts_stt = ktime_get_ns();
	walk_page_range(addr, end, &free_walk);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_WALK_RANGE, ktime_get_ns() - ts_stt);
	// yizhe: ADC_MADV_FREE_WALK_RANGE End
#else
	walk_page_range(addr, end, &free_walk);
#endif

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_FLUSH_TLB Start
	ts_stt = ktime_get_ns();
	tlb_end_vma(tlb, vma);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_FLUSH_TLB, ktime_get_ns() - ts_stt);
	// yizhe: ADC_MADV_FREE_FLUSH_TLB End
#else
	tlb_end_vma(tlb, vma);
#endif
}

static int __maybe_unused madvise_free_single_vma(struct vm_area_struct *vma,
			unsigned long start_addr, unsigned long end_addr)
{
	unsigned long start, end;
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_gather tlb;

	/* MADV_FREE works for only anon vma at the moment */
	if (!vma_is_anonymous(vma))
		return -EINVAL;

	start = max(vma->vm_start, start_addr);
	if (start >= vma->vm_end)
		return -EINVAL;
	end = min(vma->vm_end, end_addr);
	if (end <= vma->vm_start)
		return -EINVAL;

	lru_add_drain();
	tlb_gather_mmu(&tlb, mm, start, end);
	update_hiwater_rss(mm);

	mmu_notifier_invalidate_range_start(mm, start, end);
	madvise_free_page_range(&tlb, vma, start, end);
	mmu_notifier_invalidate_range_end(mm, start, end);
	tlb_finish_mmu(&tlb, start, end);

	return 0;
}

static int __maybe_unused madvise_free_single_vma_profiling(struct vm_area_struct *vma,
			unsigned long start_addr, unsigned long end_addr, uint64_t madv_breakdown[])
{
	unsigned long start, end;
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_gather tlb;

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	uint64_t ts_stt = 0;
#endif
	/* MADV_FREE works for only anon vma at the moment */
	if (!vma_is_anonymous(vma))
		return -EINVAL;

	start = max(vma->vm_start, start_addr);
	if (start >= vma->vm_end)
		return -EINVAL;
	end = min(vma->vm_end, end_addr);
	if (end <= vma->vm_start)
		return -EINVAL;

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_LRU_DRAIN Begin
	ts_stt = ktime_get_ns();
	lru_add_drain();
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_LRU_DRAIN, ktime_get_ns() - ts_stt);
	// yizhe: ADC_MADV_FREE_LRU_DRAIN End
#else
	lru_add_drain();
#endif
	tlb_gather_mmu(&tlb, mm, start, end);
	update_hiwater_rss(mm);

	mmu_notifier_invalidate_range_start(mm, start, end);
#ifdef PROFILE_MADV_FREE_BREAKDOWN
	madvise_free_page_range_profiling(&tlb, vma, start, end, madv_breakdown);
#else
	madvise_free_page_range(&tlb, vma, start, end);
#endif
	mmu_notifier_invalidate_range_end(mm, start, end);
	
#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_FLUSH_TLB Start
	ts_stt = ktime_get_ns();
	tlb_finish_mmu(&tlb, start, end);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_FLUSH_TLB, ktime_get_ns() - ts_stt);
	// yizhe: ADC_MADV_FREE_FLUSH_TLB End
#else
	tlb_finish_mmu(&tlb, start, end);
#endif
	return 0;
}

/*
 * Application no longer needs these pages.  If the pages are dirty,
 * it's OK to just throw them away.  The app will be more careful about
 * data it wants to keep.  Be sure to free swap resources too.  The
 * zap_page_range call sets things up for shrink_active_list to actually free
 * these pages later if no one else has touched them in the meantime,
 * although we could add these pages to a global reuse list for
 * shrink_active_list to pick up before reclaiming other pages.
 *
 * NB: This interface discards data rather than pushes it out to swap,
 * as some implementations do.  This has performance implications for
 * applications like large transactional databases which want to discard
 * pages in anonymous maps after committing to backing store the data
 * that was kept in them.  There is no reason to write this data out to
 * the swap area if the application is discarding it.
 *
 * An interface that causes the system to free clean pages and flush
 * dirty pages is already available as msync(MS_INVALIDATE).
 */
static long madvise_dontneed_single_vma(struct vm_area_struct *vma,
					unsigned long start, unsigned long end)
{
	zap_page_range(vma, start, end - start);
	return 0;
}

static long __maybe_unused madvise_dontneed_free(struct vm_area_struct *vma,
				  struct vm_area_struct **prev,
				  unsigned long start, unsigned long end,
				  int behavior)
{
	*prev = vma;
	if (!can_madv_dontneed_vma(vma))
		return -EINVAL;

	if (!userfaultfd_remove(vma, start, end)) {
		*prev = NULL; /* mmap_sem has been dropped, prev is stale */

		down_read(&current->mm->mmap_sem);
		vma = find_vma(current->mm, start);
		if (!vma)
			return -ENOMEM;
		if (start < vma->vm_start) {
			/*
			 * This "vma" under revalidation is the one
			 * with the lowest vma->vm_start where start
			 * is also < vma->vm_end. If start <
			 * vma->vm_start it means an hole materialized
			 * in the user address space within the
			 * virtual range passed to MADV_DONTNEED
			 * or MADV_FREE.
			 */
			return -ENOMEM;
		}
		if (!can_madv_dontneed_vma(vma))
			return -EINVAL;
		if (end > vma->vm_end) {
			/*
			 * Don't fail if end > vma->vm_end. If the old
			 * vma was splitted while the mmap_sem was
			 * released the effect of the concurrent
			 * operation may not cause madvise() to
			 * have an undefined result. There may be an
			 * adjacent next vma that we'll walk
			 * next. userfaultfd_remove() will generate an
			 * UFFD_EVENT_REMOVE repetition on the
			 * end-vma->vm_end range, but the manager can
			 * handle a repetition fine.
			 */
			end = vma->vm_end;
		}
		VM_WARN_ON(start >= end);
	}

	if (behavior == MADV_DONTNEED)
		return madvise_dontneed_single_vma(vma, start, end);
	else if (behavior == MADV_FREE)
		return madvise_free_single_vma(vma, start, end);
	else
		return -EINVAL;
}

static long __maybe_unused madvise_dontneed_free_profiling(struct vm_area_struct *vma,
				  struct vm_area_struct **prev,
				  unsigned long start, unsigned long end,
				  int behavior, uint64_t madv_breakdown[])
{
	*prev = vma;
	if (!can_madv_dontneed_vma(vma))
		return -EINVAL;

	if (!userfaultfd_remove(vma, start, end)) {
		*prev = NULL; /* mmap_sem has been dropped, prev is stale */

		down_read(&current->mm->mmap_sem);
		vma = find_vma(current->mm, start);
		if (!vma)
			return -ENOMEM;
		if (start < vma->vm_start) {
			/*
			 * This "vma" under revalidation is the one
			 * with the lowest vma->vm_start where start
			 * is also < vma->vm_end. If start <
			 * vma->vm_start it means an hole materialized
			 * in the user address space within the
			 * virtual range passed to MADV_DONTNEED
			 * or MADV_FREE.
			 */
			return -ENOMEM;
		}
		if (!can_madv_dontneed_vma(vma))
			return -EINVAL;
		if (end > vma->vm_end) {
			/*
			 * Don't fail if end > vma->vm_end. If the old
			 * vma was splitted while the mmap_sem was
			 * released the effect of the concurrent
			 * operation may not cause madvise() to
			 * have an undefined result. There may be an
			 * adjacent next vma that we'll walk
			 * next. userfaultfd_remove() will generate an
			 * UFFD_EVENT_REMOVE repetition on the
			 * end-vma->vm_end range, but the manager can
			 * handle a repetition fine.
			 */
			end = vma->vm_end;
		}
		VM_WARN_ON(start >= end);
	}

	if (behavior == MADV_DONTNEED)
		return madvise_dontneed_single_vma(vma, start, end);
	else if (behavior == MADV_FREE){
#ifdef PROFILE_MADV_FREE_BREAKDOWN
		long ret = 0;
		uint64_t ts_stt = 0;
		// yizhe: ADC_MADV_FREE_SINGLE_VMA Begin
		ts_stt = ktime_get_ns();
		ret = madvise_free_single_vma_profiling(vma, start, end, madv_breakdown);
		adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_SINGLE_VMA, ktime_get_ns() - ts_stt);
		// yizhe: ADC_MADV_FREE_SINGLE_VMA End
		return ret;
#else
		return madvise_free_single_vma(vma, start, end);
#endif
	}
	else
		return -EINVAL;
}

/*
 * Application wants to free up the pages and associated backing store.
 * This is effectively punching a hole into the middle of a file.
 */
static long madvise_remove(struct vm_area_struct *vma,
				struct vm_area_struct **prev,
				unsigned long start, unsigned long end)
{
	loff_t offset;
	int error;
	struct file *f;

	*prev = NULL;	/* tell sys_madvise we drop mmap_sem */

	if (vma->vm_flags & VM_LOCKED)
		return -EINVAL;

	f = vma->vm_file;

	if (!f || !f->f_mapping || !f->f_mapping->host) {
			return -EINVAL;
	}

	if ((vma->vm_flags & (VM_SHARED|VM_WRITE)) != (VM_SHARED|VM_WRITE))
		return -EACCES;

	offset = (loff_t)(start - vma->vm_start)
			+ ((loff_t)vma->vm_pgoff << PAGE_SHIFT);

	/*
	 * Filesystem's fallocate may need to take i_mutex.  We need to
	 * explicitly grab a reference because the vma (and hence the
	 * vma's reference to the file) can go away as soon as we drop
	 * mmap_sem.
	 */
	get_file(f);
	if (userfaultfd_remove(vma, start, end)) {
		/* mmap_sem was not released by userfaultfd_remove() */
		up_read(&current->mm->mmap_sem);
	}
	error = vfs_fallocate(f,
				FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				offset, end - start);
	fput(f);
	down_read(&current->mm->mmap_sem);
	return error;
}

#ifdef CONFIG_MEMORY_FAILURE
/*
 * Error injection support for memory error handling.
 */
static int madvise_inject_error(int behavior,
		unsigned long start, unsigned long end)
{
	struct page *page;
	struct zone *zone;
	unsigned int order;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;


	for (; start < end; start += PAGE_SIZE << order) {
		int ret;

		ret = get_user_pages_fast(start, 1, 0, &page);
		if (ret != 1)
			return ret;

		/*
		 * When soft offlining hugepages, after migrating the page
		 * we dissolve it, therefore in the second loop "page" will
		 * no longer be a compound page, and order will be 0.
		 */
		order = compound_order(compound_head(page));

		if (PageHWPoison(page)) {
			put_page(page);
			continue;
		}

		if (behavior == MADV_SOFT_OFFLINE) {
			pr_info("Soft offlining pfn %#lx at process virtual address %#lx\n",
						page_to_pfn(page), start);

			ret = soft_offline_page(page, MF_COUNT_INCREASED);
			if (ret)
				return ret;
			continue;
		}
		pr_info("Injecting memory failure for pfn %#lx at process virtual address %#lx\n",
						page_to_pfn(page), start);

		ret = memory_failure(page_to_pfn(page), 0, MF_COUNT_INCREASED);
		if (ret)
			return ret;
	}

	/* Ensure that all poisoned pages are removed from per-cpu lists */
	for_each_populated_zone(zone)
		drain_all_pages(zone);

	return 0;
}
#endif

static long __maybe_unused
madvise_vma(struct vm_area_struct *vma, struct vm_area_struct **prev,
		unsigned long start, unsigned long end, int behavior)
{
	switch (behavior) {
	case MADV_REMOVE:
		return madvise_remove(vma, prev, start, end);
	case MADV_WILLNEED:
		return madvise_willneed(vma, prev, start, end);
	case MADV_COLD:
		return madvise_cold(vma, prev, start, end);
	case MADV_FREE:
	case MADV_DONTNEED:
		return madvise_dontneed_free(vma, prev, start, end, behavior);
	default:
		return madvise_behavior(vma, prev, start, end, behavior);
	}
}

static long __maybe_unused
madvise_vma_profiling(struct vm_area_struct *vma, struct vm_area_struct **prev,
		unsigned long start, unsigned long end, int behavior, uint64_t madv_breakdown[])
{
	switch (behavior) {
	case MADV_REMOVE:
		return madvise_remove(vma, prev, start, end);
	case MADV_WILLNEED:
		return madvise_willneed(vma, prev, start, end);
	case MADV_COLD:
		return madvise_cold(vma, prev, start, end);
	case MADV_FREE:
	case MADV_DONTNEED:
#ifdef PROFILE_MADV_FREE_BREAKDOWN
		return madvise_dontneed_free_profiling(vma, prev, start, end, behavior, madv_breakdown);
#else
		return madvise_dontneed_free(vma, prev, start, end, behavior);
#endif
	default:
		return madvise_behavior(vma, prev, start, end, behavior);
	}
}

static bool
madvise_behavior_valid(int behavior)
{
	switch (behavior) {
	case MADV_DOFORK:
	case MADV_DONTFORK:
	case MADV_NORMAL:
	case MADV_SEQUENTIAL:
	case MADV_RANDOM:
	case MADV_REMOVE:
	case MADV_WILLNEED:
	case MADV_DONTNEED:
	case MADV_FREE:
	case MADV_COLD:
#ifdef CONFIG_KSM
	case MADV_MERGEABLE:
	case MADV_UNMERGEABLE:
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case MADV_HUGEPAGE:
	case MADV_NOHUGEPAGE:
#endif
	case MADV_DONTDUMP:
	case MADV_DODUMP:
	case MADV_WIPEONFORK:
	case MADV_KEEPONFORK:
#ifdef CONFIG_MEMORY_FAILURE
	case MADV_SOFT_OFFLINE:
	case MADV_HWPOISON:
#endif
		return true;

	default:
		return false;
	}
}

/*
 * The madvise(2) system call.
 *
 * Applications can use madvise() to advise the kernel how it should
 * handle paging I/O in this VM area.  The idea is to help the kernel
 * use appropriate read-ahead and caching techniques.  The information
 * provided is advisory only, and can be safely disregarded by the
 * kernel without affecting the correct operation of the application.
 *
 * behavior values:
 *  MADV_NORMAL - the default behavior is to read clusters.  This
 *		results in some read-ahead and read-behind.
 *  MADV_RANDOM - the system should read the minimum amount of data
 *		on any access, since it is unlikely that the appli-
 *		cation will need more than what it asks for.
 *  MADV_SEQUENTIAL - pages in the given range will probably be accessed
 *		once, so they can be aggressively read ahead, and
 *		can be freed soon after they are accessed.
 *  MADV_WILLNEED - the application is notifying the system to read
 *		some pages ahead.
 *  MADV_DONTNEED - the application is finished with the given range,
 *		so the kernel can free resources associated with it.
 *  MADV_FREE - the application marks pages in the given range as lazy free,
 *		where actual purges are postponed until memory pressure happens.
 *  MADV_REMOVE - the application wants to free up the given range of
 *		pages and associated backing store.
 *  MADV_DONTFORK - omit this area from child's address space when forking:
 *		typically, to avoid COWing pages pinned by get_user_pages().
 *  MADV_DOFORK - cancel MADV_DONTFORK: no longer omit this area when forking.
 *  MADV_WIPEONFORK - present the child process with zero-filled memory in this
 *              range after a fork.
 *  MADV_KEEPONFORK - undo the effect of MADV_WIPEONFORK
 *  MADV_HWPOISON - trigger memory error handler as if the given memory range
 *		were corrupted by unrecoverable hardware memory failure.
 *  MADV_SOFT_OFFLINE - try to soft-offline the given range of memory.
 *  MADV_MERGEABLE - the application recommends that KSM try to merge pages in
 *		this area with pages of identical content from other such areas.
 *  MADV_UNMERGEABLE- cancel MADV_MERGEABLE: no longer merge pages with others.
 *  MADV_HUGEPAGE - the application wants to back the given range by transparent
 *		huge pages in the future. Existing pages might be coalesced and
 *		new pages might be allocated as THP.
 *  MADV_NOHUGEPAGE - mark the given range as not worth being backed by
 *		transparent huge pages so the existing pages will not be
 *		coalesced into THP and new pages will not be allocated as THP.
 *  MADV_DONTDUMP - the application wants to prevent pages in the given range
 *		from being included in its core dump.
 *  MADV_DODUMP - cancel MADV_DONTDUMP: no longer exclude from core dump.
 *
 * return values:
 *  zero    - success
 *  -EINVAL - start + len < 0, start is not page-aligned,
 *		"behavior" is not a valid value, or application
 *		is attempting to release locked or shared pages,
 *		or the specified address range includes file, Huge TLB,
 *		MAP_SHARED or VMPFNMAP range.
 *  -ENOMEM - addresses in the specified range are not currently
 *		mapped, or are outside the AS of the process.
 *  -EIO    - an I/O error occurred while paging in data.
 *  -EBADF  - map exists, but area maps something that isn't a file.
 *  -EAGAIN - a kernel resource was temporarily unavailable.
 */
SYSCALL_DEFINE3(madvise, unsigned long, start, size_t, len_in, int, behavior)
{
	return do_madvise(current->mm, start, len_in, behavior);
}

static inline int do_madvise_profiling(struct mm_struct *mm, unsigned long start, size_t len_in, int behavior, int* adc_pf_bits, uint64_t* madv_breakdown) {
	unsigned long end, tmp;
	struct vm_area_struct *vma, *prev;
	int unmapped_error = 0;
	int error = -EINVAL;
	int write;
	size_t len;
	struct blk_plug plug;

	start = untagged_addr(start);

	if (!madvise_behavior_valid(behavior))
		return error;

	if (start & ~PAGE_MASK)
		return error;
	len = (len_in + ~PAGE_MASK) & PAGE_MASK;

	/* Check to see whether len was rounded up from small -ve to zero */
	if (len_in && !len)
		return error;

	end = start + len;
	if (end < start)
		return error;

	error = 0;
	if (end == start)
		return error;

#ifdef CONFIG_MEMORY_FAILURE
	if (behavior == MADV_HWPOISON || behavior == MADV_SOFT_OFFLINE)
		return madvise_inject_error(behavior, start, start + len_in);
#endif
#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_MMAP_LOCK Begin
	if (behavior == MADV_FREE) {
		uint64_t ts_stt = ktime_get_ns();
		adc_madv_breakdown_stt(madv_breakdown, ADC_MADV_FREE_MMAP_LOCK, ts_stt);
	}
#endif
	write = madvise_need_mmap_write(behavior);
	if (write) {
		if (down_write_killable(&current->mm->mmap_sem))
			return -EINTR;
	} else {
		down_read(&current->mm->mmap_sem);
	}
#ifdef PROFILE_MADV_FREE_BREAKDOWN
	if (behavior == MADV_FREE) {
		uint64_t ts_edt = ktime_get_ns();
		adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_MMAP_LOCK, ts_edt);
	}
	// yizhe: ADC_MADV_FREE_MMAP_LOCK End
#endif

	/*
	 * If the interval [start,end) covers some unmapped address
	 * ranges, just ignore them, but return -ENOMEM at the end.
	 * - different from the way of handling in mlock etc.
	 */
	vma = find_vma_prev(current->mm, start, &prev);
	if (vma && start > vma->vm_start)
		prev = vma;

	blk_start_plug(&plug);
	for (;;) {
		/* Still start < end. */
		error = -ENOMEM;
		if (!vma)
			goto out;

		/* Here start < (end|vma->vm_end). */
		if (start < vma->vm_start) {
			unmapped_error = -ENOMEM;
			start = vma->vm_start;
			if (start >= end)
				goto out;
		}

		/* Here vma->vm_start <= start < (end|vma->vm_end) */
		tmp = vma->vm_end;
		if (end < tmp)
			tmp = end;

		/* Here vma->vm_start <= start < tmp <= (end|vma->vm_end). */
#ifdef PROFILE_MADV_FREE_BREAKDOWN
		error = madvise_vma_profiling(vma, &prev, start, tmp, behavior, madv_breakdown);
#else
		error = madvise_vma(vma, &prev, start, tmp, behavior);
#endif
		if (error)
			goto out;
		start = tmp;
		if (prev && start < prev->vm_end)
			start = prev->vm_end;
		error = unmapped_error;
		if (start >= end)
			goto out;
		if (prev)
			vma = prev->vm_next;
		else	/* madvise_remove dropped mmap_sem */
			vma = find_vma(current->mm, start);
	}
out:
	blk_finish_plug(&plug);

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	// yizhe: ADC_MADV_FREE_MMAP_LOCK Begin
	if (behavior == MADV_FREE) {
		uint64_t ts_stt = ktime_get_ns();
		adc_madv_breakdown_stt(madv_breakdown, ADC_MADV_FREE_MMAP_LOCK, ts_stt);
	}
#endif
	if (write)
		up_write(&current->mm->mmap_sem);
	else
		up_read(&current->mm->mmap_sem);

#ifdef PROFILE_MADV_FREE_BREAKDOWN
	if (behavior == MADV_FREE) {
		uint64_t ts_edt = ktime_get_ns();
		adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_MMAP_LOCK, ts_edt);
	}
	// yizhe: ADC_MADV_FREE_MMAP_LOCK End
#endif
	return error;
}

int __maybe_unused do_madvise(struct mm_struct *mm, unsigned long start, size_t len_in, int behavior)
{
	return do_madvise_profiling(mm, start, len_in, behavior, NULL, NULL);
}

unsigned long __maybe_unused do_adc_madvise_profiling(struct mm_struct *mm, unsigned long start, size_t len_in, int behavior, size_t user_time)
{
#ifdef PROFILE_MADV_FREE_BREAKDOWN
	uint64_t ts_stt = ktime_get_ns();
	int adc_pf_bits = 0;
	uint64_t madv_breakdown[NUM_ADC_MADV_BREAKDOWN_TYPE] = {0};

	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_ENTER_SYSCALL, ts_stt - user_time);

	ts_stt = ktime_get_ns();
	adc_madv_breakdown_stt(madv_breakdown, ADC_MADV_FREE_TOTAL, ts_stt);
	do_madvise_profiling(mm, start, len_in, behavior, &adc_pf_bits, madv_breakdown);
	adc_madv_breakdown_end(madv_breakdown, ADC_MADV_FREE_TOTAL, ktime_get_ns());
	accum_adc_madv_breakdown(madv_breakdown, ADC_MADV_FREE_T);

#ifdef PROFILE_MADV_FREE_BREAKDOWN_PRINT
	printk(KERN_INFO "YYZ : MADV_FREE [ENTER_SYSCALL]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_ENTER_SYSCALL]);
	printk(KERN_INFO "YYZ : MADV_FREE [SINGLE_VMA]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_SINGLE_VMA]);
	printk(KERN_INFO "YYZ : MADV_FREE [LRU]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_LRU]);
	printk(KERN_INFO "YYZ : MADV_FREE [LRU_DRAIN]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_LRU_DRAIN]);
	printk(KERN_INFO "YYZ : MADV_FREE [FLUSH_TLB]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_FLUSH_TLB]);
	printk(KERN_INFO "YYZ : MADV_FREE [FLUSH_TLB_PEND]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_FLUSH_TLB_PEND]);
	printk(KERN_INFO "YYZ : MADV_FREE [WALK_RANGE]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_WALK_RANGE]);
	printk(KERN_INFO "YYZ : MADV_FREE [WALK_PMD]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_WALK_PMD]);
	printk(KERN_INFO "YYZ : MADV_FREE [WALK_PTEs]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_WALK_PTEs]);
	printk(KERN_INFO "YYZ : MADV_FREE [PTE_NOT_P]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_PTE_NOT_P]);
	printk(KERN_INFO "YYZ : MADV_FREE [MAKE_PTE]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_MAKE_PTE]);
	printk(KERN_INFO "YYZ : MADV_FREE [MMAP_LOCK]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_MMAP_LOCK]);
	printk(KERN_INFO "YYZ : MADV_FREE [PTE_LOCK]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_PTE_LOCK]);
	printk(KERN_INFO "YYZ : MADV_FREE [PAGE_LOCK]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_PAGE_LOCK]);
	printk(KERN_INFO "YYZ : MADV_FREE [TOTAL]: %lu ns\n", madv_breakdown[ADC_MADV_FREE_TOTAL]);
#endif /* PROFILE_MADV_FREE_BREAKDOWN_PRINT */
	return ktime_get_ns();
#else
	return do_madvise_profiling(mm, start, len_in, behavior, NULL, NULL);
#endif
}