/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef PIE_RTW88_REGISTERS_H
#define PIE_RTW88_REGISTERS_H
#include "rtw88.h"
#define BIT(n) RTW88_BIT(n)
#define GENMASK(h, l) ((UINT32_MAX >> (31 - (h))) & (UINT32_MAX << (l)))
#include "rtw88_regs.inc"
#endif
