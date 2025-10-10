/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2024 Tobias Schaffner <tobias.schaffner@siemens.com>.
 */
#ifndef _EVL_RISCV_ASM_UAPI_FPTEST_H
#define _EVL_RISCV_ASM_UAPI_FPTEST_H

#include <linux/types.h>

#define evl_riscv_fp  0x1

/* TODO: Implement fptest */
#define evl_set_fpregs(__features, __val)
#define evl_check_fpregs(__features, __val, __bad) 0

#endif /* !_EVL_RISCV_ASM_UAPI_FPTEST_H */
