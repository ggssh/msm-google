/*
 * Extended system calls declarations for ARM64
 *
 * This file contains declarations for custom system calls
 * that are not part of the standard Linux kernel.
 */

#ifndef __ASM_EXT_SYSCALLS_H
#define __ASM_EXT_SYSCALLS_H
 
#include "linux/mm_stat.h"
#include <linux/linkage.h>

// page bitmap
asmlinkage long sys_init_page_bitmap(unsigned long, unsigned long, unsigned long);
asmlinkage long sys_mod_page_bitmap(unsigned int, unsigned long, unsigned long);

// swap stats
asmlinkage long sys_reset_swap_stats(void);
asmlinkage long sys_get_swap_stats(struct swap_stats __user *);

// madvise profiling
asmlinkage long sys_madvise_profiling(unsigned long, unsigned long, int, unsigned long);

// madvise breakdown
asmlinkage long sys_reset_madvise_breakdown(void);
asmlinkage long sys_dump_madvise_breakdown(void);

#endif /* __ASM_EXT_SYSCALLS_H */