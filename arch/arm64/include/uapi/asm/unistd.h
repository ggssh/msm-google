/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define __ARCH_WANT_RENAMEAT

#include <asm-generic/unistd.h>

/* Custom system calls for ARM64 */
#define __NR_init_page_bitmap 1081
__SYSCALL(__NR_init_page_bitmap, sys_init_page_bitmap)

#define __NR_mod_page_bitmap 1082
__SYSCALL(__NR_mod_page_bitmap, sys_mod_page_bitmap)

#define __NR_reset_swap_stats 1083
__SYSCALL(__NR_reset_swap_stats, sys_reset_swap_stats)

#define __NR_get_swap_stats 1084
__SYSCALL(__NR_get_swap_stats, sys_get_swap_stats)

#undef __NR_syscalls
#define __NR_syscalls (__NR_get_swap_stats+1)