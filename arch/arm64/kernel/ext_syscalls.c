#include <linux/syscalls.h>
#include <linux/kernel.h>
#include <linux/mm_stat.h>
#include <linux/sched.h>

SYSCALL_DEFINE3(init_page_bitmap, unsigned long, base, unsigned long,
		page_number, unsigned long, page_size)
{
	pid_t pid = task_tgid_nr(current);

	printk(KERN_INFO
	       "YYZ: init page bitmap for PID %d: base=%#016lx, pages=%lu, size=%lu\n",
	       pid, base, page_number, page_size);

	init_adc_page_bitmap(base, page_number, page_size);
	return 0;
}

SYSCALL_DEFINE2(mod_page_bitmap, unsigned int, mode, unsigned long, page_id)
{
	mod_adc_page_bitmap(mode, page_id);
	return 0;
}

SYSCALL_DEFINE0(reset_swap_stats)
{
	printk(KERN_INFO "YYZ: reset swap stats\n");
	reset_adc_swap_stats();
	return 0;
}

SYSCALL_DEFINE1(get_swap_stats, struct swap_stats __user *, stats)
{
	struct swap_stats s;
	get_adc_swap_stats(&s);
	return copy_to_user(stats, &s, sizeof(struct swap_stats)) ? -EFAULT : 0;
}