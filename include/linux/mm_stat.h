#ifndef __MM_STAT_H__
#define __MM_STAT_H__

// #include <stdint.h>
#include "linux/types.h"

enum adc_madv_breakdown_type {
	// madv_free
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
inline void accum_adc_madv_breakdown(uint64_t madv_breakdown[],
					enum adc_madv_type madv_type);

void mm_stat_dump(void);

#endif