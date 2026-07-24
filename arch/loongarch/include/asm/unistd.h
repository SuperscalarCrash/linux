/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Author: Hanlu Li <lihanlu@loongson.cn>
 *         Huacai Chen <chenhuacai@loongson.cn>
 *
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */

#ifdef CONFIG_32BIT
#define __ARCH_WANT_STAT64
#define __ARCH_WANT_TIME32_SYSCALLS
#define __ARCH_WANT_SET_GET_RLIMIT
#endif

#include <uapi/asm/unistd.h>

#define __ARCH_WANT_NEW_STAT
#define __ARCH_WANT_SYS_CLONE
#define __ARCH_WANT_MEMFD_SECRET

#define NR_syscalls (__NR_syscalls)
