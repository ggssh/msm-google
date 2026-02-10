#include "asm/cache.h"
#include "linux/bitops.h"
#include "linux/mm.h"
#include "linux/types.h"
#include <linux/mm_stat.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sched.h>

// Hash table for per-process page bitmaps
#define ADC_BITMAP_HASH_BITS 16
#define ADC_BITMAP_HASH_SIZE (1 << ADC_BITMAP_HASH_BITS)
static DEFINE_HASHTABLE(adc_bitmap_hash, ADC_BITMAP_HASH_BITS);
static DEFINE_SPINLOCK(adc_bitmap_lock);

struct adc_madv_time_stat_list {
	atomic64_t accum_vals[NUM_ADC_MADV_BREAKDOWN_TYPE];
	atomic_t cnt;
};

struct adc_madv_time_stat_list adc_madv_breakdowns[NUM_ADC_MADV_TYPE];
static const char
	*adc_madv_breakdown_names[NUM_ADC_MADV_BREAKDOWN_TYPE] __read_mostly = {
		"ADC_MADV_FREE_TOTAL",	       "ADC_MADV_FREE_SINGLE_VMA",
		"ADC_MADV_FREE_LRU",	       "ADC_MADV_FREE_LRU_DRAIN",
		"ADC_MADV_FREE_FLUSH_TLB",     "ADC_MADV_FREE_FLUSH_TLB_PEND",
		"ADC_MADV_FREE_WALK_RANGE",    "ADC_MADV_FREE_WALK_PMD",
		"ADC_MADV_FREE_WALK_PTEs",     "ADC_MADV_FREE_PTE_NOT_P",
		"ADC_MADV_FREE_MAKE_PTE",      "ADC_MADV_FREE_MMAP_LOCK",
		"ADC_MADV_FREE_PTE_LOCK",      "ADC_MADV_FREE_PAGE_LOCK",
		"ADC_MADV_FREE_ENTER_SYSCALL",
	};

// yizhe: MADV_FREE breakdown
inline void adc_madv_breakdown_stt(uint64_t *madv_breakdown,
				   enum adc_madv_breakdown_type type,
				   uint64_t ts)
{
	if (!madv_breakdown)
		return;
	madv_breakdown[type] -= ts;
}

inline void adc_madv_breakdown_end(uint64_t *madv_breakdown,
				   enum adc_madv_breakdown_type type,
				   uint64_t ts)
{
	if (!madv_breakdown)
		return;
	madv_breakdown[type] += ts;
}

inline void reset_adc_madv_breakdown(void)
{
	int i, j;
	for (i = 0; i < NUM_ADC_MADV_TYPE; i++) {
		for (j = 0; j < NUM_ADC_MADV_BREAKDOWN_TYPE; j++) {
			atomic64_set(&adc_madv_breakdowns[i].accum_vals[j], 0);
		}
		atomic_set(&adc_madv_breakdowns[i].cnt, 0);
	}
}

inline void accum_adc_madv_breakdown(uint64_t madv_breakdown[],
				     enum adc_madv_type madv_type)
{
	const int MAX_CNT = (1 << 30);
	if (!madv_breakdown)
		return;
	if (atomic_read(&adc_madv_breakdowns[madv_type].cnt) < MAX_CNT) {
		int i;
		atomic_inc(&adc_madv_breakdowns[madv_type].cnt);
		for (i = 0; i < NUM_ADC_MADV_BREAKDOWN_TYPE; i++) {
			atomic64_add(
				madv_breakdown[i],
				&adc_madv_breakdowns[madv_type].accum_vals[i]);
		}
	}
}

inline void dump_adc_madv_breakdown(void)
{
	int i, j;
	for (i = 0; i < NUM_ADC_MADV_TYPE; i++) {
		printk(KERN_INFO "YYZ: MADV_FREE: %lu times\n",
		       atomic64_read(&adc_madv_breakdowns[i].cnt));
		for (j = 0; j < NUM_ADC_MADV_BREAKDOWN_TYPE; j++) {
			printk(KERN_INFO "YYZ: MADV_FREE [%s]: %lu ns\n",
			       adc_madv_breakdown_names[j],
			       atomic64_read(
				       &adc_madv_breakdowns[i].accum_vals[j]));
		}
	}
}

// yizhe: profile counters
atomic_long_t adc_profile_counters[NUM_ADC_COUNTER_TYPE] = { 0 };
EXPORT_SYMBOL(adc_profile_counters);

void reset_adc_swap_stats(void)
{
	int i;
	for (i = 0; i < NUM_ADC_COUNTER_TYPE; i++) {
		reset_adc_profile_counter(i);
	}
}

void get_adc_swap_stats(struct swap_stats *stats)
{
	if (!stats)
		return;
	// total stats, does not distinguish between process types.
	stats->swapin_out_heap = get_adc_profile_counter(ADC_SWAPIN_OUT_HEAP);
	stats->swapin_in_heap = get_adc_profile_counter(ADC_SWAPIN_IN_HEAP);
	stats->swapin_in_heap_free =
		get_adc_profile_counter(ADC_SWAPIN_IN_HEAP_FREE);
	stats->swapout_out_heap = get_adc_profile_counter(ADC_SWAPOUT_OUT_HEAP);
	stats->swapout_in_heap = get_adc_profile_counter(ADC_SWAPOUT_IN_HEAP);
	stats->swapout_in_heap_free =
		get_adc_profile_counter(ADC_SWAPOUT_IN_HEAP_FREE);
}

__maybe_unused static const char
	*adc_profile_counter_names[NUM_ADC_COUNTER_TYPE] = {
		[ADC_SWAPIN_OUT_HEAP] = "swapin_out_heap",
		[ADC_SWAPIN_IN_HEAP] = "swapin_in_heap",
		[ADC_SWAPIN_IN_HEAP_FREE] = "swapin_in_heap_free",
		[ADC_SWAPOUT_OUT_HEAP] = "swapout_out_heap",
		[ADC_SWAPOUT_IN_HEAP] = "swapout_in_heap",
		[ADC_SWAPOUT_IN_HEAP_FREE] = "swapout_in_heap_free",
	};

__maybe_unused static const char *adc_jvm_heap_flag_names[NUM_JVM_HEAP_FLAG] = {
	[OUT_JVM_HEAP] = "out_jvm_heap",
	[IN_JVM_HEAP] = "in_jvm_heap",
	[IN_JVM_HEAP_FREE] = "in_jvm_heap_free",
};

// Find bitmap entry by process ID
static struct adc_page_bitmap_entry *find_bitmap_by_pid(pid_t pid)
{
	struct adc_page_bitmap_entry *entry;

	hash_for_each_possible (adc_bitmap_hash, entry, node, pid) {
		if (entry->pid == pid)
			return entry;
	}
	return NULL;
}

struct adc_page_bitmap_entry *adc_get_bitmap_by_pid(pid_t pid)
{
	return find_bitmap_by_pid(pid);
}

// Get or create bitmap entry for current process
static struct adc_page_bitmap_entry *get_or_create_current_bitmap(void)
{
	pid_t pid = task_tgid_nr(current);
	struct adc_page_bitmap_entry *entry;

	spin_lock(&adc_bitmap_lock);
	entry = find_bitmap_by_pid(pid);
	if (!entry) {
		entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
		if (entry) {
			entry->pid = pid;
			hash_add(adc_bitmap_hash, &entry->node, pid);
			printk("YYZ: Created bitmap entry for PID %d\n", pid);
		} else {
			printk("YYZ: Failed to allocate bitmap entry for PID %d\n",
			       pid);
		}
	}
	spin_unlock(&adc_bitmap_lock);
	return entry;
}

// Get bitmap entry for current process (returns NULL if not exists)
static struct adc_page_bitmap_entry *get_current_bitmap(void)
{
	pid_t pid = task_tgid_nr(current);
	struct adc_page_bitmap_entry *entry;

	spin_lock(&adc_bitmap_lock);
	entry = find_bitmap_by_pid(pid);
	spin_unlock(&adc_bitmap_lock);
	return entry;
}

void init_adc_page_bitmap(unsigned long base, unsigned long page_number,
			  unsigned long page_size)
{
	struct adc_page_bitmap_entry *entry;
	struct adc_page_bitmap *pages;

	entry = get_or_create_current_bitmap();
	if (!entry) {
		printk("YYZ: Failed to get/create bitmap entry\n");
		return;
	}

	pages = &entry->bitmap;
	pages->base = base;
	pages->end = base + page_number * page_size;
	pages->page_number = page_number;
	pages->page_size = page_size;
	pages->page_size_shift = ilog2(page_size);
	if (pages->map != NULL)
		// bitmap_free(pages->map);
		kvfree(pages->map);
	// pages->map = bitmap_zalloc(page_number, GFP_KERNEL);
	pages->map =
		kvmalloc_array(BITS_TO_LONGS(page_number),
			       sizeof(unsigned long), GFP_KERNEL | __GFP_ZERO);
	if (!pages->map) {
		printk("YYZ: Failed to allocate bitmap for PID %d\n",
		       entry->pid);
		return;
	}

	// dump_page_bitmap_by_pages(pages);
	printk("YYZ: Initialized page bitmap for PID %d: base=%#016lx, pages=%lu, size=%lu\n",
	       entry->pid, base, page_number, page_size);
}

void free_adc_page_bitmap(void)
{
	pid_t pid = task_tgid_nr(current);
	struct adc_page_bitmap_entry *entry;

	spin_lock(&adc_bitmap_lock);
	entry = find_bitmap_by_pid(pid);
	if (entry) {
		// dump_page_bitmap_by_pages(&entry->bitmap);
		if (entry->bitmap.map != NULL) {
			// bitmap_free(entry->bitmap.map);
			kvfree(entry->bitmap.map);
			entry->bitmap.map = NULL;
		}
		entry->bitmap.base = 0;
		entry->bitmap.end = 0;
		entry->bitmap.page_number = 0;
		entry->bitmap.page_size = 0;
		entry->bitmap.page_size_shift = 0;
		hash_del(&entry->node);
		kfree(entry);
		printk("YYZ: Freed bitmap entry for PID %d\n", pid);
	}
	spin_unlock(&adc_bitmap_lock);
}

// Dump current process's bitmap
void dump_page_bitmap(void)
{
	struct adc_page_bitmap_entry *entry = get_current_bitmap();
	if (entry) {
		dump_page_bitmap_by_pages(&entry->bitmap);
	} else {
		printk("YYZ: dump_page_bitmap: No bitmap entry for current process (TID %d)\n",
		       task_tgid_nr(current));
	}
}

// @return 1: in flagged page
//         0: in non-flagged page
//        -1: not in pages
int test_adc_page_bitmap(unsigned long addr,
			 struct adc_page_bitmap_entry *entry)
{
	struct adc_page_bitmap *pages;

	if (!entry)
		return -1;

	pages = &entry->bitmap;
	if (pages->map != NULL && addr >= pages->base && addr < pages->end) {
		return test_bit(addr_to_pageid(pages, addr), pages->map);
	} else {
		return -1;
	}
}

void zero_adc_page_bitmap(void)
{
	struct adc_page_bitmap_entry *entry = get_current_bitmap();
	if (entry && entry->bitmap.map != NULL) {
		bitmap_zero(entry->bitmap.map, entry->bitmap.page_number);
	}
}

void clear_adc_page_bitmap(unsigned long from_page_id, unsigned long to_page_id)
{
	struct adc_page_bitmap_entry *entry = get_current_bitmap();
	if (entry && entry->bitmap.map != NULL &&
	    from_page_id < entry->bitmap.page_number &&
	    to_page_id < entry->bitmap.page_number) {
		int i;
		for (i = from_page_id; i <= to_page_id; i++) {
			__clear_bit(i, entry->bitmap.map);
		}
	} else if (entry) {
		printk("YYZ: clear page bitmap: page id %lu large than %lu\n",
		       to_page_id, entry->bitmap.page_number - 1);
	}
}

void set_adc_page_bitmap(unsigned long from_page_id, unsigned long to_page_id)
{
	struct adc_page_bitmap_entry *entry = get_current_bitmap();
	if (entry && entry->bitmap.map != NULL &&
	    from_page_id < entry->bitmap.page_number &&
	    to_page_id < entry->bitmap.page_number) {
		int i;
		for (i = from_page_id; i <= to_page_id; i++) {
			__set_bit(i, entry->bitmap.map);
		}
	} else if (entry) {
		if (entry->bitmap.map == NULL) {
			printk("YYZ: set page bitmap: bitmap is NULL\n");
			return;
		}
		printk("YYZ: set page bitmap: page id %lu large than %lu\n",
		       to_page_id, entry->bitmap.page_number - 1);
	}
}

// @mode 0: free bitmap
//       1: zero bitmap (remove all pages)
//       2: clear bits in [from_page_id, to_page_id]
//       3: set bits in [from_page_id, to_page_id]
//       other: dump page bitmap to dmesg
// @from_page_id, @to_page_id: closed interval [from_page_id, to_page_id]
//       (used for mode 2 and 3; ignored for mode 0 and 1)
void mod_adc_page_bitmap(unsigned int mode, unsigned long from_page_id,
			 unsigned long to_page_id)
{
	struct adc_page_bitmap_entry *entry = NULL;

	if (mode == 0) {
		printk("YYZ: syscall(1082): free bitmap\n");
		free_adc_page_bitmap();
	} else if (mode == 1) {
		printk("YYZ: syscall(1082): zero page bitmap\n");
		zero_adc_page_bitmap();
	} else if (mode == 2) {
		// printk("syscall(454): clear bit[%lu]\n", page_id);
		clear_adc_page_bitmap(from_page_id, to_page_id);
	} else if (mode == 3) {
		// printk("syscall(454): set bit[%lu]\n", page_id);
		set_adc_page_bitmap(from_page_id, to_page_id);
	} else {
		printk("YYZ: syscall(1082): illegal mode %u, will dump bitmap\n",
		       mode);
		entry = get_current_bitmap();
		if (entry) {
			dump_page_bitmap_by_pages(&entry->bitmap);
		} else {
			printk("YYZ: syscall(1082): no bitmap entry for current process\n");
		}
	}
}