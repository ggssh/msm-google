#ifndef __MM_STAT_H__
#define __MM_STAT_H__

#include "linux/types.h"
#include "linux/bitops.h"
#include "linux/kernel.h"
#include "linux/bitmap.h"
#include "linux/gfp.h"
#include "linux/hashtable.h"
#include "linux/sched.h"
#include "linux/spinlock.h"
#include "linux/mm_stat_macro.h"
#include "asm-generic/atomic-long.h"

// yizhe: MADV_FREE breakdown
enum adc_madv_breakdown_type {
	ADC_MADV_FREE_TOTAL,
	ADC_MADV_FREE_SINGLE_VMA,
	ADC_MADV_FREE_LRU,
	ADC_MADV_FREE_LRU_DRAIN,
	ADC_MADV_FREE_FLUSH_TLB,
	ADC_MADV_FREE_FLUSH_TLB_PEND,
	ADC_MADV_FREE_WALK_RANGE,
	ADC_MADV_FREE_WALK_PMD,
	ADC_MADV_FREE_WALK_PTEs,
	ADC_MADV_FREE_PTE_NOT_P,
	ADC_MADV_FREE_MAKE_PTE,
	ADC_MADV_FREE_MMAP_LOCK,
	ADC_MADV_FREE_PTE_LOCK,
	ADC_MADV_FREE_PAGE_LOCK,
	ADC_MADV_FREE_ENTER_SYSCALL,
	NUM_ADC_MADV_BREAKDOWN_TYPE,
};

enum adc_madv_type {
	ADC_MADV_FREE_T,
	NUM_ADC_MADV_TYPE,
};

inline void adc_madv_breakdown_stt(uint64_t *madv_breakdown,
				   enum adc_madv_breakdown_type type,
				   uint64_t ts);
inline void adc_madv_breakdown_end(uint64_t *madv_breakdown,
				   enum adc_madv_breakdown_type type,
				   uint64_t ts);
inline void reset_adc_madv_breakdown(void);
inline void dump_adc_madv_breakdown(void);
inline void accum_adc_madv_breakdown(uint64_t madv_breakdown[],
				     enum adc_madv_type madv_type);

// yizhe: swap stats
struct swap_stats {
	unsigned long swapin_out_heap;
	unsigned long swapin_in_heap;
	unsigned long swapin_in_heap_free;
	unsigned long swapout_out_heap;
	unsigned long swapout_in_heap;
	unsigned long swapout_in_heap_free;
#ifdef ADC_PROFILE_SWAP_OUT_CPUTIME
	unsigned long shrink_page_list_nr_pages;
	unsigned long long shrink_page_list_time_ns;
#endif
};

enum adc_counter_type {
	ADC_SWAPIN_OUT_HEAP,
	ADC_SWAPIN_IN_HEAP,
	ADC_SWAPIN_IN_HEAP_FREE,
	ADC_SWAPOUT_OUT_HEAP,
	ADC_SWAPOUT_IN_HEAP,
	ADC_SWAPOUT_IN_HEAP_FREE,
#ifdef ADC_PROFILE_SWAP_OUT_CPUTIME
	ADC_SHRINK_PAGE_LIST_NR_PAGES,
	ADC_SHRINK_PAGE_LIST_TIME_NS,
#endif
	NUM_ADC_COUNTER_TYPE
};

extern atomic_long_t adc_profile_counters[NUM_ADC_COUNTER_TYPE];

static inline unsigned long get_adc_profile_counter(enum adc_counter_type type)
{
	return (unsigned long)atomic_long_read(&adc_profile_counters[type]);
}

void reset_adc_swap_stats(void);
void get_adc_swap_stats(struct swap_stats *stats);

enum jvm_heap_flag {
	OUT_JVM_HEAP,
	IN_JVM_HEAP,
	IN_JVM_HEAP_FREE,
	NUM_JVM_HEAP_FLAG
};

//
// Use bitmap to manage a consecutive range of virtual address.
// The address range is splited into pages with same and fixed size.
// Each bit maps to a page.
//
// Address view     base         base +         base +                                 end
//                             page_size   2*page_size
// Page view       |   page 0   |   page 1   |   ...   | page (page_number -1) |
// Bitmap view       |   bit 0      |   bit 1      |   ...   | bit (page_number -1)    |
//
// [base, end)
struct adc_page_bitmap {
	unsigned long base; // start address of consecutive pages
	unsigned long end; // end address of consecutive pages
	unsigned long page_number; // number of pages
	unsigned long page_size; // size of page in bytes
	unsigned long page_size_shift; // used to transfer from address to page id
	unsigned long *map; // the bitmap
};

// Hash table entry for per-process page bitmap
struct adc_page_bitmap_entry {
	pid_t pid; // process ID
	struct hlist_node node; // hash table node
	struct adc_page_bitmap bitmap; // bitmap data
};

void init_adc_page_bitmap(unsigned long base, unsigned long page_number,
			  unsigned long page_size);
void free_adc_page_bitmap(void);
int test_adc_page_bitmap(unsigned long addr,
			 struct adc_page_bitmap_entry *entry);
void zero_adc_page_bitmap(void);
void clear_adc_page_bitmap(unsigned long from_page_id,
			   unsigned long to_page_id);
void set_adc_page_bitmap(unsigned long from_page_id, unsigned long to_page_id);
void mod_adc_page_bitmap(unsigned int mode, unsigned long from_page_id,
			 unsigned long to_page_id);
struct adc_page_bitmap_entry *adc_get_bitmap_by_pid(pid_t pid);

static inline unsigned long addr_to_pageid(struct adc_page_bitmap *pages,
					   unsigned long addr)
{
	return (addr - pages->base) >> pages->page_size_shift;
}

static inline unsigned long pageid_to_addr(struct adc_page_bitmap *pages,
					   unsigned long page_id)
{
	return pages->base + (page_id << pages->page_size_shift);
}

static inline void dump_page_bitmap_by_pages(struct adc_page_bitmap *pages)
{
	if (pages && pages->map) {
		int map_id;
		printk("dump page bitmap: "
		       "base %#016lx, end %#016lx, "
		       "num of page %lu, size of page %lu bytes, "
		       "shift of page %lu bits\n",
		       pages->base, pages->end, pages->page_number,
		       pages->page_size, pages->page_size_shift);
		for (map_id = 0; map_id < BITS_TO_LONGS(pages->page_number);
		     map_id++)
			printk("dump page bitmap: map[%i] %#016lx\n", map_id,
			       pages->map[map_id]);
	}
}

// Dump current process's bitmap (non-inline to access hash table)
void dump_page_bitmap(void);

static inline void adc_profile_counter_inc(enum adc_counter_type type)
{
	atomic_long_inc(&adc_profile_counters[type]);
}

static inline void adc_profile_counter_dec(enum adc_counter_type type)
{
	atomic_long_dec(&adc_profile_counters[type]);
}

static inline void adc_profile_counter_add(enum adc_counter_type type,
					   long value)
{
	atomic_long_add(value, &adc_profile_counters[type]);
}

static inline void adc_profile_counter_sub(enum adc_counter_type type,
					   long value)
{
	atomic_long_sub(value, &adc_profile_counters[type]);
}

static inline void reset_adc_profile_counter(enum adc_counter_type type)
{
	atomic_long_set(&adc_profile_counters[type], 0);
}

#endif