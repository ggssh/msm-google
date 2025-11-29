#include <linux/mm_stat.h>
#include <linux/printk.h>

// just for testing
inline void mm_stat_dump(void)
{
	printk(KERN_INFO "YYZ : mm_stat_dump\n");
}



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